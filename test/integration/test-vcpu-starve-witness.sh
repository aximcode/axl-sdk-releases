#!/bin/bash
# test-meta: arch=none needs=taskset,python3 est=12 local-only=0
# test-vcpu-starve-witness.sh -- qloop's CPU-starvation witness
# (vcpu_starve_sampler / vcpu_starve_report in scripts/axl-common.sh) is read
# PER THREAD, so a QEMU thread that exits inside the run neither drives the
# figure negative nor takes the run's wait with it.
#
# The witness was a SUMMED reading: the run delay of every thread of the
# guest's QEMU, first reading against last. A thread that exits between them
# (QEMU's I/O workers retire after ~10 s idle) leaves the last reading with
# its WHOLE history -- before the run too -- so a starved run could read idle
# or negative. On a leased (long-lived) pool guest qloop's row read about
# -59 ms. test-net-latency-qemu.sh's window had the same defect and was fixed
# per thread first (thread_run_delays, run_delay_window_ms); the sampler now
# accumulates that per-thread window between consecutive samples, so an
# exited thread keeps what it accrued up to its last sample.
#
#   SCRIPTED   thread_run_delays, proc_start_ticks and host_steal_ns stubbed
#              with fixed readings: the exact figure for a thread that
#              exits, one born mid-run, a reused tid, a pid that comes to
#              name another process (sampling stops, the figure so far
#              stands); and NOT MEASURED (never 0) for one reading, or two
#              consecutive readings sharing no thread.
#   REAL       thread-exit-spinner.py pinned to one CPU: two threads wait on
#              each other, the worker exits mid-window after ~1 s of run
#              delay before the window. The figure must be the window's own
#              wait (~1000 ms) -- not that minus the worker's history (the
#              summed witness, negative), nor the main thread's ~500 ms
#              alone (one per-thread first-against-last pair).
#   CONTROL    a process asleep through the window reads 0 ms: the witness
#              invents no wait. Stopped by a kill while the process lives,
#              as a pooled run stops it (_test_starve_stop).
#
# Usage: ./test/integration/test-vcpu-starve-witness.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

WORK="$(mktemp -d -t axl-starve-witness.XXXXXXXX)"
kids=()
cleanup() {
    if (( ${#kids[@]} > 0 )); then kill "${kids[@]}" 2>/dev/null || true; fi
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

# report FILE -- vcpu_starve_report's line, or "rc1" when it measured nothing.
report() {
    vcpu_starve_report "$1" || echo rc1
}

echo "=== scripted readings (thread_run_delays, proc_start_ticks, host_steal_ns stubbed) ==="
# scripted NAME WANT READING... -- each READING is one thread_run_delays
# output ("tid ns" lines). The sampler runs on a sleeping process; the stub
# ends that process once its readings are spent, which ends the sampler.
# The steal stub reads N ms at the Nth reading; the start-time stub reads
# 100, or the Nth element of starts=() when the caller sets it.
starts=()
scripted() {
    local name="$1" want="$2" i=0 r live smp
    shift 2
    rm -f "$WORK"/reading.* "$WORK/n" "$WORK/s.out"
    for r in "$@"; do printf '%s\n' "$r" > "$WORK/reading.$i"; i=$((i + 1)); done
    printf '%s\n' "${starts[@]}" > "$WORK/starts"
    echo 0 > "$WORK/n"
    sleep 30 & live=$!
    (
        thread_run_delays() {
            local n
            n=$(< "$WORK/n")
            if [[ ! -f "$WORK/reading.$n" ]]; then
                kill "$live" 2>/dev/null
                return 1
            fi
            echo $((n + 1)) > "$WORK/n"
            cat "$WORK/reading.$n"
        }
        host_steal_ns() { echo $(( $(< "$WORK/n") * 1000000 )); }
        proc_start_ticks() {
            local st
            st=$(sed -n "$(< "$WORK/n")p" "$WORK/starts")
            echo "${st:-100}"
        }
        vcpu_starve_sampler "$live" "$WORK/s.out" 0.01
    ) & smp=$!
    kids=("$live" "$smp")
    # A sampler that never reads the stub never ends on its own: bounded.
    timeout 5 tail --pid="$smp" -f /dev/null || echo "    (sampler still running after 5 s: stopped)"
    kill "$live" "$smp" 2>/dev/null
    wait "$smp" 2>/dev/null
    wait "$live" 2>/dev/null
    kids=()
    check "scripted: $name" "$(report "$WORK/s.out")" "$want"
}

# 1: +1 ms; 2: +50 ms then it exits; 3: born mid-run, counts from 0 (20 ms).
# A summed reading reads (7 + 20) - (5 + 900) ms here: NEGATIVE.
scripted "a thread exiting mid-run keeps what it accrued; one born counts from 0" "72 3" \
    $'1 5000000\n2 900000000' $'1 6000000\n2 950000000' $'1 7000000' $'1 7000000\n3 20000000'
scripted "a thread's own growth over many readings" "9 3" \
    "1 1000000" "1 4000000" "1 6000000" "1 10000000"
# tid 2 reused by a new thread: its counter restarts below the old one, and
# that new thread's 4 ms is counted, not lost to a clamp.
scripted "a reused tid counts the new thread from 0" "6 2" \
    $'1 1000000\n2 500000000' $'1 2000000\n2 1000000' $'1 3000000\n2 4000000'
scripted "one reading only: not measured" "rc1" "1 5000000"
scripted "consecutive readings sharing no thread: not measured" "rc1" \
    "1 5000000" "1 6000000" "2 9000000" "2 10000000"
scripted "no reading at all: not measured" "rc1"
# The 4th reading is another process's (pid reused): it is not counted, and
# sampling ends with the first three readings' figure.
starts=(100 100 100 200)
scripted "a pid naming another process ends the run at the last reading of the right one" "5 2" \
    "1 1000000" "1 3000000" "1 6000000" "1 900000000"
starts=()

echo "=== a real thread exiting mid-window (thread-exit-spinner.py) ==="
mapfile -t cpus < <(taskset -pc $$ | sed 's/.*: //' | tr ',' '\n' |
    awk -F- '{ hi = ($2 == "") ? $1 : $2; for (c = $1; c <= hi; c++) print c }')
CPU="${cpus[-1]}"
# Worker 3 s, main 4.5 s, both on one CPU: ~1 s of run delay each before the
# window opens at 2 s; in it ~0.5 s each until the worker exits at 3 s.
taskset -c "$CPU" python3 "$SCRIPT_DIR/thread-exit-spinner.py" 3 4.5 > "$WORK/spin.out" &
spin=$!
kids=("$spin")
for (( i = 0; i < 100; i++ )); do
    [[ -s "$WORK/spin.out" ]] && break
    sleep 0.05
done
check "the spinner started its worker" "$(cat "$WORK/spin.out")" "ready"
sleep 2
before=$(thread_run_delays "$spin" | wc -l)
check "the spinner has two threads when the window opens" "$before" "2"
vcpu_starve_sampler "$spin" "$WORK/real.out" &
smp=$!
kids=("$spin" "$smp")
timeout 20 tail --pid="$smp" -f /dev/null || echo "    (still running after 20 s: stopped)"
kill "$spin" "$smp" 2>/dev/null
wait "$smp" 2>/dev/null
wait "$spin" 2>/dev/null
kids=()
real=$(report "$WORK/real.out")
echo "    witness: $real (delay ms, steal ms)"
delay="${real%% *}"
# ~1000 ms is right; ~500 ms is the main thread alone (the worker's share
# dropped), negative is the summed witness. Load only adds to the figure.
check "REAL: the exited worker's share of the window is counted (>= 750 ms)" \
    "$([[ "$delay" =~ ^[0-9]+$ ]] && (( delay >= 750 )) && echo counted || echo "$delay ms")" "counted"

echo "=== control: a process asleep through the window waits for no CPU ==="
# Read only inside its sleep: starting it (fork, exec) and its wake-up are
# runnable time. Stopped by a kill, as a pooled run stops its sampler.
sleep 6 & sl=$!
kids=("$sl")
sleep 1
vcpu_starve_sampler "$sl" "$WORK/sleep.out" &
smp=$!
kids=("$sl" "$smp")
sleep 2
kill "$smp" 2>/dev/null
wait "$smp" 2>/dev/null
kill "$sl" 2>/dev/null
wait "$sl" 2>/dev/null
kids=()
real=$(report "$WORK/sleep.out")
check "CONTROL: a sleeping process reads 0 ms run delay" "${real%% *}" "0"

echo ""
echo "Results: $PASS passed, $FAIL failed"
(( FAIL == 0 ))
