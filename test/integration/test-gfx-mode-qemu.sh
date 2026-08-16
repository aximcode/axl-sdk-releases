#!/bin/bash
# test-meta: arch=both needs= est=11 local-only=0
# test-gfx-mode-qemu.sh — GOP display-mode enumerate / switch round-trip.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite (test-axl.sh) boots QEMU `-nographic`, so there is no
# GOP and the mode API can only assert its NULL-arg + no-GOP contract.
# This test wires a virtual GPU via scripts/run-qemu.sh --gpu so OVMF
# exposes a real GOP with several modes, then runs gfx-mode-selftest.efi,
# which enumerates the modes, switches to a different resolution, and
# restores the original — asserting axl_gfx_get_info / current_mode track
# the switch.
#
# Usage: ./test/integration/test-gfx-mode-qemu.sh [X64|AARCH64|both]
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
    efi="$out/gfx-mode-selftest.efi"

    echo "=== GOP mode enumerate/switch ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" gfx-mode-selftest 2>&1 | tail -2

    log="$(mktemp)"
    # --gpu wires a virtual GPU so OVMF publishes a GOP with several modes.
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --gpu --timeout 45 "$efi" 2>&1 | tee "$log" \
        | grep -iE "Display:|Switching:|PASS:|FAIL:|MODE-SELFTEST|mode [0-9]" || true

    local verdict
    verdict="$(grep -E "^MODE-SELFTEST: [0-9]+ passed, [0-9]+ failed" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ -z "$verdict" ]]; then
        echo "FAIL ($arch): no verdict line — guest did not run to completion"
        overall_fail=$((overall_fail + 1))
        return
    fi
    if [[ "$verdict" =~ ,\ 0\ failed$ ]] && [[ ! "$verdict" =~ ^MODE-SELFTEST:\ 0\ passed ]]; then
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
    echo "$overall_fail arch(es) failed the mode round-trip"
    exit 1
fi
echo "All mode round-trip checks passed."
exit 0
