#!/bin/bash
# test-meta: arch=both needs=ssh est=77 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-channel-hostile-qemu.sh -- AxlSsh session channels driven at the
# byte level by ssh-peer.py: channel flow control in both directions, and a
# peer that breaks the rules a real client keeps. test-ssh-session-qemu.sh
# drives the same fixture with a real ssh(1); this file is what ssh(1)
# cannot do -- send before a window exists, ignore a window, address a
# channel that does not exist, read nothing for seconds, cross a CLOSE.
#
# The guest runs ssh-server-selftest.efi with `channels=1` (its fixture
# shell; see that file's "THE FIXTURE SHELL"). Setup and the teardown
# checks are lib/ssh-hostile-lib.sh's, as in the session suite; the peer
# helpers and message bodies are lib/ssh-channel-lib.sh's, shared with
# test-ssh-channel-budget-qemu.sh.
#
# Every peer runs a --script (see ssh-peer.py's module doc); its transcript
# is compared WHOLE, line by line, with its exit code. The server's channel
# number is never written here: `@` bodies take the one the peer last saw
# confirmed.
#
# Scenarios, in run order:
#   exit-order        exec "echo hi": SUCCESS, the window, the data, then
#                     exit-status, EOF, CLOSE -- in that order
#   open-hook         the open hook writes, closes, exits, or closes and
#                     refuses from inside itself: each in the header's order,
#                     and no close hook for the refused open
#   second-shell      a second shell, an exec and a pty-req after the start
#                     are refused (100); the session still answers
#   refused-requests  env, subsystem, x11-req, auth-agent-req, signal,
#                     break: each refused, the channel still usable
#   resize            pty-req, then window-change without a reply asked
#                     (RESIZE, no reply) and with one (SUCCESS); without a
#                     pty a window-change is answered but reaches no hook
#   before-start      data before the shell -- more than the 10% grace --
#                     is discarded uncounted; the session then works
#   pipelined-exec    data, exec "cat", data in ONE write: the first is
#                     before the start (discarded), the second right behind
#                     it (delivered, after the reply and the window)
#   after-eof         data after the peer's EOF is discarded; extended data
#                     after it ends the connection
#   oversize-packet   a 32 KB + 1 data packet is dropped uncounted
#   adjust-past-three-packets
#                     three maximum packets (96 KB) below the window draws
#                     no adjust; one byte more draws ONE, for exactly what
#                     was consumed (OpenSSH's channel_check_window)
#   adjust-oom        the inline WINDOW_ADJUST cannot be built (its one
#                     allocation rigged to fail): the connection ends, as a
#                     failed send on that path always ended it, cleanly
#   window-violation  2 MB + 10% + 1 sent without reading a single adjust
#                     (see the scenario: the server's own adjusts cover it)
#   zero-window       a peer that grants no window: the flood is refused at
#                     the per-channel limit, nothing moves and nothing spins
#                     for 6 s; an adjust releases ONE writable and every byte
#   turn-rearm        a hook that runs the loop from a resume turn, with
#                     input received and held under it: the receive is
#                     re-armed afterwards, so later input is still read
#   close-immediate   the application closes a channel holding 258 KB the
#                     peer never let out: CLOSE, and none of the data
#   close-race        the application closes while the peer is still
#                     sending: everything that crossed our CLOSE is ignored,
#                     and a second channel on the connection works
#   unknown-recipient WINDOW_ADJUST to a channel never opened is ignored;
#                     DATA to one ends the connection
#   subsys-accept     subsystem "cat": SUCCESS, the window, data copied
#                     back, then exit-status, EOF, CLOSE at the peer's EOF
#   subsys-refuse     a name the hook refuses draws 100 and starts nothing;
#                     "cat" with ONE trailing NUL then starts it (len 3)
#   subsys-after-start
#                     after a shell starts, subsystem is refused UNPARSED --
#                     a malformed one too -- and the hook never runs
#   subsys-malformed  before the start, a NUL inside the name, or bytes
#                     after it, ends the connection; the hook never runs
#   subsys-no-hook    a server with NO subsystem hook (the fixture's second
#                     server): refused unparsed, as before the hook existed,
#                     and the channel then starts a shell
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-channel-hostile-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh channel hostile"
HOSTILE_SCENARIOS="exit-order open-hook second-shell refused-requests resize before-start pipelined-exec after-eof oversize-packet adjust-past-three-packets adjust-oom window-violation zero-window turn-rearm close-immediate close-race unknown-recipient subsys-accept subsys-refuse subsys-after-start subsys-malformed subsys-no-hook"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). Measured on aa64 (TCG), standalone, 2026-10-09:
# every scenario took 1-2 s but window-violation (4) and zero-window (about
# 9: its 2 s idle sample and 6 s hold) -- about 22 s in all. Each budget is
# about three times its measurement, so a loaded CI box has room. open-hook,
# added after, is four logins of ~1-2 s each and gets 12.
declare -A HOSTILE_BUDGET=(
    [exit-order]=4 [open-hook]=12 [second-shell]=4 [refused-requests]=4 [resize]=4
    [before-start]=4 [pipelined-exec]=4 [after-eof]=4 [oversize-packet]=5
    [adjust-past-three-packets]=5 [adjust-oom]=5 [window-violation]=8 [zero-window]=17
    [turn-rearm]=20
    [close-immediate]=4 [close-race]=4 [unknown-recipient]=5
    [subsys-accept]=4 [subsys-refuse]=4 [subsys-after-start]=4
    [subsys-malformed]=8 [subsys-no-hook]=4
)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
# The second server (guest 2223, host PORT_B) has the fixture's channel hooks
# WITHOUT the subsystem hook: subsys-no-hook's server.
PORT_B=$(test_port 1)
HOSTILE_EXTRA_ARGS="2223 ${GRACE_SEC} channels=1 channels2=1 sshloglevel=debug"
HOSTILE_EXTRA_FWD=("$PORT_B" 2223)

hostile_boot "AxlSsh channel flow control and hostile peers"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"

# --- helpers ------------------------------------------------------------------

# How many guest lines after mark $1 are the server's DEBUG disconnect line
# with reason ERE $2. A DEBUG line carries the console's timestamp and level
# in front, so it is anchored at its END and at the `ssh:func:line:` tag,
# not at the start of the line.
since_disc() {
    local n
    n=$(since "$1" | grep -cE -- "ssh:[a-z_]+:[0-9]+: disconnect: $2\$" || true)
    echo "${n:-0}"
}
wait_disc() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        if [[ "$(since_disc "$1" "$2")" -ge 1 ]]; then return 0; fi
        sleep 0.2
    done
    return 1
}

# N copies of a script action, comma-joined.
rep() { local i out=""; for ((i = 0; i < $1; i++)); do out+="${out:+,}$2"; done; echo "$out"; }

# The transcript line of an ON_ARRIVAL steady-state adjust (128 KB, every
# fourth 32 KB packet), repeated: a transcript takes "${ADJ128[@]:0:N}".
ADJ128=()
for ((i = 0; i < 32; i++)); do ADJ128+=('peer: got 93 bytes=131072'); done

# sha256 of N bytes of 'x', optionally after a prefix -- what `sink` and
# `expectsum` report for a `pour`.
xsum() { nsum x "$@"; }

# The `sink` exec's report for N bytes of 'x', as `expectdata` prints it,
# and its length on the wire.
sink_line() { printf '%s\n' "peer: data out \"\\nsink $1 $(xsum "$1")\\n\""; }
sink_len() { local l; l=$'\n'"sink $1 $(xsum "$1")"$'\n'; echo "${#l}"; }

# The QEMU process's CPU over $1 seconds, in milli-cores, from
# /proc/<pid>/stat utime+stime sampled at 10 Hz (a since-start average
# would hide the window). Prints "avg max", max being the busiest 100 ms.
cpu_mcores() {
    local secs="$1" tck t0 j0 tp jp t j d dt avg max=0 i qpid
    # No guest pid is a FAILED sample, not an idle one: "none" fails the
    # numeric check that reads it instead of passing as 0.
    qpid=$(test_guest_qemu_pid) || { echo "none none"; return 0; }
    tck=$(getconf CLK_TCK)
    read_j() { sed 's/.*) //' "/proc/$qpid/stat" | awk '{print $12 + $13}'; }
    t0=$(date +%s%N); j0=$(read_j); tp=$t0; jp=$j0
    for ((i = 0; i < secs * 10; i++)); do
        sleep 0.1
        t=$(date +%s%N); j=$(read_j)
        dt=$(( (t - tp) / 1000000 )); d=$(( j - jp ))
        if [[ "$dt" -gt 0 ]]; then
            local mc=$(( d * 1000 * 1000 / (tck * dt) ))
            if [[ "$mc" -gt "$max" ]]; then max=$mc; fi
        fi
        tp=$t; jp=$j
    done
    avg=$(( (j - j0) * 1000 * 1000 / (tck * ((t - t0) / 1000000)) ))
    echo "$avg $max"
}

# --- exit-order ----------------------------------------------------------------
#
# The reply to exec, the 2 MB window behind it, then what the exec hook wrote
# INSIDE itself, then exit-status, EOF and CLOSE: the order sshd produces
# and the header promises. ssh(1) cannot see this (it accepts data before
# the reply); only a transcript can.
if wants exit-order; then
    m=$(mark)
    peer exit-order "send:90:$OPEN,expect:91,send:98:$(exec_req 'echo hi'),expect:99,expect:93,expectdata:3,expect:98,expect:96,expect:97,send:97:$RCPT"
    check "exit-order: 99, 93 (2 MB), the data, exit-status 0, EOF, CLOSE -- in order" \
        transcript exit-order 0 "$OPENED" "$(got91 exit-order)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: data out "hi\n"' \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=4'
    check "exit-order: the confirmation carries window 0 and max packet 32768" \
        test -n "$(got91 exit-order)"
    settle "$m" 1 exit-order
    check "exit-order: one close hook" \
        test "$(since_re "$m" 'SERVER: CHANNEL-CLOSE ud=[0-9]+')" = 1
fi

# --- open-hook -------------------------------------------------------------------
#
# The open hook acting on its channel from INSIDE itself, keyed on the user
# (the fixture's "openwrite", "openclose", "opencloserefuse", "openexit";
# the header's open hook says what each must produce):
#   openwrite        a write leaves AFTER the OPEN_CONFIRMATION
#   openclose        close, then AXL_OK: the CONFIRMATION, then CLOSE; the
#                    close hook follows the peer's CLOSE, once
#   opencloserefuse  close, then a refusal: the open failure alone, reason 1
#                    (ADMINISTRATIVELY_PROHIBITED) -- no CLOSE -- and NO
#                    close hook
#   openexit         exit 0: the CONFIRMATION, exit-status 0, EOF, CLOSE
# A message out of order is `peer: unexpected T` (exit 3), so each transcript
# pins the order whole. On the guest, each connection's PRINCIPAL-RELEASED
# is waited for first: release_principal runs after every close hook of the
# connection, so a close hook missing after it is absent, not late. Then the
# connection's channel lines are compared whole, in order.
#
# The window opens only once every EARLIER login has released its principal.
# A release line carries no connection: a scenario that returned before its
# release (it waited for a close hook, as settle() once did) leaves that
# release to land after the next mark (measured, 5 of 8 runs at load 15-19),
# the window then starts with ANOTHER connection's PRINCIPAL-RELEASED, and
# the wait below is satisfied by it. Every scenario now ends in settle(),
# which waits for the release; this stays as the guard that makes a scenario
# without one fail HERE, by name. Quiet first, then mark, so every line in
# the window is this login's own.
if wants open-hook; then
    # The channel-lifecycle lines after mark $1, the varying parts cut.
    lifecycle() {
        since "$1" | grep -xE 'SERVER: (CHANNEL-OPEN .*|OPEN-[A-Z-]+ -?[0-9]+|CHANNEL-CLOSE ud=[0-9]+|PRINCIPAL-RELEASED)' \
            | sed -E 's/^SERVER: CHANNEL-OPEN .*/SERVER: CHANNEL-OPEN/; s/ud=[0-9]+$/ud=N/' || true
    }
    lifecycle_is() { local m="$1"; shift; [[ "$(lifecycle "$m")" == "$(printf '%s\n' "$@")" ]]; }
    # Every login accepted so far has released its principal. Counting rests
    # on ONE `PRINCIPAL-RELEASED` per `Accepted`: the server releases a
    # non-NULL principal exactly once (axl-ssh-server.h), and the fixture's
    # on_release prints that exact line only for g_principal_tag -- which
    # every login this suite makes adopts. A login adopting the iterate
    # principal (ITERATE-PRINCIPAL-RELEASED, which -x does not match) or NULL
    # (no release at all) would leave this waiting out its 10 s.
    all_released() {
        local acc rel
        acc=$(since 0 | grep -c 'ssh: Accepted ' || true)
        rel=$(since 0 | grep -cx 'SERVER: PRINCIPAL-RELEASED' || true)
        [[ "${rel:-0}" -ge "${acc:-0}" ]]
    }
    wait_all_released() {
        local i
        for ((i = 0; i < 50; i++)); do
            if all_released; then return 0; fi
            sleep 0.2
        done
        return 1
    }
    # oh NAME USER PW SCRIPT -- one login as USER; sets OH_M to its mark.
    oh() {
        check "open-hook $1: every earlier login released its principal first" \
            wait_all_released
        OH_M=$(mark)
        PEER_USER="$2" PEER_PW="$3" peer "$1" "$4"
        check "open-hook $1: the connection's release_principal ran" \
            wait_since "$OH_M" 1 10 'SERVER: PRINCIPAL-RELEASED'
    }

    oh open-write openwrite "$PW_OPENWRITE" \
        "send:90:$OPEN,expect:91,expectdata:7,send:97:$RCPT,expect:97"
    check "open-hook openwrite: OPEN_CONFIRMATION, THEN the data written inside the hook" \
        transcript open-write 0 "$OPENED" "$(got91 open-write)" \
            'peer: data out "opened\n"' 'peer: sent 97 seq=3' 'peer: got 97'
    check "open-hook openwrite: write 0, one close hook, before release_principal" \
        lifecycle_is "$OH_M" 'SERVER: CHANNEL-OPEN' 'SERVER: OPEN-WRITE 0' \
            'SERVER: CHANNEL-CLOSE ud=N' 'SERVER: PRINCIPAL-RELEASED'

    oh open-close openclose "$PW_OPENCLOSE" \
        "send:90:$OPEN,expect:91,expect:97,send:97:$RCPT,$KEEP"
    check "open-hook openclose: OPEN_CONFIRMATION, then CLOSE" \
        transcript open-close 0 "$OPENED" "$(got91 open-close)" \
            'peer: got 97' 'peer: sent 97 seq=3' 'peer: sent 80 seq=4' 'peer: got 82'
    check "open-hook openclose: close 0, then ONE close hook -- after the open hook, before release_principal" \
        lifecycle_is "$OH_M" 'SERVER: CHANNEL-OPEN' 'SERVER: OPEN-CLOSE 0' \
            'SERVER: CHANNEL-CLOSE ud=N' 'SERVER: PRINCIPAL-RELEASED'

    oh open-close-refuse opencloserefuse "$PW_OPENCLOSEREFUSE" \
        "send:90:$OPEN,expect:92,$KEEP"
    check "open-hook opencloserefuse: the open failure alone, ADMINISTRATIVELY_PROHIBITED (1), no CLOSE" \
        transcript open-close-refuse 0 "$OPENED" 'peer: got 92 reason=1' \
            'peer: sent 80 seq=3' 'peer: got 82'
    check "open-hook opencloserefuse: close 0, and NO close hook for the refused open" \
        lifecycle_is "$OH_M" 'SERVER: OPEN-CLOSE-REFUSE 0' 'SERVER: PRINCIPAL-RELEASED'

    oh open-exit openexit "$PW_OPENEXIT" \
        "send:90:$OPEN,expect:91,expect:98,expect:96,expect:97,send:97:$RCPT,$KEEP"
    check "open-hook openexit: OPEN_CONFIRMATION, exit-status 0, EOF, CLOSE -- in order" \
        transcript open-exit 0 "$OPENED" "$(got91 open-exit)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=3' \
            'peer: sent 80 seq=4' 'peer: got 82'
    check "open-hook openexit: exit 0, one close hook, before release_principal" \
        lifecycle_is "$OH_M" 'SERVER: CHANNEL-OPEN' 'SERVER: OPEN-EXIT 0' \
            'SERVER: CHANNEL-CLOSE ud=N' 'SERVER: PRINCIPAL-RELEASED'
fi

# --- second-shell ---------------------------------------------------------------
#
# One shell or exec per channel (RFC 4254 6.5), and pty-req only before the
# start: after it, all three are refused UNPARSED -- the exec's command is
# not even read -- and the session goes on answering.
if wants second-shell; then
    peer second-shell "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:98:$SHELL_REQ,expect:100,send:98:$(exec_req true),expect:100,send:98:$(hexof chan-req-hex --recipient @ --name pty-req --want 1 --rest "$(str xterm)$(u32 80)$(u32 24)$(u32 0)$(u32 0)$(str '')"),expect:100,send:94:$(data $'echo x\r'),expectdata:11"
    check "second-shell: a second shell, an exec and a pty-req are refused; echo answers" \
        transcript second-shell 0 "$OPENED" "$(got91 second-shell)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 98 seq=4' 'peer: got 100' \
            'peer: sent 98 seq=5' 'peer: got 100' \
            'peer: sent 98 seq=6' 'peer: got 100' \
            'peer: sent 94 seq=7' 'peer: data out "echo x\r\nx\r\n"'
fi

# --- refused-requests -------------------------------------------------------------
#
# Requests the server does not offer, each well-formed as its RFC section
# shapes it, each asking for a reply: CHANNEL_FAILURE, and the channel lives.
if wants refused-requests; then
    req() { hexof chan-req-hex --recipient @ --name "$1" --want 1 --rest "${2:-}"; }
    rr_script="send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93"
    rr_script+=",send:98:$(req env "$(str LANG)$(str C)"),expect:100"
    # subsystem reaches the fixture's hook, which serves only "cat".
    rr_script+=",send:98:$(req subsystem "$(str sftp)"),expect:100"
    rr_script+=",send:98:$(req x11-req "00$(str MIT-MAGIC-COOKIE-1)$(str 00112233445566778899aabbccddeeff)$(u32 0)"),expect:100"
    rr_script+=",send:98:$(req auth-agent-req@openssh.com),expect:100"
    rr_script+=",send:98:$(req signal "$(str INT)"),expect:100"
    rr_script+=",send:98:$(req break "$(u32 500)"),expect:100"
    rr_script+=",send:94:$(data $'echo ok\r'),expectdata:13"
    peer refused-requests "$rr_script"
    check "refused-requests: env, subsystem, x11-req, auth-agent, signal, break each draw 100; echo answers" \
        transcript refused-requests 0 "$OPENED" "$(got91 refused-requests)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 98 seq=4' 'peer: got 100' 'peer: sent 98 seq=5' 'peer: got 100' \
            'peer: sent 98 seq=6' 'peer: got 100' 'peer: sent 98 seq=7' 'peer: got 100' \
            'peer: sent 98 seq=8' 'peer: got 100' 'peer: sent 98 seq=9' 'peer: got 100' \
            'peer: sent 94 seq=10' 'peer: data out "echo ok\r\nok\r\n"'
fi

# --- resize --------------------------------------------------------------------
#
# RFC 4254 6.7 sends window-change with want_reply FALSE: the hook runs and
# nothing is answered -- the keepalive's 82 is the next message. A peer that
# asks anyway is answered SUCCESS, as sshd answers it. On a channel with no
# pty the request is answered and reaches no hook.
if wants resize; then
    m=$(mark)
    winch() { hexof chan-req-hex --recipient @ --name window-change --want "$3" --rest "$(u32 "$1")$(u32 "$2")$(u32 0)$(u32 0)"; }
    PTY_REQ=$(hexof chan-req-hex --recipient @ --name pty-req --want 1 --rest "$(str xterm)$(u32 80)$(u32 24)$(u32 0)$(u32 0)$(str '')")
    peer resize "send:90:$OPEN,expect:91,send:98:$PTY_REQ,expect:99,send:98:$(winch 100 40 0),$KEEP,send:98:$(winch 120 50 1),expect:99,send:90:$OPEN1,expect:91,send:98:$(winch 90 30 1),expect:99"
    rs91=$(grep -xE "$GOT91" "$TEST_TMPDIR/ssh-resize.log" || true)
    check "resize: the pty, a reply-less window-change, one asking a reply, and one with no pty" \
        transcript resize 0 "$OPENED" "$(echo "$rs91" | sed -n 1p)" \
            'peer: sent 98 seq=3' 'peer: got 99' \
            'peer: sent 98 seq=4' 'peer: sent 80 seq=5' 'peer: got 82' \
            'peer: sent 98 seq=6' 'peer: got 99' \
            'peer: sent 90 seq=7' "$(echo "$rs91" | sed -n 2p)" \
            'peer: sent 98 seq=8' 'peer: got 99'
    settle "$m" 1 resize
    check "resize: the guest saw the pty (xterm 80x24) and both resizes on it" \
        test "$(since_lines "$m" 'SERVER: PTY xterm 80x24')" = 1 \
            -a "$(since_lines "$m" 'SERVER: RESIZE 100x40')" = 1 \
            -a "$(since_lines "$m" 'SERVER: RESIZE 120x50')" = 1
    check "resize: ...and no resize hook for the channel without a pty" \
        test "$(since_re "$m" 'SERVER: RESIZE .*')" = 2
fi

# --- before-start ------------------------------------------------------------------
#
# Inside the window-0 gap between the confirmation and the shell: 7 x 32 KB
# (229,376 bytes, past the 10% grace of 209,715) and a command line. sshd
# ignores data on a channel that has not started and does not count it, so
# none of it reaches the data hook, none of it is fatal, and the shell then
# answers only what is sent after it.
if wants before-start; then
    m=$(mark)
    peer before-start "send:90:$OPEN,expect:91,pour:229376:32768,send:94:$(data $'echo early\r'),send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'echo ok\r'),expectdata:13"
    check "before-start: 224 KB before the start is not fatal; the session answers only later input" \
        transcript before-start 0 "$OPENED" "$(got91 before-start)" \
            'peer: poured 229376 in 7 packets seqs=3..9' 'peer: sent 94 seq=10' \
            'peer: sent 98 seq=11' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=12' 'peer: data out "echo ok\r\nok\r\n"'
    settle "$m" 1 before-start
    check "before-start: the data hook never saw the early data" \
        test "$(since_re "$m" 'SERVER: DATA-BEFORE-START [0-9]+')" = 0
fi

# --- pipelined-exec ------------------------------------------------------------------
#
# Data around the start, and replies before output. One write carries
# data "z", exec "cat" and data "y":
# "z" is BEFORE the start (discarded), "y" is right BEHIND it -- the header
# delivers that after the exec hook returned, as sshd counts it against the
# window it has just set. So the peer reads the reply, the window, then "y"
# (output never leaves ahead of the reply), then, for a later write of
# "a","b","c", "abc"; and "z" never.
if wants pipelined-exec; then
    m=$(mark)
    peer pipelined-exec "send:90:$OPEN,expect:91,batch:94:$(data z)+98:$(exec_req cat)+94:$(data y),expect:99,expect:93,expectdata:1,batch:94:$(data a)+94:$(data b)+94:$(data c),expectdata:3,send:96:$RCPT,expect:98,expect:96,expect:97,send:97:$RCPT"
    check "pipelined-exec: the reply, the window, \"y\", \"abc\" -- never \"z\" -- then cat's exit" \
        transcript pipelined-exec 0 "$OPENED" "$(got91 pipelined-exec)" \
            'peer: sent batch 94+98+94 seqs=3..5' 'peer: got 99' \
            'peer: got 93 bytes=2097152' 'peer: data out "y"' \
            'peer: sent batch 94+94+94 seqs=6..8' 'peer: data out "abc"' \
            'peer: sent 96 seq=9' \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=10'
    settle "$m" 1 pipelined-exec
    check "pipelined-exec: the data hook never saw the pre-start \"z\"" \
        test "$(since_re "$m" 'SERVER: DATA-BEFORE-START [0-9]+')" = 0
fi

# --- after-eof ------------------------------------------------------------------
#
# THE CONTROL first: extended data BEFORE the EOF is counted and discarded
# (no client sends stdin as stderr) and the connection stays up -- so what
# ends it later is the EOF rule, not extended data as such. After the
# peer's EOF, plain data is counted and discarded (the shell would echo it:
# the two keepalives come back with nothing ahead of them -- the second one
# after the loop has run any flush the first could have raced) and
# extended data ends the connection, both as sshd does.
if wants after-eof; then
    m=$(mark)
    AE_EXT=$(hexof chan-data-hex --recipient @ --text x --stream err)
    peer after-eof "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:95:$AE_EXT,$KEEP,send:94:$(data $'echo a\r'),expectdata:11,send:96:$RCPT,send:94:$(data $'echo b\r'),$KEEP,$KEEP,send:95:$AE_EXT,expect:82"
    check "after-eof: extended data before EOF is harmless; after EOF data draws nothing and extended data ends it" \
        transcript after-eof 4 "$OPENED" "$(got91 after-eof)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 95 seq=4' 'peer: sent 80 seq=5' 'peer: got 82' \
            'peer: sent 94 seq=6' 'peer: data out "echo a\r\na\r\n"' \
            'peer: sent 96 seq=7' 'peer: sent 94 seq=8' \
            'peer: sent 80 seq=9' 'peer: got 82' 'peer: sent 80 seq=10' 'peer: got 82' \
            'peer: sent 95 seq=11' 'peer: closed'
    wait_disc "$m" 'channel: extended data past the window or EOF' 10 || true
    settle "$m" 1 after-eof
    check "after-eof: the eof hook ran once, and no data hook after it" \
        test "$(since_lines "$m" 'SERVER: EOF')" = 1 \
            -a "$(since_re "$m" 'SERVER: DATA-AFTER-EOF [0-9]+')" = 0
    check "after-eof: the guest ended it for the extended data" \
        test "$(since_disc "$m" 'channel: extended data past the window or EOF')" = 1
fi

# --- oversize-packet --------------------------------------------------------------
#
# One data packet of 32 KB + 1 -- past the 32 KB maximum packet the server
# confirmed -- is dropped, as sshd drops it: not delivered (the sink counts
# exactly what follows) and not counted: the adjusts come every fourth 32 KB
# packet of what follows, for 128 KB each -- eight of them, and nothing for
# the last byte. Counted, the dropped packet would have drawn the first one
# a packet early, for 128 KB + 1.
if wants oversize-packet; then
    peer oversize-packet "send:90:$OPEN,expect:91,send:98:$(exec_req sink),expect:99,expect:93,pour:32769:32769,pour:1048577:32768,$(rep 8 expect:93),send:96:$RCPT,expectdata:$(sink_len 1048577),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "oversize-packet: dropped uncounted and undelivered; the channel carries on" \
        transcript oversize-packet 0 "$OPENED" "$(got91 oversize-packet)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 32769 in 1 packets seqs=4..4' \
            'peer: poured 1048577 in 33 packets seqs=5..37' \
            "${ADJ128[@]:0:8}" 'peer: sent 96 seq=38' \
            "$(sink_line 1048577)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=39'
fi

# --- adjust-past-three-packets ----------------------------------------------------
#
# The window is re-opened once it has fallen MORE than three maximum packets
# below its size (OpenSSH's channel_check_window; its other clause, below
# half, is implied at these sizes): exactly 98,304 bytes draws nothing (the
# keepalive's 82 is next), one byte more draws one adjust for exactly
# 98,305 -- what was consumed -- and nothing after it.
if wants adjust-past-three-packets; then
    peer adjust-past-three-packets "send:90:$OPEN,expect:91,send:98:$(exec_req sink),expect:99,expect:93,pour:98304:32768,$KEEP,pour:1:32768,expect:93,$KEEP,send:96:$RCPT,expectdata:$(sink_len 98305),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "adjust-past-three-packets: none at exactly 96 KB, one for exactly 96 KB + 1 past it" \
        transcript adjust-past-three-packets 0 "$OPENED" "$(got91 adjust-past-three-packets)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 98304 in 3 packets seqs=4..6' \
            'peer: sent 80 seq=7' 'peer: got 82' \
            'peer: poured 1 in 1 packets seqs=8..8' 'peer: got 93 bytes=98305' \
            'peer: sent 80 seq=9' 'peer: got 82' 'peer: sent 96 seq=10' \
            "$(sink_line 98305)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=11'
fi

# --- adjust-oom -----------------------------------------------------------------------
#
# The ON_ARRIVAL adjust is built and sent inline as the packet that makes it
# due is parsed. 98,303 bytes go first and are parsed whole (no adjust: not
# past three maximum packets). Then ONE write carries two 1-byte packets, so
# both land in one receive: the first takes the window to exactly 98,304 below
# its size (still no adjust), and the fixture's `adjoom 98304` -- inside that
# packet's data hook -- rigs the second allocation from then on to fail.
# The second packet, parsed straight after with nothing received in between,
# takes one allocation to decrypt and then makes an adjust of 98,305 due
# whose message cannot be built. That ends the connection, as a failed adjust
# send on this path always did: the peer reads no adjust, only the close.
# The allocator's own DEBUG line names the failed allocation (an AxlString's,
# not the decryption's) and the server's line says the adjust was not sent,
# so the failure is the one under test; and the guest runs the close hook and
# releases the principal with nothing leaked (the teardown checks). The
# control is adjust-past-three-packets: the same bytes, no failure, one
# adjust of 98,305.
if wants adjust-oom; then
    m=$(mark)
    peer adjust-oom "send:90:$OPEN,expect:91,send:98:$(exec_req 'adjoom 98304'),expect:99,expect:93,pour:98303:32768,hold:1,pour:2:1,expect:93"
    check "adjust-oom: the adjust that cannot be built ends the connection, no adjust sent" \
        transcript adjust-oom 4 "$OPENED" "$(got91 adjust-oom)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 98303 in 3 packets seqs=4..6' \
            'peer: poured 2 in 2 packets seqs=7..8' 'peer: closed'
    settle "$m" 1 adjust-oom
    ao_line=$(since "$m" | grep -cE 'ssh:chan_on_data:[0-9]+: chan [0-9]+: WINDOW_ADJUST of 98305 not sent; failing the connection$' || true)
    ao_oom=$(since "$m" | grep -cE 'mem:[a-z_]+:[0-9]+: axl_malloc\([0-9]+\) injected OOM at src/data/axl-string\.c:[0-9]+$' || true)
    check "adjust-oom: the rigged allocation was the adjust's -- an AxlString's, and the adjust's own failure line, once each" \
        test "${ao_line:-0}" = 1 -a "${ao_oom:-0}" = 1
    check "adjust-oom: armed once, the close hook ran once, the connection failed as a protocol error" \
        test "$(since_lines "$m" 'SERVER: ADJOOM armed')" = 1 \
            -a "$(since_re "$m" 'SERVER: CHANNEL-CLOSE ud=[0-9]+')" = 1 \
            -a "$(since_disc "$m" 'protocol error')" = 1
fi

# --- window-violation ---------------------------------------------------------------
#
# "2 MB + 10% + 1 sent without waiting for an adjust ends the connection"
# CANNOT HAPPEN against this server, and this pins why. On an ON_ARRIVAL
# channel (every channel the fixture's `sink` opens) the server re-opens the
# window as data arrives -- every fourth 32 KB packet, without waiting for
# the application -- so what the peer may send always stays nearly 2 MB
# ahead of what it has sent. A peer that
# never reads an adjust is still inside the window the server has GRANTED,
# and sshd judges the same way (its window grows as its child consumes).
# The grace check behind "data more than 10% past the window" stays -- it is
# what a server whose adjust lagged would need -- and the unit test pins it
# in the pure table (AxlTestSsh "chan in: one byte beyond the grace is
# AXL_ERR"), which a sabotage removing the check fails -- and this suite
# does not, as it cannot.
#
# So: 2,306,868 bytes in 32 KB packets, blind. Every byte arrives (the sink's
# count and SHA-256), the connection stays up, and the server granted
# exactly seventeen adjusts of 131,072 (one every fourth packet, through
# packet 68) -- and none for the last three, 78,644 bytes.
if wants window-violation; then
    WV=2306868
    peer window-violation "send:90:$OPEN,expect:91,send:98:$(exec_req sink),expect:99,expect:93,pour:$WV:32768,$(rep 17 expect:93),$KEEP,send:96:$RCPT,expectdata:$(sink_len $WV),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "window-violation: 2 MB + 10% + 1 blind is inside the granted window: all of it, seventeen adjusts" \
        transcript window-violation 0 "$OPENED" "$(got91 window-violation)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            "peer: poured $WV in 71 packets seqs=4..74" \
            "${ADJ128[@]:0:17}" \
            'peer: sent 80 seq=75' 'peer: got 82' 'peer: sent 96 seq=76' \
            "$(sink_line $WV)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=77'
fi

# --- zero-window ------------------------------------------------------------------
#
# A reader that reads nothing: the peer opens with window 0 (and
# a 16 KB maximum packet, so a server that ignored it is caught by the
# peer's own check). `flood 300000` queues 4 KB writes until the per-channel
# limit refuses one: after the 14-byte echo of the command line, 63 writes
# (258,048 bytes) fit under 262,144 and the 64th does not. For 6 s NOTHING
# may move -- no data (the peer's window check), no writable (the limit
# has not eased) and no busy loop (the guest's CPU, sampled at 10 Hz from
# /proc for 2 s while the peer holds 6 s). Then a 1 MB adjust: every byte, in order
# (SHA-256 of the echo and 300,000 'x'), and the writable fires ONCE.
if wants zero-window; then
    base=$(cpu_mcores 2)
    m=$(mark)
    (peer zero-window "send:90:$OPEN_Z,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'flood 300000\r'),hold:6,send:93:$(hexof chan-adjust-hex --recipient @ --bytes 1048576),expectsum:300014,send:97:$RCPT,expect:97"; exit "$PEER_RC") &
    zw_pid=$!
    BG_PIDS+=("$zw_pid")
    zw_busy=0
    if wait_since "$m" 1 15 'SERVER: BUSY [0-9]+'; then zw_busy=1; fi
    sleep 0.2
    zw_writable_pre=$(since_lines "$m" 'SERVER: WRITABLE')
    hold=$(cpu_mcores 2)
    zw_writable_post=$(since_lines "$m" 'SERVER: WRITABLE')
    # The window the two samples bracket must END before the peer's adjust
    # leaves (its hold is 6 s, the samples ~3 s in): the adjust's line not
    # yet in the transcript proves it, rather than a timing argument.
    zw_adjust_sent=$(grep -c 'peer: sent 93' "$TEST_TMPDIR/ssh-zero-window.log" || true)
    zw_rc=0; wait "$zw_pid" || zw_rc=$?
    PEER_RC=$zw_rc
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-zero-window.log")
    settle "$m" 1 zero-window
    echo "  zero-window: guest QEMU CPU, milli-cores avg/max-100ms: idle before ${base// //}, during the hold ${hold// //}"
    check "zero-window PRECONDITION: the flood was refused while the window was shut" \
        test "$zw_busy" = 1
    check "zero-window: refused at exactly 258048 bytes (the per-channel limit), once" \
        test "$(since_re "$m" 'SERVER: BUSY [0-9]+')" = 1 \
            -a "$(since_lines "$m" 'SERVER: BUSY 258048')" = 1
    check "zero-window PRECONDITION: the samples ended before the peer's adjust was sent" \
        test "${zw_adjust_sent:-0}" = 0
    check "zero-window: no writable while the peer read nothing (before and after the CPU sample)" \
        test "$zw_writable_pre" = 0 -a "$zw_writable_post" = 0
    check "zero-window: after a 1 MB adjust, every byte in order, within the window and 16 KB packets" \
        transcript zero-window 0 'peer: sent 90 seq=2' \
            "$(grep -xE 'peer: got 91 sender=[0-9]+ window=0 maxpkt=32768' "$TEST_TMPDIR/ssh-zero-window.log" | head -1 || true)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=4' 'peer: sent 93 seq=5' \
            "peer: sum out=300014 err=0 sha256=$(xsum 300000 $'flood 300000\r\n')" \
            'peer: sent 97 seq=6' 'peer: got 97'
    check "zero-window: the writable fired exactly once, and the flood finished" \
        test "$(since_lines "$m" 'SERVER: WRITABLE')" = 1 \
            -a "$(since_lines "$m" 'SERVER: FLOOD-DONE 300000')" = 1
    # An idle AXL guest measured 9-56 milli-cores on BOTH arches here --
    # under TCG too: an idle aa64 guest waits in WFI, and TCG pegs a core
    # only while it emulates work. A server that polls its flush from an
    # idle source while output is queued, window or not, measured 997 (x64)
    # and 996 (aa64) and fails this check (sabotage, 2026-10-09). A flush
    # that merely re-defers itself does NOT spin -- the loop drains deferred
    # work once per 10 ms poll tick -- and stays at idle level.
    check "zero-window: the guest idled during the hold (${hold%% *} milli-cores < 500)" \
        test "${hold%% *}" -lt 500
fi

# --- turn-rearm ---------------------------------------------------------------------
#
# Input the parse gate held is handed back on a resume TURN: the receive is
# re-armed, then the held bytes are parsed. When those bytes run a hook that
# dispatches the loop, the receive the turn just re-armed can complete
# INSIDE that hook and be held -- parked -- again. Leaving, the hook queues
# the connection for another turn; but the parse still on the stack beneath
# it goes on and parses the newly held bytes itself, and that consumes the
# queued turn. Something must still re-arm the receive afterwards, or the
# connection never reads again.
#
# Ordered by the fixture, not by timing: "holdflood" and "holdpause" run the
# loop until the gate has held the peer's next packet (SERVER: HELD).
#   1. The shut window refuses the flood (BUSY); the peer's adjust releases
#      ONE writable hook, which runs the loop until...
#   2. ..."holdpause" arrives and is held. The hook leaves; the turn
#      re-arms the receive and parses "holdpause", whose data hook runs the
#      loop until...
#   3. ..."echo z" arrives -- received by the re-armed receive, under that
#      hook -- and is held. The hook leaves, and the parse beneath it reads
#      "echo z".
#   4. "exit 0", sent after: read only if the receive was re-armed. A server
#      that lost the wakeup never reads it, and the peer times out waiting.
if wants turn-rearm; then
    TR_SUM=$({ printf 'holdflood 300000\r\n'; head -c 300000 /dev/zero | tr '\0' x
               printf 'holdpause\r\necho z\r\nz\r\nexit 0\r\n'; } | sha256sum | awk '{print $1}')
    m=$(mark)
    peer turn-rearm "send:90:$OPEN_Z,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'holdflood 300000\r'),hold:1,send:93:$(hexof chan-adjust-hex --recipient @ --bytes 1048576),idle:2,send:94:$(data $'holdpause\r'),idle:2,send:94:$(data $'echo z\r'),idle:2,send:94:$(data $'exit 0\r'),expectsum:300048,expect:98,expect:96,expect:97,send:97:$RCPT"
    settle "$m" 1 turn-rearm
    check "turn-rearm PRECONDITION: the flood was refused at the limit, and ONE writable hook finished it" \
        test "$(since_lines "$m" 'SERVER: BUSY 258048')" = 1 \
            -a "$(since_lines "$m" 'SERVER: WRITABLE')" = 1 \
            -a "$(since_lines "$m" 'SERVER: FLOOD-DONE 300000')" = 1
    check "turn-rearm PRECONDITION: both hooks saw the gate hold a packet received under them" \
        test "$(since_lines "$m" 'SERVER: HELD')" = 2 \
            -a "$(since_re "$m" 'SERVER: HOLD-WAIT-FAIL -?[0-9]+')" = 0
    check "turn-rearm: no hook of the channel ran inside another of its hooks" \
        test "$(since_lines "$m" 'SERVER: HOOK-NESTED')" = 0
    check "turn-rearm: input sent after the turn's hook is read -- every byte, exit 0, EOF, CLOSE" \
        transcript turn-rearm 0 'peer: sent 90 seq=2' "$(got91 turn-rearm)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=4' 'peer: sent 93 seq=5' 'peer: sent 94 seq=6' \
            'peer: sent 94 seq=7' 'peer: sent 94 seq=8' \
            "peer: sum out=300048 err=0 sha256=$TR_SUM" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=9'
fi

# --- close-immediate ----------------------------------------------------------------
#
# The same shut window and 258,048 queued bytes, then `quit`: the
# application's axl_ssh_channel_close. CLOSE is the next message, none of
# the queued data follows it -- not even once the peer opens a 1 MB window
# on the closed channel (two keepalives: the second after any flush the
# adjust could have scheduled) -- and the close hook runs once, when the
# peer's own CLOSE arrives.
if wants close-immediate; then
    m=$(mark)
    peer close-immediate "send:90:$OPEN_Z,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'flood 300000\r'),send:94:$(data $'quit\r'),expect:97,send:93:$(hexof chan-adjust-hex --recipient @ --bytes 1048576),$KEEP,$KEEP,send:97:$RCPT"
    check "close-immediate: CLOSE, and none of the queued data, even into an opened window" \
        transcript close-immediate 0 "$OPENED" \
            "$(grep -xE 'peer: got 91 sender=[0-9]+ window=0 maxpkt=32768' "$TEST_TMPDIR/ssh-close-immediate.log" | head -1 || true)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=4' 'peer: sent 94 seq=5' 'peer: got 97' \
            'peer: sent 93 seq=6' 'peer: sent 80 seq=7' 'peer: got 82' \
            'peer: sent 80 seq=8' 'peer: got 82' 'peer: sent 97 seq=9'
    settle "$m" 1 close-immediate
    check "close-immediate: the flood was holding 258048 bytes when the close came" \
        test "$(since_lines "$m" 'SERVER: BUSY 258048')" = 1 \
            -a "$(since_lines "$m" 'SERVER: QUIT 0')" = 1
    check "close-immediate: one close hook, no writable, the flood never finished" \
        test "$(since_re "$m" 'SERVER: CHANNEL-CLOSE ud=[0-9]+')" = 1 \
            -a "$(since_lines "$m" 'SERVER: WRITABLE')" = 0 \
            -a "$(since_re "$m" 'SERVER: FLOOD-DONE [0-9]+')" = 0
fi

# --- close-race --------------------------------------------------------------------
#
# RFC 4254 5.3 lets messages cross a CLOSE. In ONE write the peer sends
# `quit` -- the application closes from its data hook -- and behind it
# more data, a WINDOW_ADJUST, a request asking a reply, and EOF, all of which
# reach the server after its CLOSE. None is answered and none ends the
# connection (the CLOSE is the next message, then two keepalives). The
# peer's CLOSE completes it, and a second channel opened on the same
# connection works.
if wants close-race; then
    m=$(mark)
    cr_env=$(hexof chan-req-hex --recipient @ --name env --want 1 --rest "$(str LANG)$(str C)")
    peer close-race "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,batch:94:$(data $'quit\r')+94:$(data $'echo lost\r')+93:$(hexof chan-adjust-hex --recipient @ --bytes 4096)+98:$cr_env+96:$RCPT,expect:97,$KEEP,$KEEP,send:97:$RCPT,send:90:$OPEN1,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'echo again\r'),expectdata:19,send:97:$RCPT,expect:97"
    cr91=$(grep -xE "$GOT91" "$TEST_TMPDIR/ssh-close-race.log" || true)
    check "close-race: nothing that crossed the CLOSE is answered; a second channel works" \
        transcript close-race 0 "$OPENED" "$(echo "$cr91" | sed -n 1p)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent batch 94+94+93+98+96 seqs=4..8' 'peer: got 97' \
            'peer: sent 80 seq=9' 'peer: got 82' 'peer: sent 80 seq=10' 'peer: got 82' \
            'peer: sent 97 seq=11' 'peer: sent 90 seq=12' "$(echo "$cr91" | sed -n 2p)" \
            'peer: sent 98 seq=13' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=14' 'peer: data out "echo again\r\nagain\r\n"' \
            'peer: sent 97 seq=15' 'peer: got 97'
    settle "$m" 1 close-race
    check "close-race: two close hooks, one per channel; no data reached a closed one" \
        test "$(since_re "$m" 'SERVER: CHANNEL-CLOSE ud=[0-9]+')" = 2 \
            -a "$(since_re "$m" 'SERVER: DATA-(NO-STATE|AFTER-EOF) [0-9]+')" = 0 \
            -a "$(since_lines "$m" 'SERVER: QUIT 0')" = 1
    check "close-race: the guest disconnected nobody" \
        test "$(since_disc "$m" '.*')" = 0
fi

# --- unknown-recipient ---------------------------------------------------------------
#
# A channel number the server never confirmed: a WINDOW_ADJUST to it is
# ignored (sshd's "Received window adjust for non-open channel"), CHANNEL_DATA
# to it ends the connection (sshd's "referred to nonexistent channel"). And
# the peer's own control: an `@` body before any confirmation is refused by
# the peer, not sent.
if wants unknown-recipient; then
    m=$(mark)
    peer unknown-recipient "send:93:$(hexof chan-adjust-hex --recipient 999 --bytes 1),$KEEP,send:94:$(hexof chan-data-hex --recipient 999 --text x),expect:82"
    check "unknown-recipient: an adjust to channel 999 is ignored, data to it ends the connection" \
        transcript unknown-recipient 4 'peer: sent 93 seq=2' 'peer: sent 80 seq=3' \
            'peer: got 82' 'peer: sent 94 seq=4' 'peer: closed'
    wait_disc "$m" 'channel: nonexistent channel' 10 || true
    check "unknown-recipient: the guest ended it as a nonexistent channel" \
        test "$(since_disc "$m" 'channel: nonexistent channel')" = 1
    peer unknown-recipient-control "send:94:$(data x)"
    check "unknown-recipient CONTROL: the peer refuses an @ body with no channel confirmed (rc 3)" \
        transcript unknown-recipient-control 3 'peer: no channel confirmed'
fi

# --- subsystem ---------------------------------------------------------------------
#
# The subsystem hook (RFC 4254 6.5). The fixture's first server serves only
# "cat" (copies data back, exits 0 at EOF) and prints SERVER: SUBSYSTEM
# <name> <len> for every call; its second server has no subsystem hook.
subsys_req() { hexof chan-req-hex --recipient @ --name subsystem --want 1 --rest "$1"; }
# "ca\0t": a NUL inside the name.
SUB_NUL="$(u32 4)63610074"

if wants subsys-accept; then
    m=$(mark)
    peer subsys-accept "send:90:$OPEN,expect:91,send:98:$(subsys_req "$(str cat)"),expect:99,expect:93,send:94:$(data hello),expectdata:5,send:96:$RCPT,expect:98,expect:96,expect:97,send:97:$RCPT"
    check "subsys-accept: 99, 93 (2 MB), the data copied back, then exit-status 0, EOF, CLOSE" \
        transcript subsys-accept 0 "$OPENED" "$(got91 subsys-accept)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=4' 'peer: data out "hello"' 'peer: sent 96 seq=5' \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=6'
    settle "$m" 1 subsys-accept
    check "subsys-accept: the hook saw \"cat\", 3 bytes, once" \
        test "$(since_lines "$m" 'SERVER: SUBSYSTEM cat 3')" = 1 \
            -a "$(since_re "$m" 'SERVER: SUBSYSTEM .*')" = 1
fi

if wants subsys-refuse; then
    m=$(mark)
    peer subsys-refuse "send:90:$OPEN,expect:91,send:98:$(subsys_req "$(str nope)"),expect:100,send:98:$(subsys_req "$(u32 4)63617400"),expect:99,expect:93,send:94:$(data x),expectdata:1,send:97:$RCPT,expect:97"
    check "subsys-refuse: \"nope\" draws 100 and starts nothing; \"cat\\0\" then starts the channel" \
        transcript subsys-refuse 0 "$OPENED" "$(got91 subsys-refuse)" \
            'peer: sent 98 seq=3' 'peer: got 100' \
            'peer: sent 98 seq=4' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=5' 'peer: data out "x"' \
            'peer: sent 97 seq=6' 'peer: got 97'
    settle "$m" 1 subsys-refuse
    check "subsys-refuse: the hook saw \"nope\" (4) then \"cat\" (3, the trailing NUL dropped)" \
        test "$(since "$m" | grep -xE 'SERVER: SUBSYSTEM .*' | tr '\n' '|')" \
            = 'SERVER: SUBSYSTEM nope 4|SERVER: SUBSYSTEM cat 3|'
fi

if wants subsys-after-start; then
    m=$(mark)
    peer subsys-after-start "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:98:$(subsys_req "$(str cat)"),expect:100,send:98:$(subsys_req "$SUB_NUL"),expect:100,send:94:$(data $'echo ok\r'),expectdata:13"
    check "subsys-after-start: subsystem, well-formed or not, draws 100; echo answers" \
        transcript subsys-after-start 0 "$OPENED" "$(got91 subsys-after-start)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 98 seq=4' 'peer: got 100' 'peer: sent 98 seq=5' 'peer: got 100' \
            'peer: sent 94 seq=6' 'peer: data out "echo ok\r\nok\r\n"'
    settle "$m" 1 subsys-after-start
    check "subsys-after-start: the hook never ran" \
        test "$(since_re "$m" 'SERVER: SUBSYSTEM .*')" = 0
fi

if wants subsys-malformed; then
    m=$(mark)
    peer subsys-malformed-nul "send:90:$OPEN,expect:91,send:98:$(subsys_req "$SUB_NUL"),expect:99"
    check "subsys-malformed: a NUL inside the name ends the connection" \
        transcript subsys-malformed-nul 4 "$OPENED" "$(got91 subsys-malformed-nul)" \
            'peer: sent 98 seq=3' 'peer: closed'
    peer subsys-malformed-tail "send:90:$OPEN,expect:91,send:98:$(subsys_req "$(str cat)00"),expect:99"
    check "subsys-malformed: a byte after the name ends the connection" \
        transcript subsys-malformed-tail 4 "$OPENED" "$(got91 subsys-malformed-tail)" \
            'peer: sent 98 seq=3' 'peer: closed'
    settle "$m" 2 subsys-malformed
    check "subsys-malformed: the hook never ran" \
        test "$(since_re "$m" 'SERVER: SUBSYSTEM .*')" = 0
fi

if wants subsys-no-hook; then
    m=$(mark)
    HOST_PORT=$PORT_B peer subsys-no-hook "send:90:$OPEN,expect:91,send:98:$(subsys_req "$(str cat)"),expect:100,send:98:$(subsys_req "$SUB_NUL"),expect:100,send:98:$SHELL_REQ,expect:99,expect:93,send:94:$(data $'echo ok\r'),expectdata:13"
    check "subsys-no-hook: refused unparsed, malformed or not; the channel then starts a shell" \
        transcript subsys-no-hook 0 "$OPENED" "$(got91 subsys-no-hook)" \
            'peer: sent 98 seq=3' 'peer: got 100' 'peer: sent 98 seq=4' 'peer: got 100' \
            'peer: sent 98 seq=5' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 94 seq=6' 'peer: data out "echo ok\r\nok\r\n"'
    settle "$m" 1 subsys-no-hook
    check "subsys-no-hook: no hook ran" \
        test "$(since_re "$m" 'SERVER: SUBSYSTEM .*')" = 0
fi

# Whole boot: no data hook ran where the library promised it would not, and
# no hook of a channel nested inside another of its own.
if [[ "$ABUSE" == 1 ]]; then
    test_clean_log
    check "every scenario: no data before a start, after an EOF, or on a retired handle" \
        test "$(grep -cE '^SERVER: DATA-(BEFORE-START|AFTER-EOF|NO-STATE) ' "$TEST_CLEAN_LOG" || true)" = 0
    check "every scenario: no channel hook nested inside another of its channel's" \
        test "$(guest_lines 'SERVER: HOOK-NESTED')" = 0
fi

hostile_finish
