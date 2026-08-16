#!/bin/bash
# test-meta: arch=x64 needs= est=17 local-only=0
# test-shell-fv-qemu.sh — launch the firmware-embedded UEFI Shell from a
# Firmware Volume, with NO Shell.efi staged on any filesystem.
#
# The no-file-staged counterpart of test-shell-coexist-qemu.sh. It proves the
# axl_shell_launch_fv / axl_shell_locate round-trip: under OVMF (which embeds
# the ShellPkg Shell in a readable FV, FFS file GUID gUefiShellFileGuid), the
# app reports LOCATE=2 (AXL_SHELL_FIRMWARE) and then StartImages the FV Shell
# in the foreground. A 200 from the host curl, returned WHILE the FV Shell is
# foreground, proves LoadImage+StartImage transferred control to the
# FV-embedded image and the background HTTP server kept serving off the driver
# tick. Unlike shell-coexist, NOTHING named Shell.efi is staged — so an
# AXL_SHELL_FIRMWARE result here is unambiguous.
#
# AxlTestNet.efi serve-shell-fv-coexist: start HTTP on 8080, attach_driver,
# print LOCATE=<AxlShellSource>, then axl_shell_launch_fv() (blocks). The inner
# Shell gets no input, so it never exits — we assert on the curl + LOCATE and
# let QEMU time out. If the firmware carries no FV Shell, the app prints
# NO_FV_SHELL and we SKIP-balance.
#
# Usage: ./test/integration/test-shell-fv-qemu.sh

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

# Deliberately stage NO Shell.efi — the whole point is launching the
# firmware-embedded copy. (test_build_image still drops a BOOT<arch>.EFI for
# the boot path; that is not named Shell.efi, so axl_driver_locate("Shell.efi")
# won't find it and locate must resolve to the FV.)

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

echo Starting FV-shell-coexistence spike...
AxlTestNet.efi serve-shell-fv-coexist
NSHEOF

test_build_image

echo "=== FV-embedded Shell launch + HTTP coexistence ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, port: $HOST_PORT -> $GUEST_PORT"
echo "  Waiting for server + FV Shell launch..."

if ! test_wait_for "READY" 60; then
    echo "FAIL: server did not reach READY within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi

echo "  Server is ready; foreground FV Shell should now be launching"
sleep 3

PASS=0
FAIL=0
SKIP=0
BASE_URL="http://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s -H "Connection: close" --max-time 10)
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

test_clean_log

# If the firmware carries no FV-embedded Shell, SKIP-balance the three
# assertions instead of failing on such a runner.
if grep -q "NO_FV_SHELL" "$TEST_CLEAN_LOG"; then
    skip "no FV Shell on this firmware — LOCATE=FIRMWARE"
    skip "no FV Shell on this firmware — coexistence GET (1/2)"
    skip "no FV Shell on this firmware — coexistence GET (2/2)"
else
    # axl_shell_locate() must report AXL_SHELL_FIRMWARE (==2): no Shell.efi is
    # staged, but OVMF embeds the Shell in a readable FV.
    if grep -q "^LOCATE=2" "$TEST_CLEAN_LOG"; then
        pass "axl_shell_locate() == AXL_SHELL_FIRMWARE (no file staged)"
    else
        fail "axl_shell_locate() != FIRMWARE ($(grep '^LOCATE=' "$TEST_CLEAN_LOG" | head -1))"
    fi

    # The crux: HTTP must answer WHILE the foreground FV Shell blocks in
    # StartImage. A 200 proves LoadImage+StartImage transferred control to
    # the FV-embedded image and the driver tick keeps pumping the loop.
    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTP GET /plain returns 200 while FV Shell is foreground" \
        || fail "HTTP GET /plain (got '$CODE' — FV Shell didn't start, or loop starved?)"

    RESP=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE_URL}/plain" 2>/dev/null || true)
    CODE=$(echo "$RESP" | tail -1)
    [[ "$CODE" == "200" ]] \
        && pass "HTTP GET /plain still 200 (timer keeps pumping under FV Shell)" \
        || fail "HTTP second GET (got '$CODE')"
fi

# The Shell must NOT have exited — FV_SHELL_EXITED would mean StartImage didn't
# block the foreground (which would defeat the whole model).
if grep -q "FV_SHELL_EXITED" "$TEST_CLEAN_LOG"; then
    fail "FV Shell exited unexpectedly — foreground StartImage did not block"
else
    pass "FV Shell held the foreground (no premature exit)"
fi

echo ""
printf "FV-shell coexistence: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIP" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
