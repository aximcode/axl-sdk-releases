#!/bin/bash
# test-meta: arch=x64 needs= est=17 local-only=0
# test-ws-teardown-driver-qemu.sh — WS connect/disconnect must not wedge a
# resident driver-tick loop.
#
# Regression for the SoftBMC console-mirror RemoteShell wedge: an HTTPS server
# with a PER_CLIENT WebSocket endpoint (add_websocket_ex), pumped by
# axl_loop_attach_driver (TPL_CALLBACK) rather than a top-level axl_loop_run.
# A single wss client connect + clean disconnect used to WEDGE the whole loop —
# process_websocket_data's WS_OP_CLOSE echo (and axl_ws_conn_close) did a
# SYNCHRONOUS axl_tls_write, which spins a nested ephemeral axl_loop_run that
# cannot make progress at the tick's raised TPL (the adbf5461 / axl_tls_free
# hazard). The next HTTPS GET then timed out (curl 000). With the async-pong +
# FIN-conveys-close fix the loop keeps serving after a WS connect/disconnect.
#
# Probe sequence (the crux):
#   1. HTTPS GET /api/version            -> 200 (loop serving)
#   2. wss connect to /ws-console, 101, send a CLOSE frame, close cleanly
#   3. HTTPS GET /api/version again      -> MUST be 200 (RED: times out = wedge)
#
# AxlTestNet.efi serve-tls-ws-driver hosts the server; test-https-driver-qemu.sh
# is the no-WS companion (idle HTTPS coexistence only).
#
# Requires: AXL_TLS=1 build. Usage: ./test/integration/test-ws-teardown-driver-qemu.sh

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8443

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

echo Starting HTTPS+WS server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-ws-driver
NSHEOF

test_build_image

echo "=== AxlTls WS-teardown resident-loop Integration Test ($TEST_ARCH) ==="

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

# Step 1 — baseline: the loop serves an HTTPS GET before any WS activity.
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET /api/version returns 200 (pre-WS baseline)" \
                       || fail "HTTPS GET baseline (got '$CODE')"

# Step 2 — wss connect + clean disconnect. Speak TLS, do the WS handshake,
# send a masked CLOSE frame (0x88), close. This is the teardown that wedged
# the loop. We only assert the handshake reached 101; the close is the trigger.
WS_RESULT=$(python3 - "$HOST_PORT" << 'PYEOF'
import sys, socket, ssl, hashlib, base64, os, time

port = int(sys.argv[1])
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

raw = socket.create_connection(("127.0.0.1", port), timeout=10)
s = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
s.settimeout(10)

key = base64.b64encode(os.urandom(16)).decode()
req = (
    "GET /ws-console HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
)
s.sendall(req.encode())

resp = b""
while b"\r\n\r\n" not in resp:
    chunk = s.recv(1024)
    if not chunk:
        break
    resp += chunk

status_line = resp.split(b"\r\n")[0].decode("latin-1")
if "101" not in status_line:
    print(f"HANDSHAKE_FAIL {status_line!r}")
    s.close()
    sys.exit(0)

# Send a masked CLOSE frame — the WS clean-disconnect that triggers the
# server's close-frame echo (the wedge path).
time.sleep(0.3)
close_frame = bytearray([0x88, 0x80]) + os.urandom(4)
try:
    s.sendall(close_frame)
except Exception:
    pass
time.sleep(0.3)
try:
    s.close()
except Exception:
    pass
print("HANDSHAKE_OK")
PYEOF
)
echo "$WS_RESULT" | grep -q "HANDSHAKE_OK" \
    && pass "wss connect to /ws-console reached 101 + closed" \
    || fail "wss handshake/close (got '$WS_RESULT')"

# Step 3 — THE crux: the loop must still serve after the WS teardown.
# Against the buggy build this curl times out (000 / 28).
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET after WS disconnect returns 200 (loop not wedged)" \
                       || fail "HTTPS GET after WS disconnect (got '$CODE' — loop wedged)"

# A second post-teardown request proves it keeps serving (not a one-shot).
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET /plain after WS disconnect returns 200" \
                       || fail "HTTPS /plain after WS disconnect (got '$CODE')"

echo ""
printf "WS-teardown resident-loop tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; test_clean_log; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
