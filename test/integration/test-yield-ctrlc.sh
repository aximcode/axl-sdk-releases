#!/bin/bash
# test-meta: arch=both needs=socat est=7 local-only=0
# arch=both since 2026-08-19: verified passing on AARCH64 unchanged. It was x64-only
# with no stated reason -- never ported, not unportable.
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
#   scripts/install.sh --arch x64     # one-time, produces stage/bin/axl-cc
#   test/integration/test-yield-ctrlc.sh
#
# Requires: socat, a staged SDK at out/ (from scripts/install.sh).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

AXL_CC="$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/bin/axl-cc"
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

# Launch QEMU in background. --serial-socket exposes the guest serial
# as a UNIX socket. QEMU's chardev with server=on,wait=off accepts a
# SINGLE bidirectional client connection — that's why the previous
# two-socat split (one for reading, one for writing) couldn't work:
# the second connection was rejected, and 0x03 never reached the
# guest. We now run a single bidirectional socat that reads from a
# control FIFO AND tees the socket output to the log file.
# QEMU's outer timeout: 60 s on KVM, 240 s on TCG. The test's own
# inner polling (idle_wait_iters etc.) gives up earlier on success
# paths; this just keeps QEMU alive long enough for slow TCG boots.
qemu_timeout=60
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    qemu_timeout=240
fi
"$RUN_QEMU" \
    --background --timeout "$qemu_timeout" \
    --serial-socket "$SOCK" \
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

mkfifo "$QIN"

# Hold the FIFO writer open in the shell BEFORE starting socat so
# socat's stdin doesn't see EOF immediately when no other writer is
# attached. fd 9 is the long-lived writer; the cleanup trap closes
# it on exit. Opening O_RDWR (<>) avoids the no-reader block on the
# named pipe.
exec 9<>"$QIN"

# Single bidirectional bridge using socat's stdio mode.
#   stdin   ← QIN ← shell fd 9 ← printf '\x03' (host control)
#   stdout  → tee → LOG (guest serial output)
# socat copies stdin → socket, socket → stdout. tee appends socat's
# stdout (the guest's serial output) to LOG. Drops tee's stdout
# because nothing further consumes it.
socat - "UNIX-CONNECT:$SOCK" < "$QIN" | tee -a "$LOG" > /dev/null &
BRIDGE_PID=$!

# Wait briefly for socat to actually open the socket — if it fails
# we'd hang forever waiting for the idle marker.
for _ in $(seq 1 30); do
    grep -q "" "$LOG" 2>/dev/null && break    # any byte means it's running
    [[ -n $(lsof -tU "$SOCK" 2>/dev/null) ]] && break
    sleep 0.1
done

# Wait for the "entering idle" marker. KVM finishes the sort+boot
# inside ~12-18 s; TCG (CI runners with no /dev/kvm access) needs
# ~3-4x longer because the array sort runs slower. Bump the budget
# accordingly.
idle_wait_iters=100   # 25 s at 0.25 s/iter
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    idle_wait_iters=480   # 120 s for TCG
fi
echo "waiting for idle marker..."
for _ in $(seq 1 "$idle_wait_iters"); do
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
if ! grep -q "SHUTDOWN-MARKER" "$LOG"; then
    echo "=== FAIL: SHUTDOWN-MARKER never appeared ==="
    echo ""
    tail -40 "$LOG"
    exit 1
fi
# The default (no-handler) break path must print the universal interrupt
# notice on stderr — proof every tool self-announces a Ctrl-C exit.
if ! grep -qF "Interrupted (Ctrl-C)" "$LOG"; then
    echo "=== FAIL: Ctrl-C shutdown ran but the 'Interrupted (Ctrl-C)' notice is missing ==="
    echo ""
    tail -40 "$LOG"
    exit 1
fi
echo "=== PASS: Ctrl-C routed through yield → auto-exit → atexit, notice printed ==="
echo ""
grep -E "yield-test:|SHUTDOWN-MARKER|Interrupted \(Ctrl-C\)|mem: no leaks" "$LOG" || true
exit 0
