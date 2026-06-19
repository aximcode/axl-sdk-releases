#!/bin/bash
# test-meta: arch=both needs= est=11 local-only=0
# test-console-text-mode-qemu.sh — text-console mode enumerate / switch
# round-trip.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite (test-axl.sh) boots QEMU `-nographic`, whose serial
# console usually exposes a single 80x25 text mode — enough to assert the
# enumerate contract, not a switch. This test wires a virtual GPU via
# scripts/run-qemu.sh --gpu so OVMF's graphics console publishes several
# text modes, then runs console-text-mode-selftest.efi, which enumerates
# the modes, switches to a different geometry, and restores the original —
# asserting axl_console_text_current_mode tracks the switch.
#
# Usage: ./test/integration/test-console-text-mode-qemu.sh [X64|AARCH64|both]
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
    efi="$out/console-text-mode-selftest.efi"

    echo "=== text-console mode enumerate/switch ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" console-text-mode-selftest 2>&1 | tail -2

    log="$(mktemp)"
    # --gpu wires a virtual GPU so OVMF's graphics console publishes several
    # text modes (the serial console alone usually has one).
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --gpu --timeout 45 "$efi" 2>&1 | tee "$log" \
        | grep -iE "Console:|Switching:|Max mode:|PASS:|FAIL:|TEXT-MODE-SELFTEST|mode [0-9]" || true

    local verdict
    verdict="$(grep -E "^TEXT-MODE-SELFTEST: [0-9]+ passed, [0-9]+ failed" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ -z "$verdict" ]]; then
        echo "FAIL ($arch): no verdict line — guest did not run to completion"
        overall_fail=$((overall_fail + 1))
        return
    fi
    if [[ "$verdict" =~ ,\ 0\ failed$ ]] && [[ ! "$verdict" =~ ^TEXT-MODE-SELFTEST:\ 0\ passed ]]; then
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
echo "All text-mode round-trip checks passed."
exit 0
