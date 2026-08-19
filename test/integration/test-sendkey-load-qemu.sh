#!/bin/bash
# test-meta: arch=x64 needs=socat est=24 local-only=1
# test-sendkey-load-qemu.sh — run-qemu.sh --sendkey must deliver EVERY key,
# including when the host is busy.
#
# The bug this pins: --sendkey delivered reliably on an idle host and dropped
# whole keystrokes under CPU contention. A consumer's visual suite runs several
# QEMUs in parallel and hit it every run -- and the failures presented as
# application regressions (a search reporting "[not found]" for a pattern that
# is present, because the pattern was only half typed), which is what made it
# expensive rather than merely annoying.
#
# Why LOAD and not parallelism: running N guests at once is a way to starve the
# host, not the property under test. The property is "key delivery does not
# depend on how busy the host is", so this loads the host DELIBERATELY (one
# busy loop per core) and runs a SINGLE guest. That isolates delivery from
# scheduling luck and makes the failure reproducible instead of occasional.
#
# The oracle is the guest, not a screenshot. input-keys-selftest.c prints one
# "KEY ..." line per key-down and "INPUT-DONE" once it has seen KEYS_EXPECTED
# of them; its watchdog otherwise prints "INPUT-TIMEOUT (saw N keys)". So a
# dropped key is reported as a number, by the only party that actually knows.
#
# Usage: ./test/integration/test-sendkey-load-qemu.sh [--arch X64|AARCH64]

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
DEMO_C="$SCRIPT_DIR/input-keys-selftest.c"

# Ten keys: long enough that a per-key drop rate shows up, short enough to stay
# inside a sane timeout on aa64/TCG. All plain letters -- no modifiers, nothing
# the shell traps (see test-input-keys-qemu.sh on why ctrl-c is avoided).
KEY_TOKENS="a b c d e f g h i j"
KEY_COUNT=10

_arch_lc=$( [[ "$TEST_ARCH" == "AARCH64" ]] && echo aa64 || echo x64 )

if ! command -v socat >/dev/null 2>&1; then
    echo "ERROR: socat is required for this test" >&2; exit 2
fi
if [[ ! -x "$AXL_CC" ]]; then
    echo "ERROR: $AXL_CC not found; run scripts/install.sh --arch $_arch_lc first" >&2
    exit 2
fi

WORK=$(mktemp -d)
# AXL_KEEP_WORK=1 preserves the guest serial log + run.log for diagnosis; the
# interesting artefacts are otherwise deleted exactly when a failure makes you
# want them.
LOAD_PIDS=()
cleanup() {
    for p in "${LOAD_PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [[ "${AXL_KEEP_WORK:-0}" == "1" ]]; then
        echo "[kept] $WORK" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

EFI="$WORK/input-keys.efi"
SHOT="$WORK/shot.ppm"
SERIAL="$WORK/serial.log"
NSH="$WORK/keys.nsh"

# KEYS_EXPECTED matches what we inject, so "INPUT-DONE" means every key landed
# and the watchdog's "saw N keys" names the shortfall when one did not.
#
# The watchdog must outlive the whole injection window. run-qemu.sh injects at
# SHOT_WAIT (default TIMEOUT-3), so the app has to still be running then --
# otherwise the keys arrive at the Shell prompt behind a dead app and the run
# reports "saw 0 keys", which looks like total delivery failure and is not.
"$AXL_CC" --arch "$_arch_lc" -DKEYS_EXPECTED=$KEY_COUNT -DWATCHDOG_MS=180000 \
    "$DEMO_C" -o "$EFI" >"$WORK/build.log" 2>&1 || {
    echo "ERROR: failed to build $DEMO_C" >&2; cat "$WORK/build.log" >&2; exit 2
}

printf '@echo -off\ninput-keys.efi\n' > "$NSH"

# Load every core. `nproc` spinners is deliberately heavy-handed: the point is
# to make the guest compete for CPU the way it does under a parallel suite.
ncpu=$(nproc 2>/dev/null || echo 4)
echo "--- loading host with $ncpu busy loops ---"
for _i in $(seq 1 "$ncpu"); do
    ( while :; do :; done ) & LOAD_PIDS+=("$!")
done

# SHOT_WAIT is how long run-qemu waits before it starts typing. It is the whole
# question this test asks: the app must be UP by then, and under load the guest
# boots slower while this timer does not move. Overridable so the failing and
# passing configurations can both be driven from outside.
SHOT_WAIT="${SHOT_WAIT:-40}"
export SHOT_WAIT

echo "--- injecting $KEY_COUNT keys under load (arch $TEST_ARCH, SHOT_WAIT=$SHOT_WAIT) ---"
# --sendkey-after is the fix under test. The guest prints INPUT-READY the moment
# it has bound ConIn, so typing starts when the APP is ready rather than when a
# wall clock says it probably is. Without it, a slow boot under load pushes the
# first keystrokes ahead of the app and they land on the Shell prompt.
timeout 300 "$RUN_QEMU" --arch "$TEST_ARCH" --nsh "$NSH" \
    --extra "$EFI:input-keys.efi" \
    --screenshot "$SHOT" --sendkey "$KEY_TOKENS" \
    --sendkey-after 'INPUT-READY' \
    --serial-log "$SERIAL" \
    --timeout 200 "$EFI" >"$WORK/run.log" 2>&1 || true

for p in "${LOAD_PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
LOAD_PIDS=()

pass=0; fail=0
check() {
    if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1));
    else echo "FAIL: $2"; fail=$((fail+1)); fi
}

if [[ ! -s "$SERIAL" ]]; then
    echo "FAIL: no serial output captured (guest never ran?)"
    echo "--- run.log ---"; tail -20 "$WORK/run.log"
    echo "sendkey-under-load: 0 passed, 1 failed"
    exit 1
fi

# grep -a: the serial capture can carry a stray NUL, and one NUL makes GNU grep
# treat the whole log as binary -- the COUNT survives that but the listing does
# not, so a report would show a number with no evidence under it.
seen=$(/usr/bin/grep -ac '^KEY ' "$SERIAL" || true)

# NOTE the `|| rc=1` form. common-test.sh runs under `set -e`, so the tempting
# `[[ cond ]]; check "$?" ...` idiom KILLS the script the moment an assertion
# fails -- silently, before the FAIL line or the summary is printed. The first
# run of this test did exactly that: the keys really were lost, and the harness
# had no way to say so. An assertion that cannot report its own failure is
# worse than none.
rc=0; [[ "$seen" -eq "$KEY_COUNT" ]] || rc=1
check "$rc" "all $KEY_COUNT injected keys reached the guest (saw $seen)"

rc=0; /usr/bin/grep -qa 'INPUT-DONE' "$SERIAL" || rc=1
check "$rc" "guest reported INPUT-DONE (no watchdog timeout)"

if [[ "$fail" -ne 0 ]]; then
    echo "--- guest serial (tail) ---"
    /usr/bin/grep -a 'KEY \|INPUT-' "$SERIAL" | tail -15
fi

echo "--- results ---"
echo "sendkey-under-load: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
