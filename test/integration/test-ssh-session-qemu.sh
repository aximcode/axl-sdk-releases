#!/bin/bash
# test-meta: arch=both needs=ssh est=60 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-session-qemu.sh -- AxlSsh session channels, driven by a real
# ssh(1): `ssh host cmd`, `ssh -tt host`, stdin through `cat`, several
# sessions on one connection through ControlMaster, and the ways a channel
# ends -- the client leaving, the application closing it from inside its own
# hook, and the server being freed from inside one.
#
# The guest runs ssh-server-selftest.efi with `channels=1` (the fixture shell:
# see that file's "THE FIXTURE SHELL") and `stale=1` (every close hook arms a
# timer that writes to the closed handle 200 ms later). The setup, helpers
# and teardown checks are lib/ssh-hostile-lib.sh's: the boot, the summed
# deadline, the heartbeat, one principal release per successful login, the
# heap-corruption verdict, the re-entry tripwire and the leak verdict.
#
# Scenarios, in run order (free-in-hook frees the server, so it is LAST):
#   accessors        the open hook's view of its channel, read back through
#                    every accessor: user, peer, principal, user data
#   stale            a write to a closed channel's handle from a timer is
#                    AXL_NOT_FOUND, and the heap stays clean
#   exec-echo        `ssh host 'echo hello'`, written INSIDE the exec hook
#   exec-status      exit codes 1, 3 (with stderr) and 127
#   exec-stdin       stdin through `cat`, EOF ends it
#   interactive      `ssh -tt`: a pty, the line shell, `exit 3`
#   quit             the application closes from INSIDE the data hook: the
#                    close hook runs once, after it
#   gate             the parse gate: input that arrives while a hook of its
#                    connection dispatches the loop is held, not parsed
#                    under that hook -- no hook nests in another of its
#                    channel, and every line arrives once, in order
#   backlog          300 KB of input while a hook runs the loop: the receive
#                    is parked, not buffered toward the input-backlog limit
#   forward          real `ssh -W` and `ssh -L`: a non-session open is
#                    refused with UNKNOWN_CHANNEL_TYPE and the session lives
#   open-shapes      an RFC-shaped x11 open is refused, the connection up;
#                    a `session` open with trailing bytes ends it (sshd)
#   close-oom        axl_ssh_channel_close whose CLOSE cannot be built: the
#                    close hook follows the call, never runs inside it
#   two-sessions     two connections at once, distinct handles
#   controlmaster    ten sessions on one connection, the eleventh refused
#                    with RESOURCE_SHORTAGE, the connection still up
#   no-close-for-refused
#                    an open the application refuses never gets a close hook
#   kill-client      a master with two sessions killed: two close hooks, both
#                    before the connection's release_principal
#   free-in-hook     axl_ssh_server_free from inside the data hook: the close
#                    hook runs after that hook returns, exactly once, and the
#                    stale handle is refused
#
# Every ssh(1) string pinned below was MEASURED from a real transcript
# (OpenSSH 9.9p1), not assumed. Transcripts: SSH_HOSTILE_KEEP=<dir>.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-session-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh session"
HOSTILE_SCENARIOS="accessors stale exec-echo exec-status exec-stdin interactive quit gate backlog forward open-shapes close-oom two-sessions controlmaster no-close-for-refused kill-client free-in-hook"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). Measured on the slower arch (aa64, TCG), plus the
# scenario's own fixed waits.
declare -A HOSTILE_BUDGET=(
    [accessors]=8 [stale]=6 [exec-echo]=5 [exec-status]=12 [exec-stdin]=5
    [interactive]=6 [quit]=6 [gate]=12 [backlog]=16 [forward]=12
    [open-shapes]=6 [close-oom]=6 [two-sessions]=10 [controlmaster]=24
    [no-close-for-refused]=6 [kill-client]=12 [free-in-hook]=8
)

if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host -- every scenario here is a real session"
    exit 77
fi

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
HOSTILE_EXTRA_ARGS="channels=1 stale=1 sshloglevel=debug"

hostile_boot "AxlSsh session channels"

PEER="$(dirname "$0")/ssh-peer.py"

# --- helpers ------------------------------------------------------------------

# One spelling of a publickey login with the staged key, the developer's own
# keys and agent kept out (good_login_works' reasoning).
SSH_PK=(-p "$HOST_PORT" -o StrictHostKeyChecking=no
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes
        -o ConnectTimeout=20 -o IdentitiesOnly=yes -o IdentityAgent=none
        -o PreferredAuthentications=publickey -i "$TEST_TMPDIR/good")

# sess NAME USER [ssh args...] -- one ssh(1) run. Its stdout goes to
# NAME.out and its stderr to NAME.err, EXACTLY as the remote end wrote them;
# ssh's own -v log goes to ssh-NAME.log with -E, so the two streams carry
# nothing else -- and the "Authenticated to" line the teardown's principal
# count reads lands in an ssh-*.log, where hostile_finish looks. Stdin is
# SESS_IN (default /dev/null). Sets SESS_RC.
sess() {
    local name="$1" user="$2"
    shift 2
    SESS_RC=0
    timeout 60 ssh -v -E "$TEST_TMPDIR/ssh-$name.log" "${SSH_PK[@]}" \
        "$@" "$user@127.0.0.1" ${SESS_CMD+"$SESS_CMD"} \
        > "$TEST_TMPDIR/$name.out" 2> "$TEST_TMPDIR/$name.err" \
        < "${SESS_IN:-/dev/null}" || SESS_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$name.log")
}

# The guest log's line count now, so a scenario can grade only what it made.
mark() { test_clean_log; wc -l < "$TEST_CLEAN_LOG" | tr -d ' '; }

# The guest lines after mark $1.
since() { test_clean_log; sed -n "$(($1 + 1)),\$p" "$TEST_CLEAN_LOG"; }

# How many lines after mark $1 are EXACTLY $2 / match the ERE $2 whole.
since_lines() { local n; n=$(since "$1" | grep -cxF -- "$2" || true); echo "${n:-0}"; }
since_re()    { local n; n=$(since "$1" | grep -cxE -- "$2" || true); echo "${n:-0}"; }

# Wait up to $3 s for at least $2 lines after mark $1 matching ERE $4.
wait_since() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        if [[ "$(since_re "$1" "$4")" -ge "$2" ]]; then return 0; fi
        sleep 0.2
    done
    return 1
}

# ssh(1)'s -E log $1 holds the whole line matching ERE $2. Its logit lines
# end in CRLF (measured), so the CR is dropped before the match.
log_line() { tr -d '\r' < "$1" | grep -qxE -- "$2"; }

# The handle in the first CHANNEL-OPEN line after mark $1.
open_handle() { since "$1" | grep -xE "$OPEN_RE" | head -1 | awk '{print $3}' || true; }

# The 1-based position, after mark $1, of the FIRST line matching ERE $2
# (0 if none) -- and of the LAST.
first_at() { since "$1" | grep -nxE -- "$2" | head -1 | cut -d: -f1 || true; }
last_at()  { since "$1" | grep -nxE -- "$2" | tail -1 | cut -d: -f1 || true; }

CLOSE_RE='SERVER: CHANNEL-CLOSE ud=[0-9]+'
OPEN_RE='SERVER: CHANNEL-OPEN [0-9]+ .*'
STALE_RE='SERVER: STALE-WRITE [0-9]+ -?[0-9]+'

# --- accessors -----------------------------------------------------------------
#
# FIRST, so its channel is the boot's first and its user data is ud=1. A
# PASSWORD login, so the principal is the one the fixture's password path
# adopts (principal=1), and the user is pwuser. The peer is the address the
# guest really sees ($HOST_AS_SEEN, measured by the hostile suite's penalty
# scenario); its port is the client's ephemeral one, so it is matched by
# shape. The handle is the image-wide counter's, so it too is matched by
# shape here and compared between channels in two-sessions.
if wants accessors; then
    m=$(mark)
    : > "$TEST_TMPDIR/acc.count"
    SESS_RC=0
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$TEST_TMPDIR/acc.count" \
    timeout 60 ssh -v -E "$TEST_TMPDIR/ssh-accessors.log" -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 true > "$TEST_TMPDIR/accessors.out" \
        2> "$TEST_TMPDIR/accessors.err" < /dev/null || SESS_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-accessors.log")
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    check "accessors: ssh pwuser@host true exits 0 with no output" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/accessors.out"
    check "accessors: the open line reads user, peer, principal and user data back exactly" \
        test "$(since_re "$m" "SERVER: CHANNEL-OPEN [0-9]+ pwuser peer=${HOST_AS_SEEN//./\\.}:[0-9]+ principal=1 ud=1")" = 1
    check "accessors: the hook's principal argument equals the accessor's" \
        test "$(since_lines "$m" 'SERVER: PRINCIPAL-MISMATCH')" = 0
    check "accessors: the close hook got the user data back (ud=1), once" \
        test "$(since_lines "$m" 'SERVER: CHANNEL-CLOSE ud=1')" = 1
    check "accessors: inside the close hook the handle is already retired (NOT_FOUND)" \
        test "$(since_lines "$m" 'SERVER: CLOSE-LOOKUP -5')" = 1
fi

# --- stale -----------------------------------------------------------------------
#
# The realistic application mistake -- a handle kept past
# its channel's close, written to from the application's own timer. With an
# integer handle it is AXL_NOT_FOUND (-5) and touches nothing; the heap's
# verdict at teardown is the evidence that nothing was written through.
if wants stale; then
    m=$(mark)
    SESS_CMD=true sess stale root
    st_h=$(open_handle "$m")
    wait_since "$m" 1 10 "SERVER: STALE-WRITE ${st_h:-x} -?[0-9]+" || true
    check "stale: ssh host true exits 0" test "$SESS_RC" = 0
    check "stale: a write to the closed handle ${st_h:-?} from a timer is AXL_NOT_FOUND" \
        test -n "$st_h" -a "$(since_lines "$m" "SERVER: STALE-WRITE $st_h -5")" = 1
fi

# --- exec-echo ---------------------------------------------------------------------
#
# The exec hook writes INSIDE itself, before its CHANNEL_SUCCESS is sealed.
# This checks the OUTPUT only: ssh(1) accepts exec data that arrives before
# the CHANNEL_SUCCESS, so it cannot see whether the reply left first -- the
# packet order (a write made inside a hook leaves after that hook's reply)
# is pinned by ssh-peer.py's transcripts in test-ssh-channel-hostile-qemu.sh
# (exit-order, open-hook), not here.
if wants exec-echo; then
    SESS_CMD='echo hello' sess exec-echo root
    check "exec-echo: stdout is exactly 'hello', rc 0" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/exec-echo.out" 'hello'
    check "exec-echo: nothing on stderr" \
        file_is "$TEST_TMPDIR/exec-echo.err"
fi

# --- exec-status ---------------------------------------------------------------------
if wants exec-status; then
    SESS_CMD=false sess exec-false root
    check "exec-status: false exits 1, no output" \
        peer_is "$SESS_RC" 1 "$TEST_TMPDIR/exec-false.out"
    SESS_CMD='err boom' sess exec-err root
    check "exec-status: 'err boom' exits 3 with stdout empty" \
        peer_is "$SESS_RC" 3 "$TEST_TMPDIR/exec-err.out"
    check "exec-status: ...and stderr exactly 'boom'" \
        file_is "$TEST_TMPDIR/exec-err.err" 'boom'
    SESS_CMD='no-such-command' sess exec-unknown root
    check "exec-status: an unknown command exits 127, stdout empty" \
        peer_is "$SESS_RC" 127 "$TEST_TMPDIR/exec-unknown.out"
    check "exec-status: ...with stderr exactly 'unknown command'" \
        file_is "$TEST_TMPDIR/exec-unknown.err" 'unknown command'
fi

# --- exec-stdin ---------------------------------------------------------------------
#
# stdin reaches the data hook in order, EOF reaches the eof hook once, and
# the channel stays writable after it: `cat` exits only at EOF.
if wants exec-stdin; then
    printf 'a\nb\n' > "$TEST_TMPDIR/stdin-ab"
    m=$(mark)
    SESS_IN="$TEST_TMPDIR/stdin-ab" SESS_CMD=cat sess exec-stdin root
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    check "exec-stdin: printf 'a\\nb\\n' | ssh host cat prints exactly a, b; rc 0" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/exec-stdin.out" 'a' 'b'
    check "exec-stdin: the eof hook ran once" \
        test "$(since_lines "$m" 'SERVER: EOF')" = 1
fi

# --- interactive ---------------------------------------------------------------------
#
# `ssh -tt` with a pipe on stdin: a pty-req (TERM pinned to xterm here, so
# the line does not depend on the runner's environment), the shell, and the
# fixture shell's echo -- every byte echoed, a line end as \r\n.
if wants interactive; then
    printf 'echo hi\nexit 3\n' > "$TEST_TMPDIR/stdin-interactive"
    m=$(mark)
    TERM=xterm SESS_IN="$TEST_TMPDIR/stdin-interactive" sess interactive root -tt
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    tr -d '\r' < "$TEST_TMPDIR/interactive.out" > "$TEST_TMPDIR/interactive.lf"
    check "interactive: ssh -tt exits with the shell's status, 3" \
        test "$SESS_RC" = 3
    check "interactive: the transcript is the echo, the output, the echo" \
        file_is "$TEST_TMPDIR/interactive.lf" 'echo hi' 'hi' 'exit 3'
    # 0x0: ssh(1) reads the size from its own stdin, a pipe here, and sends
    # zeros when that fails (measured).
    check "interactive: the guest saw the pty-req: xterm, 0x0" \
        test "$(since_lines "$m" 'SERVER: PTY xterm 0x0')" = 1
fi

# --- quit ------------------------------------------------------------------------------
#
# axl_ssh_channel_close from INSIDE the channel's own data
# hook. The close hook must run once, and never nested inside the data hook
# -- the fixture's QUIT line is printed by the data hook after the close
# returned, so a nested close hook would print CHANNEL-CLOSE before it.
if wants quit; then
    printf 'quit\n' > "$TEST_TMPDIR/stdin-quit"
    m=$(mark)
    TERM=xterm SESS_IN="$TEST_TMPDIR/stdin-quit" sess quit root -tt
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    q_at=$(first_at "$m" 'SERVER: QUIT 0')
    c_at=$(first_at "$m" "$CLOSE_RE")
    check "quit: the close from inside the data hook returned AXL_OK" \
        test "$(since_lines "$m" 'SERVER: QUIT 0')" = 1
    check "quit: the close hook ran exactly once" \
        test "$(since_re "$m" "$CLOSE_RE")" = 1
    check "quit: ...after the data hook's own line (never nested inside it)" \
        test "${q_at:-0}" -gt 0 -a "${c_at:-0}" -gt "${q_at:-0}"
    check "quit: ssh(1) saw the channel end without an exit status (rc 255)" \
        test "$SESS_RC" = 255
fi

# --- gate ---------------------------------------------------------------------
#
# The data hook is handed a pointer INTO the connection's payload buffer, so
# while any hook of a connection is on the stack the server parses nothing
# for it -- the parse gate. "slowflood" makes the WRITABLE hook dispatch the
# loop (200 ms) before each refill, and the feeder sends a line every 150 ms
# meanwhile, so lines land while a hook of their own channel is running: the
# gate must hold them (the DEBUG line proves it engaged) and they must come
# out once each, in order, with no hook nested inside another of the
# channel's (SERVER: HOOK-NESTED). "pause" then dispatches from INSIDE the
# data hook and goes on reading the rest of its buffer ("echo after" comes
# from the same packet as the pause): a liveness check on a data hook that
# runs the loop. It is not gate evidence by itself -- a data hook reached
# from the receive callback runs with that receive not yet re-armed.
# The flood's '.' bytes are stripped before the transcript is compared.
if wants gate; then
    mkfifo "$TEST_TMPDIR/gate.fifo"
    m=$(mark)
    {
        printf 'slowflood 1000000\r'
        for i in 1 2 3 4 5 6 7 8; do sleep 0.15; printf 'echo %s\r' "$i"; done
        sleep 0.15; printf 'pause 400\recho after\r'
        sleep 0.1; printf 'echo 9\r'
        # Not before the flood is done: an exit ends the channel, and on a
        # slow guest a fixed sleep ended it mid-flood.
        wait_since "$m" 1 50 'SERVER: FLOOD-DONE 1000000' || true
        printf 'exit 0\r'
    } > "$TEST_TMPDIR/gate.fifo" &
    gate_feed=$!
    BG_PIDS+=("$gate_feed")
    TERM=xterm SESS_IN="$TEST_TMPDIR/gate.fifo" sess gate root -tt
    wait "$gate_feed" 2>/dev/null || true
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    tr -d '.\r' < "$TEST_TMPDIR/gate.out" > "$TEST_TMPDIR/gate.lf"
    gate_held=$(since "$m" \
        | grep -cE 'ssh:[a-z_]+:[0-9]+: chan: input held while a channel hook runs$' \
        || true)
    check "gate PRECONDITION: the flood ran through writable hooks and finished" \
        test "$(since_lines "$m" 'SERVER: FLOOD-DONE 1000000')" = 1 \
            -a "$(since_lines "$m" 'SERVER: WRITABLE')" -ge 2
    check "gate PRECONDITION: input arrived under a hook and was held (${gate_held:-0} holds)" \
        test "${gate_held:-0}" -ge 1
    check "gate: no hook of a channel ran inside another of its hooks" \
        test "$(since_lines "$m" 'SERVER: HOOK-NESTED')" = 0
    check "gate: every line came through once, in order, the paused buffer intact; rc 0" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/gate.lf" 'slowflood 1000000' \
            'echo 1' '1' 'echo 2' '2' 'echo 3' '3' 'echo 4' '4' \
            'echo 5' '5' 'echo 6' '6' 'echo 7' '7' 'echo 8' '8' \
            'pause 400' 'echo after' 'after' 'echo 9' '9' 'exit 0'
fi

# --- backlog --------------------------------------------------------------------
#
# A hook that dispatches the loop must make its connection WAIT, not buffer:
# the peer may send up to its 2 MB window meanwhile, and input buffered past
# RX_BACKLOG_MAX (70,000 bytes) ends the connection as "input backlog". So
# the gate parks the receive, as output backpressure does, and the held
# input stays in TCP. "sinkflood" floods 2 MB through slowflood's
# loop-running writable hook while 300 KB of input arrives (a second in,
# so it lands during those hooks), and reports the input's byte count and
# SHA-256 once both are done: all of it, in order, on a connection that
# stayed up.
if wants backlog; then
    head -c 300000 /dev/urandom > "$TEST_TMPDIR/sink.in"
    sink_sum=$(sha256sum "$TEST_TMPDIR/sink.in" | awk '{print $1}')
    mkfifo "$TEST_TMPDIR/sink.fifo"
    m=$(mark)
    { sleep 1; cat "$TEST_TMPDIR/sink.in"; } > "$TEST_TMPDIR/sink.fifo" &
    sink_feed=$!
    BG_PIDS+=("$sink_feed")
    SESS_IN="$TEST_TMPDIR/sink.fifo" SESS_CMD=sinkflood sess backlog root
    wait "$sink_feed" 2>/dev/null || true
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    tr -d '.' < "$TEST_TMPDIR/backlog.out" > "$TEST_TMPDIR/backlog.lf"
    bl_held=$(since "$m" \
        | grep -cE 'ssh:[a-z_]+:[0-9]+: chan: input held while a channel hook runs$' \
        || true)
    check "backlog PRECONDITION: input arrived while a hook ran the loop (${bl_held:-0} holds)" \
        test "${bl_held:-0}" -ge 1
    check "backlog: no input-backlog disconnect" \
        test "$(since "$m" | grep -cF 'disconnect: input backlog' || true)" = 0
    check "backlog: all 300000 bytes arrived, in order (SHA-256), rc 0" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/backlog.lf" '' "sink 300000 $sink_sum"
fi

# --- forward ----------------------------------------------------------------------
#
# Every channel type but `session` carries type-specific fields after max
# packet (RFC 4254 sections 6.3.2 and 7.2), and must be REFUSED with
# UNKNOWN_CHANNEL_TYPE -- the connection, and any session on it, stays up,
# as sshd refuses a type it does not serve. `ssh -W` opens direct-tcpip at
# once; `ssh -L` opens one per forwarded connection, here in the middle of a
# live shell that must carry on afterwards. No disconnect line in the guest.
if wants forward; then
    m=$(mark)
    fw_rc=0
    timeout 30 ssh -v -E "$TEST_TMPDIR/ssh-forward-w.log" "${SSH_PK[@]}" \
        -W 127.0.0.1:1 root@127.0.0.1 > /dev/null 2>&1 < /dev/null || fw_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-forward-w.log")
    LPORT=$(test_port 1)
    mkfifo "$TEST_TMPDIR/fwd.fifo"
    {
        printf 'echo before\r'
        # Until the shell has answered `before`, bounded at 30 s: ssh(1)
        # sets its -L listener up before it starts the session, so that line
        # says the forward is listening. A fixed sleep lost that race under
        # load: the connect came first, no direct-tcpip open was made, and
        # the refusal check below failed.
        for ((fwd_i = 0; fwd_i < 150; fwd_i++)); do
            if tr -d '\r' < "$TEST_TMPDIR/forward-l.out" 2>/dev/null | grep -qx before; then
                break
            fi
            sleep 0.2
        done
        # One connection through the forward: the client opens
        # direct-tcpip for it, which the server refuses.
        (exec 3<>"/dev/tcp/127.0.0.1/$LPORT" && sleep 1) 2>/dev/null || true
        sleep 1
        printf 'echo after\r'
        sleep 0.5
        printf 'exit 0\r'
    } > "$TEST_TMPDIR/fwd.fifo" &
    fwd_feed=$!
    BG_PIDS+=("$fwd_feed")
    TERM=xterm SESS_IN="$TEST_TMPDIR/fwd.fifo" sess forward-l root -tt \
        -L "127.0.0.1:$LPORT:127.0.0.1:1"
    wait "$fwd_feed" 2>/dev/null || true
    tr -d '\r' < "$TEST_TMPDIR/forward-l.out" > "$TEST_TMPDIR/forward-l.lf"
    check "forward: ssh -W is refused with UNKNOWN_CHANNEL_TYPE" \
        log_line "$TEST_TMPDIR/ssh-forward-w.log" \
            'channel [0-9]+: open failed: unknown channel type: '
    check "forward: ssh -W exits 255 (its only channel was refused)" \
        test "$fw_rc" = 255
    check "forward: ssh -L's forwarded connection is refused the same way" \
        log_line "$TEST_TMPDIR/ssh-forward-l.log" \
            'channel [0-9]+: open failed: unknown channel type: '
    check "forward: ...and the shell on that connection carries on (rc 0)" \
        peer_is "$SESS_RC" 0 "$TEST_TMPDIR/forward-l.lf" 'echo before' \
            'before' 'echo after' 'after' 'exit 0'
    check "forward: the guest disconnected nobody" \
        test "$(since "$m" | grep -cE 'disconnect: ' || true)" = 0
fi

# --- open-shapes -------------------------------------------------------------------
#
# The same rule at the byte level, through ssh-peer.py: an x11 open in RFC
# 4254 section 6.3.2's full shape (originator address and port included)
# draws OPEN_FAILURE and the connection answers a global request after it;
# a `session` open with a byte after max packet ends the connection, as
# sshd's sshpkt_get_end does for the one type it accepts.
if wants open-shapes; then
    os_x11_rc=0
    os_x11=$(python3 "$PEER" x11-open-hex) || os_x11_rc=$?
    os_rc=0
    timeout 30 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" --user pwuser \
        --password "$PW_OK" \
        --script "send:90:$os_x11,expect:92,global:keepalive@openssh.com:1,expect:82" \
        > "$TEST_TMPDIR/ssh-open-x11.log" 2>&1 || os_rc=$?
    SESSION_TRAIL_HEX="0000000773657373696f6e000000000000800000008000""00"
    ot_rc=0
    timeout 30 python3 "$PEER" login 127.0.0.1 "$HOST_PORT" --user pwuser \
        --password "$PW_OK" --script "send:90:$SESSION_TRAIL_HEX,expect:91" \
        > "$TEST_TMPDIR/ssh-open-trail.log" 2>&1 || ot_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-open-x11.log" "$TEST_TMPDIR/ssh-open-trail.log")
    check "open-shapes PRECONDITION: x11-open-hex printed the 36-byte body" \
        test "$os_x11_rc" = 0 -a "${#os_x11}" = 72
    check "open-shapes: an RFC-shaped x11 open is refused and the connection answers after it" \
        peer_is "$os_rc" 0 "$TEST_TMPDIR/ssh-open-x11.log" 'peer: authenticated' \
            'peer: sent 90 seq=2' 'peer: got 92 reason=3' 'peer: sent 80 seq=3' 'peer: got 82'
    check "open-shapes: a session open with a trailing byte ends the connection" \
        peer_is "$ot_rc" 4 "$TEST_TMPDIR/ssh-open-trail.log" 'peer: authenticated' \
            'peer: sent 90 seq=2' 'peer: closed'
fi

# --- close-oom ----------------------------------------------------------------------
#
# The header promises the close hook is never called from inside an
# axl_ssh_channel_* call. "closeoom" closes from a timer -- no hook of the
# connection on the stack -- with the CLOSE packet's allocation failing, so
# the connection must end; the close hook must still come AFTER the call
# returned (the timer's own CLOSE-OOM-AFTER line), exactly once.
if wants close-oom; then
    printf 'closeoom\n' > "$TEST_TMPDIR/stdin-closeoom"
    m=$(mark)
    TERM=xterm SESS_IN="$TEST_TMPDIR/stdin-closeoom" sess close-oom root -tt
    wait_since "$m" 1 10 "$CLOSE_RE" || true
    co_after=$(first_at "$m" 'SERVER: CLOSE-OOM-AFTER')
    co_close=$(first_at "$m" "$CLOSE_RE")
    check "close-oom: the close returned AXL_OK" \
        test "$(since_lines "$m" 'SERVER: CLOSE-OOM 0')" = 1
    check "close-oom: the close hook ran exactly once" \
        test "$(since_re "$m" "$CLOSE_RE")" = 1
    check "close-oom: ...after the call returned, never inside it" \
        test "${co_after:-0}" -gt 0 -a "${co_close:-0}" -gt "${co_after:-0}"
fi

# --- two-sessions -----------------------------------------------------------------
if wants two-sessions; then
    m=$(mark)
    # Each in a subshell that exits with ssh's own status, so `wait` reads
    # it: `sess` itself always returns 0 (it records the status instead).
    ( SESS_CMD='hold 2000' sess two-a root; exit "$SESS_RC" ) &
    pa=$!
    ( SESS_CMD='hold 2000' sess two-b root; exit "$SESS_RC" ) &
    pb=$!
    BG_PIDS+=("$pa" "$pb")
    rca=0; wait "$pa" || rca=$?
    rcb=0; wait "$pb" || rcb=$?
    wait_since "$m" 2 10 "$CLOSE_RE" || true
    handles=$(since "$m" | grep -xE "$OPEN_RE" | awk '{print $3}' \
        | sort -u | wc -l || true)
    check "two-sessions: both sessions ran concurrently and exited 0" \
        test "$rca" = 0 -a "$rcb" = 0
    check "two-sessions: two opens with DISTINCT handles" \
        test "$(since_re "$m" "$OPEN_RE")" = 2 -a "$handles" = 2
    check "two-sessions: two close hooks" \
        test "$(since_re "$m" "$CLOSE_RE")" = 2
    check "two-sessions: neither hold's exit was refused" \
        test "$(since_re "$m" 'SERVER: HOLD-EXIT .*')" = 0
fi

# --- controlmaster ---------------------------------------------------------------
#
# Every session multiplexed through one master is a channel on ONE
# connection, so ten at once fill the default session cap (sshd's
# MaxSessions, 10) and the eleventh draws OPEN_FAILURE RESOURCE_SHORTAGE --
# and the connection stays up: a twelfth, after the ten end, succeeds.
if wants controlmaster; then
    CM="$TEST_TMPDIR/cm"
    m=$(mark)
    cm_rc=0
    timeout 30 ssh -v -E "$TEST_TMPDIR/ssh-cm-master.log" "${SSH_PK[@]}" \
        -o ControlMaster=yes -o ControlPath="$CM" -fN root@127.0.0.1 \
        > /dev/null 2>&1 < /dev/null || cm_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-cm-master.log")
    # The clients go through the master. One the master cannot serve FALLS
    # BACK to a direct connection (measured), so these options make that
    # fallback fail before authenticating, the same way every run: no known
    # host key and no prompt to accept one.
    CM_CLIENT=(-o ControlMaster=no -o ControlPath="$CM" -p "$HOST_PORT"
               -o BatchMode=yes -o StrictHostKeyChecking=yes
               -o UserKnownHostsFile=/dev/null)
    cm_pids=()
    for i in $(seq 1 11); do
        timeout 60 ssh -v -E "$TEST_TMPDIR/cm-$i.log" "${CM_CLIENT[@]}" \
            root@127.0.0.1 'hold 3000' \
            > "$TEST_TMPDIR/cm-$i.out" 2> "$TEST_TMPDIR/cm-$i.err" \
            < /dev/null &
        cm_pids+=("$!")
    done
    BG_PIDS+=("${cm_pids[@]}")
    cm_ok=0; cm_bad=0; cm_bad_i=0
    for i in $(seq 1 11); do
        r=0; wait "${cm_pids[$((i - 1))]}" || r=$?
        if [[ "$r" == 0 ]]; then cm_ok=$((cm_ok + 1)); else cm_bad=$((cm_bad + 1)); cm_bad_i=$i; fi
    done
    m12=$(mark)
    r12=0
    timeout 60 ssh "${CM_CLIENT[@]}" \
        root@127.0.0.1 'echo twelfth' > "$TEST_TMPDIR/cm-12.out" \
        2> "$TEST_TMPDIR/cm-12.err" < /dev/null || r12=$?
    ssh -o ControlPath="$CM" -O exit root@127.0.0.1 > /dev/null 2>&1 || true
    wait_since "$m" 11 10 "$CLOSE_RE" || true
    echo "  controlmaster: master rc=$cm_rc; $cm_ok ok, $cm_bad refused (#$cm_bad_i);" \
         "the refused one's stderr: '$(tr '\n' '|' < "$TEST_TMPDIR/cm-$cm_bad_i.err" 2>/dev/null)'"
    check "controlmaster: the master logged in and backgrounded (rc 0)" \
        test "$cm_rc" = 0
    check "controlmaster: ten sessions at once on one connection exit 0" \
        test "$cm_ok" = 10
    check "controlmaster: the eleventh is refused" test "$cm_bad" = 1
    check "controlmaster: ...with RESOURCE_SHORTAGE, as the master logs it" \
        log_line "$TEST_TMPDIR/ssh-cm-master.log" \
            'channel [0-9]+: open failed: resource shortage: '
    check "controlmaster: ...and as the refused client reports it" \
        log_line "$TEST_TMPDIR/cm-$cm_bad_i.log" \
            'mux_client_request_session: session request failed: Session open refused by peer'
    cm_opens=$(( $(since_re "$m" "$OPEN_RE") - $(since_re "$m12" "$OPEN_RE") ))
    check "controlmaster: the guest opened exactly ten channels for the eleven" \
        test "$cm_opens" = 10
    check "controlmaster: the connection stayed up -- a twelfth session succeeds" \
        peer_is "$r12" 0 "$TEST_TMPDIR/cm-12.out" 'twelfth'
    check "controlmaster: every opened channel got its close hook (eleven)" \
        test "$(since_re "$m" "$CLOSE_RE")" = 11
fi

# --- no-close-for-refused ------------------------------------------------------------
#
# An open the application refuses is ADMINISTRATIVELY_PROHIBITED, and since
# the channel was never opened, its close hook is never called.
if wants no-close-for-refused; then
    m=$(mark)
    SESS_CMD=true sess refused refuse
    wait_since "$m" 1 10 'SERVER: CHANNEL-REFUSED [0-9]+' || true
    sleep 1
    check "no-close-for-refused: the open hook refused" \
        test "$(since_re "$m" 'SERVER: CHANNEL-REFUSED [0-9]+')" = 1
    check "no-close-for-refused: ssh(1) reports ADMINISTRATIVELY_PROHIBITED" \
        log_line "$TEST_TMPDIR/ssh-refused.log" \
            'channel 0: open failed: administratively prohibited: '
    check "no-close-for-refused: ssh(1) exits 255" test "$SESS_RC" = 255
    check "no-close-for-refused: no close hook ran for it" \
        test "$(since_re "$m" "$CLOSE_RE")" = 0
fi

# --- kill-client ------------------------------------------------------------------
#
# A master holding two sessions is killed outright: the connection drops
# with both channels open. Each gets its close hook, exactly once, and both
# BEFORE the connection's release_principal -- a principal a channel holds
# is still valid in its close hook.
if wants kill-client; then
    CM2="$TEST_TMPDIR/cm2"
    km=$(mark)
    timeout 30 ssh -v -E "$TEST_TMPDIR/ssh-kill-master.log" "${SSH_PK[@]}" \
        -o ControlMaster=yes -o ControlPath="$CM2" -fN root@127.0.0.1 \
        > /dev/null 2>&1 < /dev/null || true
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-kill-master.log")
    k_pids=()
    for i in 1 2; do
        timeout 60 ssh -o ControlMaster=no -o ControlPath="$CM2" \
            -p "$HOST_PORT" root@127.0.0.1 'hold 20000' > /dev/null 2>&1 \
            < /dev/null &
        k_pids+=("$!")
    done
    BG_PIDS+=("${k_pids[@]}")
    wait_since "$km" 2 15 "$OPEN_RE" || true
    master_pid=$(ssh -o ControlPath="$CM2" -O check root@127.0.0.1 2>&1 \
        | grep -oE 'pid=[0-9]+' | grep -oE '[0-9]+' || true)
    m=$(mark)
    if [[ -n "$master_pid" ]]; then kill -9 "$master_pid" 2>/dev/null || true; fi
    for p in "${k_pids[@]}"; do wait "$p" 2>/dev/null || true; done
    wait_since "$m" 1 10 'SERVER: PRINCIPAL-RELEASED' || true
    c_last=$(last_at "$m" "$CLOSE_RE")
    r_at=$(first_at "$m" 'SERVER: PRINCIPAL-RELEASED')
    check "kill-client PRECONDITION: two sessions were open on the master" \
        test "$(since_re "$km" "$OPEN_RE")" = 2 -a -n "$master_pid"
    check "kill-client: exactly two close hooks" \
        test "$(since_re "$m" "$CLOSE_RE")" = 2
    check "kill-client: the connection's principal released once" \
        test "$(since_lines "$m" 'SERVER: PRINCIPAL-RELEASED')" = 1
    check "kill-client: every close hook ran before release_principal" \
        test "${c_last:-0}" -gt 0 -a "${r_at:-0}" -gt "${c_last:-0}"
fi

# --- free-in-hook ------------------------------------------------------------------
#
# axl_ssh_server_free from INSIDE the data hook (the fixture shell's
# `freeserver`). The free returns with that hook still on the stack; the
# channel's close hook must wait for the hook to return -- so it follows the
# fixture's FREE-RETURNED and AFTER-FREE lines -- and run exactly once; and
# the handle, used right after the free, is refused without the freed server
# being read. LAST: the server is gone after it. The heartbeat, the finish
# timer and the leak verdict in hostile_finish are what show the deferred free
# completed.
#
# The close is matched by THIS channel's own ud, from its CHANNEL-OPEN line:
# the free closes every channel the server still holds, so a connection an
# earlier scenario left open closes here too, and counted by $CLOSE_RE it
# would read as a double close of this one. It is reported as itself
# instead, by the last check.
if wants free-in-hook; then
    printf 'freeserver\n' > "$TEST_TMPDIR/stdin-free"
    m=$(mark)
    TERM=xterm SESS_IN="$TEST_TMPDIR/stdin-free" sess free-in-hook root -tt
    fih_ud=$(since "$m" | grep -xE "$OPEN_RE" | head -1 | sed -n 's/.* ud=\([0-9][0-9]*\)$/\1/p' || true)
    fih_close="SERVER: CHANNEL-CLOSE ud=${fih_ud:-none}"
    wait_since "$m" 1 10 "$fih_close" || true
    f_at=$(first_at "$m" 'SERVER: FREE-RETURNED')
    a_at=$(first_at "$m" 'SERVER: AFTER-FREE -5')
    c_at=$(first_at "$m" "$fih_close")
    # Sampled after a short settle: another channel's close hook landing a
    # moment after this one's must be in the log when the line is read, or
    # the check below could pass by reading too early.
    sleep 1
    fih_others=$(since "$m" | grep -xE "$CLOSE_RE" | grep -vxF "$fih_close" | tr '\n' ' ' || true)
    check "free-in-hook PRECONDITION: its channel opened, ud ${fih_ud:-unknown}" \
        test -n "$fih_ud"
    check "free-in-hook: the free returned inside the hook" \
        test "$(since_lines "$m" 'SERVER: FREE-RETURNED')" = 1
    check "free-in-hook: the handle is refused right after the free (NOT_FOUND)" \
        test "${a_at:-0}" -gt "${f_at:-0}"
    check "free-in-hook: the close hook ran exactly once" \
        test "$(since_lines "$m" "$fih_close")" = 1
    check "free-in-hook: ...after the hook returned (after FREE-RETURNED and AFTER-FREE)" \
        test "${c_at:-0}" -gt "${a_at:-0}" -a "${a_at:-0}" -gt 0
    check "free-in-hook: no other channel was still open when the server was freed (${fih_others:-none})" \
        test -z "$fih_others"
fi

# No hook of any channel ran nested in another of the same channel's, in the
# whole boot -- the close hook included (the fixture checks that one too).
if [[ "$ABUSE" == 1 ]]; then
    test_clean_log
    check "every scenario: no channel hook nested inside another of its channel's" \
        test "$(guest_lines 'SERVER: HOOK-NESTED')" = 0
fi

# Every stale write of the boot -- stale=1 arms one per close while the loop
# runs -- was refused. The positive count, which keeps "none was anything
# else" from passing on a log with no stale writes at all, is required only
# when the stale scenario ran: it waits for its own write, where the last
# close of a narrower selection can still be inside its 200 ms here.
if [[ "$ABUSE" == 1 ]]; then
    test_clean_log
    st_all=$(grep -cxE "$STALE_RE" "$TEST_CLEAN_LOG" || true)
    st_bad=$(grep -xE "$STALE_RE" "$TEST_CLEAN_LOG" | grep -cvE ' -5$' || true)
    st_min=0
    if wants stale; then st_min=1; fi
    check "stale (whole boot): every stale write was AXL_NOT_FOUND (${st_all:-0} writes, ${st_bad:-0} otherwise)" \
        test "${st_all:-0}" -ge "$st_min" -a "${st_bad:-0}" = 0
fi

hostile_finish
