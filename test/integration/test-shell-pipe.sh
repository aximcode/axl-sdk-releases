#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0 guest=standard
# test-shell-pipe.sh — verify axl_stdin works as the RHS of a UEFI
# Shell `|` pipe. Exercises hexdump and grep with stdin input.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# x86 only — OVMF ships the EDK2 ShellPkg with `|` support; OVMF for
# AArch64 does too but we haven't validated there yet.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tools 2>&1 | tail -1

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/tools/hexdump.efi"
test_add_efi "$NATIVE_DIR/tools/grep.efi"

test_use_nsh

test_build_image
test_build_qemu_cmd

echo "=== axl_stdin / shell-pipe integration test (X64) ==="
test_run_foreground 30 || true

test_clean_log
LOG="$TEST_CLEAN_LOG"

echo
echo "--- relevant serial log (PROBE sections) ---"
sed -n '/PROBE-1-START/,/=== DONE ===/p' "$LOG" || cat "$LOG" | tail -60
echo "--------------------------------------------"

fail=0
expect_contains() {
    local label="$1" pattern="$2"
    if grep -qE "$pattern" "$LOG"; then
        echo "PASS: $label"
    else
        echo "FAIL: $label  (looked for: $pattern)"
        fail=$((fail + 1))
    fi
}
expect_not_contains() {
    local label="$1" pattern="$2" boundary_start="$3" boundary_end="$4"
    if sed -n "/$boundary_start/,/$boundary_end/p" "$LOG" | grep -qE "$pattern"; then
        echo "FAIL: $label  (unexpected: $pattern)"
        fail=$((fail + 1))
    else
        echo "PASS: $label"
    fi
}

# PROBE 1: hexdump should show the raw wire bytes the shell wrote,
# which are UCS-2 LE with a BOM (the shell wraps text output that
# way). 'hello' encoded as UCS-2 LE = 68 00 65 00 6c 00 6c 00 6f 00,
# preceded by FF FE BOM. axl_stdin is intentionally byte-raw —
# higher-level tools (grep) decode UCS-2; hexdump shows wire truth.
expect_contains "hexdump-stdin: BOM + UCS-2 'hello' bytes visible" "fffe 6800 6500 6c00 6c00 6f00"

# PROBE 2: grep should echo the matching line.
expect_contains "grep-stdin: matching line emitted" "line-with-needle"

# PROBE 3: grep should produce NO line content between PROBE-3
# markers (just the markers themselves).
expect_not_contains "grep-stdin: no-match emits no line" "nomatch" \
                    "PROBE-3-START" "PROBE-3-END"

# PROBE 4: grep should filter ver output to lines containing 'UEFI'.
expect_contains "grep-stdin: filters multiline shell output" "UEFI"

if (( fail > 0 )); then
    echo "$fail expectation(s) failed"
    exit 1
fi
echo "All shell-pipe expectations met."
