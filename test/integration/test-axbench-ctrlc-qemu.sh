#!/bin/bash
# test-meta: arch=x64 needs=socat est=20 local-only=0
# test-axbench-ctrlc-qemu.sh — proof that Ctrl-C aborts axbench cleanly
# while it is CPU-bound in an AP-pool scenario.
#
# axbench runs tight BSP-side orchestration loops (no event loop). Each
# such loop polls axl_interrupted() and, on Ctrl-C, prints an "interrupted"
# line and calls axl_exit(); an axl_atexit hook frees the task pool, which
# stops the AP workers before the image unloads. This test injects 0x03
# (Ctrl-C) over the serial socket once the bench is busy in scenario 4 and
# asserts:
#   - "interrupted (Ctrl-C)" appears  (a loop observed the flag), and
#   - "mem: no leaks detected" follows (atexit + cleanup ran -> pool freed,
#     workers joined, no leak / no double-free crash).
#
# Unlike test-yield-ctrlc.sh (idle app), here the guest is at ~100% CPU
# when interrupted, so we do NOT wait for CPU to settle — we inject as soon
# as the busy marker shows.
#
# Usage: test/integration/test-axbench-ctrlc-qemu.sh
# Requires: socat.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
EFI="$PROJECT_DIR/out/native-x64/tools/axbench.efi"

export TEST_SKIP_RATCHET=1

TMP=$(mktemp -d)
SOCK="$TMP/serial.sock"
QIN="$TMP/stdin.fifo"
LOG="$TMP/serial.log"
RUN_LOG="$TMP/run.out"
BRIDGE_PID=""
QEMU_PID=""

cleanup() {
    [[ -n "$BRIDGE_PID" ]] && kill "$BRIDGE_PID"  2>/dev/null || true
    [[ -n "$QEMU_PID"   ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

command -v socat >/dev/null 2>&1 || { echo "ERROR: socat required" >&2; exit 2; }

make -C "$PROJECT_DIR" ARCH=x64 axbench 2>&1 | tail -1

qemu_timeout=120
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then qemu_timeout=300; fi

"$RUN_QEMU" --background --timeout "$qemu_timeout" \
    --serial-socket "$SOCK" \
    --qemu-arg -smp --qemu-arg 4 "$EFI" > "$RUN_LOG" 2>&1

QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$RUN_LOG")
[[ -n "$QEMU_PID" ]] || { echo "ERROR: no QEMU pid"; cat "$RUN_LOG"; exit 1; }
echo "launched QEMU pid=$QEMU_PID"

for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; exit 1; }

mkfifo "$QIN"
exec 9<>"$QIN"
socat - "UNIX-CONNECT:$SOCK" < "$QIN" | tee -a "$LOG" >/dev/null &
BRIDGE_PID=$!

# Wait until the bench is busy in a compute scenario (scenario 4 runs for
# many seconds). Single-core boots have no AP work to interrupt -> SKIP.
busy_iters=160   # 40 s at 0.25 s/iter (KVM)
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then busy_iters=800; fi   # 200 s TCG
echo "waiting for busy marker..."
for _ in $(seq 1 "$busy_iters"); do
    grep -q "Single-core mode" "$LOG" 2>/dev/null && {
        echo "SKIP: single-core boot (no AP workers to interrupt)"; exit 0; }
    grep -q "Compute break-even sweep" "$LOG" 2>/dev/null && break
    sleep 0.25
done
grep -q "Compute break-even sweep" "$LOG" 2>/dev/null \
    || { echo "ERROR: bench never reached scenario 4"; sed -n '1,60p' "$LOG"; exit 1; }
echo "bench is busy; sending Ctrl-C"

# Give it a beat to be deep in a wave, then inject Ctrl-C (0x03).
sleep 0.5
printf '\x03' > "$QIN"
echo "Ctrl-C (0x03) sent"

echo "waiting for clean-abort markers..."
for _ in $(seq 1 40); do
    grep -q "mem: no leaks detected" "$LOG" 2>/dev/null && break
    sleep 0.25
done

echo ""
if grep -q "interrupted (Ctrl-C)" "$LOG" && grep -q "mem: no leaks detected" "$LOG"; then
    echo "=== PASS: Ctrl-C aborted axbench cleanly (pool freed, no leak) ==="
    grep -E "interrupted \(Ctrl-C\)|mem: no leaks detected" "$LOG" || true
    exit 0
fi
echo "=== FAIL: clean-abort markers not both present ==="
echo "interrupted line: $(grep -c 'interrupted (Ctrl-C)' "$LOG" || true)"
echo "no-leaks line:    $(grep -c 'mem: no leaks detected' "$LOG" || true)"
tail -40 "$LOG"
exit 1
