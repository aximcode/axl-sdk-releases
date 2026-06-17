#!/bin/bash
# test-console-mirror-qemu.sh — AxlConsoleMirror positive proof (P1/P2 core).
#
# Exercises the full mirror mechanism in its OWN QEMU boot: install (wrap gST
# ConIn/ConOut + ReinstallProtocolInterface on ConsoleInHandle), drive the
# wrapped ConOut and assert the VT/ANSI translation, inject xterm input and
# pop it back through the wrapped ConIn, then uninstall (restoring the
# console) and print results. This cannot run in the combined unit boot —
# wrapping the parent harness Shell's console wedges that Shell for the next
# test binary — so it runs standalone here and idles after printing (the app
# never returns to the wedged parent Shell; we assert on the serial log).
#
# AxlTestNet.efi mirror-selftest does the work and prints MIRROR_SELFTEST_*
# lines + a final MIRROR_SELFTEST_DONE PASS/FAIL.
#
# Usage: ./test/integration/test-console-mirror-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \
echo Running console-mirror self-test...
AxlTestNet.efi mirror-selftest
NSHEOF

test_build_image

echo "=== AxlConsoleMirror self-test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID"
echo "  Waiting for self-test to finish..."

if ! test_wait_for "MIRROR_SELFTEST_DONE" 60; then
    echo "FAIL: self-test did not finish within 60 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

test_clean_log

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Each MIRROR_SELFTEST line reports "<name>=1" on success. Assert every one.
check() {
    local name="$1" label="$2"
    if grep -q "MIRROR_SELFTEST: ${name}=1" "$TEST_CLEAN_LOG"; then
        pass "$label"
    else
        fail "$label"
    fi
}

check install            "install succeeds"
check dbl_install_blocked "second install rejected (single global console)"
check query_size         "QueryMode reports remote size (80x25)"
check clear              "ClearScreen -> ESC[2J ESC[H"
check cursor             "SetCursorPosition(4,2) -> ESC[3;5H"
check sgr                "SetAttribute(lightred) -> ESC[0;91;40m"
check text               "OutputString mirrored as UTF-8"
check inject_up          "inject_text ESC[A -> Up scan 0x01"
check inject_printable   "inject_text printable -> unicode key"
check inject_key_f2      "inject_key(F2) round-trips via ConIn"

if grep -q "MIRROR_SELFTEST_DONE PASS" "$TEST_CLEAN_LOG"; then
    pass "self-test overall PASS"
else
    fail "self-test overall (MIRROR_SELFTEST_DONE not PASS)"
fi

echo ""
printf "Console-mirror self-test: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log (mirror lines) ---"
    grep "MIRROR_SELFTEST" "$TEST_CLEAN_LOG" | tail -20
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
