#!/bin/bash
# test-meta: arch=both needs= est=24 local-only=0
# test-task-pool-mp-qemu.sh — multi-core AxlTaskPool race regression.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet). The
# unit suite boots QEMU single-core, so it only exercises AxlTaskPool's
# synchronous fallback. This test boots `-smp 4` so OVMF brings up real
# AP workers, then runs task-pool-mp-selftest.efi, which drives the pool
# the way the contract permits — each wave submits exactly
# axl_task_pool_available() tasks and trusts every submit to succeed —
# for thousands of waves.
#
# Regression target: a torn read in axl_task_pool_available()/_submit()
# (the (task,done,running) flags were three separate volatile loads) made
# a just-completed slot look idle, so available() over-reported and
# submit() could clobber an unreaped completion -> dropped task -> hang
# (the Dell R6725 axbench hang). The fixed pool reports PASS; the buggy
# pool reports FAIL (invalid submits and/or a stall).
#
# aarch64 QEMU has no MP Services here, so that arch prints SKIP (which
# passes) — the meaningful coverage is x64 under KVM.
#
# Usage: ./test/integration/test-task-pool-mp-qemu.sh [X64|AARCH64|both]
#        (default: both)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

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
    efi="$out/task-pool-mp-selftest.efi"

    echo "=== AxlTaskPool multi-core race regression ($arch) ==="
    make -C "$PROJECT_DIR" ARCH="$native_arch" task-pool-mp-selftest 2>&1 | tail -2

    log="$(mktemp)"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$arch" --timeout 150 \
        --qemu-arg -smp --qemu-arg 4 "$efi" 2>&1 | tee "$log" \
        | grep -iE "MP-POOL:|EXCEPTION|ASSERT" || true

    local verdict
    verdict="$(grep -E "^MP-POOL: (PASS|FAIL|SKIP)" "$log" | tail -1 || true)"
    rm -f "$log"

    if [[ -z "$verdict" ]]; then
        echo "FAIL ($arch): no MP-POOL verdict — guest did not run to completion"
        overall_fail=$((overall_fail + 1))
        return
    fi
    case "$verdict" in
        "MP-POOL: PASS"*) echo "OK ($arch): $verdict" ;;
        "MP-POOL: SKIP"*) echo "SKIP ($arch): $verdict" ;;
        *)                echo "FAIL ($arch): $verdict"
                          overall_fail=$((overall_fail + 1)) ;;
    esac
}

for arch in "${ARCHES[@]}"; do
    run_one "$arch"
    echo
done

if (( overall_fail > 0 )); then
    echo "$overall_fail arch(es) failed the task-pool MP race regression"
    exit 1
fi
echo "All task-pool MP race checks passed."
exit 0
