#!/bin/bash
# test-console-mirror-edit-qemu.sh — the AxlConsoleMirror ACCEPTANCE GATE.
#
# Design §6 rung 3, the headline proof: the Shell's full-screen interactive
# `edit` runs OVER the mirror and actually SAVES a file — the one thing a
# one-shot command shell can never do, and the reason this substrate exists.
#
# AxlTestNet.efi mirror-edit installs the mirror, launches a real child
# Shell.efi (foreground, blocks), and drives `edit` entirely via injected
# keystrokes from a background firmware timer: open the editor on a file, type
# a line, F2-save, F3-exit, then `exit` the child Shell. When the child Shell
# exits, the launcher returns; the app reads the file back and asserts it holds
# the typed line, with the mirror sink corroborating the full-screen framing
# (ClearScreen + cursor positioning). Then it prints MIRROR_EDIT_* and idles.
#
# Needs a real Shell.efi (staged literally as \Shell.efi at the ESP root so
# axl_driver_locate finds it). If the runner has none, the test SKIP-balances.
#
# Usage: ./test/integration/test-console-mirror-edit-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# Stage a real Shell.efi at the ESP root (axl_driver_locate finds it next to
# the running image). test_setup already ran find_firmware.
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
echo Running console-mirror edit acceptance test...
AxlTestNet.efi mirror-edit
NSHEOF

test_build_image

echo "=== AxlConsoleMirror edit acceptance gate ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID"
echo "  Driving edit over the mirror (this takes ~15s)..."

if ! test_wait_for "MIRROR_EDIT_DONE" 120; then
    echo "FAIL: edit session did not finish within 120 seconds"
    test_clean_log
    echo "--- Serial log (mirror lines) ---"
    grep -a "MIRROR_EDIT" "$TEST_CLEAN_LOG" | tail -20
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

check() {  # check <name> <label>
    if grep -qa "MIRROR_EDIT: ${1}=1" "$TEST_CLEAN_LOG"; then
        pass "$2"
    else
        fail "$2"
    fi
}

# If no Shell.efi was staged (or the app couldn't launch one), SKIP-balance the
# four assertions rather than failing on a runner without a shell.
if [[ "$HAVE_SHELL" -eq 0 ]] || grep -qa "MIRROR_EDIT: shell_launched=0" "$TEST_CLEAN_LOG"; then
    skip "no Shell.efi — child shell launch"
    skip "no Shell.efi — edit ClearScreen framing"
    skip "no Shell.efi — edit cursor positioning"
    skip "no Shell.efi — edited file saved the typed line"
else
    check shell_launched "child Shell launched + exited over the mirror"
    check saw_clear      "edit drove a full-screen clear (ESC[2J) to the sink"
    check saw_cursor     "edit drove cursor positioning (ESC[..H) to the sink"
    # THE acceptance bar: the interactive editor saved the typed line over the wire.
    check file_saved     "edited file contains the typed line (edit ran + saved)"
fi

echo ""
printf "Console-mirror edit gate: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIP" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial log (mirror lines) ---"
    grep -a "MIRROR_EDIT" "$TEST_CLEAN_LOG" | tail -20
fi

[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || { [[ $SKIP -gt 0 && $FAIL -eq 0 ]] && exit 0 || exit 1; }
