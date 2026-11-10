#!/bin/bash
# test-meta: arch=x64 needs=taskset est=40 cpu=2 local-only=0 guest=none
# test-meta: builds=ARCH=x64,task-pool-mp-selftest
# guest=none: the test pins its own guest's vCPU threads on the host, so it
#   must own that QEMU -- a pooled guest's threads are the pool's.
# test-task-pool-mp-starved-qemu.sh -- AxlTaskPool keeps a worker whose AP
# the host has descheduled.
#
# A virtual AP makes no progress while its host thread waits for a CPU, for
# as long as the host decides. axl_task_pool_new used to bound its wait for a
# worker's liveness answer in BSP spins -- ~25 ms on an unhindered BSP, and
# the BSP is exactly the processor still running when an AP is not -- so a
# loaded host made it retire a live worker. That surfaced as
# "slot reclaim: workers=2 expected=3" from test-task-pool-mp-qemu.sh, at
# whatever rate the box's load happened to produce (CI run 42578726165).
#
# This test produces the load on purpose instead of waiting for it: the three
# AP vCPU threads share ONE host CPU with HOGS busy loops while the BSP's vCPU
# is pinned to another (shared only with the box's ordinary load), so each AP
# answer waits behind ~HOGS scheduler slices while the BSP's own clock runs. task-pool-mp-selftest.efi --pools
# POOLS then creates and frees POOLS pools and requires each to count every
# worker. Measured (2026-11-09, HOGS=24): the spin bound FAILED 4 of 4 runs,
# one of them by counting a single worker in EVERY pool -- which is why the
# worker count is checked against the AP threads, not only for consistency.
# Fixed: PASS 3 of 3, 14-22 s, with no [WARN] line.
#
# Why not starve harder: a nice-19 AP beside two hogs gets ~1.5% of a CPU and
# failed the spin bound just as surely, but the fixed pool then took up to
# 142 s, because the stall grows with whatever else runs on that CPU. Equal
# weight keeps the stall at the hogs' share of a shared box.
#
# Controls, so a PASS cannot come from a harness that did nothing: the pins
# are read back from the kernel, the fixture must report every AP as a
# worker, and the AP threads' run delay (/proc/<tid>/schedstat, time
# runnable but waiting for a CPU) must reach MIN_AP_DELAY_MS -- an unstarved
# run has next to none.
#
# x64 only, and only under KVM: the threads it pins are KVM vCPUs (TCG names
# them differently and is a different scheduling problem), and aarch64 QEMU's
# firmware publishes no MP Services, where test-task-pool-mp-qemu.sh SKIPs
# too.
#
# The hogs are this test's to clean up even when it is killed: a TERM or INT
# exits through the EXIT trap, and each hog is under its own timeout, so a
# SIGKILL cannot leave 24 busy loops pinned to a CPU of a shared box.
#
# Usage: ./test/integration/test-task-pool-mp-starved-qemu.sh [--arch X64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

HOGS=24
POOLS=12
MIN_AP_DELAY_MS=1000

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-X64}"; else WHICH="${1:-X64}"; fi
case "$WHICH" in
    X64|both) ;;
    AARCH64) echo "SKIP (AARCH64): x64 only -- see the header"; exit 0 ;;
    *) echo "usage: $0 [--arch X64]" >&2; exit 2 ;;
esac

# The last two CPUs this process may run on: one for the BSP, one for the
# APs and the hogs.
mapfile -t cpus < <(taskset -pc $$ | sed 's/.*: //' | tr ',' '\n' |
    awk -F- '{ hi = ($2 == "") ? $1 : $2; for (c = $1; c <= hi; c++) print c }')
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    echo "SKIP: needs KVM (/dev/kvm); the vCPU threads it pins are KVM's"
    exit 0
fi
if (( ${#cpus[@]} < 2 )); then
    echo "SKIP: needs two usable CPUs, has ${#cpus[@]}"
    exit 0
fi
BSP_CPU="${cpus[-2]}"
AP_CPU="${cpus[-1]}"

out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)"
efi="$out/task-pool-mp-selftest.efi"
echo "=== AxlTaskPool with starved APs (X64): APs + $HOGS hogs on CPU $AP_CPU, BSP on CPU $BSP_CPU ==="
"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 task-pool-mp-selftest 2>&1 | tail -2

log="$(mktemp)"
hogs=()
rq=""
cleanup() {
    if (( ${#hogs[@]} > 0 )); then kill "${hogs[@]}" 2>/dev/null || true; fi
    if [[ -n "$rq" ]]; then kill "$rq" 2>/dev/null || true; fi
    wait 2>/dev/null || true
    rm -f "$log"
    return 0
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

# debug-threads=on names the vCPU threads "CPU <n>/KVM"; without it every
# thread is "qemu-system-x86" and the APs cannot be told from the BSP.
timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --arch X64 --timeout 150 \
    --qemu-arg -smp --qemu-arg 4 \
    --qemu-arg -name --qemu-arg guest=starved,debug-threads=on \
    "$efi" --pools "$POOLS" > "$log" 2>&1 &
rq=$!

descendants() {
    local p
    for p in $(cat /proc/"$1"/task/*/children 2>/dev/null); do
        echo "$p"; descendants "$p"
    done
}

# vCPU thread ids by name: vcpu_tids <qemu-pid> <ap|bsp>
vcpu_tids() {
    local c name
    for c in /proc/"$1"/task/*/comm; do
        name="$(cat "$c" 2>/dev/null)" || continue
        case "$2:$name" in
            "bsp:CPU 0/KVM") basename "$(dirname "$c")" ;;
            "ap:CPU 0/KVM") ;;
            "ap:CPU "*"/KVM") basename "$(dirname "$c")" ;;
        esac
    done
}

qemu=""
for _ in $(seq 300); do
    for p in $(descendants "$rq"); do
        [[ "$(cat /proc/"$p"/comm 2>/dev/null)" == qemu-system-x86* ]] && qemu="$p"
    done
    if [[ -n "$qemu" ]] && (( $(vcpu_tids "$qemu" ap | wc -l) == 3 )); then
        break
    fi
    sleep 0.1
done
mapfile -t ap_tids < <([[ -n "$qemu" ]] && vcpu_tids "$qemu" ap)
mapfile -t bsp_tid < <([[ -n "$qemu" ]] && vcpu_tids "$qemu" bsp)
if (( ${#ap_tids[@]} != 3 || ${#bsp_tid[@]} != 1 )); then
    echo "FAIL: found ${#ap_tids[@]} AP and ${#bsp_tid[@]} BSP vCPU thread(s) (QEMU pid '${qemu}'); cannot starve the APs"
    exit 1
fi

for (( h = 0; h < HOGS; h++ )); do
    timeout 200 taskset -c "$AP_CPU" bash -c 'while :; do :; done' &
    hogs+=($!)
done
taskset -pc "$BSP_CPU" "${bsp_tid[0]}" > /dev/null
for t in "${ap_tids[@]}"; do taskset -pc "$AP_CPU" "$t" > /dev/null; done

# Control: the kernel holds the pins we asked for.
pins_ok=1
for t in "${ap_tids[@]}"; do
    [[ "$(taskset -pc "$t" | sed 's/.*: //')" == "$AP_CPU" ]] || pins_ok=0
done
[[ "$(taskset -pc "${bsp_tid[0]}" | sed 's/.*: //')" == "$BSP_CPU" ]] || pins_ok=0

ap_delay_ns() {
    local t d sum=0
    for t in "${ap_tids[@]}"; do
        { read -r _ d _ < /proc/"$qemu"/task/"$t"/schedstat; } 2>/dev/null || continue
        sum=$(( sum + d ))
    done
    echo "$sum"
}
delay0="$(ap_delay_ns)"
delay1="$delay0"
while kill -0 "$rq" 2>/dev/null; do
    d="$(ap_delay_ns)"
    (( d > delay1 )) && delay1="$d"
    sleep 0.2
done
rc=0
wait "$rq" || rc=$?
rq=""
ap_delay_ms=$(( (delay1 - delay0) / 1000000 ))

grep -aiE "MP-POOL:|EXCEPTION|ASSERT|\[(WARN|ERROR)\]" "$log" || true
verdict="$(grep -aE "^MP-POOL: (PASS|FAIL|SKIP)" "$log" | tail -1 || true)"
echo "AP vCPU run delay: ${ap_delay_ms} ms (control needs >= ${MIN_AP_DELAY_MS}); run-qemu rc=$rc"

fail=0
if (( ! pins_ok )); then
    echo "FAIL: the vCPU pins did not hold; the APs were not starved"
    fail=1
fi
if (( ap_delay_ms < MIN_AP_DELAY_MS )); then
    echo "FAIL: the APs waited ${ap_delay_ms} ms for a CPU; the harness did not starve them"
    fail=1
fi
# The pool's own account of a short or slow worker -- a retired slot, a
# worker that did not acknowledge quit, an event never signalled -- is a
# failure here too: under this load every worker answers within the budget.
if grep -qaE "\[(WARN|ERROR)\]" "$log"; then
    echo "FAIL: the pool warned (above) while its workers were merely slow"
    fail=1
fi
# A pool that retired workers at creation and then kept retiring the same
# number reports a consistent count; only the AP count says it is short.
case "$verdict" in
    "MP-POOL: PASS (W=${#ap_tids[@]},"*) echo "OK (X64): $verdict" ;;
    "MP-POOL: PASS"*) echo "FAIL (X64): $verdict -- expected W=${#ap_tids[@]}, one worker per AP"; fail=1 ;;
    "")               echo "FAIL (X64): no MP-POOL verdict -- guest did not run to completion"; fail=1 ;;
    *)                echo "FAIL (X64): $verdict"; fail=1 ;;
esac

if (( fail )); then
    exit 1
fi
echo "AxlTaskPool kept every starved worker."
exit 0
