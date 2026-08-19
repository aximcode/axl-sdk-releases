#!/bin/bash
# test-meta: arch=x64 needs=gpu est=21 local-only=1
# test-sendkey-render-qemu.sh — the CAPTURE must reflect every injected key.
#
# The sibling test (test-sendkey-load-qemu.sh) asks "did the key arrive". This
# asks the harder and more useful question: "is its effect in the frame we
# captured". Those differ, and the difference is a real reported failure --
# after --sendkey-after fixed the LEADING keys, a consumer still lost the
# TRAILING one under 6-way parallelism (three shift-rights leaving the cursor
# at offset 2 in the shot).
#
# Measured before writing this: the guest finishes RECEIVING the last key
# ~1.2-1.3 s before the screendump under both spinner load and 6-VM load. So
# receipt is not the gap; anything missing is between "processed" and
# "painted", and only a rendering guest can show that.
#
# Oracle: one pure-red square per key, counted in the PPM. N keys must leave
# exactly N squares. Fewer means the shot was taken mid-repaint, and the
# harness reports HOW MANY -- a number, not a pixel diff, so a failure says
# which key was lost rather than that something changed.
#
# LOCAL-ONLY: needs a GPU-capable QEMU for a real GOP framebuffer.
#
# Usage: ./test/integration/test-sendkey-render-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

TEST_ARCH="${TEST_ARCH:-X64}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
AXL_CC="$(test_sdk_dir)/bin/axl-cc"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
DEMO_C="$SCRIPT_DIR/sendkey-render-selftest.c"

# Three keys: the consumer's most CONSISTENT failure was its shortest sequence
# (3 keys = 1.2 s of typing), because typing time doubles as incidental settle
# and a short sequence has the least of it. A long sequence would hide this.
KEY_TOKENS="a b c"
KEY_COUNT=3
SQ_PIXELS=$(( 32 * 32 ))

_arch_lc=$( [[ "$TEST_ARCH" == "AARCH64" ]] && echo aa64 || echo x64 )

if [[ ! -x "$AXL_CC" ]]; then
    echo "ERROR: $AXL_CC not found; run scripts/install.sh --arch $_arch_lc first" >&2
    exit 2
fi

WORK=$(mktemp -d)
LOAD_PIDS=()
cleanup() {
    for p in "${LOAD_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [[ "${AXL_KEEP_WORK:-0}" == "1" ]]; then echo "[kept] $WORK" >&2
    else rm -rf "$WORK"; fi
}
trap cleanup EXIT

EFI="$WORK/rr.efi"; SHOT="$WORK/shot.ppm"; SERIAL="$WORK/serial.log"; NSH="$WORK/rr.nsh"

"$AXL_CC" --arch "$_arch_lc" -DKEYS_EXPECTED=$KEY_COUNT -DWATCHDOG_MS=180000 \
    -DSLOW_PAINT_MS="${AXL_SLOW_PAINT_MS:-0}" \
    "$DEMO_C" -o "$EFI" >"$WORK/build.log" 2>&1 || {
    echo "ERROR: failed to build $DEMO_C" >&2; cat "$WORK/build.log" >&2; exit 2
}
printf '@echo -off\nrr.efi\n' > "$NSH"

ncpu=$(nproc 2>/dev/null || echo 4)
echo "--- loading host with $ncpu busy loops ---"
for _i in $(seq 1 "$ncpu"); do ( while :; do :; done ) & LOAD_PIDS+=("$!"); done

# AXL_RENDER_SETTLE lets a caller drive the trailing settle for A/B work.
SETTLE_ARG=()
[[ -n "${AXL_RENDER_SETTLE:-}" ]] && SETTLE_ARG=(--sendkey-settle "$AXL_RENDER_SETTLE")
# AXL_SCREENSHOT_AFTER=1 gates on the guest's end-of-sequence marker.
# AXL_GATE_PAT / AXL_GATE_COUNT drive the ORDINAL path instead: the app prints
# "RENDER-KEY n" after every paint, which is precisely the per-repaint marker
# that satisfies a first-match gate at key 1. Pointing the gate at that pattern
# reproduces the green-by-construction trap; adding the count fixes it.
GATE_ARG=()
if [[ -n "${AXL_GATE_PAT:-}" ]]; then
    GATE_ARG=(--screenshot-after "$AXL_GATE_PAT")
    [[ -n "${AXL_GATE_COUNT:-}" ]] && GATE_ARG+=(--screenshot-after-count "$AXL_GATE_COUNT")
elif [[ "${AXL_SCREENSHOT_AFTER:-0}" == "1" ]]; then
    GATE_ARG=(--screenshot-after 'RENDER-DONE')
fi

echo "--- injecting $KEY_COUNT keys, capturing (arch $TEST_ARCH) ---"
SHOT_WAIT=40 timeout 300 "$RUN_QEMU" --arch "$TEST_ARCH" --nsh "$NSH" \
    --extra "$EFI:rr.efi" \
    --screenshot "$SHOT" --sendkey "$KEY_TOKENS" \
    --sendkey-after 'RENDER-READY' "${SETTLE_ARG[@]}" "${GATE_ARG[@]}" \
    --serial-log "$SERIAL" \
    --timeout 200 "$EFI" >"$WORK/run.log" 2>&1 || true

for p in "${LOAD_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; LOAD_PIDS=()

pass=0; fail=0
check() {
    if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1));
    else echo "FAIL: $2"; fail=$((fail+1)); fi
}

if [[ ! -s "$SHOT" ]]; then
    echo "FAIL: no screenshot captured"
    tail -20 "$WORK/run.log"
    echo "sendkey-render: 0 passed, 1 failed"; exit 1
fi

# Count pure-red pixels. Nothing else on screen produces 0xFF0000, so the
# count divides cleanly into whole squares.
red=$(python3 - "$SHOT" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
# P6 header: magic, width, height, maxval — whitespace separated, '#' comments.
fields, i = [], 2
while len(fields) < 3:
    while i < len(d) and d[i:i+1].isspace(): i += 1
    if d[i:i+1] == b'#':
        while i < len(d) and d[i:i+1] != b'\n': i += 1
        continue
    j = i
    while j < len(d) and not d[j:j+1].isspace(): j += 1
    fields.append(int(d[i:j])); i = j
i += 1                      # single whitespace byte after maxval
px = d[i:]
print(sum(1 for k in range(0, len(px) - 2, 3)
          if px[k] == 0xFF and px[k+1] == 0 and px[k+2] == 0))
PY
)

want=$(( KEY_COUNT * SQ_PIXELS ))
squares=$(( red / SQ_PIXELS ))

# `|| rc=1`, never `[[ ... ]]; check "$?"`: common-test.sh runs under set -e and
# this script does not disable it, so the bare form would abort on the FIRST
# failing assertion and take the summary with it.
if [[ "${AXL_GATE_EXPECT_TRAP:-0}" == "1" ]]; then
    # Trap arm. A per-repaint marker with no ordinal satisfies the gate at the
    # FIRST paint, so the capture is short by construction. The bug worth
    # guarding is not that -- it is a harness reporting it as a pass. Assert
    # BOTH: that the early capture happened, and that the run said so. Pinning
    # only the warning would pass against a gate that never fired at all.
    rc=0; [[ "$red" -lt "$want" ]] || rc=1
    check "$rc" "trap arm captured an early frame ($squares of $KEY_COUNT squares)"

    rc=0
    /usr/bin/grep -qa "matched .* times but the gate waited for" "$WORK/run.log" || rc=1
    check "$rc" "a per-repaint marker with no --screenshot-after-count is REPORTED"
else
    rc=0; [[ "$red" -eq "$want" ]] || rc=1
    check "$rc" "capture shows all $KEY_COUNT keys ($squares of $KEY_COUNT squares; $red/$want red px)"

    rc=0; /usr/bin/grep -qa 'RENDER-DONE' "$SERIAL" || rc=1
    check "$rc" "guest processed every key (RENDER-DONE)"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- guest serial ---"
    /usr/bin/grep -a 'RENDER-' "$SERIAL" | tail -10
    echo "--- injection timing ---"
    /usr/bin/grep -a 'sendkey-after\|screenshot-after\|WARNING' "$WORK/run.log" | tail -8
fi

echo "--- results ---"
echo "sendkey-render: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
