#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 builds=timer-res-selftest guest=plain_firmware
# test-timer-resolution-qemu.sh -- the cross-image and exit-path half of
# axl_timer_resolution_* (the counting and the tick's effect are pinned by
# the unit tests): every result line timer-res-selftest.efi prints, in order,
# against the exact expected sequence.
#
#   1. the firmware boots at 10 ms
#   2. an image that exits still holding a request leaves 10 ms behind it
#   3. a child's request ends with the child; the parent's survives it, and
#      the parent's end restores 10 ms
#   4. an image that leaves through gBS->Exit (no AXL exit path: a crashed
#      image) leaves its request held -- until the next begin drops it,
#      because its image is gone. (Not covered: a dead image's handle reused
#      by the next one, which the per-image token and the recorded load
#      address handle; nothing here forces the firmware to reuse it.)
#   5. a tick changed behind AXL's back is left alone by the last end
#
# Usage: ./test/integration/test-timer-resolution-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" timer-res-selftest 2>&1 | tail -1
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"

WORK="$(mktemp -d -t axl-timerres.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
# Named relatively (the Shell's current directory: the boot disk's root, or a
# pooled run's own directory), with no command echo -- so the script runs on a
# leased guest too (run-qemu.sh's pooled mode). The class's agent leaves the
# firmware's tick alone (agent_latency = "firmware"): the 10 ms every
# PERIOD= line below starts from.
T='timer-res-selftest.efi'
cat > "$WORK/run.nsh" <<EOF
@echo -off
fs0:
$T period
$T hold 1000
$T period
$T child 5000 $T
$T raw-exit 1000
$T period
$T prune 5000
$T foreign 5000
$T period
EOF

timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" --timeout 150 \
    --nsh "$WORK/run.nsh" "$PREFIX/timer-res-selftest.efi" > "$WORK/log" 2>&1
tr -d '\r' < "$WORK/log" | grep -aE '^(PERIOD|GRANTED|CHILD-DONE|END|BEGIN-FAILED|CHILD-FAILED|FOREIGN-FAILED)' \
    > "$WORK/got"

cat > "$WORK/want" <<'EOF'
PERIOD=10000
GRANTED=1000
PERIOD=10000
GRANTED=5000
GRANTED=1000
CHILD-DONE PERIOD=5000
END rc=0 PERIOD=10000
GRANTED=1000
PERIOD=1000
GRANTED=5000
END rc=0 PERIOD=10000
GRANTED=5000
END rc=0 PERIOD=2000
PERIOD=10000
EOF

echo "=== axl_timer_resolution across images ($TEST_ARCH) ==="
if diff -u "$WORK/want" "$WORK/got"; then
    echo "  PASS: all $(wc -l < "$WORK/want") observations, in order"
    echo "timer-resolution ($TEST_ARCH): 1 passed, 0 failed"
    exit 0
fi
echo "  FAIL: the sequence differs (above: - expected, + got)"
tail -20 "$WORK/log" | sed 's/^/      /'
echo "timer-resolution ($TEST_ARCH): 0 passed, 1 failed"
exit 1
