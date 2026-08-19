#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0
# test-minimal-log-qemu.sh — a --minimal-runtime image with no log engine
# no-ops axl_error and keeps running; the same image with `=log` logs normally.
#
# The linkage half is test-log-link-granularity.sh. This is the half a link
# check cannot see. axl_log_full is a trampoline that forwards to a WEAK
# _axl_log_vdispatch, so in an image that never asked for the engine the target
# is address ZERO -- a missing NULL check is a #GP or a hang, not a quiet
# regression. And silence has to be provably silence: if the no-engine image
# faulted inside axl_error, "the record is absent" would look exactly the same.
# So both images print an unconditional marker AFTER the log call, and the
# marker is what tells the two failures apart.
#
# One source (minimal-log-selftest.c), two link lines, both arches -- x64 and
# aa64 differ in how an indirect call through a null function pointer fails
# (#GP vs a synchronous exception), and one arch passing says nothing about the
# other.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-minimal-log-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    minimal-log-off minimal-log-on 2>&1 | tail -2

test_add_efi "$NATIVE_DIR/minimal-log-off.efi" "app/mlog-off.efi"
test_add_efi "$NATIVE_DIR/minimal-log-on.efi"  "app/mlog-on.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo MLOG_OFF_BEGIN"
    echo "app\\mlog-off.efi"
    echo "echo MLOG_OFF_RC=%lasterror%"
    echo "echo MLOG_ON_BEGIN"
    echo "app\\mlog-on.efi"
    echo "echo MLOG_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== the log engine is opt-in, and opting out is silent not broken ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

if ! test_wait_for "MLOG_DONE" 120; then
    echo "FAIL: fixture did not finish within 120s"
    test_clean_log; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# The two images run in sequence in one boot, so the assertions are made
# against the slice of the transcript each one produced. Splitting on the
# shell's own echoes rather than counting occurrences globally: a global count
# of 1 for MINLOG-ENGINE-LINE is satisfied by the WRONG image printing it.
awk '/MLOG_OFF_BEGIN/{f=1;next} /MLOG_ON_BEGIN/{f=0} f' "$TEST_CLEAN_LOG" > "$TEST_TMPDIR/off.txt"
awk '/MLOG_ON_BEGIN/{f=1;next} /MLOG_DONE/{f=0} f'      "$TEST_CLEAN_LOG" > "$TEST_TMPDIR/on.txt"

# The marker is an EXACT whole line: it goes out through axl_printf, whose
# output this test also protects, and a substring match would wave through a
# truncated or doubled write.
marker_count() { grep -c -F -x "MINLOG-MARKER" "$1" 2>/dev/null || true; }
# The record is matched on its TAIL anchored to end-of-line: the console
# renderer prefixes a timestamp and a level, and puts the domain immediately
# before the message, so "minlog: <msg>$" pins everything about the record that
# is not a clock reading.
record_count() { grep -c -E "minlog: MINLOG-ENGINE-LINE$" "$1" 2>/dev/null || true; }

if [[ "$(marker_count "$TEST_TMPDIR/off.txt")" == "1" ]]; then
    pass "no-engine image runs to completion (axl_error did not fault)"
else
    fail "no-engine image printed no MINLOG-MARKER — axl_error through a NULL
      weak seam faulted or hung instead of no-opping"
    sed 's/^/      /' "$TEST_TMPDIR/off.txt" | tail -20
fi

if [[ "$(record_count "$TEST_TMPDIR/off.txt")" == "0" ]]; then
    pass "no-engine image emits no log record"
else
    fail "no-engine image logged anyway — the engine is being linked after all"
fi

if [[ "$(marker_count "$TEST_TMPDIR/on.txt")" == "1" ]]; then
    pass "--minimal-runtime=log image runs to completion"
else
    fail "--minimal-runtime=log image printed no MINLOG-MARKER"
    sed 's/^/      /' "$TEST_TMPDIR/on.txt" | tail -20
fi

if [[ "$(record_count "$TEST_TMPDIR/on.txt")" == "1" ]]; then
    pass "--minimal-runtime=log image logs the record through the real engine"
else
    fail "--minimal-runtime=log image did not log — the opt-in does not work,
      so the silence asserted above is not evidence of anything"
    sed 's/^/      /' "$TEST_TMPDIR/on.txt" | tail -20
fi

# A null indirect call raises a CPU exception rather than printing nothing, so
# name it: it turns a future failure into a diagnosis instead of a mystery.
if grep -qiE 'Exception Type|Synchronous Exception' "$TEST_CLEAN_LOG"; then
    fail "a CPU exception was raised — axl_log_full is calling the weak
      _axl_log_vdispatch without a NULL check"
    grep -iE 'Exception Type|Synchronous Exception' "$TEST_CLEAN_LOG" | head -2 | sed 's/^/      /'
else
    pass "no CPU exception"
fi

# The armed exit path still works with no engine linked: a minimal image that
# cannot report its status is a worse regression than a missing log line.
if grep -q -F -x "MLOG_OFF_RC=0x0" "$TEST_CLEAN_LOG"; then
    pass "no-engine image exits 0"
else
    fail "no-engine image did not exit 0"
    grep -F "MLOG_OFF_RC=" "$TEST_CLEAN_LOG" | sed 's/^/      /'
fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
test_cleanup
[[ "$FAIL" -eq 0 ]]
