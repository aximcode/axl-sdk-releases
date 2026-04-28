#!/bin/bash
# test-run-qemu-flags.sh -- argument-parsing tests for run-qemu.sh.
#
# These run on the host (no QEMU), so they're cheap and live outside
# the QEMU integration matrix. They cover the bits of run-qemu.sh
# that don't need a guest:
#   - bash -n syntax pass
#   - --help exits 0 and advertises the flags we care about
#   - --interactive rejects --background and --screenshot
#   - missing EFI file is reported

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected_rc="$2" expected_pat="$3"
    shift 3
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    if [[ "$rc" != "$expected_rc" ]]; then
        echo "FAIL: $name -- expected rc=$expected_rc, got $rc"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ -n "$expected_pat" ]] && ! grep -qE "$expected_pat" <<< "$out"; then
        echo "FAIL: $name -- output did not match /$expected_pat/"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS: $name"
    PASS=$((PASS + 1))
}

# --- syntax ---------------------------------------------------------------
if bash -n "$RUN_QEMU"; then
    echo "PASS: bash -n"
    PASS=$((PASS + 1))
else
    echo "FAIL: bash -n"
    FAIL=$((FAIL + 1))
fi

# --- --help ---------------------------------------------------------------
check "--help exits 0 and lists --interactive" 0 \
    "(-i|--interactive)" \
    "$RUN_QEMU" --help

# --- mutually-exclusive guards --------------------------------------------
DUMMY="$(mktemp --suffix=.efi)"
trap 'rm -f "$DUMMY"' EXIT

check "--interactive + --background rejected" 1 \
    "cannot be combined with --background" \
    "$RUN_QEMU" -i --background "$DUMMY"

check "--interactive + --screenshot rejected" 1 \
    "cannot be combined with --screenshot" \
    "$RUN_QEMU" --interactive --screenshot /tmp/x.png "$DUMMY"

# --- missing file guard (sanity, also exercises arg parsing) --------------
check "missing EFI file rejected" 1 \
    "file not found" \
    "$RUN_QEMU" -i /nonexistent/missing.efi

# --- bare-shell mode requires --interactive -------------------------------
check "no EFI + no -i rejected" 1 \
    "or: .* --interactive" \
    "$RUN_QEMU"

# --- --mount validation ---------------------------------------------------
check "--mount rejects missing dir" 1 \
    "is not a directory" \
    "$RUN_QEMU" -i --mount /nonexistent/dir/abc

check "--mount help text mentions virtiofs" 0 \
    "virtiofs" \
    "$RUN_QEMU" --help

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
