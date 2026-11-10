#!/bin/bash
# test-meta: arch=x64 needs=taskset est=90 cpu=2 local-only=0
# test-meta: builds=ARCH=x64,net-latency-selftest
# It launches no guest itself: test-net-latency-qemu.sh boots its own (booted,
# guest=none) and pins them on the host -- a pooled guest's threads are the pool's.
# test-net-latency-starved-qemu.sh -- test-net-latency-qemu.sh reads a timing
# miss as UNJUDGED only when the host starved the guest, and only when timing
# is ALL that failed.
#
# test-net-latency-qemu.sh times round trips from the host, and on a busy box
# (CI's runner shares it with local agents) its 5 ms p90 bound missed at load
# 18+ with nothing wrong in the code. Its witness is qloop's: how long the
# guest's QEMU threads waited for a CPU (schedstat run delay, read per thread
# over each round-trip window: thread_run_delays and run_delay_window_ms in
# scripts/axl-common.sh) and judged against AXL_STARVE_MAX_MS. Here that is driven on purpose, one
# guest per case (NETLAT_TEST_ONLY=always/driver: the ready and done tick
# checks are functional, the p90 bound is the one timing check):
#
#   STARVED   NETLAT_TEST_PIN_CPU pins the guest's QEMU, just before its
#             round trips, onto a CPU running HOGS busy loops, and
#             NETLAT_TEST_P90_MS=0.001 makes the miss certain. Must read
#             UNJUDGED (exit 77, the `UNJUDGED:` line run-one.sh reads), and
#             the wait it reports must exceed the limit -- the CONTROL that
#             the witness MOVES.
#   QUIET     the same miss, unpinned, no hogs, with the limit raised out of
#             reach (NETLAT_TEST_STARVE_MAX_MS): a timing miss whose window
#             did not wait past the limit is a FAIL. The limit is raised
#             rather than trusted, because this box is shared and a
#             "quiet" run is not a thing a test can promise (it read 176 ms
#             at load 21). The other half of the control -- the witness
#             invents no wait -- is a process asleep through a window,
#             which must read 0 ms.
#   FUNCTIONAL the STARVED run plus a functional failure
#             (NETLAT_TEST_WANT_READY_US=1, a tick the guest never reads):
#             starvation never relabels a functional failure, so FAIL.
#   NO-FIGURES the STARVED run with no round trip timed
#             (NETLAT_TEST_RTT_COUNT=0, so tcp-rtt.py reports nothing): a
#             measurement that did not happen is functional, not a timing
#             miss, however starved the window -- FAIL. Its window is only a
#             connect, too short to wait 50 ms even pinned, so the limit is
#             0 here: the window counts as starved, and only the
#             classification can keep it a FAIL.
#   PER-WINDOW two measurements (always/driver, always/sync), both pinned and
#             both missing, one of them read with no wait at all
#             (NETLAT_TEST_UNMEASURED=always/sync): each miss is judged by
#             ITS OWN window, and an unmeasured one is not starved -- FAIL,
#             though the other window waited far past the limit.
#
# The hogs are this test's to clean up even when it is killed: a TERM or INT
# exits through the EXIT trap, and each hog is under its own timeout.
#
# x64 only, and only under KVM (the pinned threads are a KVM guest's; TCG is a
# different scheduling problem). NETLAT_TEST_* exist only for this test.
#
# Usage: ./test/integration/test-net-latency-starved-qemu.sh [--arch X64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

HOGS=6

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-X64}"; else WHICH="${1:-X64}"; fi
case "$WHICH" in
    X64|both) ;;
    AARCH64) echo "SKIP (AARCH64): x64 only -- see the header"; exit 77 ;;
    *) echo "usage: $0 [--arch X64]" >&2; exit 2 ;;
esac
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    echo "SKIP: needs KVM (/dev/kvm); the threads it pins are a KVM guest's"
    exit 77
fi
# The last CPU this process may run on: the guest and the hogs share it.
mapfile -t cpus < <(taskset -pc $$ | sed 's/.*: //' | tr ',' '\n' |
    awk -F- '{ hi = ($2 == "") ? $1 : $2; for (c = $1; c <= hi; c++) print c }')
HOG_CPU="${cpus[-1]}"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 net-latency-selftest 2>&1 | tail -1

WORK="$(mktemp -d -t axl-netlat-starved.XXXXXXXX)"
hogs=()
run=""
cleanup() {
    if [[ -n "$run" ]]; then kill "$run" 2>/dev/null || true; fi
    if (( ${#hogs[@]} > 0 )); then kill "${hogs[@]}" 2>/dev/null || true; fi
    wait 2>/dev/null || true
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (got '$2', want '$3')"; FAIL=$((FAIL + 1))
    fi
}

hogs_on() {
    local h
    for (( h = 0; h < HOGS; h++ )); do
        timeout 120 taskset -c "$HOG_CPU" bash -c 'while :; do :; done' &
        hogs+=($!)
    done
}
hogs_off() {
    if (( ${#hogs[@]} > 0 )); then kill "${hogs[@]}" 2>/dev/null || true; fi
    wait "${hogs[@]}" 2>/dev/null || true
    hogs=()
}

# netlat CASE [VAR=VALUE...] -- one restricted run of the real test; its
# output in $WORK/CASE.log, its exit status in RC, its window's wait in WAIT.
# In the background and waited for: `timeout` puts it in its own process
# group, so a TERM to ours reaches only this shell, whose trap must not wait
# out the run (with the hogs still spinning) before it can clean up.
netlat() {
    local name="$1"; shift
    env NETLAT_TEST_ONLY=always/driver NETLAT_TEST_P90_MS=0.001 "$@" \
        timeout 120 "$SCRIPT_DIR/test-net-latency-qemu.sh" --arch X64 > "$WORK/$name.log" 2>&1 &
    run=$!
    wait "$run"; RC=$?
    run=""
    WAIT=$(grep -oP '^  always/driver: .* cpu_wait_ms=\K[0-9]+$' "$WORK/$name.log")
    echo "--- $name: exit $RC, QEMU waited ${WAIT:-?} ms for a CPU over its round trips"
    grep -E '^  (PASS|FAIL): |^UNJUDGED: |^net-latency ' "$WORK/$name.log" | sed 's/^/    | /'
}

echo "=== the window's arithmetic (run_delay_window_ms, host only) ==="
# Per thread, so a thread that exits inside the window takes nothing away
# (a summed reading lost its whole history), one born inside counts from 0,
# and readings that cannot be compared are NOT MEASURED, never 0.
wcase() {   # wcase NAME BEFORE AFTER WANT ("rc1" = not measured)
    local got
    got=$(run_delay_window_ms "$2" "$3") || got=rc1
    check "window: $1" "$got" "$4"
}
wcase "a thread's own growth" "1 5000000" "1 12000000" "7"
wcase "a thread exiting mid-window subtracts nothing" $'1 5000000\n2 900000000' "1 7000000" "2"
wcase "a thread born mid-window counts from 0" "1 5000000" $'1 7000000\n3 50000000' "52"
wcase "no first reading: not measured" "" "1 5000000" "rc1"
wcase "no second reading: not measured" "1 5000000" "" "rc1"
wcase "no thread in common: not measured" "2 1000000" "3 100000000" "rc1"

echo "=== test-net-latency-qemu.sh under starvation (X64): guest + $HOGS hogs on CPU $HOG_CPU ==="

hogs_on
netlat starved NETLAT_TEST_PIN_CPU="$HOG_CPU"
hogs_off
check "STARVED: a timing miss under starvation exits 77 (UNJUDGED)" "$RC" "77"
check "...saying UNJUDGED, for run-one.sh" \
    "$(grep -E '^UNJUDGED: ' "$WORK/starved.log")" \
    "UNJUDGED: 1 timing bound missed while the guest's QEMU waited over $AXL_STARVE_MAX_MS ms for a CPU -- load, not a verdict"
check "...and the verdict line says so" "$(tail -1 "$WORK/starved.log")" \
    "net-latency (X64): UNJUDGED -- 2 passed, 1 timing bound missed under CPU starvation; re-run on a quiet box"
check "CONTROL: the witness moved past the limit under the hogs" \
    "$([[ "$WAIT" =~ ^[0-9]+$ ]] && (( WAIT > AXL_STARVE_MAX_MS )) && echo over)" "over"

netlat quiet NETLAT_TEST_STARVE_MAX_MS=1000000
check "QUIET: a timing miss whose window did not wait past the limit is a FAIL (exit 1)" "$RC" "1"
check "...with no UNJUDGED line" "$(grep -cE '^UNJUDGED: ' "$WORK/quiet.log")" "0"
check "...and the verdict line counts it failed" "$(tail -1 "$WORK/quiet.log")" \
    "net-latency (X64): 2 passed, 1 failed"
# The witness must not INVENT a wait either. The quiet run's own figure
# cannot show that -- this box is shared, and it read 176 ms at load 21 --
# so the control is a process that sleeps through the window: never
# runnable, it can wait for no CPU, under any load.
# Read once it is asleep: starting it (fork, exec) is runnable time, and a
# first reading taken then caught 6 ms of it under load.
sleep 6 & _sl=$!
sleep 1
_b=$(thread_run_delays "$_sl"); sleep 2; _a=$(thread_run_delays "$_sl")
kill "$_sl" 2>/dev/null; wait "$_sl" 2>/dev/null
check "CONTROL: a process asleep through a window reads 0 ms, whatever the load" \
    "$(run_delay_window_ms "$_b" "$_a")" "0"

hogs_on
netlat functional NETLAT_TEST_PIN_CPU="$HOG_CPU" NETLAT_TEST_WANT_READY_US=1
hogs_off
check "FUNCTIONAL: a functional failure beside a starved timing miss is a FAIL (exit 1)" "$RC" "1"
check "...with no UNJUDGED line" "$(grep -cE '^UNJUDGED: ' "$WORK/functional.log")" "0"
check "...and the verdict line counts both failed" "$(tail -1 "$WORK/functional.log")" \
    "net-latency (X64): 1 passed, 2 failed"
check "...though the guest really was starved (else this proves nothing)" \
    "$([[ "$WAIT" =~ ^[0-9]+$ ]] && (( WAIT > AXL_STARVE_MAX_MS )) && echo over)" "over"

hogs_on
netlat nofigures NETLAT_TEST_PIN_CPU="$HOG_CPU" NETLAT_TEST_RTT_COUNT=0 NETLAT_TEST_STARVE_MAX_MS=0
hogs_off
check "NO-FIGURES: no round trip timed, under starvation, is a FAIL (exit 1)" "$RC" "1"
check "...with no UNJUDGED line" "$(grep -cE '^UNJUDGED: ' "$WORK/nofigures.log")" "0"
check "...naming the functional check" \
    "$(grep -E '^  FAIL: ' "$WORK/nofigures.log")" \
    "  FAIL: always/driver: the round trips were measured (no RTT figures)"
check "...and the verdict line counts it failed" "$(tail -1 "$WORK/nofigures.log")" \
    "net-latency (X64): 2 passed, 1 failed"
check "...though its window counted as starved (else this proves nothing)" \
    "$([[ "$WAIT" =~ ^[0-9]+$ ]] && (( WAIT > 0 )) && echo over)" "over"

hogs_on
netlat perwindow NETLAT_TEST_ONLY="always/driver always/sync" NETLAT_TEST_PIN_CPU="$HOG_CPU" \
    NETLAT_TEST_UNMEASURED=always/sync
hogs_off
check "PER-WINDOW: a miss in an unmeasured window beside a starved one is a FAIL (exit 1)" "$RC" "1"
check "...with no UNJUDGED line" "$(grep -cE '^UNJUDGED: ' "$WORK/perwindow.log")" "0"
check "...the unmeasured window says so" \
    "$(grep -cE '^  always/sync: .* cpu_wait=unmeasured$' "$WORK/perwindow.log")" "1"
check "...and the verdict line counts both failed" "$(tail -1 "$WORK/perwindow.log")" \
    "net-latency (X64): 4 passed, 2 failed"
check "...though the measured window was starved (else this proves nothing)" \
    "$([[ "$WAIT" =~ ^[0-9]+$ ]] && (( WAIT > AXL_STARVE_MAX_MS )) && echo over)" "over"

echo "net-latency-starved (X64): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
