#!/bin/bash
# test-meta: arch=both needs= est=11 local-only=0
# test-cursor-qemu.sh — AxlCursor on-screen compositing test.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite (test-axl.sh) boots QEMU `-nographic`, so there is no
# GOP and axl-test-cursor.c can only assert the Option-C invariant in
# RAM (the cursor never writes the bound scene).  It cannot see what
# actually lands on the screen.  This test wires a virtual GPU via
# scripts/run-qemu.sh --gpu so OVMF exposes a real linear-framebuffer
# GOP, then runs cursor-selftest.efi, which presents a known scene,
# shows + moves the cursor, and reads the framebuffer back in-guest via
# axl_gfx_capture to assert: the sprite is visible at the new position,
# the old position is back to the clean scene (no trail), and the bound
# scene buffer was never modified.
#
# Both arches are exercised because they hit DIFFERENT present paths
# under the hood (X64 direct-FB write, AARCH64 GOP Blt fallback) — the
# cursor's erase/compose/present must read back identically on both.
#
# Usage: ./test/integration/test-cursor-qemu.sh [X64|AARCH64|both]
#        (default: both)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Auxiliary; don't touch test-axl.sh's pass-count baseline.
export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

overall_fail=0

run_one() {
    local arch="$1"
    local native_arch out efi log

    case "$arch" in
        X64)     native_arch="x64";  out="$PROJECT_DIR/out/native-x64"  ;;
        AARCH64) native_arch="aa64"; out="$PROJECT_DIR/out/native-aa64" ;;
    esac
    efi="$out/cursor-selftest.efi"

    echo "=== AxlCursor on-screen compositing ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" cursor-selftest 2>&1 | tail -2

    log="$(mktemp)"
    # --gpu wires a virtual GPU so OVMF publishes a linear-FB GOP.
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --gpu --timeout 45 "$efi" 2>&1 | tee "$log" \
        | grep -iE "Display:|PASS:|FAIL:|CURSOR-SELFTEST|too small" || true

    local verdict
    verdict="$(grep -E "^CURSOR-SELFTEST: [0-9]+ passed, [0-9]+ failed" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ -z "$verdict" ]]; then
        echo "FAIL ($arch): no verdict line — guest did not run to completion"
        overall_fail=$((overall_fail + 1))
        return
    fi
    if [[ "$verdict" =~ ,\ 0\ failed$ ]] && [[ ! "$verdict" =~ ^CURSOR-SELFTEST:\ 0\ passed ]]; then
        echo "OK ($arch): $verdict"
    else
        echo "FAIL ($arch): $verdict"
        overall_fail=$((overall_fail + 1))
    fi
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( overall_fail > 0 )); then
    echo "$overall_fail arch(es) failed the cursor compositing test"
    exit 1
fi
echo "All cursor compositing checks passed."
exit 0
