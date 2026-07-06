#!/bin/bash
# test-meta: arch=x64 needs=socat est=15 local-only=0
# test-console-readline-qemu.sh — end-to-end verification of the
# interactive console line reader (axl_console_readline / _ex) and the
# transparent axl_stdin console-line-editing fallback.
#
# Reproduces (and locks the fix for) the consumer bug where a line typed
# at the console came back with a leading CR, needed a second Enter, and
# captured only the first character. It drives console-readline-selftest.c
# over a serial console: after "READLINE-READY", it feeds one line at a
# time via the guest's serial ConIn (TerminalDxe turns serial bytes into
# keystrokes) and asserts the EXACT echoed result line for each:
#
#   feed "hello\r"            -> LINE1=[hello]        (basic line)
#   feed "ab\bc\r"            -> LINE2=[ac]           (Backspace erased 'b')
#   feed "\r"                 -> LINE3=[]             (immediate Enter)
#   feed "s3cret\r" (hidden)  -> PASS4=[s3cret]       (echo suppressed:
#                                the literal must appear ONCE in the log —
#                                only in the result line, never echoed)
#   feed "world\r"            -> FALLBACK=[world]     (axl_readline(axl_stdin)
#                                line-cooked the interactive console)
#   plus                        INTERACTIVE=true
#
# The exact-match assertions inherently prove the bug is gone: a leading
# CR, a dropped character, or a doubled Enter would all change these lines.
#
# Coverage scope: this drives a DIRECTLY-launched EFI (app/launcher path). The
# resident-shared-driver interactive path is covered by construction — the line
# assembly reads gST->ConIn, a system-table global shared by every image, so a
# driver reads the identical console via the identical code; the bridge affects
# only WHICH handle axl_stdin_is_interactive() probes, and test-driver-stdio-qemu.sh
# verifies that predicate returns the byte path (false) for pipe/`<` through the
# bridge. A dedicated driver+keystroke harness for the interactive-through-bridge
# combination is deferred (documented in the code-review notes).
#
# Usage:
#   scripts/install.sh --arch x64     # one-time, produces out/bin/axl-cc
#   test/integration/test-console-readline-qemu.sh            # X64 (default)
#   test/integration/test-console-readline-qemu.sh --arch AARCH64
#
# Requires: socat, a staged SDK at out/ (from scripts/install.sh).

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg '$1' (try --help)" >&2; exit 2 ;;
    esac
done

case "$ARCH" in
    X64)     CC_ARCH="x64" ;;
    AARCH64) CC_ARCH="aa64" ;;
    *) echo "ERROR: --arch must be X64 or AARCH64 (got '$ARCH')" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

AXL_CC="$PROJECT_DIR/out/bin/axl-cc"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
SRC_C="$SCRIPT_DIR/console-readline-selftest.c"

TMP=$(mktemp -d)
EFI="$TMP/readline.efi"
SOCK="$TMP/serial.sock"
QIN="$TMP/stdin.fifo"
LOG="$TMP/serial.log"
RUN_LOG="$TMP/run.out"

cleanup() {
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID"  2>/dev/null || true
    [[ -n "${QEMU_PID:-}"   ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

if ! command -v socat >/dev/null 2>&1; then
    echo "ERROR: socat is required for this test" >&2; exit 2
fi
if [[ ! -x "$AXL_CC" ]]; then
    echo "ERROR: $AXL_CC not found; run scripts/install.sh --arch $CC_ARCH first" >&2
    exit 2
fi
if [[ ! -f "$SRC_C" ]]; then
    echo "ERROR: selftest source missing: $SRC_C" >&2; exit 2
fi

"$AXL_CC" --arch "$CC_ARCH" "$SRC_C" -o "$EFI" >/dev/null

qemu_timeout=90
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    qemu_timeout=240
fi

rm -f "$SOCK"
"$RUN_QEMU" \
    --arch "$ARCH" \
    --background --timeout "$qemu_timeout" \
    --serial-socket "$SOCK" \
    "$EFI" > "$RUN_LOG" 2>&1

QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$RUN_LOG" || true)
if [[ -z "$QEMU_PID" ]]; then
    echo "ERROR: could not capture QEMU pid"; cat "$RUN_LOG"; exit 1
fi
echo "launched QEMU pid=$QEMU_PID (arch=$ARCH, serial=$SOCK)"

for _ in $(seq 1 50); do
    [[ -S "$SOCK" ]] && break
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; cat "$RUN_LOG"; exit 1; }

mkfifo "$QIN"
exec 9<>"$QIN"   # long-lived writer so socat's stdin never sees EOF
socat - "UNIX-CONNECT:$SOCK" < "$QIN" | tee -a "$LOG" > /dev/null &
BRIDGE_PID=$!

# Wait for a marker to appear in the serial log.
wait_for() {
    local marker="$1" iters="${2:-200}"
    for _ in $(seq 1 "$iters"); do
        grep -aq "$marker" "$LOG" 2>/dev/null && return 0
        sleep 0.25
    done
    return 1
}

ready_iters=200
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    ready_iters=600
fi

echo "waiting for READLINE-READY..."
if ! wait_for "READLINE-READY" "$ready_iters"; then
    echo "ERROR: READLINE-READY never appeared; first 80 log lines:"
    sed -n '1,80p' "$LOG"; exit 1
fi
echo "READLINE-READY seen; feeding lines"

# Feed a line (raw bytes, %b interprets \xNN) then wait for its result.
feed_line() {
    local bytes="$1" marker="$2"
    printf '%b' "$bytes" > "$QIN"
    if ! wait_for "$marker" 40; then
        echo "ERROR: '$marker' never appeared after feeding '$bytes'"
        sed -n '1,120p' "$LOG"; exit 1
    fi
    echo "  fed line -> $marker"
}

feed_line 'hello\r'   'LINE1='
feed_line 'ab\bc\r'   'LINE2='
feed_line '\r'        'LINE3='
feed_line 's3cret\r'  'PASS4='
feed_line 'world\r'   'FALLBACK='

wait_for "READLINE-DONE" 40 || { echo "ERROR: READLINE-DONE never appeared"; sed -n '1,120p' "$LOG"; exit 1; }

echo ""
echo "=== observed marker lines ==="
grep -aE "READLINE-READY|LINE[0-9]=|PASS4=|FALLBACK=|INTERACTIVE=|READLINE-DONE" "$LOG" || true
echo ""

fail=0
assert_line() {
    local want="$1"
    if grep -aqF -- "$want" "$LOG"; then
        echo "PASS: $want"
    else
        echo "FAIL: expected line '$want'"
        fail=1
    fi
}

assert_line "LINE1=[hello]"
assert_line "LINE2=[ac]"
assert_line "LINE3=[]"
assert_line "PASS4=[s3cret]"
assert_line "FALLBACK=[world]"
assert_line "INTERACTIVE=true"

# Password must NOT have been echoed: the literal appears exactly once in
# the whole log — only in the PASS4 result line, never as an echo.
pass_hits=$(grep -aoF "s3cret" "$LOG" | wc -l)
if [[ "$pass_hits" -eq 1 ]]; then
    echo "PASS: hidden entry not echoed (s3cret appears once, in PASS4 only)"
else
    echo "FAIL: password echo leak — 's3cret' appears $pass_hits times (want 1)"
    fail=1
fi

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): interactive console readline + fallback verified ==="
    exit 0
else
    echo "=== FAIL ($ARCH): readline behavior did not match expected ==="
    exit 1
fi
