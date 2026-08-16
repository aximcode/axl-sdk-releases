#!/bin/bash
# test-meta: arch=x64 needs=openssl est=29 local-only=0
# test-consumer-emulator-qemu.sh — the consumer-emulator harness.
#
# Models SoftBMC's execution topology in-repo so the SDK breaks first, not the
# consumer. The unit suite only ever runs ONE server on its OWN loop at
# TPL_APPLICATION; the bugs that cost days of back-and-forth all lived in the
# RESIDENT-DRIVER model the unit tests never modeled. This boots the canonical
# hazardous shape (AxlTestNet serve-hazard-driver): HTTPS (8443) AND plain HTTP
# (8081) on ONE shared loop, pumped from an axl_loop_attach_driver tick at
# raised TPL, with a per-client WS endpoint and a broadcast-burst endpoint.
#
# It walks the scenarios that each surfaced a wedge and runs a LIVENESS PROBE
# after every one — these bugs manifest as a loop that silently stops
# dispatching, so a probe that times out is the failure signal:
#   A. HTTPS GET                         (loop serving)            -> probe
#   B. plain GET on the 2nd server       (adbf5461 dead-accept)    -> probe
#   C. wss connect + clean close         (e90b87e4 teardown wedge) -> probe
#   D. wss connect + broadcast burst     (4563aabf TLS desync)     -> probe
#   E. no AXL_DEBUG_ASSERT fired          (debug-build invariants held)
#
# See docs/AXL-Concurrency.md "Testing the model". New net/loop behavior should
# run through this harness.
#
# Requires: AXL_TLS=1 build, python3, openssl-less (self-signed in-guest).
# Usage: ./test/integration/test-consumer-emulator-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1
test_parse_args "$@"
test_setup

if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: consumer-emulator is X64-only (AARCH64/TCG has no guest NIC link)"
    exit 0
fi

H_TLS=$(test_port 0)     # host -> guest 8443 (HTTPS)
H_PLAIN=$(test_port 1)   # host -> guest 8081 (plain HTTP)

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" AXL_TLS=1 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

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

echo Starting consumer-emulator (serve-hazard-driver)...
AxlTestNet.efi serve-hazard-driver
NSHEOF

test_build_image

echo "=== AXL consumer-emulator Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device "$(_test_nic_device),netdev=net0"
    -netdev "user,id=net0,hostfwd=tcp::${H_TLS}-:8443,hostfwd=tcp::${H_PLAIN}-:8081"
)
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID  (host $H_TLS->8443 HTTPS, $H_PLAIN->8081 plain)"
echo "  Waiting for servers..."
if ! test_wait_for "READY" 60; then
    echo "FAIL: servers did not start within 60 seconds"
    test_clean_log; echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Servers ready"
sleep 2

TLS_URL="https://127.0.0.1:${H_TLS}"
PLAIN_URL="http://127.0.0.1:${H_PLAIN}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 10)

# Speak TLS + the WS handshake to $1 (a path), then take $2 action:
#   close  -> send a masked CLOSE frame and disconnect (teardown trigger)
#   burst  -> send one TEXT frame (server fires a back-to-back broadcast burst),
#             drain a few frames, then close
# Prints HANDSHAKE_OK on a 101, else HANDSHAKE_FAIL.
ws_exercise() {
    python3 - "$H_TLS" "$1" "$2" << 'PYEOF'
import sys, socket, ssl, base64, os, time

port, path, action = int(sys.argv[1]), sys.argv[2], sys.argv[3]
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

raw = socket.create_connection(("127.0.0.1", port), timeout=10)
s = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
s.settimeout(10)

key = base64.b64encode(os.urandom(16)).decode()
s.sendall((
    f"GET {path} HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
).encode())

resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk
if b"101" not in resp.split(b"\r\n", 1)[0]:
    print("HANDSHAKE_FAIL", resp.split(b"\r\n", 1)[0])
    s.close()
    sys.exit(0)

time.sleep(0.3)
if action == "burst":
    # A masked TEXT frame "go" — the server replies with a burst of broadcasts.
    payload = b"go"
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)
    time.sleep(0.5)
    try:
        while True:
            if not s.recv(4096):
                break
    except Exception:
        pass

# Masked CLOSE frame, then disconnect.
try:
    s.sendall(bytes([0x88, 0x80]) + os.urandom(4))
    time.sleep(0.3)
    s.close()
except Exception:
    pass
print("HANDSHAKE_OK")
PYEOF
}

# --- A. HTTPS serving + liveness ----------------------------------------
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" "${TLS_URL}/api/version" 2>/dev/null || true)
[[ "$CODE" == "200" ]] && test_host_pass "HTTPS GET /api/version (baseline)" \
                       || test_host_fail "HTTPS GET baseline (got '$CODE')"
test_liveness_probe "${TLS_URL}/api/version" "after HTTPS baseline"

# --- B. plain second server on the SHARED loop dispatches + liveness -----
CODE=$(curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" "${PLAIN_URL}/plain" 2>/dev/null || true)
[[ "$CODE" == "200" ]] && test_host_pass "plain HTTP GET :8081 (2nd server on shared loop dispatches)" \
                       || test_host_fail "plain HTTP GET :8081 (got '$CODE' — adbf5461 class)"
test_liveness_probe "${TLS_URL}/api/version" "after plain 2nd-server GET"

# --- C. wss connect + clean close must not wedge the loop + liveness -----
R=$(ws_exercise "/ws-console" close)
echo "$R" | grep -q HANDSHAKE_OK && test_host_pass "wss /ws-console connect + clean close" \
                                 || test_host_fail "wss /ws-console (got '$R')"
test_liveness_probe "${TLS_URL}/api/version" "after WS connect/close (e90b87e4 wedge)"

# --- D. wss broadcast burst over TLS must not desync the stream + liveness
R=$(ws_exercise "/ws-burst" burst)
echo "$R" | grep -q HANDSHAKE_OK && test_host_pass "wss /ws-burst connect + broadcast burst" \
                                 || test_host_fail "wss /ws-burst (got '$R')"
test_liveness_probe "${TLS_URL}/api/version" "after WS broadcast burst (4563aabf desync)"
# And the plain server still answers after all the TLS/WS churn.
test_liveness_probe "${PLAIN_URL}/plain" "plain server still alive after WS churn"

# --- E. no debug-build invariant fired -----------------------------------
test_refute_debug_assert

if ! test_host_summary "consumer-emulator tests ($TEST_ARCH)"; then
    echo ""; test_clean_log; echo "--- Serial log (tail) ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
