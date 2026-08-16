#!/bin/bash
# test-meta: arch=x64 needs= est=19 local-only=0
# Driver-mode HTTP server integration test.
#
# Boots the example DXE driver `http-server-driver.efi` from the
# UEFI shell, returns to the shell prompt (driver runs in
# background via axl_loop_attach_driver), then hits the server with
# multiple curl requests and verifies BOTH headers AND body arrive
# every time.
#
# This is the regression test for the bug axl-webfs reported:
# "headers arrive but body never does, server stuck in CLOSE-WAIT,
# subsequent connections refused." Root cause was send_response's
# ephemeral-loop axl_loop_run path calling gBS->WaitForEvent at
# TPL_CALLBACK (illegal). The fully-async send_response path that
# this test exercises has no WaitForEvent dependency.
#
# Usage: ./test/integration/test-driver-http.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

# Build the driver
make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} http-server-driver 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/http-server-driver.efi"

# startup.nsh: bring up networking (required for the server to
# actually bind), then load the driver. The driver returns to the
# shell after axl_loop_attach_driver succeeds; firmware-managed
# notifications drive the loop from there.
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

echo Loading HTTP server driver...
load http-server-driver.efi
stall 1000000

echo Driver loaded; firmware timer is now driving the loop.
echo READY
NSHEOF

test_build_image

echo "=== Driver-Mode HTTP Server Integration Test ($TEST_ARCH) ==="

trap 'test_cleanup' EXIT

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for driver and server..."

if ! test_wait_for "READY" 90; then
    echo "FAIL: driver did not finish loading within 90 seconds"
    test_clean_log
    echo "--- Serial log (tail) ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

# The driver prints PASS lines as it brings each piece up. Verify
# all of them landed (load → routes → server-attach → loop-attach).
echo ""
echo "  --- Driver bring-up ---"
DRIVER_PASS=$(grep -c '^PASS: ' "$TEST_LOG" || true)
DRIVER_FAIL=$(grep -c '^FAIL: ' "$TEST_LOG" || true)
grep -E '^(PASS|FAIL): ' "$TEST_LOG" | while IFS= read -r line; do
    echo "  $line"
done

if [[ $DRIVER_FAIL -ne 0 ]]; then
    echo "FAIL: driver bring-up reported errors"
    exit 1
fi

# Wait an extra second to ensure the firmware notify has fired at
# least once (50 ms tick × 20).
sleep 1

# ---------------------------------------------------------------------------
# HTTP tests via curl — the actual regression coverage.
#
# We hit each endpoint multiple times to exercise:
#   - Connection: close path (body delivered, FIN sent)
#   - Subsequent connections accepted (no backlog wedge)
#   - Different response shapes (JSON, text, echo with body)
# ---------------------------------------------------------------------------

PASS=0
FAIL=0
BASE_URL="http://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s -H "Connection: close" --max-time 8)

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

http_get() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}$1" 2>/dev/null || true
}

http_post() {
    curl "${CURL_OPTS[@]}" -w "\n%{http_code}" -X POST -d "$2" "${BASE_URL}$1" 2>/dev/null || true
}

echo ""
echo "  --- HTTP requests against driver-mode server ---"

# Request 1: GET / — large body. The original bug manifested here:
# headers arrived but the body never did. We assert the FULL body
# is delivered.
RESP=$(http_get "/")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET / returns 200 (request 1)" \
                     || fail "GET / first request (got $CODE)"
echo "$BODY" | grep -q "Hello from a DXE-driver-mode HTTP server" \
    && pass "GET / first response has full body (NOT just headers)" \
    || fail "GET / first response missing/truncated body"

# Request 2: GET /version — JSON response, immediately after #1.
# Validates the listener accepts subsequent connections (the bug
# reproducer reported requests 2+ wedging at TCP-handshake-only).
RESP=$(http_get "/version")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "GET /version returns 200 (request 2)" \
                     || fail "GET /version (got $CODE)"
echo "$BODY" | grep -q '"version"' && pass "/version body has version field" \
                                   || fail "/version body missing version"
echo "$BODY" | grep -q '"mode":"driver"' && pass "/version reports driver mode" \
                                         || fail "/version mode mismatch"

# Request 3: POST /echo — round-trip payload. Echoes request body.
RESP=$(http_post "/echo" "round-trip-payload")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "POST /echo returns 200 (request 3)" \
                     || fail "POST /echo (got $CODE)"
echo "$BODY" | grep -q "round-trip-payload" \
    && pass "/echo returned posted body" \
    || fail "/echo body mismatch"

# Requests 4-8: hammer GET / five more times in a row to confirm
# the listener doesn't wedge after the first round of traffic
# (CLOSE-WAIT accumulation was the original symptom signature).
for i in 4 5 6 7 8; do
    RESP=$(http_get "/")
    CODE=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    if [[ "$CODE" == "200" ]] \
       && echo "$BODY" | grep -q "Hello from a DXE-driver-mode HTTP server"
    then
        pass "GET / request $i delivers complete body"
    else
        fail "GET / request $i broken (code=$CODE)"
        break
    fi
done

# Error-path regression: a malformed request line must return a
# 400 response, AND the listener must still accept normal traffic
# afterward. Bug shape this protects against: with one-event-per-
# tick driver-mode dispatch, tx-event completions queued behind
# the older accept signal; conn slots stayed active=true forever
# because on_response_sent never ran. The 9th connection saw NO
# FREE SLOT and the listener appeared wedged — only the
# success-path test (8 conns) didn't surface it because the slot
# pool happens to be exactly 8.
echo ""
echo "  --- Error-path regression (malformed request) ---"
GARBAGE_RESPONSE=$(python3 - "$HOST_PORT" <<'PYEOF' 2>/dev/null || true
import socket, sys
s = socket.socket(); s.settimeout(8)
s.connect(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"INVALID\r\nHost: x\r\n\r\n")
buf = b""
try:
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        buf += chunk
except socket.timeout: pass
s.close()
sys.stdout.write(buf.decode(errors="replace"))
PYEOF
)
echo "$GARBAGE_RESPONSE" | grep -qE '^HTTP/1\.[01] 400' \
    && pass "malformed request gets 400" \
    || fail "malformed request — expected 400, got: $(echo "$GARBAGE_RESPONSE" | head -1)"

# Listener must still serve a normal GET afterward.
RESP=$(http_get "/version")
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "GET /version after malformed (listener not wedged)" \
                     || fail "listener wedged after malformed request (got $CODE)"

# Sanity: confirm CLOSE-WAIT sockets drain (the original bug
# signature was "CLOSE-WAIT permanently stuck because server's
# TCP4 never advanced past close-rearm"). Give the guest's TCP4
# state machine ~3 s to drain — active closes go through
# TIME_WAIT (≈2 RTT), and slirp adds a forwarding hop. After
# settle, no CLOSE-WAIT on this port should remain.
sleep 3
CLOSE_WAIT_COUNT=$(ss -tn 2>/dev/null \
    | awk -v port="$HOST_PORT" 'index($0, ":"port) && $1=="CLOSE-WAIT"' \
    | wc -l)
[[ "$CLOSE_WAIT_COUNT" == "0" ]] \
    && pass "No CLOSE-WAIT sockets remain after settle (server FIN'd cleanly)" \
    || fail "$CLOSE_WAIT_COUNT CLOSE-WAIT sockets after 3s — server stuck mid-close"

echo ""
echo "Driver bring-up: $DRIVER_PASS passed, $DRIVER_FAIL failed"
echo "HTTP requests:   $PASS passed, $FAIL failed"

if [[ $FAIL -eq 0 && $PASS -gt 0 && $DRIVER_FAIL -eq 0 ]]; then
    echo "Driver-mode HTTP test: OK"
    exit 0
else
    echo "Driver-mode HTTP test: FAILED"
    test_clean_log
    echo "--- Serial log (tail) ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
