# shellcheck shell=bash
# ssh-idle-cases.sh -- the body of the AxlSsh idle suites against a real
# ssh(1) and ssh-peer.py: sshd's UnusedConnectionTimeout and
# ClientAliveInterval / ClientAliveCountMax (axl_ssh_server_set_unused_timeout and
# axl_ssh_server_set_client_alive, include/axl/axl-ssh-server.h; spec
# docs/superpowers/specs/2026-08-21-axl-ssh-design.md section 12a.5).
#
# THREE TESTS, ONE BODY. This was test-ssh-idle-qemu.sh, one boot running
# all twelve scenarios in ~113 s; one boot is the unit the runner's pool
# parallelises, so it was split by scenario into three thin wrappers that
# set HOSTILE_SUMMARY and HOSTILE_SCENARIOS and source this file -- the
# setup, the servers and every scenario stay here, once:
#
#   test-ssh-idle-unused-qemu.sh  answers-are-not-unimplemented unused-ssh-N
#                                 unused-larval unused-with-session
#                                 unused-after-close disconnect-control
#   test-ssh-idle-alive-qemu.sh   alive-ssh-answers alive-ssh-bulk-quiet-client
#   test-ssh-idle-probe-qemu.sh   hook-gated-not-dropped
#                                 peer-silent-channel-probe
#                                 chatty-peer-not-probed peer-silent-dropped
#                                 disconnect-control
#
# A part's scenarios must be a subsequence of HOSTILE_SCENARIOS_ALL below,
# in its order (lib/ssh-hostile-lib.sh checks it, before the boot): the
# INFO-line pins described further down depend on which scenario runs first
# on each server in a boot. The parts together must name every scenario
# (also checked there). disconnect-control -- the proof that the
# `disconnect:` DEBUG line is visible at this log level -- ends every part
# that has a "no disconnect: line" check, so each such absence has its
# control in the same boot; it is the one scenario in more than one part.
#
# NOT A TEST. lib/discover.sh schedules test/integration/test-*.sh only.
#
# ONE BOOT, TWO SERVERS. Each scenario's settings leave ONE mechanism that
# can end it; a peer that answers keepalives (ssh(1), and ssh-peer.py by
# default) is immune to client-alive, so those may share an alive-on server.
#
#   server  port        settings                    scenarios
#   A       HOST_PORT   unused=3 alive=2:2,         answers-are-not-unimplemented
#                       channels=1                  unused-ssh-N unused-larval
#                                                   unused-with-session
#                                                   unused-after-close
#                                                   alive-ssh-answers
#                                                   alive-ssh-bulk-quiet-client
#                                                   hook-gated-not-dropped
#                                                   peer-silent-channel-probe
#   B       PORT_B      unused2=0 alive2=2:2,       chatty-peer-not-probed
#                       no channels                 peer-silent-dropped
#                                                   disconnect-control
#
# Both servers' reaper tick is 1000 ms (axl_ssh_reap_tick_ms: half of the
# 2 s alive interval). THE DROP BOUND for alive=2:2 is (2 + 1) x (2 + 1) =
# 9 s after the peer was last heard; the unused drop lands between 3 s and
# 3 s + one tick after the last channel in use was freed, or after login.
#
# EVERY DROP TIMING IS READ OFF THE SERVER'S OWN CLOCK: at each idle drop
# the server writes, at DEBUG, how long the silence or the unused stretch
# was by the monotonic clock that timed it (`idle: dropping, nothing heard
# for N ms`, `idle: dropping, no channel in use for N ms`). Neither of the
# other clocks will do. The host's does not keep pace with the guest's under
# load (measured slow on x64), and the guest CONSOLE's timestamps are an RTC
# second with a fraction from an unrelated counter (src/log/axl-log.c says
# so): measured, they put drops 2.3 s after a 3 s timeout's start. The few
# host-clock waits left are loose sanity limits (a peer or ssh that must
# END, within 20 s), never a rule's bound. Each UPPER bound allows 1 s past
# the rule for a tick the loop dispatches late (measured: a busy loop after
# fifteen holders' teardown); none allows anything BELOW the rule.
#
# no-disconnect-on-drop is not a scenario of its own: every scenario that
# drops asserts the guest wrote no `disconnect:` DEBUG line from its mark
# on, and disconnect-control proves that line is visible at this log level
# (a KEXINIT before authentication, which disconnect() ends).
#
# The idle audit lines are rate-limited to one INFO line per KIND per minute
# per SERVER, the rest at DEBUG. The order below makes unused-ssh-N server
# A's first unused line, peer-silent-channel-probe A's first unresponsive
# line and peer-silent-dropped B's -- in their parts' boots as in the one
# boot this was: those three pin the whole INFO line
# (an exact INFO-line match); every other count reads either level.
#
# sshloglevel=debug (the whole boot): the probe lines and `disconnect:` are
# DEBUG, and at the default level a DEBUG line is ABSENT, not demoted.
#
# Every ssh(1) string pinned below was MEASURED from a real OpenSSH 9.9p1
# -vvv transcript (SSH_HOSTILE_KEEP=<dir> keeps them); each carries its
# source in a comment.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage (a part): ./test/integration/test-ssh-idle-<part>-qemu.sh [--arch X64|AARCH64]

HOSTILE_SCENARIOS_ALL="answers-are-not-unimplemented unused-ssh-N unused-larval unused-with-session unused-after-close alive-ssh-answers alive-ssh-bulk-quiet-client hook-gated-not-dropped peer-silent-channel-probe chatty-peer-not-probed peer-silent-dropped disconnect-control"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh): the CEILING the boot ends at only when the
# host's finish signal never comes, so a passing run does not pay it.
# Measured 2026-10-09 (aa64 TCG / x64, at host load 1-3 and 13-15):
# answers 0-1, unused-ssh-N 6-10, larval 3-4, with-session 6 (a 6 s hold
# then; 9 s now), after-close 5, alive 20 (20 s quiet then; until ssh(1)
# has logged 5 keepalives now, 10-15 s), bulk 40 MiB 8-10 s at low load and
# 27 s on aa64 at load 14, gated 14, silent-channel 7-9, chatty 6, silent
# 7-9, control 0. Each budget is about the slowest run seen; bulk's is under
# its one 27 s outlier, which its part's margins cover -- one overrun
# borrows from the rest of its part, and only each part's sum has to hold.
declare -A HOSTILE_BUDGET=(
    [answers-are-not-unimplemented]=2 [unused-ssh-N]=10 [unused-larval]=5
    [unused-with-session]=11 [unused-after-close]=7 [alive-ssh-answers]=28
    [alive-ssh-bulk-quiet-client]=22 [hook-gated-not-dropped]=15
    [peer-silent-channel-probe]=9 [chatty-peer-not-probed]=7
    [peer-silent-dropped]=8 [disconnect-control]=1
)

if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host -- the idle scenarios are real sessions"
    exit 77
fi

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
PORT_B=$(test_port 1)
GUEST_PORT2=2223
HOSTILE_EXTRA_ARGS="${GUEST_PORT2} ${GRACE_SEC} channels=1 sshloglevel=debug unused=3 alive=2:2 unused2=0 alive2=2:2"
HOSTILE_EXTRA_FWD=("$PORT_B" "$GUEST_PORT2")

hostile_boot "AxlSsh idle"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"
source "$(dirname "$0")/lib/ssh-idle-lib.sh"

# --- helpers ------------------------------------------------------------------

SSH_PK=(-p "$HOST_PORT" -o StrictHostKeyChecking=no
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes
        -o ConnectTimeout=20 -o IdentitiesOnly=yes -o IdentityAgent=none
        -o PreferredAuthentications=publickey -i "$TEST_TMPDIR/good")

PW_LOGIN=(--user pwuser --password "$PW_OK")
CHAN_OPEN=$(python3 "$PEER" chan-open-hex --sender 0)

# --- answers-are-not-unimplemented -------------------------------------------
#
# The four answers a keepalive (or any request) draws -- REQUEST_SUCCESS 81,
# REQUEST_FAILURE 82, CHANNEL_SUCCESS 99, CHANNEL_FAILURE 100 -- are accepted
# silently after authentication, as sshd's dispatch sends them to
# server_input_keep_alive (serverloop.c:380-390). The next message the peer
# reads is the REQUEST_FAILURE for its own global request; an UNIMPLEMENTED
# for any of the four would arrive first and print `peer: unexpected 3`.
# The peer closes right after, inside the 3 s unused timeout.
if wants answers-are-not-unimplemented; then
    t0=$SECONDS
    m=$(mark)
    peer_ts answers login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --script 'send:82,send:81,send:100:00000000,send:99:00000000,global:x@y:1,expect:82'
    check "answers-are-not-unimplemented: 81/82/99/100 draw nothing; the next message is the global's REQUEST_FAILURE" \
        peer_is "$PEER_RC" 0 "$TEST_TMPDIR/ssh-answers.log" \
            'peer: authenticated' 'peer: sent 82 seq=2' 'peer: sent 81 seq=3' \
            'peer: sent 100 seq=4' 'peer: sent 99 seq=5' 'peer: sent 80 seq=6' \
            'peer: got 82'
    settle "$m" 1 answers-are-not-unimplemented
    took answers-are-not-unimplemented
fi

# --- unused-ssh-N -------------------------------------------------------------
#
# `ssh -N` opens no channel, so after 3 s (+ one 1 s tick) it is ended.
# The server's 16 slots are filled first -- 15 holders, each a real ssh(1)
# with a STARTED session (`hold`), so neither the unused timeout nor
# client-alive (ssh(1) answers) can end them, and an authenticated
# connection is never evicted -- so a 17th arrival is refused while `ssh -N`
# is in, and the follow-up `ssh host true` can succeed only if the drop
# freed its slot.
if wants unused-ssh-N; then
    t0=$SECONDS
    m=$(mark)
    hostile_fill_slots 15 '.*SERVER: CHANNEL-OPEN .*' ssh-holder \
        "${SSH_PK[@]}" root@127.0.0.1 'hold 60000'
    sleep 1
    un_open=$(since_re "$m" '.*SERVER: CHANNEL-OPEN .*')
    m2=$(mark)
    un_t0=$EPOCHREALTIME
    timeout 30 ssh -vvv -N -E "$TEST_TMPDIR/ssh-unusedN.log" "${SSH_PK[@]}" \
        root@127.0.0.1 > /dev/null 2> "$TEST_TMPDIR/unusedN.err" < /dev/null &
    un_pid=$!
    BG_PIDS+=("$un_pid")
    # The 17th, while ssh -N is still in: refused before identification.
    wait_since "$m2" 1 20 ".*Accepted publickey for root from .*" || true
    un_full_rc=0
    timeout 20 python3 "$PEER" kexwait 127.0.0.1 "$HOST_PORT" --hold 1 \
        > "$TEST_TMPDIR/full.log" 2>&1 || un_full_rc=$?
    un_rc=0
    wait "$un_pid" || un_rc=$?
    un_t1=$EPOCHREALTIME
    un_port=$(accepted_port "$m2" publickey root)
    un_dt=$(awk -v a="$un_t0" -v b="$un_t1" 'BEGIN { printf "%.1f", b - a }')
    tr -d '\r' < "$TEST_TMPDIR/ssh-unusedN.log" > "$TEST_TMPDIR/unusedN.clean"
    un_close=$(tr -d '\r' < "$TEST_TMPDIR/unusedN.err" | head -1)
    echo "  unused-ssh-N: ssh -N rc $un_rc after ${un_dt}s; port ${un_port:-?}; close line: ${un_close:-<none>}"
    check "unused-ssh-N PRECONDITION: 15 holders each started a session ($un_open)" \
        test "$un_open" = 15
    check "unused-ssh-N PRECONDITION: the server was full -- a 17th arrival was refused (peer: refused, rc 5)" \
        peer_is "$un_full_rc" 5 "$TEST_TMPDIR/full.log" 'peer: refused'
    check "unused-ssh-N: the inactive line at INFO, exactly once, for ssh -N's own port ($un_port)" \
        test -n "$un_port" -a "$(since "$m2" | grep -cxE "[0-9:.]+ \[INFO\]  ssh: $INACTIVE from user root ${HOST_AS_SEEN//./\\.} port $un_port" || true)" = 1
    un_ms=$(drop_ms "$m2" "$UNUSED_FOR")
    echo "  unused-ssh-N: dropped after ${un_ms} ms with no channel (server clock)"
    check "unused-ssh-N: dropped more than 3000 ms after login, within a tick (server clock, $un_ms)" \
        above_within "$un_ms" 3000 5000
    check "unused-ssh-N: sanity -- ssh -N ended within 20 s on the host (${un_dt}s)" \
        within "$un_dt" 0 20
    # MEASURED (OpenSSH 9.9p1, its stderr when the reset arrives; printed by
    # clientloop.c:805's quit_message): this exact line, and rc 255.
    check "unused-ssh-N: ssh -N exits 255 with 'Connection to 127.0.0.1 closed by remote host.' ($un_rc)" \
        test "$un_rc" = 255 -a "$un_close" = 'Connection to 127.0.0.1 closed by remote host.'
    check "unused-ssh-N: no disconnect: line around the drop (abortive, as sshd's cleanup_exit)" \
        test "$(since_re "$m2" "$DISC_RE")" = 0
    # Inside check: a bare call that fails would end the script under set -e
    # with no named check failed.
    check "unused-ssh-N: the follow-up ssh host true got the freed slot" \
        good_login_works unused-followup
    hostile_release_slots
    settle "$m" 17 unused-ssh-N
    took unused-ssh-N
fi

# --- unused-larval -----------------------------------------------------------
#
# A session opened and never started (no shell, no exec) is NOT in use --
# sshd's channel_still_open skips a LARVAL channel -- so it is ended by the
# unused timeout exactly as a connection with no channel is. The peer
# answers keepalives (the larval channel is the probe target, so they come
# as CHANNEL_REQUESTs), which leaves the unused timeout the one mechanism.
if wants unused-larval; then
    t0=$SECONDS
    m=$(mark)
    peer_ts larval login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --script "send:90:$CHAN_OPEN,expect:91,idle:12"
    lv_ms=$(drop_ms "$m" "$UNUSED_FOR")
    echo "  unused-larval: dropped after ${lv_ms} ms with no channel in use (server clock)"
    check "unused-larval: the peer was ended (rc 4) holding a larval channel" \
        test "$PEER_RC" = 4 -a "$(lines_of "$TEST_TMPDIR/ssh-larval.log" 'peer: closed')" = 1
    check "unused-larval: more than 3000 ms after login, within a tick (server clock, $lv_ms)" \
        above_within "$lv_ms" 3000 5000
    check "unused-larval: the guest logged one inactive line for pwuser" \
        test "$(idle_count "$m" pwuser "$INACTIVE")" = 1
    check "unused-larval: and no unresponsive line" \
        test "$(idle_count "$m" pwuser "$SILENT")" = 0
    check "unused-larval: no disconnect: line" test "$(since_re "$m" "$DISC_RE")" = 0
    settle "$m" 1 unused-larval
    took unused-larval
fi

# --- unused-with-session ------------------------------------------------------
#
# A started session holds the connection past the unused timeout: `hold
# 9000` runs three times the 3 s timeout on a LOOP timer, and the reaper's
# own probe lines during it (ssh(1) is silent and answers) prove the reaper
# ticked while it ran -- the test cannot pass by the reaper being starved.
# A probe goes out at most interval + tick = 3 s after the last bytes heard
# (the exec), so two come by 6 s of the 9 s hold: a 3 s margin.
if wants unused-with-session; then
    t0=$SECONDS
    m=$(mark)
    ws_rc=0
    timeout 40 ssh -v -E "$TEST_TMPDIR/ssh-withsess.log" "${SSH_PK[@]}" \
        root@127.0.0.1 'hold 9000' > /dev/null 2>&1 < /dev/null || ws_rc=$?
    ws_probes=$(since_re "$m" "$PROBE_CHAN_RE")
    echo "  unused-with-session: rc $ws_rc, $ws_probes probe(s) during the hold"
    check "unused-with-session: ssh host 'hold 9000' exits 0" test "$ws_rc" = 0
    check "unused-with-session: the reaper ticked during it (>= 2 channel probes, $ws_probes)" \
        test "$ws_probes" -ge 2
    check "unused-with-session: no idle drop line" \
        test "$(idle_count "$m" root "($INACTIVE|$SILENT)")" = 0
    settle "$m" 1 unused-with-session
    took unused-with-session
fi

# --- unused-after-close ------------------------------------------------------
#
# The unused clock runs from the moment the LAST channel in use is freed,
# not from the reaper's previous tick (sshd's loop refreshes the expiry on
# the pass the CLOSE wakes, serverloop.c:246-247). The peer first opens a
# session and closes it UNSTARTED (a larval slot freed: nothing restarts),
# then starts a second, closes it itself, and stays.
#
# A drop TIME cannot detect the restart: run from the previous tick
# instead, the expiry is that tick + 3 s, itself a tick boundary, so the
# drop moves only when the later tick reads the clock a millisecond late
# (measured: a sabotage removing the call passed at 3.479 s). So the server
# says what the restart set, from the same clock reading, at DEBUG
# (`idle: last channel freed; unused clock restarts, N ms`): exactly one
# such line, N exactly 3000 -- a missing call prints another N, a missing
# branch none, and a restart on ANY slot free a second line for the larval
# one. The drop's own server-clock line stays beside it as the guarantee:
# more than 3000 ms after the restart, never sooner.
if wants unused-after-close; then
    t0=$SECONDS
    m=$(mark)
    EXEC_HOLD=$(python3 "$PEER" chan-req-hex --recipient @ --name exec --want 1 \
        --rest "$(printf '%08x' 10)$(printf 'hold 20000' | od -An -tx1 | tr -d ' \n')")
    peer_ts afterclose login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --script "send:90:$CHAN_OPEN,expect:91,send:97:@00000000,expect:97,send:90:$CHAN_OPEN,expect:91,send:98:$EXEC_HOLD,expect:99,expect:93,hold:1,send:97:@00000000,idle:8"
    ac_ms=$(drop_ms "$m" "$UNUSED_FOR")
    echo "  unused-after-close: restart lines: $(since_re "$m" "$RESTART_RE"); dropped after ${ac_ms} ms with no channel in use (server clock)"
    check "unused-after-close: the peer closed a larval and then a started session, and was ended (rc 4)" \
        test "$PEER_RC" = 4 -a "$(tail -1 "$TEST_TMPDIR/ssh-afterclose.log")" = 'peer: closed' \
            -a "$(since_re "$m" "${DBG}chan [0-9]+: closed")" = 2
    check "unused-after-close: one restart line, for the started session only" \
        test "$(since_re "$m" "$RESTART_RE")" = 1
    check "unused-after-close: ...and it set exactly the unused period, 3000 ms" \
        test "$(since_re "$m" "${DBG}idle: last channel freed; unused clock restarts, 3000 ms")" = 1
    check "unused-after-close: dropped more than 3000 ms after the restart, never sooner (server clock, $ac_ms)" \
        above_within "$ac_ms" 3000 5000
    settle "$m" 1 unused-after-close
    took unused-after-close
fi

# --- alive-ssh-answers --------------------------------------------------------
#
# A session the client never speaks in is NOT dropped,
# because ssh(1) answers every probe. `ssh -tt` into the line shell, stdin
# quiet until ssh(1) has logged its 5th keepalive request (at most 40 s of
# HOST time), then `exit 5`. A probe goes out at most every 3 s of guest
# time, so the 5th comes by ~15 s. What discriminates is the COUNT: a
# server that did not take ssh's answers as hearing from it drops the
# connection at the third due tick, after only 2 probes, so 5 answered
# probes on a live session can only come from a server that counts answers.
# Waiting for those lines rather than sleeping a fixed 26 s changes when the
# session ends, not what it must show; the 40 s cap leaves the count check
# (and the exit status) to fail on a server that never probes or drops.
# ssh(1)'s line for each probe, MEASURED (OpenSSH 9.9p1 -vvv, clientloop.c
# client_input_channel_req's debug()):
#   debug1: client_input_channel_req: channel 0 rtype keepalive@openssh.com reply 1
KA_CLIENT_RE='^debug1: client_input_channel_req: channel 0 rtype keepalive@openssh\.com reply 1$'
if wants alive-ssh-answers; then
    t0=$SECONDS
    m=$(mark)
    fifo="$TEST_TMPDIR/alive.fifo"
    mkfifo "$fifo"
    {
        for ((i = 0; i < 400; i++)); do
            n=$(tr -d '\r' 2>/dev/null < "$TEST_TMPDIR/ssh-alive.log" \
                | grep -cE "$KA_CLIENT_RE" || true)
            if [[ "${n:-0}" -ge 5 ]]; then break; fi
            sleep 0.1
        done
        printf 'exit 5\r'
        sleep 2
    } > "$fifo" &
    BG_PIDS+=("$!")
    aa_rc=0
    TERM=xterm timeout 60 ssh -vvv -tt -E "$TEST_TMPDIR/ssh-alive.log" \
        "${SSH_PK[@]}" root@127.0.0.1 > "$TEST_TMPDIR/alive.out" 2>&1 \
        < "$fifo" || aa_rc=$?
    aa_ka=$(tr -d '\r' < "$TEST_TMPDIR/ssh-alive.log" | grep -cE "$KA_CLIENT_RE" || true)
    echo "  alive-ssh-answers: rc $aa_rc, ssh logged ${aa_ka:-0} keepalive request(s)"
    check "alive-ssh-answers: ssh -tt exits with the shell's status, 5" test "$aa_rc" = 5
    check "alive-ssh-answers: ssh received >= 5 keepalive@openssh.com requests (${aa_ka:-0})" \
        test "${aa_ka:-0}" -ge 5
    check "alive-ssh-answers: the guest never logged the unresponsive line" \
        test "$(idle_count "$m" root "$SILENT")" = 0
    settle "$m" 1 alive-ssh-answers
    took alive-ssh-answers
fi

# --- alive-ssh-bulk-quiet-client ---------------------------------------------
#
# Bulk output to a client that sends nothing of its own: the server probes
# whenever the interval has passed since the client was last heard (spec
# section 12a.7 deviation 9), ssh(1) answers, and the output is exact.
# 40 MiB: about 10 s on aa64 (4 MB/s measured), the brief's floor.
BULK_BYTES=${SSH_IDLE_BULK_BYTES:-41943040}
if wants alive-ssh-bulk-quiet-client; then
    t0=$SECONDS
    m=$(mark)
    stream_file "$BULK_BYTES" "$TEST_TMPDIR/bulk.expect"
    bq_want=$(sha256sum < "$TEST_TMPDIR/bulk.expect" | cut -d' ' -f1)
    rm -f "$TEST_TMPDIR/bulk.expect"
    bq_rc=0
    bq_t=$SECONDS
    bq_got=$(timeout 90 ssh -v -E "$TEST_TMPDIR/ssh-bulk.log" "${SSH_PK[@]}" \
        root@127.0.0.1 "stream $BULK_BYTES" < /dev/null | sha256sum | cut -d' ' -f1) \
        || bq_rc=$?
    bq_secs=$((SECONDS - bq_t))
    echo "  alive-ssh-bulk-quiet-client: $BULK_BYTES bytes in ${bq_secs}s, rc $bq_rc"
    check "alive-ssh-bulk-quiet-client: ssh host 'stream $BULK_BYTES' exits 0 with the output exact (SHA-256)" \
        test "$bq_rc" = 0 -a "$bq_got" = "$bq_want"
    check "alive-ssh-bulk-quiet-client: no idle drop line" \
        test "$(idle_count "$m" root "($INACTIVE|$SILENT)")" = 0
    settle "$m" 1 alive-ssh-bulk-quiet-client
    took alive-ssh-bulk-quiet-client
fi

# --- hook-gated-not-dropped ---------------------------------------------------
#
# A hook that runs the loop past the drop bound: the line shell's
# `pause 12000` dispatches the loop from
# INSIDE the data hook for 12 s -- past the 9 s drop bound -- while the
# peer's input waits behind the parse gate. The server must neither probe
# nor count meanwhile (the answers would sit unread because of OUR hook),
# so a live ssh(1) is not dropped.
if wants hook-gated-not-dropped; then
    t0=$SECONDS
    m=$(mark)
    fifo="$TEST_TMPDIR/gated.fifo"
    mkfifo "$fifo"
    { sleep 1; printf 'pause 12000\r'; sleep 13; printf 'exit 6\r'; sleep 2; } > "$fifo" &
    BG_PIDS+=("$!")
    hg_rc=0
    TERM=xterm timeout 60 ssh -v -tt -E "$TEST_TMPDIR/ssh-gated.log" \
        "${SSH_PK[@]}" root@127.0.0.1 > "$TEST_TMPDIR/gated.out" 2>&1 \
        < "$fifo" || hg_rc=$?
    echo "  hook-gated-not-dropped: rc $hg_rc"
    check "hook-gated-not-dropped: ssh -tt survived the 12 s hook and exits 6" \
        test "$hg_rc" = 6
    check "hook-gated-not-dropped: no idle drop line" \
        test "$(idle_count "$m" root "($INACTIVE|$SILENT)")" = 0
    check "hook-gated-not-dropped: no PAUSE-FAIL (the hook really dispatched for 12 s)" \
        test "$(since "$m" | grep -c 'SERVER: PAUSE-FAIL' || true)" = 0
    settle "$m" 1 hook-gated-not-dropped
    took hook-gated-not-dropped
fi

# --- peer-silent-channel-probe -----------------------------------------------
#
# A started session, and a peer that has stopped answering: the probes are
# CHANNEL_REQUESTs on that channel (sshd's channel_find_open), two go out,
# and the third due tick ends it (3 > 2), within the 9 s drop bound.
if wants peer-silent-channel-probe; then
    t0=$SECONDS
    m=$(mark)
    EXEC_REQ=$(python3 "$PEER" chan-req-hex --recipient @ --name exec --want 1 \
        --rest "$(printf '%08x' 10)$(printf 'hold 30000' | od -An -tx1 | tr -d ' \n')")
    peer_ts silentchan login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --keepalive ignore \
        --script "send:90:$CHAN_OPEN,expect:91,send:98:$EXEC_REQ,expect:99,expect:93,idle:14"
    sc_port=$(accepted_port "$m" password pwuser)
    sc_ms=$(drop_ms "$m" "$SILENT_FOR")
    sc_host=$(ts_gap silentchan 'peer: got 93 bytes=2097152' 'peer: closed')
    echo "  peer-silent-channel-probe: dropped after ${sc_ms} ms of silence (server clock); ${sc_host}s (host); port ${sc_port:-?}"
    check "peer-silent-channel-probe: two channel probes ignored, then closed (rc 4)" \
        test "$PEER_RC" = 4 \
            -a "$(lines_of "$TEST_TMPDIR/ssh-silentchan.log" 'peer: ignored keepalive 98')" = 2 \
            -a "$(lines_of "$TEST_TMPDIR/ssh-silentchan.log" 'peer: ignored keepalive 80')" = 0 \
            -a "$(tail -1 "$TEST_TMPDIR/ssh-silentchan.log")" = 'peer: closed'
    check "peer-silent-channel-probe: the guest sent them as channel requests (2), none global" \
        test "$(since_re "$m" "$PROBE_CHAN_RE")" = 2 -a "$(since_re "$m" "$PROBE_GLOB_RE")" = 0
    # The third due tick: no sooner than 3 x 2 s, no later than the bound.
    check "peer-silent-channel-probe: dropped within the 9 s bound, no sooner than the third due tick (server clock, $sc_ms)" \
        within "$sc_ms" 6000 10000
    check "peer-silent-channel-probe: sanity -- the peer was closed within 20 s on the host ($sc_host)" \
        within "$sc_host" 0 20
    check "peer-silent-channel-probe: the unresponsive line at INFO, once, for the peer's port" \
        test -n "$sc_port" -a "$(since "$m" | grep -cxE "[0-9:.]+ \[INFO\]  ssh: $SILENT from user pwuser ${HOST_AS_SEEN//./\\.} port $sc_port" || true)" = 1
    check "peer-silent-channel-probe: no disconnect: line" test "$(since_re "$m" "$DISC_RE")" = 0
    # The drop frees the started session in teardown. That is not the last
    # channel in use being RELEASED -- nothing is left to time -- so no
    # restart line. The connection's PRINCIPAL-RELEASED follows every close
    # of the teardown, so the absence is checked after it.
    settle "$m" 1 peer-silent-channel-probe
    check "peer-silent-channel-probe: ...and restarted no unused clock freeing the session (no restart line)" \
        test "$(since_re "$m" "$RESTART_RE")" = 0
    took peer-silent-channel-probe
fi

# --- chatty-peer-not-probed --------------------------------------------------
#
# Server B (no unused timeout): a peer that sends an IGNORE every second is
# HEARD every second, so the 2 s probe never comes due -- any bytes restart
# the silence clock (serverloop.c:239-240). A server that restarted it only
# on a probe would send one every interval.
if wants chatty-peer-not-probed; then
    t0=$SECONDS
    m=$(mark)
    peer_ts chatty login 127.0.0.1 "$PORT_B" "${PW_LOGIN[@]}" \
        --script 'send:2:,hold:1,send:2:,hold:1,send:2:,hold:1,send:2:,hold:1,send:2:,hold:1,send:2:,hold:1'
    check "chatty-peer-not-probed: the peer ran its 6 s of IGNOREs (rc 0)" \
        test "$PEER_RC" = 0 -a "$(grep -c '^peer: sent 2 seq=' "$TEST_TMPDIR/ssh-chatty.log" || true)" = 6
    check "chatty-peer-not-probed: the guest sent it no probe" \
        test "$(since_re "$m" "($PROBE_GLOB_RE|$PROBE_CHAN_RE)")" = 0
    settle "$m" 1 chatty-peer-not-probed
    took chatty-peer-not-probed
fi

# --- peer-silent-dropped ------------------------------------------------------
#
# Server B (no unused timeout): a peer logs in, opens nothing, and stops
# answering. The probes are GLOBAL_REQUESTs; exactly two are ignored, the
# third due tick drops it (sshd increments before it compares, 3 > 2).
if wants peer-silent-dropped; then
    t0=$SECONDS
    m=$(mark)
    peer_ts silent login 127.0.0.1 "$PORT_B" "${PW_LOGIN[@]}" \
        --keepalive ignore --script 'idle:14'
    sd_port=$(accepted_port "$m" password pwuser)
    sd_ms=$(drop_ms "$m" "$SILENT_FOR")
    sd_host=$(ts_gap silent 'peer: authenticated' 'peer: closed')
    echo "  peer-silent-dropped: dropped after ${sd_ms} ms of silence (server clock); ${sd_host}s (host); port ${sd_port:-?}"
    check "peer-silent-dropped: exactly two keepalives ignored, then closed (rc 4)" \
        peer_is "$PEER_RC" 4 "$TEST_TMPDIR/ssh-silent.log" \
            'peer: authenticated' 'peer: ignored keepalive 80' \
            'peer: ignored keepalive 80' 'peer: closed'
    check "peer-silent-dropped: dropped within the 9 s bound, no sooner than the third due tick (server clock, $sd_ms)" \
        within "$sd_ms" 6000 10000
    check "peer-silent-dropped: sanity -- the peer was closed within 20 s on the host ($sd_host)" \
        within "$sd_host" 0 20
    check "peer-silent-dropped: the guest sent them as global requests (2)" \
        test "$(since_re "$m" "$PROBE_GLOB_RE")" = 2
    check "peer-silent-dropped: the unresponsive line at INFO, once, for the peer's port" \
        test -n "$sd_port" -a "$(since "$m" | grep -cxE "[0-9:.]+ \[INFO\]  ssh: $SILENT from user pwuser ${HOST_AS_SEEN//./\\.} port $sd_port" || true)" = 1
    check "peer-silent-dropped: no disconnect: line" test "$(since_re "$m" "$DISC_RE")" = 0
    settle "$m" 1 peer-silent-dropped
    took peer-silent-dropped
fi

# --- disconnect-control -------------------------------------------------------
#
# The control for every "no disconnect: line" above: a KEXINIT before
# authentication is ended by disconnect(), whose DEBUG line must be visible
# at this log level -- otherwise those checks could not fail.
if wants disconnect-control; then
    t0=$SECONDS
    m=$(mark)
    dc_rc=0
    timeout 30 python3 "$PEER" login 127.0.0.1 "$PORT_B" "${PW_LOGIN[@]}" \
        --preauth send:20:00 > "$TEST_TMPDIR/ssh-dctl.log" 2>&1 || dc_rc=$?
    wait_since "$m" 1 10 "$DISC_RE" || true
    check "disconnect-control: the guest's disconnect: line is visible (CONTROL)" \
        test "$dc_rc" = 4 -a "$(since_re "$m" "${DBG}disconnect: key re-exchange before authentication")" = 1
    took disconnect-control
fi

hostile_finish
