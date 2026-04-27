#!/bin/bash
# test-cpu-idle.sh -- CPU-idle regression guard.
#
# Runs AxlTestCpuIdle.efi (six x 500ms event-driven waits = ~3s
# guest wallclock) under QEMU and measures the host process's
# CPU-time / walltime ratio. Asserts the ratio stays below a
# threshold that cleanly separates idle (~0.2-0.4) from busy-poll
# (~1.0 saturation).
#
# Why ratio: absolute CPU% depends on host hardware, load, and TCG
# vs KVM mode. Ratio is portable because busy-poll saturates to
# ~1.0 on any host while idle stays well below on all of them.
#
# Usage:
#   make tests                              # build AxlTestCpuIdle.efi
#   ./test/integration/test-cpu-idle.sh     # run + assert
#
# Environment:
#   CPU_IDLE_THRESHOLD -- override the pass threshold (default 0.60)
#   CPU_IDLE_RUNS      -- number of runs, reports median (default 3)
#   ARCH               -- X64 (default) or AARCH64

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARCH="${ARCH:-X64}"
case "$ARCH" in
    X64|x64)     ARCH_SUFFIX="x64" ;;
    AARCH64|aa64) ARCH_SUFFIX="aa64" ;;
    *)           echo "error: unsupported ARCH=$ARCH"; exit 2 ;;
esac

THRESHOLD="${CPU_IDLE_THRESHOLD:-0.60}"
RUNS="${CPU_IDLE_RUNS:-3}"
TEST_EFI="$PROJECT_DIR/out/native-$ARCH_SUFFIX/AxlTestCpuIdle.efi"

if [ ! -f "$TEST_EFI" ]; then
    echo "error: $TEST_EFI not found"
    echo "       run: make ARCH=$ARCH_SUFFIX tests"
    exit 2
fi

# ---- Per-run timing helper ------------------------------------------------
#
# `time` on a pipeline captures the direct child (run-qemu.sh), whose
# CPU accounting (RUSAGE_CHILDREN) aggregates qemu-system and any
# OVMF helper processes. We use bash's builtin `time` with TIMEFORMAT
# to get parseable output.

run_one() {
    local qemu_log="$1"
    local timing_file="$2"

    TIMEFORMAT="%R %U %S"
    # QEMU timeout: KVM ~30s suffices (~6s boot + 10s waits + slack);
    # TCG (CI runners with no /dev/kvm access) needs ~3-4x longer for
    # OVMF boot alone. Auto-bump when KVM is unavailable.
    local timeout_sec=30
    if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
        timeout_sec=120
    fi
    { time "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" \
            --timeout "$timeout_sec" "$TEST_EFI" > "$qemu_log" 2>&1 ; } 2> "$timing_file"
    unset TIMEFORMAT

    # Parse "real user sys" (seconds, floating point)
    local real user sys cpu ratio
    read -r real user sys < "$timing_file"
    cpu=$(awk "BEGIN{print $user + $sys}")
    ratio=$(awk "BEGIN{print $cpu / $real}")
    echo "$real $cpu $ratio"
}

# ---- Sanity check the guest binary actually completed ---------------------

expect_guest_done() {
    local log="$1"
    if ! grep -q "cpu-idle: done" "$log"; then
        echo "error: guest binary did not print 'cpu-idle: done'"
        echo "       full qemu log (cleaned, $(wc -l < "$log") lines):"
        cat "$log"
        echo "       --- end qemu log ---"
        return 1
    fi
    return 0
}

# ---- Main -----------------------------------------------------------------

echo "CPU-idle regression test ($ARCH)"
echo "  binary:    $TEST_EFI"
echo "  threshold: $THRESHOLD  (CPU-time / walltime)"
echo "  runs:      $RUNS"
echo

declare -a RATIOS
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

for i in $(seq 1 "$RUNS"); do
    qemu_log="$TMPDIR/qemu-$i.log"
    timing_file="$TMPDIR/timing-$i.txt"
    read -r real cpu ratio <<<"$(run_one "$qemu_log" "$timing_file")"
    expect_guest_done "$qemu_log" || exit 1
    printf "  run %d: walltime=%ss cpu=%ss ratio=%s\n" "$i" "$real" "$cpu" "$ratio"
    RATIOS+=("$ratio")
done

# Median across runs
MEDIAN=$(printf '%s\n' "${RATIOS[@]}" | sort -g | \
         awk 'BEGIN{count=0} {a[count++]=$1} END{if(count%2==1) print a[int(count/2)]; else print (a[count/2-1]+a[count/2])/2}')

echo
echo "  median ratio: $MEDIAN"
echo

if awk "BEGIN{exit ($MEDIAN < $THRESHOLD) ? 0 : 1}"; then
    echo "PASS: median CPU ratio $MEDIAN < threshold $THRESHOLD"
    exit 0
fi

cat <<EOF
FAIL: median CPU ratio $MEDIAN >= threshold $THRESHOLD

This looks like a busy-poll regression in the event / wait
primitives. Things to check:

  * src/event/axl-event.c and axl-wait.c -- any new while-loops
    without an axl_loop_* or axl_backend_event_wait inside?
  * src/loop/axl-loop.c -- axl_loop_next_event should block in the
    backend event_wait, not spin.
  * Anything replacing axl_wait_* with raw axl_backend_stall loops?

Per-run ratios: ${RATIOS[*]}
Threshold:      $THRESHOLD   (override via CPU_IDLE_THRESHOLD)
EOF
exit 1
