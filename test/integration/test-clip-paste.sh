#!/bin/bash
# test-meta: arch=x64 needs= est=9 local-only=0
# test-clip-paste.sh — cross-app clipboard proof. `clip` and `paste` are
# separate image invocations within one boot; the clipboard is AXL shared
# memory (axl-shm) in EfiBootServicesData pool + an installed protocol, so
# it must survive `clip` exiting and be readable by a later `paste`. `clip`
# prints nothing, so any clipboard text that shows up in `paste`'s output
# came across the app boundary.
#
# Auxiliary single-scenario test (opt-out of the test-axl.sh ratchet).
# x86 only — like test-shell-pipe.sh, relies on OVMF EDK2 ShellPkg `|`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/tools/clip.efi"
test_add_efi "$NATIVE_DIR/tools/paste.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    # 1. Copy text in ONE app, paste it in ANOTHER. clip prints nothing,
    #    so 'axl-cross-app-clip' in the log can only have come from paste
    #    reading what the separate clip invocation stored.
    echo "echo PROBE-1-START"
    echo "echo axl-cross-app-clip | clip.efi"
    echo "paste.efi"
    echo "echo PROBE-1-END"
    echo ""
    # 2. A MIME tag set by clip is reported by a later paste --mime.
    echo "echo PROBE-2-START"
    echo "echo tagged | clip.efi -m application/x-axl-test"
    echo "paste.efi --mime"
    echo "echo PROBE-2-END"
    echo ""
    # 3. clip --clear empties it across apps: the following paste prints
    #    nothing between the markers.
    echo "echo PROBE-3-START"
    echo "clip.efi --clear"
    echo "paste.efi"
    echo "echo PROBE-3-END"
    echo ""
    echo "echo === DONE ==="
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd

echo "=== clip/paste cross-app clipboard integration test (X64) ==="
test_run_foreground 30 || true

test_clean_log
LOG="$TEST_CLEAN_LOG"

echo
echo "--- relevant serial log (PROBE sections) ---"
sed -n '/PROBE-1-START/,/=== DONE ===/p' "$LOG" || tail -60 "$LOG"
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
expect_not_between() {
    local label="$1" pattern="$2" b0="$3" b1="$4"
    if sed -n "/$b0/,/$b1/p" "$LOG" | grep -qE "$pattern"; then
        echo "FAIL: $label  (unexpected: $pattern)"
        fail=$((fail + 1))
    else
        echo "PASS: $label"
    fi
}

# 1. The pasted text survived the clip app exiting (cross-app).
expect_contains "clipboard survives app exit: paste prints clip's text" \
                "axl-cross-app-clip"
# 2. The MIME tag crossed the app boundary too.
expect_contains "MIME tag readable cross-app via paste --mime" \
                "application/x-axl-test"
# 3. After clip --clear, paste prints no stale text between the markers.
expect_not_between "clip --clear empties the clipboard cross-app" \
                   "axl-cross-app-clip" "PROBE-3-START" "PROBE-3-END"

if (( fail > 0 )); then
    echo "$fail expectation(s) failed"
    exit 1
fi
echo "All clip/paste cross-app expectations met."
