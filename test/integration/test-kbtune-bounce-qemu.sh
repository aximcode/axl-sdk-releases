#!/bin/bash
# test-meta: arch=x64 needs= est=140 local-only=1
# test-kbtune-bounce — reproduce the UEFI USB-keyboard typematic "bounce" in QEMU
# (no hardware) and run the kbtune read-cadence A/B against it.
#
# The bounce: a KVM's delayed USB key-up makes UsbKbDxe treat the key as held and
# synthesize typematic repeats, which the shell's greedy reader surfaces as extra
# characters. run-qemu's --holdkey holds a USB key down via QMP past the typematic
# delay, reproducing exactly that. kbprobe is the reader (F1 greedy / F3 throttle /
# F2 debounce via --throttle/--debounce), logging each surfaced event to serial.
#
# What this pins (the open §7 question in docs/AXL-KbTune-Design.md, answered in a
# QEMU model of the mechanism — NOT the exact real-KVM link, so the assertions are
# on the RELATIONSHIP between readers, not absolute counts):
#   1. CONTROL: a short tap surfaces ~1 event (no bounce) — the harness isn't
#      inventing repeats.
#   2. GREEDY (shell): a held key surfaces a burst of typematic repeats (bounce).
#   3. THROTTLE (an EXPLICIT slow reader): a slower per-key reader surfaces FEWER
#      repeats than greedy — UsbKbDxe's bounded queue drops repeats when the
#      consumer lags. This proves the CADENCE LEVER exists. It does NOT prove
#      axedit's app-level immunity: the real apps (axedit/axcon/axterm) all
#      render ~16 here because QEMU's GOP is too fast to supply axedit's real-HW
#      slow-render throttle. So this asserts the mechanism via kbprobe's explicit
#      --throttle knob, not that any real app's rendering is enough. See §7.
#   4. DEBOUNCE (the render-independent filter): collapses same-key repeats. This
#      is what "A" ships; it works regardless of render speed.
#
# LOCAL-ONLY: it's timing-sensitive (SHOT_WAIT vs boot) and not yet proven robust
# under CI load; needs a GPU-capable QEMU + usb-kbd + QMP (all stock). No special
# firmware (stock OVMF). Ratchet-exempt (end-to-end scenario, not a unit count).
#
# Usage: ./test/integration/test-kbtune-bounce-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

# The bounce is UsbKbDxe typematic; drive it on X64 (kbprobe is built x64 and the
# timing constants below are tuned for X64/KVM boot).
if [[ "$TEST_ARCH" != "X64" ]]; then
    echo "kbtune-bounce test: SKIP (tuned for X64; $TEST_ARCH not supported)"
    exit 0
fi

RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
KBPROBE="$(test_build_dir x64)/kbprobe.efi"

# Build the reader. A build failure is a real FAIL (it's the test's own fixture).
if ! make -C "$PROJECT_DIR" ARCH=x64 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} kbprobe 2>&1 | tail -5; then
    echo "kbtune-bounce test: FAIL (kbprobe build failed)"
    exit 1
fi
if [[ ! -f "$KBPROBE" ]]; then
    echo "kbtune-bounce test: FAIL (kbprobe not produced despite a clean build)"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Run one A/B arm: launch kbprobe (with mode args) under run-qemu, hold KEY for
# HOLD_MS via --holdkey, and read back kbprobe's per-event log from serial. Sets
# the globals SHOWN (events surfaced) and DROP (debounced-drops).
SHOWN=0
DROP=0
run_arm() {
    local label="$1" probe_args="$2" hold_ms="$3"
    local nsh="$WORK/$label.nsh" log="$WORK/$label.log"
    printf '@echo -off\nkbprobe.efi %s\n' "$probe_args" > "$nsh"

    # --screenshot is required for the --holdkey injection phase; the shot itself
    # is unused here (kbprobe renders to the console, which we read via serial).
    SHOT_WAIT=18 timeout 90 "$RUN_QEMU" --arch X64 \
        --screenshot "$WORK/$label.png" --serial-log "$log" \
        --holdkey "a:$hold_ms" --nsh "$nsh" "$KBPROBE" >/dev/null 2>&1 || true

    local clean
    # `|| true`: if run-qemu died before creating $log (bad env), don't let the
    # failed pipeline abort the whole test under set -e -- fall through to the
    # never-started sentinel below, which records a FAIL.
    clean=$(sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' "$log" 2>/dev/null | tr -d '\r' || true)
    if ! printf '%s\n' "$clean" | grep -q 'kbprobe: press keys'; then
        SHOWN=-1; DROP=-1   # sentinel: kbprobe never started
        return
    fi
    # grep -c exits 1 on zero matches; under set -e that would abort, so || true.
    SHOWN=$(printf '%s\n' "$clean" | grep -cE '^#[0-9]' || true)
    DROP=$(printf '%s\n' "$clean" | grep -cE '\[drop\]' || true)
}

echo "=== kbtune bounce A/B (QEMU, hardware-free) ==="

# 1. CONTROL — a 60 ms tap is below the typematic delay: exactly one event, no
#    bounce. Proves the harness surfaces real keys and doesn't fabricate repeats.
run_arm "control" "" 60
echo "  control (60ms tap): shown=$SHOWN"
if [[ $SHOWN -ge 1 && $SHOWN -le 2 ]]; then
    test_host_pass "control: a short tap is clean (1 event, no bounce)"
else
    test_host_fail "control: a short tap should surface ~1 event, got $SHOWN"
fi

# 2. GREEDY (F1 shell) — a held key surfaces a burst. This is the bounce.
run_arm "greedy" "" 800
echo "  greedy (hold 800ms, no filter): shown=$SHOWN"
GREEDY=$SHOWN
if [[ $GREEDY -ge 8 ]]; then
    test_host_pass "greedy reader surfaces the typematic bounce (>=8 events)"
else
    test_host_fail "greedy reader should surface a bounce burst (>=8), got $GREEDY"
fi

# 3. THROTTLE (an EXPLICIT slow kbprobe reader) — surfaces FEWER repeats than
#    greedy: proves the cadence LEVER exists (bounded-queue drops). NOT a claim
#    about any real app's render being enough (QEMU renders too fast) -- see §7.
run_arm "throttle" "--throttle 80" 800
echo "  throttle 80ms (hold 800ms): shown=$SHOWN"
# Lower bound 1 (not 0): a throttled reader that surfaced NOTHING is a broken
# read, not cadence suppression -- that must FAIL, not masquerade as "fewer".
if [[ $SHOWN -ge 1 && $SHOWN -lt $GREEDY ]]; then
    test_host_pass "explicit throttle surfaces FEWER repeats than greedy ($SHOWN < $GREEDY) -- the cadence lever exists"
else
    test_host_fail "throttled reader should surface 1..<greedy ($GREEDY), got $SHOWN"
fi

# 4. DEBOUNCE (F2 axedit filter) — collapses same-key repeats (explicit drops).
run_arm "debounce" "--debounce 50" 800
echo "  debounce 50ms (hold 800ms): shown=$SHOWN drop=$DROP"
if [[ $DROP -ge 5 && $SHOWN -ge 0 && $SHOWN -le 3 ]]; then
    test_host_pass "debounce filter collapses the bounce (drops>=5, shown<=3)"
else
    test_host_fail "debounce filter should drop the repeats (drop>=5, shown<=3), got drop=$DROP shown=$SHOWN"
fi

test_host_summary "kbtune-bounce test (X64)"
