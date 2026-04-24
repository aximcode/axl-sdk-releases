#!/bin/bash
# test-yield-ctrlc.sh — end-to-end proof that Ctrl-C routes through
# the axl_yield → default-exit → atexit path for a UEFI app running
# in QEMU.
#
# Timings: on a modern laptop + TCG the demo reaches the idle marker
# within ~12-18 s of launch (firmware + shell boot dominate); the
# CPU-idle check, Ctrl-C delivery, and clean-exit marker detection
# add ~3-4 s. The whole test completes in well under 30 s on success.
#
# The demo at test/integration/yield-test.c does a CPU-bound
# axl_array_sort (instrumented with axl_yield every 1024 outer iters),
# then enters axl_loop_run on the default loop. An axl_atexit
# callback prints a unique SHUTDOWN-MARKER line. The harness:
#
#   1. Builds the demo via axl-cc (requires scripts/install.sh first).
#   2. Launches QEMU in background with serial on a UNIX socket.
#   3. Waits for "entering idle" to appear in the serial log.
#   4. Confirms the QEMU process CPU is near zero (<5% over a
#      sub-second sample) before sending anything.
#   5. Writes a single 0x03 byte to the socket (Ctrl-C).
#   6. Greps for SHUTDOWN-MARKER in the log; that marker proves the
#      atexit callback ran, which proves the whole signal path:
#      shell break → backend notify → axl_signal on_break →
#      g_axl_interrupted set → loop observes → axl_loop_run returns →
#      main returns → _axl_cleanup → atexit list fires.
#
# Usage:
#   scripts/install.sh --arch x64     # one-time, produces out/bin/axl-cc
#   test/integration/test-yield-ctrlc.sh
#
# Requires: socat, a staged SDK at out/ (from scripts/install.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

AXL_CC="$PROJECT_DIR/out/bin/axl-cc"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
DEMO_C="$SCRIPT_DIR/yield-test.c"

TMP=$(mktemp -d)
EFI="$TMP/yield-test.efi"
SOCK="$TMP/serial.sock"
QIN="$TMP/stdin.fifo"
LOG="$TMP/serial.log"
RUN_LOG="$TMP/run.out"

cleanup() {
    [[ -n "$BRIDGE_PID"     ]] && kill "$BRIDGE_PID"     2>/dev/null || true
    [[ -n "$LOG_READER_PID" ]] && kill "$LOG_READER_PID" 2>/dev/null || true
    [[ -n "$QEMU_PID"       ]] && kill -9 "$QEMU_PID"    2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

# Prereqs
if ! command -v socat >/dev/null 2>&1; then
    echo "ERROR: socat is required for this test" >&2
    exit 2
fi
if [[ ! -x "$AXL_CC" ]]; then
    echo "ERROR: $AXL_CC not found; run scripts/install.sh --arch x64 first" >&2
    exit 2
fi
if [[ ! -f "$DEMO_C" ]]; then
    echo "ERROR: demo source missing: $DEMO_C" >&2
    exit 2
fi

# Build the demo (debug so AXL_MEM_DEBUG final leak report appears in
# the serial log as an additional exit-path signal).
"$AXL_CC" --debug "$DEMO_C" -o "$EFI" >/dev/null

mkfifo "$QIN"

# Launch QEMU in background. --serial-socket exposes the guest serial
# as a UNIX socket we can both read AND write from the host side.
"$RUN_QEMU" \
    --background --timeout 60 \
    --serial-socket "$SOCK" \
    --serial-log "$LOG" \
    "$EFI" > "$RUN_LOG" 2>&1

QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$RUN_LOG")
if [[ -z "$QEMU_PID" ]]; then
    echo "ERROR: could not capture QEMU pid"; cat "$RUN_LOG"; exit 1
fi
echo "launched QEMU pid=$QEMU_PID (socket=$SOCK)"

# Wait for QEMU to create the socket (should be sub-second)
for _ in $(seq 1 50); do
    [[ -S "$SOCK" ]] && break
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; exit 1; }

# Hold FIFO writer open for this shell so the socat reader never EOFs.
exec 9>"$QIN"

# Two one-way socat bridges. QEMU's chardev socket accepts a single
# connection, but splitting direction across two short-lived
# connections works because QEMU reconnects on EOF. One writer, one
# reader.
socat -u "UNIX-CONNECT:$SOCK" "CREATE:$LOG,append=1"  &
LOG_READER_PID=$!
socat    "OPEN:$QIN,rdonly"   "UNIX-CONNECT:$SOCK"    &
BRIDGE_PID=$!

# Wait for the "entering idle" marker — at most 25 s of polling.
echo "waiting for idle marker..."
for _ in $(seq 1 100); do
    grep -q "entering idle" "$LOG" 2>/dev/null && break
    sleep 0.25
done
if ! grep -q "entering idle" "$LOG" 2>/dev/null; then
    echo "ERROR: idle marker never appeared; showing first 80 log lines:"
    sed -n '1,80p' "$LOG"
    exit 1
fi
echo "idle marker seen"

# Confirm QEMU process CPU has settled near zero before injecting
# Ctrl-C. Sample utime+stime (jiffies) across a 400 ms window — at
# 100 Hz the delta × 2.5 is the percent-of-one-core figure.
read_cpu_pct() {
    local pid=$1
    local t1 t2
    t1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    sleep 0.4
    t2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    # Multiply by 2.5 since we sampled 0.4 s (100 * 1/0.4 = 250 → /100 = 2.5).
    awk "BEGIN{ printf \"%.0f\", ($t2-$t1) * 2.5 }"
}

for attempt in 1 2 3; do
    cpu=$(read_cpu_pct "$QEMU_PID")
    echo "  CPU sample $attempt: ${cpu}% of one core"
    if [[ $cpu -lt 5 ]]; then
        echo "CPU confirmed idle (<5%); sending Ctrl-C"
        break
    fi
done
if [[ $cpu -ge 5 ]]; then
    echo "ERROR: QEMU CPU stayed >=5% for 3 samples; not idling"
    tail -30 "$LOG"
    exit 1
fi

# Inject Ctrl-C (0x03, ETX) via the FIFO held open on fd 9.
printf '\x03' > "$QIN"
echo "Ctrl-C (0x03) sent"

# Wait for SHUTDOWN-MARKER — shutdown path should complete in <2 s.
echo "waiting for SHUTDOWN-MARKER..."
for _ in $(seq 1 20); do
    grep -q "SHUTDOWN-MARKER" "$LOG" 2>/dev/null && break
    sleep 0.25
done

echo ""
if grep -q "SHUTDOWN-MARKER" "$LOG"; then
    echo "=== PASS: Ctrl-C routed through yield → auto-exit → atexit ==="
    echo ""
    grep -E "yield-test:|SHUTDOWN-MARKER|mem: no leaks" "$LOG" || true
    exit 0
else
    echo "=== FAIL: SHUTDOWN-MARKER never appeared ==="
    echo ""
    tail -40 "$LOG"
    exit 1
fi
