#!/bin/bash
# test-meta: arch=x64 needs= est=17 local-only=0
# test-shell-coexist-qemu.sh — Console Mirror concurrency linchpin.
#
# Proves the one thing the whole Console Mirror feature rests on: a real
# foreground Shell.efi (gBS->StartImage blocks — the Shell owns the
# foreground) coexists with a background HTTP server pumped off a firmware
# periodic timer (axl_loop_attach_driver, the resident-driver model). While
# the Shell sits at its prompt in WaitForEvent(ConIn) — which lowers TPL and
# lets the periodic timer fire — the loop is drained and the server keeps
# serving. A 200 from the host curl, returned WHILE the Shell is foreground,
# is the proof.
#
# AxlTestNet.efi serve-shell-coexist: start HTTP on 8080, attach_driver, then
# axl_shell_launch() (StartImage Shell.efi, blocks). The inner Shell gets no
# input, so it never exits — we assert on the curl and let QEMU time out.
#
# A real Shell.efi is staged literally as \Shell.efi at the ESP root (next to
# AxlTestNet.efi, so axl_driver_locate("Shell.efi") finds it). If the runner
# has no standalone Shell.efi, the test SKIP-balances (the app prints
# NO_SHELL) rather than failing.
#
# Usage: ./test/integration/test-shell-coexist-qemu.sh

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

# Stage a real Shell.efi literally named Shell.efi at the ESP root, so the
# launcher's axl_driver_locate("Shell.efi") finds it in the running image's
# own directory. test_setup already ran find_firmware, so find_shell_efi
# (which needs FW_CODE) works here.
SHELL_SRC=$(find_shell_efi "$TEST_ARCH" 2>/dev/null || true)
HAVE_SHELL=0
if [[ -n "$SHELL_SRC" && -f "$SHELL_SRC" ]]; then
    test_add_efi "$SHELL_SRC" "Shell.efi"
    HAVE_SHELL=1
    echo "  Staged Shell.efi from: $SHELL_SRC"
else
    echo "  WARNING: no standalone Shell.efi found — test will SKIP-balance"
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

echo Starting shell-coexistence spike...
AxlTestNet.efi serve-shell-coexist
NSHEOF

test_build_image

echo "=== Console Mirror shell+HTTP coexistence spike ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server + Shell launch..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server is ready; foreground Shell should now be launching"
sleep 3

PASS=0
FAIL=0
SKIP=0
BASE_URL="http://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s -H "Connection: close" --max-time 10)
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

# If the app reported it couldn't find a Shell.efi, SKIP-balance the two
# coexistence assertions instead of failing on a runner without a shell.
test_clean_log
if [[ "$HAVE_SHELL" -eq 0 ]] || grep -q "NO_SHELL" "$TEST_CLEAN_LOG"; then
    skip "no Shell.efi staged — coexistence GET (1/2)"
    skip "no Shell.efi staged — coexistence GET (2/2)"
    skip "no Shell.efi staged — sources un-masking (file+fv)"
else
    # Un-masking: a staged Shell.efi must NOT hide the firmware FV Shell.
    # axl_shell_sources reports each independently — with the file staged AND
    # OVMF's FV Shell present, both flags are set (whereas axl_shell_locate
    # collapses to FILE and the FV becomes invisible). Only assertable when the
    # firmware actually carries an FV Shell to un-mask (fv=1); else SKIP-balance.
    SRC_LINE=$(grep -oE 'SOURCES:file=[01],fv=[01],fvn=[0-9]+' "$TEST_CLEAN_LOG" | head -1)
    SRC_FILE=$(echo "$SRC_LINE" | sed -E 's/.*file=([01]).*/\1/')
    SRC_FV=$(echo "$SRC_LINE" | sed -E 's/.*,fv=([01]).*/\1/')
    if [[ "$SRC_FV" == "1" ]]; then
        [[ "$SRC_FILE" == "1" ]] \
            && pass "axl_shell_sources reports file AND fv (staged Shell.efi does not mask the FV)" \
            || fail "axl_shell_sources missed the staged Shell.efi ('$SRC_LINE')"
    else
        skip "no FV Shell on this firmware — sources un-masking not exercisable ('$SRC_LINE')"
    fi

    # The crux: HTTP must answer WHILE the foreground Shell blocks in
    # StartImage. The driver-tick timer pumps the loop during the Shell's
    # WaitForEvent. A 200 here proves shell + HTTP coexist.
    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTP GET /plain returns 200 while Shell is foreground" \
        || fail "HTTP GET /plain (got '$CODE' — loop starved by blocked Shell?)"

    # A second request proves the timer keeps pumping (not a one-shot fluke).
    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTP GET /plain still 200 (timer keeps pumping under Shell)" \
        || fail "HTTP second GET (got '$CODE')"
fi

# The Shell must NOT have exited — if SHELL_EXITED appears, StartImage didn't
# actually block the foreground (which would defeat the whole model).
if grep -q "SHELL_EXITED" "$TEST_CLEAN_LOG"; then
    fail "Shell exited unexpectedly — foreground StartImage did not block"
else
    pass "Shell held the foreground (no premature exit)"
fi

echo ""
printf "Shell-coexistence spike: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIP" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
