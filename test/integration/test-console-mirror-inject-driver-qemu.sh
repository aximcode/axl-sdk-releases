#!/bin/bash
# test-meta: arch=x64 needs= est=24 local-only=0
# test-console-mirror-inject-driver-qemu.sh — the AxlConsoleMirror P1 GATE.
#
# Proves axl_console_mirror_inject_text wakes the FOREGROUND Shell when the
# injection happens from a WS HANDLER running inside the axl_loop_attach_driver
# dispatch (raised TPL) — the exact SoftBMC RemoteShell shape, and the gap the
# console-mirror handoff flagged P1 against. (The edit gate already proved the
# standalone-timer injection path; this proves the pumped-loop-WS-handler path.)
#
# AxlTestNet.efi serve-ws-shell-inject starts a plain HTTP server on a loop with
# a per-client WS endpoint whose handler forwards each received frame into the
# console mirror, installs the mirror, attaches the driver tick, then launches a
# real child Shell.efi (foreground, blocks). The harness's WS client sends a
# command that writes a unique marker to a file, then `exit`. When the child
# Shell exits, the launcher returns; the app reads the file back. The marker
# present == the keys injected from the pumped-loop WS handler actually drove
# the Shell (deterministic file readback, like the edit gate).
#
# Transport is plain ws:// on purpose: the injection wake path (SignalEvent on
# the wrapped ConIn wait_key) is transport-agnostic and post-decrypt, so plain
# ws isolates the P1 variable. The TLS teardown path is covered separately by
# test-ws-teardown-driver-qemu.sh.
#
# Needs a real Shell.efi staged at the ESP root; SKIP-balances otherwise.
#
# Usage: ./test/integration/test-console-mirror-inject-driver-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Stage a real Shell.efi at the ESP root (axl_driver_locate finds it next to
# the running image).
SHELL_SRC=$(find_shell_efi "$TEST_ARCH" 2>/dev/null || true)
HAVE_SHELL=0
if [[ -n "$SHELL_SRC" && -f "$SHELL_SRC" ]]; then
    test_add_efi "$SHELL_SRC" "Shell.efi"
    HAVE_SHELL=1
    echo "  Staged Shell.efi from: $SHELL_SRC"
else
    echo "  WARNING: no standalone Shell.efi — test will SKIP-balance"
fi

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting HTTP+WS server + mirror, launching Shell...
AxlTestNet.efi serve-ws-shell-inject
NSHEOF

test_build_image

echo "=== AxlConsoleMirror P1 inject gate ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server..."

if ! test_wait_for "READY" 90; then
    echo "FAIL: server did not start within 90 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server ready; letting the child Shell reach its prompt..."
# The child Shell.efi boots to its interactive prompt after READY. inject_text
# buffers keys in the wrapped ConIn ring, so exact timing is forgiving — but
# give the child a head start so it isn't mid-boot when keys land.
sleep 8

# WS client: connect, send the marker command frame, then the exit frame. The
# server's WS handler injects each frame's bytes into the mirror.
echo "  Driving the Shell via injected WS frames..."
python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, base64, os, time

port = int(sys.argv[1])

def ws_send_text(s, msg):
    data = msg.encode()
    frame = bytearray([0x81, 0x80 | len(data)])
    mask = os.urandom(4)
    frame += mask
    frame += bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    s.sendall(frame)

s = socket.create_connection(("127.0.0.1", port), timeout=10)
s.settimeout(10)
key = base64.b64encode(os.urandom(16)).decode()
req = ("GET /ws-console HTTP/1.1\r\nHost: 127.0.0.1\r\n"
       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk
if b"101" not in resp.split(b"\r\n")[0]:
    print("WS_HANDSHAKE_FAIL")
    s.close()
    sys.exit(0)

# Inject a command that writes a unique marker to a file, then exit the Shell.
# \r is decoded to Enter by inject_text (proven by the edit gate).
ws_send_text(s, "echo P1INJECTOK > fs0:\\p1inj.txt\r")
time.sleep(3)
ws_send_text(s, "exit\r")
time.sleep(1)
s.close()
print("WS_DRIVE_DONE")
PYEOF

echo "  Waiting for the Shell to exit + file readback..."
if ! test_wait_for "P1_INJECT_DONE\|NO_SHELL" 90; then
    echo "FAIL: P1 inject session did not finish within 90 seconds"
    test_clean_log
    echo "--- Serial log (P1 lines) ---"
    grep -a "P1_INJECT\|NO_SHELL" "$TEST_CLEAN_LOG" | tail -20
    echo "--- tail ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

test_clean_log

PASS=0
FAIL=0
SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

# No Shell.efi staged (or launch failed) -> SKIP-balance rather than fail.
if [[ "$HAVE_SHELL" -eq 0 ]] || grep -qa "NO_SHELL" "$TEST_CLEAN_LOG"; then
    skip "no Shell.efi — inject-from-WS-handler drove the Shell"
else
    if grep -qa "P1_INJECT: file_marker=1" "$TEST_CLEAN_LOG"; then
        pass "inject_text from pumped-loop WS handler woke the Shell + ran the command"
    else
        fail "injected command did not reach the Shell (marker file missing)"
    fi
fi

echo ""
printf "Console-mirror P1 inject gate: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIP" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log (P1 lines) ---"
    grep -a "P1_INJECT\|NO_SHELL" "$TEST_CLEAN_LOG" | tail -20
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || { [[ $SKIP -gt 0 && $FAIL -eq 0 ]] && exit 0 || exit 1; }
