#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,tmux,mtools,xxd est=60 local-only=0 builds=sshd guest=none
# guest=none: it types into the guest Shell (test_send_stdin, QMP send-key)
#   and reads its boot disk back with mtools.
# test-sshd-edit-qemu.sh -- the UEFI Shell's `edit` driven over ssh through
# several open / modify / save (F2) / exit (F3) cycles, with the file changed
# from the Shell between them, and the file's EXACT bytes asserted after every
# step.
#
# Every key goes through a real ssh(1) running in a tmux window (an xterm):
# tmux sends the F-keys, arrows, Home/End and Delete as a terminal does, and
# the take-over turns them back into the keys a USB keyboard reports. F2/F3,
# never ^S/^Q: ^S is also the Shell's output-halt key.
#
# Two oracles for the bytes:
#   guest  after every step the session types `echo %T%-s<N>`,
#          `hexdump.efi FILE`, `echo %T%-e<N>`, and the serial-log lines
#          between the two markers that match `^[0-9a-f]{8}: ` are compared,
#          whole lines and all of them, with xxd(1) of the expected bytes
#          (hexdump.efi prints xxd's format; the DEBUG build's leak-verdict
#          line after it does not match the pattern). This is the per-step
#          oracle: the image is live, so the host cannot read it mid-run
#          without racing the guest's FAT cache.
#   host   after the guest powers off, `mcopy` reads rt2.txt, new.txt and
#          u.txt straight off the disk image and `cmp`s them with the
#          expected bytes -- a second oracle that does not go through the
#          guest at all.
#
# Each edit session ends at the Shell: after F3 the session types
# `echo %T%-x<N>` and the serial log must show its output line. A key left in
# the queue by the editor, or an editor still running, breaks that line.
#
# The expectations follow EDK2's FileBuffer rules: a file without a BOM saves
# as ASCII, a NEW file and one with FF FE as UCS-2 with a BOM; every line keeps
# its own line ending and a new line takes the previous one's; nothing follows
# the last line, so a file ending in CRLF reloads with an empty last line and
# saves back as it was. `echo >> FILE` on an ASCII file fails ("Unable to
# redirect file."), so the Shell side appends with `>>a`.
#
# Steps (rt.txt starts as `alpha\r\nbeta\r\n`, written by the host):
#   s0   control: rt.txt as staged (proves the comparator reads bytes)
#   s1   edit rt.txt, type X, save                    Xalpha\r\nbeta\r\n
#   s2   Shell: echo gamma >>a rt.txt                 ...\r\ngamma\r\n
#   s3   edit, Down End, type !, save                 beta! on line 2
#   s4   edit, Down Down Home Enter, save             an empty line 3
#   s5   edit, Delete, Down End BSpace, save          X and ! gone
#   s6   Shell: cp -q rt.txt rt2.txt, del -q rt.txt   rt2.txt as s5; `ls rt.txt`
#        fails with %lasterror% 0xE (SHELL_NOT_FOUND)
#   s7   edit new.txt (absent), type hi, save         FF FE h\0 i\0
#   s8a  Shell: echo hi > u.txt                       UCS-2, CRLF
#   s8b  edit u.txt, type o, save                     FF FE o\0 h\0 i\0 CR\0 LF\0
#   s9a  edit rt2.txt, F3 with no change              unchanged
#   s9b  edit rt2.txt, type Z, F3, "File modified", n unchanged
#
# Usage: ./test/integration/test-sshd-edit-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/sshd-tmux-lib.sh"

test_parse_args "$@"
test_setup

# Does the tmux on PATH actually WORK? Found is not the question: this box's
# /usr/bin/tmux ("next-3.4", a pre-release snapshot) starts a session and then
# dies on the first capture-pane ("server exited unexpectedly"), which would
# read as "the ssh session never showed the Shell" -- a guest failure for a
# host tool that could not run. tmux_probe (lib/sshd-tmux-lib.sh) starts a
# throwaway server and captures it, every call bounded: a tmux that hangs
# is as unusable as one that dies.

# A host without one of these cannot run the suite. Under CI that is a
# FAILURE, not a skip: the runner only lists skips, so a CI host that lost
# tmux would otherwise stop running this suite and still report green. A
# local run without them skips, and says so loudly.
_missing=()
for _tool in ssh ssh-keygen tmux mcopy xxd; do
    command -v "$_tool" >/dev/null 2>&1 || _missing+=("$_tool")
done
if command -v tmux >/dev/null 2>&1 && ! tmux_probe; then
    _missing+=("a working tmux ($(command -v tmux): $(timeout 5 tmux -V 2>&1 || echo "no version") cannot capture a pane)")
fi
if (( ${#_missing[@]} > 0 )); then
    if [[ -n "${CI:-}" ]]; then
        test_host_fail "CI host lacks: ${_missing[*]} (needs=ssh,ssh-keygen,tmux,mtools,xxd)"
        test_host_summary "sshd-edit" || true
        test_cleanup
        exit 1
    fi
    echo "SKIP: *** missing on this host: ${_missing[*]} -- the edit sessions are real ssh(1) in tmux; NOTHING here ran ***"
    test_cleanup
    exit 77
fi

# The sessions are lib/sshd-tmux-lib.sh's: one ssh(1) in a 132x60
# xterm-256color tmux window per session, its own server on a socket in the
# run's scratch dir, every tmux call bounded, closed by its EXIT trap.

# ---------------------------------------------------------------------------
# The steps
# ---------------------------------------------------------------------------

# A command line typed into the session, then its Enter.
sh_cmd() { tmux_text E "$1"; tmux_keys E Enter; }

# The serial-log line of a marker echoed by the session, waited for.
shell_mark() {
    sh_cmd "echo %T%-$1"
    wait_guest "p4f-$1" 1 30
}

# expect_bytes STEP FILE ESCAPED -- the guest's hexdump of FILE, between this
# step's markers, is exactly xxd of the bytes printf %b makes of ESCAPED.
expect_bytes() {
    local step="$1" file="$2" want got
    printf '%b' "$3" > "$TEST_TMPDIR/want-$step"
    want=$(xxd < "$TEST_TMPDIR/want-$step")
    sh_cmd "echo %T%-s$step"
    sh_cmd "hexdump.efi $file"
    sh_cmd "echo %T%-e$step"
    if ! wait_guest "p4f-e$step" 1 30; then
        fail "$step: $file -- the read-back never finished"
        return
    fi
    test_slice_log "p4f-s$step" "p4f-e$step" "$TEST_TMPDIR/slice-$step" \
        || { fail "$step: $file -- a read-back marker is missing from the serial log"; return; }
    got=$(grep -aE '^[0-9a-f]{8}: ' "$TEST_TMPDIR/slice-$step" || true)
    if [[ "$got" == "$want" ]]; then
        pass "$step: $file is exactly $3"
    else
        fail "$step: $file is not exactly $3"
        echo "    want:"; sed 's/^/      /' <<< "$want"
        echo "    got:";  sed 's/^/      /' <<< "${got:-(nothing)}"
    fi
}

# edit_open FILE -- `edit FILE`, waiting for the editor's title bar. The
# previous editor cleared the screen on its way out, so a title still showing
# would make this wait vacuous: that is a failure of its own.
edit_open() {
    if grep -qF "UEFI EDIT" <<< "$(tmux_screen E || true)"; then
        echo "    the screen still shows an editor before \`edit $1\`"
        return 1
    fi
    sh_cmd "edit $1"
    tmux_wait_screen E "UEFI EDIT $1 " 30
}

# edit_save FILE LINES -- F2; the prompt names FILE and Enter keeps that
# name; the status bar reports LINES lines written (EDK2's own count: a file
# ending in CRLF reloads with an empty last line).
edit_save() {
    tmux_keys E F2
    tmux_wait_screen E "File to Save: [$1]" 20 || return 1
    tmux_keys E Enter
    tmux_wait_screen E " $2 Lines Written" 20
}

# edit_exit STEP -- F3 out of an unmodified (or just-saved) buffer, then the
# next command must run exactly: the editor is gone and left no key behind.
edit_exit() {
    tmux_keys E F3
    at_shell "$1: after F3 the Shell runs the next command exactly" "x$1"
}

# at_shell LABEL TAG -- the session's next command runs, or the run ends
# here: with an editor still holding the keyboard every later step would type
# into it, and each would fail on a timeout that proves nothing new.
at_shell() {
    if shell_mark "$2"; then
        pass "$1"
        return
    fi
    fail "$1"
    echo "    the session's screen:"
    tmux_screen E | sed 's/^/      /'
    printf "sshd-edit: %d passed, %d failed (%s) -- stopped at %s\n" \
        "$PASS" "$FAIL" "$TEST_ARCH" "$2"
    exit 1
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

sshd_build
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_NATIVE[$TEST_ARCH]}" tools \
    > "$TEST_TMPDIR/make-tools.log" 2>&1 \
    || { echo "FAIL: the tools did not build"; tail -20 "$TEST_TMPDIR/make-tools.log"; exit 1; }
test_add_efi "$TEST_BUILD_DIR/tools/hexdump.efi"
# sshd finds the key at the root of its own volume with no -k.
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
printf 'alpha\r\nbeta\r\n' > "$TEST_STAGING/rt.txt"

echo "=== sshd edit round trips ($TEST_ARCH) ==="
sshd_boot ""

send 'sshd\r'
if ! wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60; then
    echo "FAIL: sshd did not start"
    test_clean_log; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
tmux_ssh_open E
if ! tmux_wait_screen E 'FS0:\>' 40; then
    echo "FAIL: the ssh session never showed the Shell"
    tmux_screen E; tail -20 "$TEST_TMPDIR/ssh-E.log"
    exit 1
fi
check "session: a command typed in tmux runs on the Shell" shell_mark ready

# s0 -- the comparator, on bytes the host wrote.
expect_bytes s0 rt.txt '\x61\x6c\x70\x68\x61\x0d\x0a\x62\x65\x74\x61\x0d\x0a'

# s1 -- an existing ASCII file stays ASCII.
check "s1: edit opens rt.txt" edit_open rt.txt
tmux_text E X
check "s1: F2 saves 3 lines" edit_save rt.txt 3
edit_exit s1
expect_bytes s1 rt.txt 'Xalpha\r\nbeta\r\n'

# s2 -- the Shell appends between edits.
sh_cmd 'echo gamma >>a rt.txt'
expect_bytes s2 rt.txt 'Xalpha\r\nbeta\r\ngamma\r\n'

# s3 -- a key at the end of line 2.
check "s3: edit opens rt.txt" edit_open rt.txt
tmux_keys E Down End
tmux_text E '!'
check "s3: F2 saves 4 lines" edit_save rt.txt 4
edit_exit s3
expect_bytes s3 rt.txt 'Xalpha\r\nbeta!\r\ngamma\r\n'

# s4 -- Enter splits a line: the new one takes CRLF.
check "s4: edit opens rt.txt" edit_open rt.txt
tmux_keys E Down Down Home Enter
check "s4: F2 saves 5 lines" edit_save rt.txt 5
edit_exit s4
expect_bytes s4 rt.txt 'Xalpha\r\nbeta!\r\n\r\ngamma\r\n'

# s5 -- Delete and Backspace.
check "s5: edit opens rt.txt" edit_open rt.txt
tmux_keys E DC Down End BSpace
check "s5: F2 saves 5 lines" edit_save rt.txt 5
edit_exit s5
expect_bytes s5 rt.txt 'alpha\r\nbeta\r\n\r\ngamma\r\n'

# s6 -- copy then delete from the Shell; the old name is gone.
sh_cmd 'cp -q rt.txt rt2.txt'
sh_cmd 'del -q rt.txt'
expect_bytes s6 rt2.txt 'alpha\r\nbeta\r\n\r\ngamma\r\n'
sh_cmd 'ls rt.txt'
sh_cmd 'echo %T%-ls%lasterror%'
check "s6: \`ls rt.txt\` fails with %lasterror% 0xE" wait_guest "p4f-ls0xE" 1 30

# s7 -- a new file is UCS-2 with a BOM, and no newline after its last line.
check "s7: edit opens new.txt" edit_open new.txt
tmux_text E hi
check "s7: F2 saves 1 line" edit_save new.txt 1
edit_exit s7
expect_bytes s7 new.txt '\xff\xfe\x68\x00\x69\x00'

# s8 -- the Shell's `>` writes UCS-2; edit keeps it UCS-2.
sh_cmd 'echo hi > u.txt'
expect_bytes s8a u.txt '\xff\xfe\x68\x00\x69\x00\x0d\x00\x0a\x00'
check "s8b: edit opens u.txt" edit_open u.txt
tmux_text E o
check "s8b: F2 saves 2 lines" edit_save u.txt 2
edit_exit s8b
expect_bytes s8b u.txt '\xff\xfe\x6f\x00\x68\x00\x69\x00\x0d\x00\x0a\x00'

# s9 -- leaving without saving changes nothing.
S5_BYTES='alpha\r\nbeta\r\n\r\ngamma\r\n'
check "s9a: edit opens rt2.txt" edit_open rt2.txt
edit_exit s9a
expect_bytes s9a rt2.txt "$S5_BYTES"
check "s9b: edit opens rt2.txt" edit_open rt2.txt
tmux_text E Z
tmux_keys E F3
# The prompt is "... (Yes/No/Cancel) ? " -- capture-pane drops the trailing
# space. Its 'n' is answered with no Enter (a one-character input bar), so an
# Enter here would be a stray key for the Shell.
check "s9b: F3 on a modified buffer asks to save" \
    tmux_wait_screen E "File modified. Save (Yes/No/Cancel) ?" 20
tmux_text E n
at_shell "s9b: answering n returns to the Shell, which runs the next command exactly" xs9b
expect_bytes s9b rt2.txt "$S5_BYTES"

tmux_close E
sshd_poweroff

# The host's oracle: the disk image itself.
for f in rt2.txt:s9b new.txt:s7 u.txt:s8b; do
    name=${f%%:*} step=${f##*:}
    if mcopy -n -i "$TEST_DISK" "::$name" "$TEST_TMPDIR/host-$name" 2>/dev/null \
       && cmp -s "$TEST_TMPDIR/host-$name" "$TEST_TMPDIR/want-$step"; then
        pass "host: $name on the disk image is exactly the bytes of $step"
    else
        fail "host: $name on the disk image is not the bytes of $step"
        xxd "$TEST_TMPDIR/host-$name" 2>/dev/null | sed 's/^/      /'
    fi
done
check "host: rt.txt is gone from the disk image" \
    test "$(mdir -b -i "$TEST_DISK" ::rt.txt 2>/dev/null)" = ""

n=$(heap_corruptions)
check "no heap corruption reported in the run ($n)" test "$n" = 0
n=$(cpu_exceptions)
check "no CPU exception reported in the run ($n)" test "$n" = 0
test_clean_log
if test_check_leaks; then
    pass "leak gate: every verdict in the run is clean"
else
    fail "leak gate: a leak or a missing verdict"
fi

echo ""
printf "sshd-edit: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

EXPECTED_PASS=44
if [[ $PASS -ne $EXPECTED_PASS ]]; then
    echo "sshd-edit: expected $EXPECTED_PASS checks to pass, $PASS did"
fi
[[ $FAIL -eq 0 && $PASS -eq $EXPECTED_PASS ]] && exit 0 || exit 1
