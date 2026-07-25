#!/bin/bash
# test-meta: arch=x64 needs= est=18 local-only=0
# test-tcp-close-pendtx-driver-qemu.sh — a GRACEFUL EFI_TCP4.Close() must not
# wedge a resident driver-tick loop when the connection still has un-flushed
# outbound TCP data.
#
# Root cause (softbmc/docs/axl-sdk-console-reshape-snapshot-wedge-handoff.md):
# from a driver-pump notify (axl_loop_attach_driver, TPL_CALLBACK), reset_connection
# -> axl_tcp_close(GRACEFUL) -> EFI_TCP4.Close() flushes the send buffer + drives
# the FIN handshake, whose transmit/ACK needs the MNP periodic timer to fire BELOW
# TPL_CALLBACK. Holding TPL_CALLBACK, that never happens, so Close() spins in
# firmware forever (one core pegged, curl 000, reboot the only exit). The fix
# promotes a raised-TPL connection close to ABORTIVE (RST) — nothing to flush.
#
# Repro shape: AxlTestNet.efi serve-tls-ws-close-pendtx-driver pumps an HTTPS+WS
# server off a 50 ms driver tick and sends a 400 KB frame to each /ws-console
# client on connect. The client reads the 101, then sends a WS CLOSE WITHOUT
# draining — so the server holds ~hundreds of KB of un-flushed TX when it resets
# the connection at raised TPL.
#
# Probe:
#   1. HTTPS GET /api/version                 -> 200 (loop serving)
#   2. wss connect x5: read 101, send CLOSE, DON'T drain the 400 KB, close
#   3. HTTPS GET /api/version                 -> MUST be 200 (RED: 000 = wedge)
#
# Requires: AXL_TLS=1 build.

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8443

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

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

echo Starting HTTPS+WS pending-TX close server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-ws-close-pendtx-driver
NSHEOF

test_build_image

echo "=== TCP close pending-TX driver-loop Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTPS+WS server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not start within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server is ready"
sleep 2

PASS=0
FAIL=0
BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 10)
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Step 1 — baseline.
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET baseline returns 200" \
                       || fail "HTTPS GET baseline (got '$CODE')"

# Step 2 — the trigger: connect, read the 101, send a WS CLOSE, close WITHOUT
# draining the 400 KB frame. Repeat; the server resets each conn at raised TPL
# holding un-flushed TX.
WS_RESULT=$(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, ssl, base64, os, time

port = int(sys.argv[1])

def close_without_drain(i):
    # A wedged server makes even the TLS handshake / connect hang — the whole
    # attempt is guarded so a wedge surfaces as the final GET verdict, not a
    # client-side traceback.
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection(("127.0.0.1", port), timeout=8)
    s = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
    s.settimeout(8)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /ws-console HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n")
    s.sendall(req.encode())
    # Read ONLY the 101 handshake (up to the header terminator); leave the
    # 400 KB frame undrained so the server's send buffer stays full.
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(256)   # small reads: stop right at the header
        if not chunk:
            break
        resp += chunk
    if b"101" not in resp.split(b"\r\n")[0]:
        print(f"HANDSHAKE_FAIL_{i}")
        s.close()
        return False
    # Masked WS CLOSE frame (0x88), then close without reading the big frame.
    close_frame = bytearray([0x88, 0x80]) + os.urandom(4)
    try:
        s.sendall(close_frame)
    except Exception:
        pass
    time.sleep(0.2)
    try:
        s.close()
    except Exception:
        pass
    return True

done = 0
for i in range(1, 6):
    try:
        if not close_without_drain(i):
            break
        done += 1
    except Exception as e:
        # A wedged loop hangs the next connect/handshake — expected on RED.
        print(f"CONN_{i}_ERROR {type(e).__name__}")
        break
    time.sleep(0.2)
print(f"WS_ATTEMPTS_OK {done}")
PYEOF
)
echo "--- WS client output ---"
echo "$WS_RESULT"
echo "------------------------"

# Informational: on RED the loop wedges on the FIRST close, so later connects
# hang — the authoritative check is the post-close GET below.
echo "$WS_RESULT" | grep -qE "WS_ATTEMPTS_OK|CONN_._ERROR" \
    && echo "  INFO: WS client phase ran" \
    || echo "  INFO: WS client produced no summary"

# Step 3 — verdict: the loop must still serve after resetting conns with pending TX.
sleep 1
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET after pending-TX closes returns 200 (loop NOT wedged)" \
                       || fail "HTTPS GET after pending-TX closes (got '$CODE') — LOOP WEDGED"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
