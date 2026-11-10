#!/bin/bash
# test-meta: arch=both needs= est=240 local-only=0 builds=net-latency-selftest guest=none
# guest=none: not converted yet (pool design §6.7 step 8 item 7): a
#   --background run-qemu.sh guest per measurement.
# test-net-latency-qemu.sh -- axl_net_set_latency's round trips, measured:
# a guest echo server (net-latency-selftest.efi) under each policy, timed
# from the host by lib/tcp-rtt.py over a forwarded port, in two shapes:
# ASYNC (an event loop that sleeps between events -- the shape a real server
# has, and the one the firmware's 10 ms tick holds back) and SYNC
# (axl_tcp_recv/send, whose nested loops poll while they wait). FIRMWARE is
# the control that shows what the policies remove.
#
# LOAD MUST NOT READ AS A TIMING FAIL. On a shared box (CI's runner beside
# local agents) the p90 bound missed at load 18+ with nothing wrong in the
# code. The witness is qloop's (scripts/qloop-verdict.sh, one limit:
# AXL_STARVE_MAX_MS in scripts/axl-common.sh): how long the guest's QEMU
# threads waited for a CPU (schedstat run delay, per thread:
# thread_run_delays / run_delay_window_ms), read just
# before and just after each guest's round trips -- the window its timing
# checks judge, not its boot. Every measurement line carries it
# (`cpu_wait_ms=N`, or `cpu_wait=unmeasured`). Measured 2026-11-09 on 12 CPUs
# over five full runs each: 0-34 ms per window at the box's usual load, all
# green; 15-179 ms beside 18 busy loops, where 13 p90 misses (5.11-8.89 ms)
# each came in a window of 78 ms or more -- four runs UNJUDGED, one green.
# The checks are of two kinds:
#   TIMING      the p90 bound and the first-exchange bound -- a number of
#               milliseconds measured from the host;
#   FUNCTIONAL  everything else: a guest launched, the round trips measured
#               at all (a reply came back, echoed right), the tick read at
#               ready and at done, the exit check.
# A run whose failures are ALL timing misses, EACH in a window that waited over
# the limit, is UNJUDGED: it prints `UNJUDGED: <why>` and exits 77, which
# lib/run-one.sh reads (retried; still UNJUDGED, named and counted apart --
# not red, never green). A functional failure, a timing miss in a window that
# did not wait (or whose wait was not measured), or any mix with one, stays
# FAIL: a real regression on a quiet box still fails. The witness sees this
# kernel's scheduler only: it does not see the host-side client (tcp-rtt.py)
# starved alone, nor a slowdown that does not preempt (a slower core, a
# frequency drop) -- those still read FAIL.
#
# TEST-ONLY KNOBS, for test-net-latency-starved-qemu.sh, which drives the
# verdict on purpose (each is announced when set, checked, and refused --
# exit 2 -- unless NETLAT_TEST_ONLY makes the run a restricted one):
#   NETLAT_TEST_ONLY           the measurements to run ("always/driver ...");
#                              set, the exit check is not run
#   NETLAT_TEST_P90_MS         the p90 bound (default 5)
#   NETLAT_TEST_STARVE_MAX_MS  the wait limit (default AXL_STARVE_MAX_MS)
#   NETLAT_TEST_WANT_READY_US  the tick the ready check expects
#   NETLAT_TEST_PIN_CPU        pin the guest's QEMU to this CPU just before
#                              its round trips
#   NETLAT_TEST_UNMEASURED     take no wait reading for that measurement
#                              ("always/sync")
#   NETLAT_TEST_RTT_COUNT      round trips per measurement (default 50; 0
#                              yields no figures -- a measurement that failed)
#
# Usage: ./test/integration/test-net-latency-qemu.sh [--arch X64|AARCH64] [--measure]
#   --measure  print the numbers and assert nothing (for choosing bounds)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

TEST_ARCH="X64"
MEASURE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        --measure) MEASURE=1; shift ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"
# thread_run_delays, qemu_real_pid, AXL_STARVE_MAX_MS: qloop's witness, read
# per thread over a short window.
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

# The test-only knobs (see the header): announced, checked, and REFUSED
# outside a restricted run, so one left in an environment cannot loosen a
# full run's bounds or relabel its misses.
NETLAT_TEST_UNMEASURED="${NETLAT_TEST_UNMEASURED:-}"
declare -A _knob_re=([NETLAT_TEST_P90_MS]='^[0-9]+(\.[0-9]+)?$' [NETLAT_TEST_STARVE_MAX_MS]='^[0-9]+$'
          [NETLAT_TEST_WANT_READY_US]='^[0-9]+$' [NETLAT_TEST_PIN_CPU]='^[0-9]+$'
          [NETLAT_TEST_RTT_COUNT]='^[0-9]+$'  [NETLAT_TEST_UNMEASURED]='^[a-z]+/[a-z]+$' [NETLAT_TEST_ONLY]='^[a-z/ ]+$')
for _k in "${!_knob_re[@]}"; do
    [[ -n "${!_k:-}" ]] || continue
    echo "net-latency: TEST KNOB $_k=${!_k}"
    if [[ ! "${!_k}" =~ ${_knob_re[$_k]} ]]; then
        echo "net-latency: $_k='${!_k}' is not ${_knob_re[$_k]}" >&2; exit 2
    fi
    if [[ -z "${NETLAT_TEST_ONLY:-}" ]]; then
        echo "net-latency: $_k is a test-only knob; it needs NETLAT_TEST_ONLY (a restricted run)" >&2
        exit 2
    fi
done
P90_MS="${NETLAT_TEST_P90_MS:-5}"
STARVE_MAX_MS="${NETLAT_TEST_STARVE_MAX_MS:-$AXL_STARVE_MAX_MS}"
ONLY="${NETLAT_TEST_ONLY:-}"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" net-latency-selftest 2>&1 | tail -1
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"
WORK="$(mktemp -d -t axl-netlat.XXXXXXXX)"
QPIDS=()
cleanup() {
    local p
    for p in "${QPIDS[@]}"; do kill "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0
FAIL=0
STARVED_MISS=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# timing <key> <ok> <check> -- a TIMING check of measurement <key>, carrying
# its window's wait. A miss is a FAIL; it counts toward UNJUDGED only when
# that window's wait was measured and over the limit.
timing() {
    local w="${R_WAIT[$1]:-}" fig
    fig="QEMU waited ${w} ms for a CPU"
    [[ -n "$w" ]] || fig="QEMU's CPU wait not measured"
    if (( $2 )); then
        pass "$3 -- $fig"
        return
    fi
    fail "$3 -- $fig"
    [[ "$w" =~ ^[0-9]+$ ]] && (( w > STARVE_MAX_MS )) && STARVED_MISS=$((STARVED_MISS + 1))
    return 0
}

# measure <mode> <shape>: boot, time the round trips, collect the guest's
# lines. Sets READY_PERIOD, DONE_PERIOD, MEDIAN, P90, FIRST (empty on failure)
# and WAIT, the ms the guest's QEMU waited for a CPU over the round trips
# (empty when not measured).
measure() {
    local mode="$1" shape="$2" log="$WORK/$1-$2.run" nsh="$WORK/$1-$2.nsh"
    READY_PERIOD="" DONE_PERIOD="" MEDIAN="" P90="" FIRST="" WAIT=""
    printf 'fs0:\\net-latency-selftest.efi %s %s 7777\n' "$mode" "$shape" > "$nsh"
    timeout 60s "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" --background \
        --timeout 200 --net --hostfwd auto:7777 --nsh "$nsh" \
        "$PREFIX/net-latency-selftest.efi" > "$log" 2>&1
    local qpid serial port i
    qpid=$(grep -oP '^QEMU_PID=\K\d+' "$log")
    serial=$(grep -oP '^SERIAL_LOG=\K.*' "$log")
    port=$(grep -oP '^HOSTFWD_7777=\K\d+' "$log")
    [[ -n "$qpid" ]] && QPIDS+=("$qpid")
    if [[ -z "$qpid" || -z "$serial" || -z "$port" ]]; then
        fail "$mode/$shape: guest launched"
        sed 's/^/      /' "$log" | head -8
        return
    fi
    for i in $(seq 1 600); do
        grep -aq 'LATENCY-READY\|net-latency-selftest:' "$serial" 2>/dev/null && break
        sleep 0.2
    done
    READY_PERIOD=$(tr -d '\r' < "$serial" | grep -aoP '^LATENCY-READY .* period=\K\d+')
    # The witness, over exactly the round trips: the REAL qemu (QEMU_PID is
    # its `timeout` wrapper, whose threads never wait for anything).
    local rtt rpid w0="" w1=""
    if rpid=$(qemu_real_pid "$qpid"); then
        if [[ -n "${NETLAT_TEST_PIN_CPU:-}" ]] \
           && ! taskset -a -p -c "$NETLAT_TEST_PIN_CPU" "$rpid" > /dev/null; then
            fail "$mode/$shape: QEMU pinned to CPU $NETLAT_TEST_PIN_CPU (NETLAT_TEST_PIN_CPU)"
        fi
        [[ "$NETLAT_TEST_UNMEASURED" == "$mode/$shape" ]] || w0=$(thread_run_delays "$rpid") || w0=""
    fi
    rtt=$(python3 "$SCRIPT_DIR/lib/tcp-rtt.py" 127.0.0.1 "$port" --count "${NETLAT_TEST_RTT_COUNT:-50}")
    if [[ -n "$w0" ]] && w1=$(thread_run_delays "$rpid"); then
        WAIT=$(run_delay_window_ms "$w0" "$w1") || WAIT=""
    fi
    MEDIAN=$(grep -oP 'median_ms=\K[0-9.]+' <<< "$rtt")
    P90=$(grep -oP 'p90_ms=\K[0-9.]+' <<< "$rtt")
    FIRST=$(grep -oP 'first_ms=\K[0-9.]+' <<< "$rtt")
    for i in $(seq 1 100); do
        grep -aq 'LATENCY-DONE' "$serial" 2>/dev/null && break
        sleep 0.2
    done
    DONE_PERIOD=$(tr -d '\r' < "$serial" | grep -aoP '^LATENCY-DONE period=\K\d+')
    local wfig="cpu_wait=unmeasured"
    [[ -n "$WAIT" ]] && wfig="cpu_wait_ms=$WAIT"
    echo "  $mode/$shape: ready_period=${READY_PERIOD:-?} $rtt done_period=${DONE_PERIOD:-?} $wfig"
    kill "$qpid" 2>/dev/null
}

# lt <a> <b>: a < b, as decimals.
lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'; }

# selected <key> -- in this run (every measurement, unless NETLAT_TEST_ONLY).
selected() { [[ -z "$ONLY" || " $ONLY " == *" $1 "* ]]; }

echo "=== axl_net_set_latency round trips ($TEST_ARCH) ==="
declare -A R_MEDIAN R_P90 R_FIRST R_READY R_DONE R_WAIT
for shape in async driver sync; do
    for mode in firmware window always; do
        k="$mode/$shape"
        selected "$k" || continue
        measure "$mode" "$shape"
        R_MEDIAN[$k]="$MEDIAN"; R_P90[$k]="$P90"; R_FIRST[$k]="$FIRST"
        R_READY[$k]="$READY_PERIOD"; R_DONE[$k]="$DONE_PERIOD"; R_WAIT[$k]="$WAIT"
    done
done

# After exit, never fast again: an exit handler that runs after the policy's
# own (registered before the first socket) arms a socket; a poll timer made
# then would fire into the unloaded image. The tick must be back at 10 ms and
# the Shell alive well past a 1 ms tick. A FUNCTIONAL check; not run under
# NETLAT_TEST_ONLY.
if [[ -z "$ONLY" ]]; then
    {
        echo 'fs0:\net-latency-selftest.efi exit-arm'
        echo 'fs0:\net-latency-selftest.efi period'
        echo 'stall 300000'
        echo 'echo SHELL-ALIVE'
    } > "$WORK/exit.nsh"
    timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" --timeout 90 --net \
        --nsh "$WORK/exit.nsh" "$PREFIX/net-latency-selftest.efi" > "$WORK/exit.log" 2>&1
    EXIT_OUT=$(tr -d '\r' < "$WORK/exit.log")
    if grep -qx 'EXIT-ARM period=1000' <<< "$EXIT_OUT" && grep -qx 'PERIOD=10000' <<< "$EXIT_OUT" \
       && grep -qx 'SHELL-ALIVE' <<< "$EXIT_OUT"; then
        pass "after exit: socket work in a later exit handler leaves no poll timer behind (10 ms, Shell alive)"
    else
        fail "after exit: socket work in a later exit handler leaves no poll timer behind"
        grep -aE 'EXIT-ARM|PERIOD|SHELL-ALIVE|ERROR|!!!' <<< "$EXIT_OUT" | head -6 | sed 's/^/      /'
    fi
fi

if (( MEASURE )); then
    echo "net-latency ($TEST_ARCH): measured only"
    exit 0
fi
# The bounds, from measurement (2026-11-03, idle host; see the design doc
# §2.5): policies' p90 0.4-1.1 ms on both arches and shapes; FIRMWARE's p90
# the X64 async server ~20 ms in three runs and 0.5 ms in the fourth, and on
# AARCH64 anywhere from 1 to 10 ms: the default path is bimodal (an exchange
# that lands while the transmit pacer is still polling is fast), so FIRMWARE
# is reported, never bounded. That the bounds below CAN fail is shown by
# sabotage instead: a 1 ms tick with the poll removed fails them.
# ALWAYS's first exchange 2.5-8 ms; WINDOW's still pays the firmware (21-33).
for shape in async driver sync; do
    for mode in firmware window always; do
        k="$mode/$shape"
        selected "$k" || continue
        want_ready=1000; [[ "$mode" == firmware ]] && want_ready=10000
        want_ready="${NETLAT_TEST_WANT_READY_US:-$want_ready}"
        if [[ "${R_READY[$k]}" == "$want_ready" ]]; then
            pass "$k: the tick reads $want_ready us once the server listens"
        else
            fail "$k: the tick reads $want_ready us once the server listens (got ${R_READY[$k]:-none})"
        fi
        if [[ "${R_DONE[$k]}" == 10000 ]]; then
            pass "$k: back to 10000 us once the server is done"
        else
            fail "$k: back to 10000 us once the server is done (got ${R_DONE[$k]:-none})"
        fi
        # No figures at all is FUNCTIONAL: no reply came back, or a wrong
        # one (tcp-rtt.py says which in the measurement line above).
        if [[ -z "${R_P90[$k]}" ]]; then
            fail "$k: the round trips were measured (no RTT figures)"
        elif [[ "$mode" != firmware ]]; then
            timing "$k" "$(lt "${R_P90[$k]}" "$P90_MS" && echo 1 || echo 0)" \
                "$k: p90 round trip under $P90_MS ms (${R_P90[$k]} ms)"
        fi
    done
done
# (No figures at all is already the functional failure above.)
if selected always/async && [[ -n "${R_P90[always/async]}" ]]; then
    if [[ -z "${R_FIRST[always/async]}" ]]; then
        fail "always/async: the first exchange was measured (no figure)"
    else
        timing always/async "$(lt "${R_FIRST[always/async]}" 15 && echo 1 || echo 0)" \
            "always/async: even the first exchange is fast (${R_FIRST[always/async]} ms, want < 15)"
    fi
fi

# The wait figure, on every run: what the verdict below was judged against.
_max="" _unm=0
for k in "${!R_WAIT[@]}"; do
    if [[ -z "${R_WAIT[$k]}" ]]; then _unm=$((_unm + 1))
    elif [[ -z "$_max" ]] || (( R_WAIT[$k] > _max )); then _max="${R_WAIT[$k]}"; fi
done
echo "net-latency ($TEST_ARCH): the guests' QEMU waited at most ${_max:-?} ms for a CPU over a round-trip window (limit $STARVE_MAX_MS ms)$( (( _unm )) && echo "; $_unm window(s) not measured")"

# UNJUDGED only when EVERY failure is a timing miss in a starved window.
if (( FAIL > 0 && FAIL == STARVED_MISS )); then
    _b="bound"; (( FAIL > 1 )) && _b="bounds"
    echo "UNJUDGED: $FAIL timing $_b missed while the guest's QEMU waited over $STARVE_MAX_MS ms for a CPU -- load, not a verdict"
    echo "net-latency ($TEST_ARCH): UNJUDGED -- $PASS passed, $FAIL timing $_b missed under CPU starvation; re-run on a quiet box"
    exit 77
fi
echo "net-latency ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
