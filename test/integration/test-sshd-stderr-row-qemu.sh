#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,tmux est=35 local-only=0 builds=sshd,stderr-row,pack-fixture-w guest=none
# guest=none: it types on the guest's USB keyboard (QMP send-key) and into
#   its Shell (test_send_stdin); a leased guest takes neither.
# test-sshd-stderr-row-qemu.sh -- in an ssh session, standard error shares
# the console's one cursor with standard output: what test-stderr-row-qemu.sh
# shows on the serial terminal, on the screen an ssh(1) user sees.
#
# The session is a console device in both the ConOut and the StdErr fan-out,
# and never ConOut's first device: the Shell draws its prompt from the first
# device's cursor record, which a line written through StdErr does not move,
# so the prompt was drawn over the session's error lines.
#
# The oracle is the session's screen as a terminal draws it (ssh(1) in tmux),
# every row matched exactly, a log line's clock as `T`: a tool's interleaved
# stdout, stderr and log lines and the teardown's leak verdict, then the pack
# stub's lines for a corrupt packed image, each command's prompt on its own
# row; then lines measured against the console's width, each line exactly
# that wide followed by the next on the very next row.
#
# Usage: ./test/integration/test-sshd-stderr-row-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/sshd-tmux-lib.sh"
source "$(dirname "$0")/lib/pack-lib.sh"
source "$(dirname "$0")/lib/wrap-rows-lib.sh"

sshd_require_tools ssh ssh-keygen tmux
test_parse_args "$@"
test_setup
tmux_probe || missing_tool "a working tmux ($(command -v tmux) cannot capture a pane)"

EXPECTED='FS0:\> stderr-row.efi
SR-partial SR-line
SR-err-1
SR-out-2
SR-mix SR-err-2
T [ERROR] stderr-row: SR-log-1
SR-out-3
T [INFO]  mem: no leaks detected
FS0:\> echo %T%-after
p4f-after
FS0:\> pack-corrupt.efi
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 2 of 2)
axl-pack: packed image is corrupt
FS0:\> echo %T%-done
p4f-done
FS0:\>'

# A command line typed into the session, then its Enter.
sh_cmd() { tmux_text S "$1"; tmux_keys S Enter; }

# The session's screen: trailing blanks and blank trailing rows dropped, a log
# line's clock as `T`.
session_screen() {
    tmux_screen S | sed -E 's/[[:space:]]+$//; s/^[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)? \[/T [/' \
        | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

sshd_build
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
declare -A _PE=([X64]=34404 [AARCH64]=43620)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_NATIVE[$TEST_ARCH]}" \
    stderr-row pack-fixture-w \
    > "$TEST_TMPDIR/make-fixtures.log" 2>&1 \
    || { echo "FAIL: the fixtures did not build"; tail -20 "$TEST_TMPDIR/make-fixtures.log"; exit 1; }
test_add_efi "$TEST_BUILD_DIR/stderr-row.efi"
pack_corrupt_copy "$TEST_BUILD_DIR/pack-fixture-w.efi" "$TEST_TMPDIR/pack-corrupt.efi" "${_PE[$TEST_ARCH]}"
if cmp -s "$TEST_BUILD_DIR/pack-fixture-w.efi" "$TEST_TMPDIR/pack-corrupt.efi"; then
    echo "FAIL: pack-fixture-w.efi has no packed payload to damage"
    exit 1
fi
test_add_efi "$TEST_TMPDIR/pack-corrupt.efi"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"

echo "=== sshd: stderr shares the session's cursor ($TEST_ARCH) ==="
sshd_boot ""

send 'sshd\r'
if ! wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60; then
    echo "FAIL: sshd did not start"
    test_clean_log; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
tmux_ssh_open S
if ! tmux_wait_screen S 'FS0:\>' 40; then
    echo "FAIL: the ssh session never showed the Shell"
    tmux_screen S; tail -20 "$TEST_TMPDIR/ssh-S.log"
    exit 1
fi

# The console's width (the session is one of its devices). A UEFI console
# moves to the next row the moment it writes the last column, and so does
# the session's device; a line exactly that wide must still be followed by
# the next line on the very next row, as on a Unix terminal. At 80 columns
# the stub's re-read lines are exactly that wide (aa64's console here; x64's
# is 100x31), and `stderr-row.efi wrap` measures its lines against the width
# at either.
sh_cmd 'stderr-row.efi size'
if ! wait_guest_re 'SR-SIZE [0-9]+x[0-9]+$' 1 30; then
    echo "FAIL: the guest did not report its console size"
    exit 1
fi
test_clean_log
COLS=$(grep -aoE 'SR-SIZE [0-9]+x' "$TEST_CLEAN_LOG" | tail -1 | tr -dc '0-9')
echo "  console: $COLS columns"

sh_cmd 'cls'
sleep 1
sh_cmd 'stderr-row.efi'
check "session: stderr-row.efi runs" wait_guest "SR-out-3" 1 30
sleep 1
sh_cmd 'echo %T%-after'
wait_guest "p4f-after" 1 20 || true
sleep 1
sh_cmd 'pack-corrupt.efi'
wait_guest_re 'axl-pack: packed image is corrupt$' 1 30 || true
sleep 1
sh_cmd 'echo %T%-done'
wait_guest "p4f-done" 1 20 || true
sleep 1

SCREEN=$(session_screen)
if [[ "$SCREEN" == "$EXPECTED" ]]; then
    pass "session: the screen reads every stdout and stderr line in order, each prompt on its own row"
else
    fail "session: the screen reads every stdout and stderr line in order, each prompt on its own row"
    diff <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$SCREEN") | sed 's/^/    | /' || true
fi

# Lines measured against the width: exactly it (stdout, two in a row,
# stderr, a log line), one short of it and one past it.
sh_cmd 'cls'
sleep 1
sh_cmd 'stderr-row.efi wrap'
wait_guest_re 'SR-W-END' 1 30 || true
sleep 1
sh_cmd 'echo %T%-wrapped'
wait_guest "p4f-wrapped" 1 20 || true
sleep 1
WRAP_EXPECTED=$(printf 'FS0:\\> stderr-row.efi wrap\n'
                wrap_rows
                printf 'FS0:\\> echo %%T%%-wrapped\np4f-wrapped\nFS0:\\>')
SCREEN=$(session_screen)
if [[ "$SCREEN" == "$WRAP_EXPECTED" ]]; then
    pass "session: a line exactly the width is followed by the next on the very next row"
else
    fail "session: a line exactly the width is followed by the next on the very next row"
    diff <(printf '%s\n' "$WRAP_EXPECTED") <(printf '%s\n' "$SCREEN") | sed 's/^/    | /' || true
fi

sshd_poweroff
test_clean_log
if test_check_leaks; then
    pass "boot: every leak verdict is clean"
else
    fail "boot: a leak or a missing verdict"
fi
n=$(heap_corruptions)
check "boot: no heap corruption reported ($n)" test "$n" = 0

echo ""
echo "sshd-stderr-row ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 ))
