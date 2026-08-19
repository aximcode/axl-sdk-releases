#!/bin/bash
# test-meta: arch=both needs= est=12 local-only=0
# arch=both since 2026-08-19: verified passing on AARCH64 unchanged (21/0). It was
# x64-only with no stated reason -- never ported, not unportable.
# test-time-qemu.sh — round-trip the RTC-write API
# (axl_time_set_realtime / axl_time_set_unix) against OVMF's emulated
# real-time clock.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# gRT->SetTime mutates real firmware state, so there is no unit-test
# seam for it — the write path is only exercisable end-to-end under
# QEMU/OVMF. The selftest sets a known calendar time and a known Unix
# instant, reads each back via axl_time_realtime, asserts the fields
# round-trip, checks input validation, and restores the original clock.
#
# Usage: ./test/integration/test-time-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/time-settime-selftest.efi"
make -C "$PROJECT_DIR" ARCH=x64 time-settime-selftest 2>&1 | tail -2

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 40 "$EFI" 2>&1 \
    | tee "$log" \
    | grep -iE "RT:|UX:|TIME-SETTIME-SELFTEST|FAIL:|EXCEPTION" || true

fail=0
grep -qE "^TIME-SETTIME-SELFTEST: [0-9]+ passed, 0 failed" "$log" \
    || { echo "  MISS: clean selftest verdict"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$log" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: RTC SetTime round-trip"
    exit 1
fi
echo "All RTC SetTime round-trip checks passed."
exit 0
