#!/bin/bash
# test-https-driver-qemu.sh — HTTPS served by a RESIDENT driver-tick loop.
#
# Regression test for the TLS handshake stalling when axl_http_server is
# driven by a resident AxlService / DXE driver loop (axl_loop_attach_driver,
# TPL_CALLBACK) instead of a top-level axl_loop_run. The old synchronous
# handshake nested an ephemeral axl_loop_run inside the accept callback,
# which cannot run at the tick's raised TPL — TCP connected and the
# ClientHello arrived, but the server sent no ServerHello and curl timed out
# (curl 28). With the async handshake (axl_tls_handshake_async on the
# server's own loop) the handshake completes under the resident loop.
#
# AxlTestNet.efi serve-tls-driver uses axl_loop_attach_driver + idle (the
# resident shape); the companion test-https.sh covers the axl_loop_run path.
#
# Requires: AXL_TLS=1 build. Usage: ./test/integration/test-https-driver-qemu.sh

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=18444
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

echo Starting HTTPS server (resident driver-tick loop)...
AxlTestNet.efi serve-tls-driver
NSHEOF

test_build_image

echo "=== AxlTls HTTPS resident-loop Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTPS server..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: HTTPS server did not start within 60 seconds"
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

# The crux: a TLS handshake must COMPLETE under the resident driver loop.
# Against the buggy (synchronous-handshake) build this curl times out (28).
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/api/version" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
[[ "$CODE" == "200" ]] && pass "HTTPS GET /api/version returns 200 (handshake completed under driver loop)" \
                       || fail "HTTPS GET (got '$CODE' — handshake likely stalled)"
echo "$BODY" | grep -q '"version"' && pass "HTTPS response body intact" || fail "HTTPS missing version"

# A second request proves the loop keeps serving (not a one-shot fluke).
RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
CODE=$(echo "$RESP" | tail -1)
[[ "$CODE" == "200" ]] && pass "HTTPS GET /plain returns 200" || fail "HTTPS /plain (got '$CODE')"

echo ""
printf "HTTPS resident-loop tests: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; test_clean_log; echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
