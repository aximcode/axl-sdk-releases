#!/bin/bash
# test-meta: arch=x64 needs= est=16 local-only=0
# test-console-mirror-https-qemu.sh — Console Mirror rung 5 (P3).
#
# The deployment-faithful SoftBMC shape, all three at once: the
# AxlConsoleMirror wrapping the console, an HTTPS server pumped by
# axl_loop_attach_driver (TPL_CALLBACK), and a real foreground child
# Shell.efi. Proves the TLS handshake completes under the timer pump WHILE
# the mirrored Shell holds the foreground — the sharp async-TLS-at-raised-TPL
# envelope combined with a blocked StartImage. A 200 from the host curl,
# returned while the Shell is foreground, is the proof.
#
# Builds AXL_TLS=1. Stages a real Shell.efi at the ESP root; SKIP-balances if
# the runner has none. Requires: AXL_TLS=1 toolchain support.
#
# Usage: ./test/integration/test-console-mirror-https-qemu.sh [--arch X64|AARCH64]

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

# Stage a real Shell.efi at the ESP root (axl_driver_locate finds it).
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

echo Starting HTTPS + mirror + foreground Shell...
AxlTestNet.efi serve-tls-shell-coexist
NSHEOF

test_build_image

echo "=== Console Mirror rung 5: HTTPS + mirror + foreground Shell ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for HTTPS server + Shell launch..."

if ! test_wait_for "READY" 60; then
    # No-TLS build is a hard config error, not a SKIP.
    test_clean_log
    if grep -qa "NO_TLS" "$TEST_CLEAN_LOG"; then
        echo "FAIL: built without AXL_TLS=1"
    else
        echo "FAIL: server did not reach READY within 60 seconds"
    fi
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Ready; foreground Shell launching"
sleep 3

PASS=0
FAIL=0
SKIP=0
BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 10)
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

test_clean_log
if [[ "$HAVE_SHELL" -eq 0 ]] || grep -qa "NO_SHELL" "$TEST_CLEAN_LOG"; then
    skip "no Shell.efi staged — HTTPS coexistence GET (1/2)"
    skip "no Shell.efi staged — HTTPS coexistence GET (2/2)"
else
    # The crux: a TLS handshake must COMPLETE under the driver-tick pump WHILE
    # the foreground Shell blocks in StartImage. A 200 proves all three coexist.
    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTPS GET /plain returns 200 (handshake completed under the mirrored Shell)" \
        || fail "HTTPS GET /plain (got '$CODE' — handshake stalled or loop starved)"

    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTPS GET /plain still 200 (timer keeps serving under the Shell)" \
        || fail "HTTPS second GET (got '$CODE')"
fi

# The Shell must still hold the foreground (no premature exit/return).
if grep -qa "SHELL_EXITED\|MIRROR" "$TEST_CLEAN_LOG"; then
    fail "launcher returned unexpectedly"
else
    pass "Shell held the foreground while HTTPS served"
fi

echo ""
printf "Console-mirror rung 5: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIP" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
