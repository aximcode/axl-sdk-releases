#!/bin/bash
# test-meta: arch=both needs=gpu est=12 local-only=0
# test-console-reshape-qemu.sh — a passthrough take-over device must not hide the
# physical console's text modes, and must reshape through them.
#
# A co-painting (passthrough_local) device is a READER: it does not evict the
# firmware console, so it can honour every mode that console can. It used to
# advertise exactly ONE mode, and ConSplitter intersects its members' modes by
# (Columns, Rows) — so a SetMode routed through gST->ConOut half-landed:
#
#   RED   the PHYSICAL console switched to 80x25 while our device kept
#         advertising 100x31 and returned EFI_UNSUPPORTED — two co-painting
#         consoles silently on different grids, consumer never told
#         (6 of 17 checks failed)
#   GREEN mirroring the physical list makes SetMode reshape BOTH consoles, and
#         the device re-advertises, clears, and fires the resize op
#         (20 passed, 0 failed)
#
# The fixture runs as an APP, which works precisely because passthrough does not
# evict: the firmware console stays in the fan-out, so the assertions still reach
# the serial log while the device is installed.
#
# --gpu is REQUIRED: OVMF's GraphicsConsole enumerates its text modes from the
# GOP, and without one there is a single mode and nothing to switch to. The
# fixture reports "SKIP" in that case rather than passing hollowly, and this
# runner treats a SKIP as a failure — a test that cannot discriminate is not a
# passing test.
#
# Usage: ./test/integration/test-console-reshape-qemu.sh [X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

EXPECTED_CHECKS=20

overall_fail=0

run_one() {
    local arch="$1" native_arch out efi log verdict

    case "$arch" in
        X64)     native_arch="x64";  out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)" ;;
        AARCH64) native_arch="aa64"; out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)" ;;
    esac
    efi="$out/console-reshape-selftest.efi"

    echo "=== console reshape ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" \
        ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} console-reshape-selftest 2>&1 | tail -1

    log="$(mktemp)"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --gpu --timeout 60 "$efi" > "$log" 2>&1 || true

    grep -E "^(PASS|FAIL|RESHAPE):|^ +want=" "$log" || true

    # A firmware that enumerates one mode cannot exercise the reshape at all.
    # Fail loudly rather than let a hollow "0 failed" read as coverage.
    if grep -q "RESHAPE: SKIP" "$log"; then
        echo "FAIL ($arch): only one text mode enumerated — nothing was tested"
        echo "  (is --gpu wired? OVMF derives its text modes from the GOP)"
        rm -f "$log"
        overall_fail=$((overall_fail + 1))
        return
    fi

    verdict="$(grep -E '^RESHAPE: [0-9]+ passed, [0-9]+ failed' "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ "$verdict" == "RESHAPE: $EXPECTED_CHECKS passed, 0 failed" ]]; then
        echo "OK ($arch): $verdict"
    else
        echo "FAIL ($arch): ${verdict:-no verdict line}"
        overall_fail=$((overall_fail + 1))
    fi
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( overall_fail > 0 )); then
    echo "$overall_fail console-reshape check(s) failed"
    exit 1
fi
echo "All console-reshape checks passed."
exit 0
