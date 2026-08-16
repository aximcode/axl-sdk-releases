#!/bin/bash
# test-meta: arch=both needs= est=11 local-only=0
# test-gfx-present-qemu.sh — GOP present-pipeline round-trip test (G17+G18).
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite (test-axl.sh) boots QEMU `-nographic`, so there is no
# GOP and gfx tests can only assert in-RAM pixel buffers.  This test
# wires a virtual GPU via scripts/run-qemu.sh --gpu so OVMF exposes a
# real linear-framebuffer GOP, then runs gfx-present-selftest.efi, which
# presents a known pattern to the screen and reads it back in-guest via
# axl_gfx_capture, asserting the pixels survived the framebuffer write
# and pixel-format conversion.
#
# Both arches are exercised because they hit DIFFERENT present paths:
#   * X64 (-device VGA)            → linear FrameBufferBase != 0, so the
#                                     direct-framebuffer write path (G17).
#   * AARCH64 (-device virtio-gpu) → FrameBufferBase == 0, so the GOP
#                                     Blt fallback path.
# Together they cover both branches of buffer_present_region().
#
# Usage: ./test/integration/test-gfx-present-qemu.sh [X64|AARCH64|both]
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
        X64)     native_arch="x64";  out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)"  ;;
        AARCH64) native_arch="aa64"; out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)" ;;
    esac
    efi="$out/gfx-present-selftest.efi"

    echo "=== GOP present round-trip ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" gfx-present-selftest 2>&1 | tail -2

    log="$(mktemp)"
    # --gpu wires a virtual GPU so OVMF publishes a linear-FB GOP.
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --gpu --timeout 45 "$efi" 2>&1 | tee "$log" \
        | grep -iE "Display:|PASS:|FAIL:|PRESENT-SELFTEST|too small" || true

    local verdict
    verdict="$(grep -E "^PRESENT-SELFTEST: [0-9]+ passed, [0-9]+ failed" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ -z "$verdict" ]]; then
        echo "FAIL ($arch): no verdict line — guest did not run to completion"
        overall_fail=$((overall_fail + 1))
        return
    fi
    if [[ "$verdict" =~ ,\ 0\ failed$ ]] && [[ ! "$verdict" =~ ^PRESENT-SELFTEST:\ 0\ passed ]]; then
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
    echo "$overall_fail arch(es) failed the present round-trip"
    exit 1
fi
echo "All present round-trip checks passed."
exit 0
