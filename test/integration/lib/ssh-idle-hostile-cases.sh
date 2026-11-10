# shellcheck shell=bash
# ssh-idle-hostile-cases.sh -- the body of the AxlSsh idle suites (sshd's
# UnusedConnectionTimeout and ClientAliveInterval / ClientAliveCountMax;
# spec docs/superpowers/specs/2026-08-21-axl-ssh-design.md section 12a.5)
# against HOSTILE byte-level peers (ssh-peer.py): above all the peer that
# output backpressure has paused and that then half-closes, which held its
# slot for as long as it did not read until the idle rules bounded it.
# lib/ssh-idle-cases.sh proves the same rules against a real ssh(1).
#
# THREE TESTS, ONE BODY. This was test-ssh-idle-hostile-qemu.sh, one boot
# running all five scenarios; one boot is the unit the runner's pool
# parallelises, so it was split by scenario into three thin wrappers that
# set HOSTILE_SUMMARY and HOSTILE_SCENARIOS and source this file -- the
# setup, the five servers and every scenario stay here, once:
#
#   test-ssh-idle-hostile-pause-qemu.sh    paused-half-close
#                                          keepalive-held-in-rekey
#                                          pause-mid-rekey
#   test-ssh-idle-hostile-control-qemu.sh  paused-half-close-control
#   test-ssh-idle-hostile-churn-qemu.sh    channel-churn-unused
#
# A part's scenarios must be a subsequence of HOSTILE_SCENARIOS_ALL below,
# in its order, and the parts together must name all of them
# (lib/ssh-hostile-lib.sh checks both, before the boot). Every part boots
# all five servers, so each scenario keeps its server and its place in that
# server's history. NOT its place in the BOOT's: several counts read every
# server's lines -- keepalive-held-in-rekey's probes_between, and
# pause-mid-rekey's pause, resume and rekey counts since its mark. After the
# split keepalive-held-in-rekey follows paused-half-close directly, and the
# holders that scenario killed were still probed during its exchange, so it
# now waits for a quiet boot first (wait_probe_quiet). pause-mid-rekey now
# follows keepalive-held-in-rekey, whose rekey lines on B all come before
# its own mark.
#
# NOT A TEST. lib/discover.sh schedules test/integration/test-*.sh only.
#
# ONE BOOT, FIVE SERVERS. Each scenario's settings leave ONE mechanism that
# can end it; a peer that answers keepalives is immune to client-alive.
#
#   server  port       settings                      scenarios
#   A       HOST_PORT  channels=1 rekeylimit=65536   channel-churn-unused
#                      unused=3 alive=5:2 (tick
#                      1500 ms)
#   B       PORT_B     unused 0, alive 2:3,          keepalive-held-in-rekey
#                      rekeylimit 65536 (tick 1000)
#   C       PORT_C     unused 0, alive 2:2 (1000),   paused-half-close
#                      channels3=1
#   D       PORT_D     unused 0, alive 0:0,          paused-half-close-control
#                      channels4=1
#   E       PORT_E     unused 0, alive 2:10 (1000),  pause-mid-rekey
#                      channels5=1
#
# B to E are the fixture's password-only servers (port2, port3=, port4=,
# port5=); B has no channel hooks, so every CHANNEL_OPEN there is refused.
#
# THE DROP BOUND is (count_max + 1) x (interval + tick) after the peer was
# last heard: 9 s for C, 33 s for E. Every bound is read off the SERVER's clock (the
# DEBUG `idle: dropping, ... for N ms` lines, lib/ssh-idle-lib.sh); host
# waits are loose sanity limits. The bound already spends a whole tick per
# probe interval; what it cannot contain is a tick the loop DISPATCHES late,
# and C's check allows 1 s for that across its three deciding ticks, as
# lib/ssh-idle-cases.sh does: the window holds fifteen connected holders,
# and a loaded host delays a dispatch whatever they are doing.
#
# THE ONE-FREE-SLOT SERVER. C and D are each filled with fifteen real
# `ssh -N` holders before their scenario (hostile_fill_slots): an
# authenticated connection is never evicted and ssh(1) answers every probe,
# so the paused peer takes the sixteenth slot and a later arrival gets in
# only if the paused peer's slot was freed. The holders log in by public
# key, which these servers accept with NO principal (no publickey hook), so
# their transcripts are hold-*.log rather than ssh-*.log: hostile_finish
# counts one principal release per ssh-*.log login, and so is the
# follow-up `ssh host true` kept out of that count (followup-*.log).
#
# sshloglevel=debug for the whole boot: the pause, probe, rekey and drop
# lines are DEBUG, and at the default level a DEBUG line is ABSENT.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage (a part): ./test/integration/test-ssh-idle-hostile-<part>-qemu.sh [--arch X64|AARCH64]

HOSTILE_SCENARIOS_ALL="paused-half-close paused-half-close-control keepalive-held-in-rekey channel-churn-unused pause-mid-rekey"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh): the CEILING the boot ends at only when the
# host's finish signal never comes, so a passing run does not pay it.
# Measured 2026-10-10 at host load 0-1, x64 and aa64 within 1 s of each
# other: paused-half-close 16, control 27,
# held-in-rekey 4-5, churn 14-15; mid-rekey 21 (both arches, host load
# 2-3, two suites at once). Each budget is about 1.5x that, and at least
# twice it where the scenario is a few seconds. keepalive-held-in-rekey's
# also covers the >= 4 s of quiet it now waits for before it begins.
declare -A HOSTILE_BUDGET=(
    [paused-half-close]=24 [paused-half-close-control]=36
    [keepalive-held-in-rekey]=15 [channel-churn-unused]=22
    [pause-mid-rekey]=32
)

if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host -- the one-free-slot holders are real ssh(1)"
    exit 77
fi

# Five forwarded ports (HOST_PORT and PORT_B..PORT_E), claimed when the
# harness is sourced.
TEST_PORT_SLOTS=5
source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
REKEY_LIMIT=65536
PORT_B=$(test_port 1)
PORT_C=$(test_port 2)
PORT_D=$(test_port 3)
PORT_E=$(test_port 4)
HOSTILE_EXTRA_ARGS="2223 ${GRACE_SEC} channels=1 sshloglevel=debug rekeylimit=$REKEY_LIMIT unused=3 alive=5:2 unused2=0 alive2=2:3 rekeylimit2=$REKEY_LIMIT port3=2224 unused3=0 alive3=2:2 channels3=1 port4=2225 unused4=0 alive4=0:0 channels4=1 port5=2226 unused5=0 alive5=2:10 channels5=1"
HOSTILE_EXTRA_FWD=("$PORT_B" 2223 "$PORT_C" 2224 "$PORT_D" 2225 "$PORT_E" 2226)

hostile_boot "AxlSsh idle against hostile peers"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"
source "$(dirname "$0")/lib/ssh-idle-lib.sh"

# --- helpers ------------------------------------------------------------------

ssh_pk_to() {
    printf '%s\n' -p "$1" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes \
        -o ConnectTimeout=20 -o IdentitiesOnly=yes -o IdentityAgent=none \
        -o PreferredAuthentications=publickey -i "$TEST_TMPDIR/good"
}
ACCEPTED_PK='.*Accepted publickey for root from .*'
PAUSE_RE="${DBG}tx: pausing input, [0-9]+ bytes queued"
OURS_RE="${DBG}rekey: started by us, [0-9]+ bytes out, [0-9]+ in"
DONE_RE="${DBG}rekey: complete"
HELD_RE="${DBG}rekey: sealed [1-9][0-9]* held after our NEWKEYS"
NOSUCH=$(hexof open-hex --type nosuch)
IGN=$(head -c 16000 /dev/zero | od -An -tx1 -v | tr -d ' \n')
PW_LOGIN=(--user pwuser --password "$PW_OK")

# A password login to port $1 that does nothing more: transcript NAME.log
# ($2; ssh-*.log, so a success owes a principal release). Sets SLOT_RC.
slot_login() {
    SLOT_RC=0
    timeout 30 python3 "$PEER" login 127.0.0.1 "$1" "${PW_LOGIN[@]}" \
        > "$TEST_TMPDIR/ssh-$2.log" 2>&1 || SLOT_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$2.log")
}

# THE PAUSED PEER -- a dead reader, built the way one really goes dead.
# A flood of requests alone does not hold the server paused: its replies
# (8,000 or 24,000 OPEN_FAILUREs, 0.35-1 MB) drain into QEMU's host socket
# and the peer's receive buffer within milliseconds whether or not the peer
# reads (measured: every pause resumed 11-38 ms later, with default buffers
# and with a 4 KB SO_RCVBUF), and a half-close then reaches a server that
# is reading again. So the host's buffers are filled FIRST:
#   1. a session with the largest window (2^32-1) and an exec "stream" of
#      STREAM_BYTES, after which the peer reads nothing;
#   2. the host waits until the server's stream output stops leaving --
#      no send of a stream-sized run (8 KB or more) for QUIET_SEC -- with
#      the host side full, the transmit queue then sits at or above TXQ_LOW,
#      which stops channel output (rule 7 in axl-ssh-server.c);
#   3. only then the 8,000 opens: their replies go straight onto that queue,
#      past TXQ_HIGH, and the input pause engages and STAYS.
# The peer sends an IGNORE every second while it waits (`chatter`): it has
# stopped reading, not talking, and client-alive would otherwise end it
# before the flood -- correctly, but that is not the path under test. After
# the pause it half-closes and holds, reading nothing.
#
# STREAM_BYTES: measured 2026-10-10, the stream stops leaving after
# 2,886,824 bytes on both arches (44 stream-sized sends; run to run the same
# figure, the host's buffers being what fills). 16 MiB is over five times
# that, so the stream is still running -- never finished -- when it stalls.
STREAM_BYTES=16777216
QUIET_SEC=2
OPEN_BIG=$(hexof chan-open-hex --window 4294967295 --maxpkt 32768)
TX_RUN_RE="${DBG}tx: [0-9]+ frames, [0-9]+ bytes"
# The stream-sized sends after mark $1: "<count> <bytes>".
stream_sends() {
    since "$1" | grep -oE -- "^${TX_RUN_RE}\$" | awk '$(NF - 1) + 0 >= 8192 {
        n++; b += $(NF - 1) } END { print n + 0, b + 0 }' || true
}
# pause_peer PORT NAME [OPEN STREAM FLOOD TAIL PEERARGS] -- the whole
# choreography above, against the server on PORT, transcript ssh-NAME.log.
# The optional five replace the session open (OPEN_BIG), the stream's length
# (STREAM_BYTES), the flood action(s) (8,000 opens), what the peer does
# once the pause line was seen (half-close and hold), and add login options
# for ssh-peer.py (none). Sets PP_M (the mark before it),
# PP_FILLED (0 once the stream stopped leaving), PP_FILL ("<sends>
# <bytes>"), PP_FILL_SECS, PP_TF (the host's clock as the flood was let
# go), PP_PAUSED (0 once the pause line came), PP_TP (the host's clock at
# that line) and PAUSED_PID.
pause_peer() {
    local port="$1" name="$2" g1="$TEST_TMPDIR/$2.fill" g2="$TEST_TMPDIR/$2.pause"
    local open="${3:-$OPEN_BIG}" bytes="${4:-$STREAM_BYTES}"
    local flood="${5:-flood:8000:90:$NOSUCH}" tail="${6:-shutdown,hold:60}"
    local -a extra=()
    read -r -a extra <<< "${7:-}"
    local t0 last="" cur quiet=0 i
    PP_M=$(mark)
    t0=$EPOCHREALTIME
    timeout 120 python3 -u "$PEER" login 127.0.0.1 "$port" "${PW_LOGIN[@]}" \
        ${extra[@]+"${extra[@]}"} --script "send:90:$open,expect:91,send:98:$(exec_req "stream $bytes"),chatter:1:$g1,$flood,await:$g2,$tail" \
        > "$TEST_TMPDIR/ssh-$name.log" 2>&1 &
    PAUSED_PID=$!
    BG_PIDS+=("$PAUSED_PID")
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$name.log")
    PP_FILLED=1
    for ((i = 0; i < 60 * 4; i++)); do
        sleep 0.25
        cur=$(stream_sends "$PP_M")
        if [[ "${cur%% *}" -gt 0 && "$cur" == "$last" ]]; then
            quiet=$((quiet + 1))
            if (( quiet >= QUIET_SEC * 4 )); then PP_FILLED=0; break; fi
        else
            quiet=0
        fi
        last=$cur
    done
    PP_FILL=$last
    PP_FILL_SECS=$(secs "$t0" "$EPOCHREALTIME")
    PP_TF=$EPOCHREALTIME
    touch "$g1"
    PP_PAUSED=0
    wait_since "$PP_M" 1 30 "$PAUSE_RE" || PP_PAUSED=$?
    PP_TP=$EPOCHREALTIME
    touch "$g2"
}
# The paused peer's transcript $1 is exactly its lines, and its flood
# took the seqs right after its K IGNOREs. Whether the flood's write
# finished before the half-close depends on how much of it the socket
# buffers took, so that line is either form -- each pinned whole.
paused_transcript() {
    local f="$TEST_TMPDIR/ssh-$1.log" k w
    k=$(grep -oxE 'peer: chattered [0-9]+' "$f" | grep -oE '[0-9]+$' || true)
    [[ -n "$k" ]] || return 1
    for w in 'peer: wrote 8000 x 90' 'peer: flood cut short'; do
        if file_is "$f" 'peer: authenticated' 'peer: sent 90 seq=2' \
               "$(grep -xE "$GOT91" "$f" | head -1)" 'peer: sent 98 seq=3' \
               "peer: chattered $k" \
               "peer: writing 8000 x 90 seqs=$((4 + k))..$((4 + k + 7999))" \
               "$w" 'peer: shut down writing'; then
            return 0
        fi
    done
    return 1
}
RESUME_RE="${DBG}tx: resuming input, peak [0-9]+ bytes queued"

# Seconds, to 0.1, between two $EPOCHREALTIME readings.
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f", b - a }'; }

# A transcript with the lines a probe adds taken out and the seqs dropped:
# a keepalive answer is a packet, so how many the server's probes drew (a
# timing matter) moves every later seq. What the answers were is graded
# separately, against the guest's own probe count.
norm() {
    grep -vxE 'peer: answered keepalive (80|98)|peer: rekeyed, started by the server' \
        "$TEST_TMPDIR/ssh-$1.log" | sed -E 's/^(peer: sent [0-9]+) seq=[0-9]+$/\1/' || true
}
answered() { local n; n=$(grep -cxE 'peer: answered keepalive (80|98)' "$TEST_TMPDIR/ssh-$1.log" || true); echo "${n:-0}"; }
# The guest's probe lines strictly between the first line matching ERE $2
# and the first after it matching ERE $3, after mark $1.
# The EREs go through the environment, not -v: awk -v processes escapes,
# and turns the patterns' \[ into [.
probes_between() {
    since "$1" | PB_A="$2" PB_B="$3" \
        PB_P="${DBG}idle: keepalive probe [0-9]+, as a (global|channel) request" awk '
        !in_ && $0 ~ ("^" ENVIRON["PB_A"] "$") { in_ = 1; next }
        in_ && $0 ~ ("^" ENVIRON["PB_B"] "$") { exit }
        in_ && $0 ~ ("^" ENVIRON["PB_P"] "$") { n++ }
        END { print n + 0 }'
}

# --- paused-half-close --------------------------------------------------------
#
# Server C (unused 0, alive 2:2), one free slot: fifteen holders, then the
# paused peer (pause_peer above), which half-closes as soon as the pause
# line is seen. Paused, the server parses nothing and reads nothing more,
# so the peer is never heard again and nothing it sends can answer a probe:
# the bound holds from the last bytes heard, where the receive parked at
# the pause. The pause must PERSIST up to the drop -- no resume line. A
# probe due while the output is paused is COUNTED but not queued (spec
# 12a.7 deviation 8), so none of its probes reaches probe 2 (the holders
# answer every one of theirs, and so never do either), and the third due
# tick ends it with the unresponsive line.
if wants paused-half-close; then
    t0=$SECONDS
    m=$(mark)
    mapfile -t SSH_C < <(ssh_pk_to "$PORT_C")
    hostile_fill_slots 15 "$ACCEPTED_PK" hold-c "${SSH_C[@]}" -N root@127.0.0.1
    # hostile_fill_slots has already waited for all fifteen accept lines
    # (its last wave); this only replaces a fixed second after it with that
    # same condition, which holds at once.
    wait_since "$m" 15 10 "$ACCEPTED_PK" || true
    hc_held=$(since_re "$m" "$ACCEPTED_PK")
    pause_peer "$PORT_C" halfclose
    hc_full_rc=0
    timeout 20 python3 "$PEER" kexwait 127.0.0.1 "$PORT_C" --hold 1 \
        > "$TEST_TMPDIR/full-c.log" 2>&1 || hc_full_rc=$?
    hc_dropped=0
    wait_since "$PP_M" 1 25 ".*ssh(:[a-z_]+:[0-9]+)?: $SILENT from user pwuser .*" || hc_dropped=$?
    hc_td=$EPOCHREALTIME
    hc_ms=$(drop_ms "$PP_M" "$SILENT_FOR")
    hc_port=$(accepted_port "$PP_M" password pwuser)
    hc_resumed=$(since_re "$PP_M" "$RESUME_RE")
    echo "  paused-half-close: stream stopped leaving after ${PP_FILL_SECS}s (sends bytes: $PP_FILL);" \
         "dropped after ${hc_ms} ms of silence (server clock), $(secs "$PP_TP" "$hc_td")s after the pause (host); resumes $hc_resumed"
    hc_follow_rc=0
    timeout 30 ssh -v -E "$TEST_TMPDIR/followup-c.log" "${SSH_C[@]}" \
        root@127.0.0.1 true > /dev/null 2>&1 < /dev/null || hc_follow_rc=$?
    DUMP_LOGS+=("$TEST_TMPDIR/followup-c.log")
    check "paused-half-close PRECONDITION: 15 holders logged in ($hc_held)" \
        test "$hc_held" = 15
    check "paused-half-close GUARD (the proof is the persisted pause below): the stream stopped leaving -- host buffers full" \
        test "$PP_FILLED" = 0
    check "paused-half-close PRECONDITION: the server paused the peer's input" \
        test "$PP_PAUSED" = 0 -a "$(since_re "$PP_M" "$PAUSE_RE")" = 1
    check "paused-half-close PRECONDITION: the server was full while it was paused (peer: refused, rc 5)" \
        peer_is "$hc_full_rc" 5 "$TEST_TMPDIR/full-c.log" 'peer: refused'
    check "paused-half-close: the peer opened, streamed, flooded once the stream stalled, and half-closed" \
        paused_transcript halfclose
    check "paused-half-close: the pause persisted until the drop (no resume line)" \
        test "$hc_resumed" = 0
    check "paused-half-close: the unresponsive line at INFO, once, for the peer's port" \
        test "$hc_dropped" = 0 -a -n "$hc_port" \
            -a "$(since "$PP_M" | grep -cxE "[0-9:.]+ \[INFO\]  ssh: $SILENT from user pwuser ${HOST_AS_SEEN//./\\.} port $hc_port" || true)" = 1
    check "paused-half-close: dropped within the 9 s bound + 1 s, no sooner than the third due tick (server clock, $hc_ms)" \
        within "$hc_ms" 6000 10000
    check "paused-half-close: sanity -- the drop came within the bound + 3 s of the pause on the host ($(secs "$PP_TP" "$hc_td")s)" \
        within "$(secs "$PP_TP" "$hc_td")" 0 12
    check "paused-half-close: no probe was queued while paused (no 'probe 2' line)" \
        test "$(since_re "$PP_M" "${DBG}idle: keepalive probe 2, as a (channel|global) request")" = 0
    check "paused-half-close: no disconnect: line (abortive, as sshd's cleanup_exit)" \
        test "$(since_re "$PP_M" "$DISC_RE")" = 0
    check "paused-half-close: the follow-up ssh host true got the freed slot (rc $hc_follow_rc)" \
        test "$hc_follow_rc" = 0
    kill "$PAUSED_PID" 2>/dev/null || true
    wait "$PAUSED_PID" 2>/dev/null || true
    hostile_release_slots
    took paused-half-close
fi

# --- paused-half-close-control ------------------------------------------------
#
# The same peer against server D (unused 0, alive 0:0): nothing may end it,
# so the drop above is the idle rules' and nothing else's. First the control
# for `--keepalive ignore` itself: against a server that never probes, a peer
# that ignores keepalives prints no `ignored keepalive` line -- the lines the
# other scenarios count are the server's probes, not something the peer
# makes up. Then, with one slot free, the paused peer takes it; past the
# bound + 5 s the pause has not resumed and the next arrival is REFUSED.
# Ending the peer gives the slot back.
if wants paused-half-close-control; then
    t0=$SECONDS
    m=$(mark)
    peer_ts ignorectl login 127.0.0.1 "$PORT_D" "${PW_LOGIN[@]}" \
        --keepalive ignore --script 'idle:5'
    check "paused-half-close-control: --keepalive ignore prints no ignored line against a server that never probes" \
        peer_is "$PEER_RC" 0 "$TEST_TMPDIR/ssh-ignorectl.log" 'peer: authenticated'
    # D has no unused timeout, so freeing its last started session restarts
    # nothing and says nothing. The answered global request after the CLOSE
    # orders the check after the slot was freed: the server handles them in
    # turn on one connection.
    m0=$(mark)
    peer_ts unusedoff login 127.0.0.1 "$PORT_D" "${PW_LOGIN[@]}" \
        --script "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:97:$RCPT,expect:97,global:x@y:1,expect:82"
    check "paused-half-close-control: a started session opened and closed on D (unused off), then a global request answered" \
        test "$PEER_RC" = 0 -a "$(since_re "$m0" "${DBG}chan [0-9]+: closed")" = 1
    check "paused-half-close-control: ...and freeing it restarted no unused clock (no restart line)" \
        test "$(since_re "$m0" "$RESTART_RE")" = 0
    mapfile -t SSH_D < <(ssh_pk_to "$PORT_D")
    m1=$(mark)
    hostile_fill_slots 15 "$ACCEPTED_PK" hold-d "${SSH_D[@]}" -N root@127.0.0.1
    wait_since "$m1" 15 10 "$ACCEPTED_PK" || true
    pc_held=$(since_re "$m1" "$ACCEPTED_PK")
    pause_peer "$PORT_D" halfclose-ctl
    # The bound (9 s) + 5 s, on the HOST clock from the pause line. D runs
    # no idle rule, so the guest writes no line that times this wait; the
    # host's elapsed reading at the refused arrival is printed as
    # information. It is not checked: after `sleep 14` it cannot be less.
    sleep 14
    pc_resumed=$(since_re "$PP_M" "$RESUME_RE")
    slot_login "$PORT_D" control-refused
    pc_refused_rc=$SLOT_RC
    pc_el=$(secs "$PP_TP" "$EPOCHREALTIME")
    m3=$(mark)
    kill "$PAUSED_PID" 2>/dev/null || true
    wait "$PAUSED_PID" 2>/dev/null || true
    pc_gone=0
    wait_since "$m3" 1 20 'SERVER: PRINCIPAL-RELEASED' || pc_gone=$?
    echo "  paused-half-close-control: stream stopped leaving after ${PP_FILL_SECS}s (sends bytes: $PP_FILL); resumes in 14 s: $pc_resumed"
    slot_login "$PORT_D" control-back
    check "paused-half-close-control PRECONDITION: 15 holders logged in ($pc_held)" \
        test "$pc_held" = 15
    check "paused-half-close-control GUARD (the proof is the persisted pause below): the stream stopped leaving -- host buffers full" \
        test "$PP_FILLED" = 0
    check "paused-half-close-control PRECONDITION: the server paused the peer's input" \
        test "$PP_PAUSED" = 0 -a "$(since_re "$PP_M" "$PAUSE_RE")" = 1
    check "paused-half-close-control: the peer opened, streamed, flooded once the stream stalled, and half-closed" \
        paused_transcript halfclose-ctl
    echo "  paused-half-close-control: the refused arrival came ${pc_el}s after the pause line (host)"
    check "paused-half-close-control: the pause persisted past C's 9 s bound + 5 s (no resume line)" \
        test "$pc_resumed" = 0
    check "paused-half-close-control: ...and the peer still held the slot -- the next arrival was refused (${pc_el}s after the pause line, host)" \
        peer_is "$pc_refused_rc" 5 "$TEST_TMPDIR/ssh-control-refused.log" 'peer: refused'
    check "paused-half-close-control: no idle line and no probe meanwhile" \
        test "$(idle_count "$PP_M" pwuser "($INACTIVE|$SILENT)")" = 0 \
            -a "$(since_re "$PP_M" "($PROBE_GLOB_RE|$PROBE_CHAN_RE)")" = 0
    check "paused-half-close-control: ending the peer released its connection" \
        test "$pc_gone" = 0
    check "paused-half-close-control: ...and the slot came back" \
        peer_is "$SLOT_RC" 0 "$TEST_TMPDIR/ssh-control-back.log" 'peer: authenticated'
    hostile_release_slots
    took paused-half-close-control
fi

# --- keepalive-held-in-rekey --------------------------------------------------
#
# Server B (alive 2:3, rekeylimit 65536). Five 16,000-byte IGNOREs carry the
# server's inbound count past the limit, so the SERVER starts an exchange;
# the peer answers its KEXINIT at once and then holds its KEX_ECDH_INIT for
# 3.5 s, past one probe interval (a probe comes due 2-3 s after the peer was
# last heard, at a 1 s tick). That probe falls between the server's KEXINIT
# and its NEWKEYS, so it must be HELD -- the peer's wire check ends the run
# as `peer: got 80 inside the server's exchange` if it is not -- and sealed
# after the server's NEWKEYS, where the peer, which ignores keepalives,
# reads it. Every KEX packet the server decodes resets the count (sshd's
# packet.c:1814), and the stall is far inside the drop bound,
# (3 + 1) x (2 + 1) = 12 s.
if wants keepalive-held-in-rekey; then
    t0=$SECONDS
    # The probes it counts are anyone's: run it on a quiet boot. The longest
    # probe interval + tick of a server with connections before it is C's
    # 2 s + 1 s (paused-half-close's holders), so 4 s of quiet.
    hk_quiet=0
    wait_probe_quiet 4 20 || hk_quiet=$?
    m=$(mark)
    peer_ts heldka login 127.0.0.1 "$PORT_B" "${PW_LOGIN[@]}" --keepalive ignore \
        --script "burst:5:2:$IGN,rekey:server:3.5,idle:1,global:x@y:1,expect:82"
    hk_ign=$(lines_of "$TEST_TMPDIR/ssh-heldka.log" 'peer: ignored keepalive 80')
    hk_in=$(probes_between "$m" "$OURS_RE" "$DONE_RE")
    hk_x=$(ts_gap heldka 'peer: sent burst 5 x 2 seqs=2..6' 'peer: rekeyed, started by the server')
    echo "  keepalive-held-in-rekey: $hk_ign ignored after the exchange; $hk_in probe(s) inside it; exchange ${hk_x}s (host) against the 12 s bound"
    hk_want=('peer: authenticated' 'peer: sent burst 5 x 2 seqs=2..6'
             'peer: rekeyed, started by the server')
    for ((i = 0; i < hk_ign; i++)); do hk_want+=('peer: ignored keepalive 80'); done
    hk_want+=('peer: sent 80 seq=0' 'peer: got 82')
    check "keepalive-held-in-rekey PRECONDITION: no other connection was being probed as it began (4 s quiet)" \
        test "$hk_quiet" = 0
    check "keepalive-held-in-rekey: nothing 50+ inside the server's exchange; the held keepalive came after its NEWKEYS" \
        peer_is "$PEER_RC" 0 "$TEST_TMPDIR/ssh-heldka.log" "${hk_want[@]}"
    check "keepalive-held-in-rekey: at least one probe fell inside the exchange, and each reached the peer after it ($hk_in in, $hk_ign after)" \
        test "$hk_in" -ge 1 -a "$hk_ign" = "$hk_in"
    check "keepalive-held-in-rekey: one exchange, the server's, completed, holding output" \
        test "$(since_re "$m" "$OURS_RE")" = 1 -a "$(since_re "$m" "$DONE_RE")" = 1 \
            -a "$(since_re "$m" "$HELD_RE")" -ge 1
    settle "$m" 1 keepalive-held-in-rekey
    check "keepalive-held-in-rekey: no idle drop line" \
        test "$(idle_count "$m" pwuser "($INACTIVE|$SILENT)")" = 0
    took keepalive-held-in-rekey
fi

# --- channel-churn-unused -----------------------------------------------------
#
# Server A (unused 3, tick 1500 ms). Client-alive is ON here (5:2) because A
# also serves pause-mid-rekey; this peer answers every probe, so alive cannot
# end it, and what is graded is the server's exact restart line, not drop
# timing alone (a tick that divides the unused period hides a missed restart
# from timing).
#
# Two started sessions, held 5 s -- past the unused period, but in use. The
# SECOND is closed first: one is still in use, so the clock must NOT
# restart. `pick:0` points the peer back at the first, which it then closes:
# the last channel in use is freed, and the clock restarts there -- exactly
# one `unused clock restarts, 3000 ms` line. Two seconds on the peer is
# still connected (its global request is answered), and the drop comes more
# than 3000 ms after the restart, within a tick plus slack.
#
# The variant: one started session, half-closed (EOF, no CLOSE), held 5 s --
# still in use, as sshd never closes a session channel on EOF alone, so the
# peer is still connected; then it closes and leaves.
if wants channel-churn-unused; then
    t0=$SECONDS
    m=$(mark)
    peer_ts churn login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --script "$(printf '%s,' "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93" \
            "send:90:$OPEN1,expect:91,send:98:$SHELL_REQ,expect:99,expect:93" \
            "hold:5,send:97:$RCPT,expect:97,pick:0,send:97:$RCPT,expect:97" \
            "idle:2,global:x@y:1,expect:82")idle:8"
    ch_ms=$(drop_ms "$m" "$UNUSED_FOR")
    ch_still=$(awk '{ t = $1; $1 = ""; sub(/^ /, "") }
        $0 == "peer: got 97" { if (++k == 2) a = t } $0 == "peer: got 82" { b = t }
        $0 == "peer: closed" { c = t }
        END { if (a && b && c) printf "%.1f %.1f\n", b - a, c - a; else print "none none" }' \
        "$TEST_TMPDIR/churn.ts")
    echo "  channel-churn-unused: restart lines $(since_re "$m" "$RESTART_RE");" \
         "dropped ${ch_ms} ms after the restart (server clock); 82 / closed at ${ch_still}s after the release (host)"
    check "channel-churn-unused: two sessions, the second closed, then the first; connected 2 s on; then ended (rc 4)" \
        test "$PEER_RC" = 4 -a "$(norm churn)" = "$(printf '%s\n' 'peer: authenticated' \
            'peer: sent 90' "$(got91 churn)" 'peer: sent 98' 'peer: got 99' \
            'peer: got 93 bytes=2097152' 'peer: sent 90' \
            "$(grep -xE "$GOT91" "$TEST_TMPDIR/ssh-churn.log" | sed -n 2p)" \
            'peer: sent 98' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: sent 97' 'peer: got 97' 'peer: sent 97' 'peer: got 97' \
            'peer: sent 80' 'peer: got 82' 'peer: closed')" \
            -a "$(since_re "$m" "${DBG}chan [0-9]+: closed")" = 2
    # Where it sits: the server logs `chan N: closed` before it frees the
    # slot (chan_close_slot), so the LAST release's restart line follows
    # the SECOND closed line; a restart at the first release falls between
    # the two. Counting alone cannot tell those apart.
    ch_closed2=$(since "$m" | grep -nxE -- "${DBG}chan [0-9]+: closed" | sed -n 2p | cut -d: -f1 || true)
    ch_restart=$(since "$m" | grep -nxE -- "$RESTART_RE" | head -1 | cut -d: -f1 || true)
    check "channel-churn-unused: one restart line -- at the LAST channel's release, not the first's (restart at line ${ch_restart:-none}, the second closed at ${ch_closed2:-none})" \
        test "$(since_re "$m" "$RESTART_RE")" = 1 -a -n "$ch_closed2" -a -n "$ch_restart" \
            -a "${ch_restart:-0}" -gt "${ch_closed2:-0}"
    check "channel-churn-unused: ...and it set exactly the unused period, 3000 ms" \
        test "$(since_re "$m" "${DBG}idle: last channel freed; unused clock restarts, 3000 ms")" = 1
    check "channel-churn-unused: dropped more than 3000 ms after the restart, within a tick + 1 s (server clock, $ch_ms)" \
        above_within "$ch_ms" 3000 5500
    check "channel-churn-unused: one inactive line for pwuser, no unresponsive line, no disconnect: line" \
        test "$(idle_count "$m" pwuser "$INACTIVE")" = 1 \
            -a "$(idle_count "$m" pwuser "$SILENT")" = 0 \
            -a "$(since_re "$m" "$DISC_RE")" = 0
    check "channel-churn-unused: sanity -- still answered 2 s after the release (host: ${ch_still%% *}s)" \
        within "${ch_still%% *}" 2 8
    check "channel-churn-unused: sanity -- ended within 8 s of the release (host: ${ch_still##* }s)" \
        within "${ch_still##* }" 2 8
    m2=$(mark)
    peer_ts churneof login 127.0.0.1 "$HOST_PORT" "${PW_LOGIN[@]}" \
        --script "send:90:$OPEN,expect:91,send:98:$SHELL_REQ,expect:99,expect:93,send:96:$RCPT,hold:5,global:x@y:1,expect:82,send:97:$RCPT,expect:97"
    settle "$m2" 1 "channel-churn-unused (eof)"
    check "channel-churn-unused (eof): a half-closed session held 5 s is still in use -- connected, then closed by the peer (rc 0)" \
        test "$PEER_RC" = 0 -a "$(norm churneof)" = "$(printf '%s\n' 'peer: authenticated' \
            'peer: sent 90' "$(got91 churneof)" 'peer: sent 98' 'peer: got 99' \
            'peer: got 93 bytes=2097152' 'peer: sent 96' 'peer: sent 80' \
            'peer: got 82' 'peer: sent 97' 'peer: got 97')"
    check "channel-churn-unused (eof): the server saw the EOF, and logged no idle drop" \
        test "$(since_lines "$m2" 'SERVER: EOF')" = 1 \
            -a "$(idle_count "$m2" pwuser "($INACTIVE|$SILENT)")" = 0
    took channel-churn-unused
fi

# --- pause-mid-rekey ----------------------------------------------------------
#
# Server E (alive 2:10, the default rekey limit). The input pause with a key
# exchange's message waiting behind it: pause_peer's dead reader, but with
# a flood sized to pause the server ONCE and a rekey KEXINIT of the peer's
# queued right behind the flood's last open, so it is necessarily still
# unparsed in the server's input when the pause engages. The peer then
# reads again: the server drains, resumes, parses what was left of the
# flood and then the KEXINIT, and the exchange runs to completion; the
# stream, held while it runs, then finishes and the session closes.
#
# WHY THE PEER STARTS IT. A SERVER-started exchange cannot be open across
# an input pause: from its KEXINIT to its NEWKEYS every reply the flood
# draws is HELD (rekey: send_packet -> held_push), not queued, so the
# transmit queue stops growing and never reaches TXQ_HIGH -- the 128 KB
# held cap ends the connection first. And the pause check comes before
# the trigger on every packet. So the input pause can only find the
# PEER's kex message waiting, which is the case P4d's review asked for.
# A small server rekeylimit would in any case end the fill: the server's
# KEXINIT, unanswered by a peer that has stopped reading, holds the stream.
#
# ONE PAUSE, BY ARITHMETIC. An OPEN_FAILURE is 44 bytes sealed (17-byte
# payload). With maxpkt 4096 the fill leaves the queue between TXQ_LOW
# and TXQ_LOW + one 4 KB packet (rule 7), so MR_OPENS x 44 = 88,000 bytes
# of replies always cross TXQ_HIGH (needs > 64 KB), and what is left of
# the flood after the pause -- at most 88,000 - (128 - 68.2) KB, about
# 26.8 KB -- cannot cross it again on top of a refilled 68.2 KB. At
# maxpkt 32768 no flood size does both, which is why this open differs.
#
# WHY SERVER E. This scenario grades a pause during a rekey, not
# client-alive, so it runs where client-alive cannot end it first: E's
# drop bound is (10 + 1) x (2 + 1) = 33 s. On C's 9 s, a peer starved of
# CPU while it decrypts the stream back in pure Python went 7.2 s without
# answering a probe and was dropped (2 CPUs shared with two busy loops) --
# a correct drop, and C's own scenario, paused-half-close, is the one that
# grades it; C keeps its settings for that.
#
# THE MARGIN. Paused, the server hears nothing. The pause lasts as long as
# the host takes to see the pause line and the peer to start reading. It is
# graded on the SERVER's clock -- the timestamps of its own pause and
# resume DEBUG lines -- at 5 s, the bound it had on C (4 s inside C's 9 s);
# E's longer bound is not spent on it. A host-side reading
# (the flood let go to the resume line seen) also counts the host's polls
# and the peer's start, and measured 5.2 s at host load ~15 while the
# server's own pause was 4.3 s. The host reading is still reported. And
# no idle line at all.
#
# THE STREAM MUST OUTLAST THE HOST'S BUFFERS. The fill is the stream
# stopping because QEMU's host socket and the peer's are full; a stream the
# host can swallow whole just ends, nothing piles up behind the flood, and
# no pause comes. With both buffers autotuning (the peer's up to tcp_rmem's
# max, 6 MiB on this box; QEMU's send side up to tcp_wmem's, 4 MiB) a
# 4 MiB stream was not enough: 1 run in 15 had all of it absorbed (64
# sends, 4,231,364 bytes) and the peer timed out waiting for a pause that
# could not come. So the peer's receive buffer is PINNED (--rcvbuf
# MR_RCVBUF; the kernel doubles it and stops autotuning it -- far from the
# 4096 that stalls the loopback), and the stream is sized from what the
# host can then hold at most: MR_ABSORB_MAX = twice MR_RCVBUF plus QEMU's
# send buffer at tcp_wmem's max, read from this host. The stream is that
# plus MR_MARGIN (1 MiB: slirp's own buffers, the guest's paused 128 KB
# queue, framing). The PRECONDITIONs read the same numbers -- tcp_wmem's
# max must have been read, and the fill must stop short of the stream
# built from them -- so the bound and the size cannot drift apart. Not
# STREAM_BYTES' 16 MiB: the peer reads the whole stream back through
# pure-Python decryption in 4 KB packets, ~2 s a MiB.
#
# THE TRANSCRIPT HAS TWO LEGITIMATE ORDERS. The KEXINIT action seals its
# packet and queues it behind the flood if the flood's writer thread is
# still running (ssh-peer.py Conn.send, from_read), printing `sent 20`
# before the writer's `wrote` line; if the writer has already finished --
# 88 KB of opens can all go into the socket before the main thread gets
# there -- it joins the writer first, which prints `wrote`, then writes
# and prints `sent 20`. Either way the KEXINIT is on the wire right behind
# the last open, which is what the server-side checks grade (parsed only
# after the resume). Seen in 3 suite runs on a loaded box (2026-10-23/24)
# and in 1 of 30 looped runs, at host load ~15; never at load ~1. Both
# orders are pinned whole.
MR_RCVBUF=262144
MR_WMEM_MAX=$(awk '{ print $3 }' /proc/sys/net/ipv4/tcp_wmem 2> /dev/null || true)
MR_MARGIN=1048576
MR_ABSORB_MAX=$(( 2 * MR_RCVBUF + ${MR_WMEM_MAX:-0} ))
MR_STREAM=$(( MR_ABSORB_MAX + MR_MARGIN ))
MR_PEER_ARGS="--rcvbuf $MR_RCVBUF"
MR_OPENS=2000
OPEN_SMALL=$(hexof chan-open-hex --window 4294967295 --maxpkt 4096)
STARTED_PEER_RE="${DBG}rekey: started by the peer"
# The sha256 of the fixture's "stream N": byte k is k % 251.
# Built by doubling (the lib's stream_file), not one `cat` per 251 bytes:
# that loop forked ~33,000 times for 8 MiB and took tens of seconds.
stream_sha() {
    stream_file "$1" "$TEST_TMPDIR/stream-sha.bin"
    sha256sum < "$TEST_TMPDIR/stream-sha.bin" | cut -d' ' -f1
    rm -f "$TEST_TMPDIR/stream-sha.bin"
}
# Milliseconds on the SERVER's clock from the first line after mark $1
# matching ERE $2 to the first matching ERE $3: the clean log's DEBUG lines
# start with the guest's HH:MM:SS.ffffff. "none" unless both are found. A
# span across midnight wraps once.
server_ms() {
    local a b
    a=$(since "$1" | grep -xE -- "$2" | head -1 | grep -oE '^[0-9]+:[0-9]+:[0-9]+\.[0-9]+' || true)
    b=$(since "$1" | grep -xE -- "$3" | head -1 | grep -oE '^[0-9]+:[0-9]+:[0-9]+\.[0-9]+' || true)
    if [[ -z "$a" || -z "$b" ]]; then echo none; return; fi
    awk -v a="$a" -v b="$b" 'function t(x, f) { split(x, f, ":"); return f[1] * 3600 + f[2] * 60 + f[3] }
        BEGIN { d = t(b) - t(a); if (d < 0) d += 86400; printf "%d", d * 1000 }'
}
# The line number, after mark $1, of the first line matching ERE $2.
line_of() { since "$1" | grep -nxE -- "$2" | head -1 | cut -d: -f1 || true; }
if wants pause-mid-rekey; then
    t0=$SECONDS
    # How oversubscribed the host is while the pause runs: the pause ends
    # only once the host-side peer has decrypted the piled-up stream back in
    # pure Python, so its length is partly the peer's CPU. Sampled off a
    # sentinel that lives exactly as long as the scenario (host_load_sampler,
    # scripts/axl-common.sh -- the unit budget's machinery).
    mr_load="$TEST_TMPDIR/midrekey-load.txt"
    sleep 600 & mr_sentinel=$!
    BG_PIDS+=("$mr_sentinel")
    host_load_sampler "$mr_sentinel" "$mr_load" 0.25 &
    mr_sampler=$!
    pause_peer "$PORT_E" midrekey "$OPEN_SMALL" "$MR_STREAM" \
        "flood:$MR_OPENS:90:$NOSUCH,kexinit" \
        "rekey,expect:99,expectsum:$MR_STREAM:$MR_OPENS:92,expect:98,expect:96,expect:97,send:97:$RCPT" \
        "$MR_PEER_ARGS"
    mr_resumed=0
    wait_since "$PP_M" 1 30 "$RESUME_RE" || mr_resumed=$?
    mr_tres=$EPOCHREALTIME
    mr_rc=0
    wait "$PAUSED_PID" || mr_rc=$?
    mr_tr=$EPOCHREALTIME
    settle "$PP_M" 1 pause-mid-rekey
    mr_pause=$(line_of "$PP_M" "$PAUSE_RE")
    mr_resume=$(line_of "$PP_M" "$RESUME_RE")
    mr_kex=$(line_of "$PP_M" "$STARTED_PEER_RE")
    mr_done=$(line_of "$PP_M" "$DONE_RE")
    mr_figs=$(since "$PP_M" | grep -oE -- "(pausing input, |resuming input, peak )[0-9]+" | grep -oE '[0-9]+$' | tr '\n' ' ' || true)
    mr_ms=$(server_ms "$PP_M" "$PAUSE_RE" "$RESUME_RE")
    kill "$mr_sentinel" 2>/dev/null || true
    wait "$mr_sampler" 2>/dev/null || true
    # THE BOUND SCALES WITH MEASURED LOAD (Mike, 2026-11-07): 5000 ms on an
    # idle box -- a regression there still fails at 5 s -- widened by the
    # host's contention factor (mean runnable / CPUs, floor 1, cap 3x;
    # host_contention_factor). Unmeasured is 1.00: no widening. Under the
    # full suite at -j 16 the pause measured 5,028 and 5,445 ms against a
    # quiet box's ~4 s, the peer's decrypt starved of CPU.
    mr_factor=$(host_contention_factor "$mr_load") || mr_factor=1.00
    mr_bound=$(awk -v f="$mr_factor" 'BEGIN { printf "%d", 5000 * f }')
    echo "  pause-mid-rekey: stream stopped leaving after ${PP_FILL_SECS}s (sends bytes: $PP_FILL);" \
         "queue at pause / peak: ${mr_figs}; lines pause $mr_pause resume $mr_resume kex $mr_kex done $mr_done;" \
         "paused ${mr_ms} ms (server clock);" \
         "host: flood let go to resume seen $(secs "$PP_TF" "$mr_tres")s, pause seen to resume seen $(secs "$PP_TP" "$mr_tres")s, to peer done $(secs "$PP_TP" "$mr_tr")s; $(answered midrekey) keepalive(s) answered"
    mr_k=$(grep -oxE 'peer: chattered [0-9]+' "$TEST_TMPDIR/ssh-midrekey.log" | grep -oE '[0-9]+$' || true)
    mr_k=${mr_k:-0}
    check "pause-mid-rekey GUARD (the proof is the one pause below): the stream stopped leaving -- host buffers full" \
        test "$PP_FILLED" = 0
    check "pause-mid-rekey PRECONDITION: tcp_wmem's max was read from this host (${MR_WMEM_MAX:-none})" \
        test -n "$MR_WMEM_MAX" -a "${MR_WMEM_MAX:-0}" -gt 0
    check "pause-mid-rekey PRECONDITION: the fill stopped short of the stream's end (${PP_FILL##* } bytes sent of $MR_STREAM = $MR_ABSORB_MAX the host can hold + $MR_MARGIN)" \
        test "${PP_FILL##* }" -lt "$MR_STREAM"
    check "pause-mid-rekey: exactly one input pause, then one resume" \
        test "$PP_PAUSED" = 0 -a "$(since_re "$PP_M" "$PAUSE_RE")" = 1 \
            -a "$(since_re "$PP_M" "$RESUME_RE")" = 1 \
            -a -n "$mr_pause" -a -n "$mr_resume" -a "${mr_resume:-0}" -gt "${mr_pause:-0}"
    check "pause-mid-rekey: the peer's KEXINIT waited out the pause -- parsed only after the resume (lines $mr_resume < $mr_kex)" \
        test -n "$mr_kex" -a "${mr_kex:-0}" -gt "${mr_resume:-0}" \
            -a "$(since_re "$PP_M" "$STARTED_PEER_RE")" = 1 \
            -a "$(since_re "$PP_M" "$OURS_RE")" = 0
    check "pause-mid-rekey: the exchange completed (rekey: complete, once, after it began)" \
        test -n "$mr_done" -a "${mr_done:-0}" -gt "${mr_kex:-0}" \
            -a "$(since_re "$PP_M" "$DONE_RE")" = 1
    mr_got=$(norm midrekey)
    mr_ok=1
    for mr_order in "sent 20|wrote" "wrote|sent 20"; do
        mr_a="peer: ${mr_order%%|*}"; mr_b="peer: ${mr_order##*|}"
        mr_a=${mr_a/peer: wrote/peer: wrote $MR_OPENS x 90}
        mr_b=${mr_b/peer: wrote/peer: wrote $MR_OPENS x 90}
        if [[ "$mr_got" == "$(printf '%s\n' 'peer: authenticated' \
            'peer: sent 90' "$(got91 midrekey)" 'peer: sent 98' "peer: chattered $mr_k" \
            "peer: writing $MR_OPENS x 90 seqs=$((4 + mr_k))..$((4 + mr_k + MR_OPENS - 1))" \
            "$mr_a" "$mr_b" 'peer: rekeyed, started by us' \
            'peer: got 99' \
            "peer: sum out=$MR_STREAM err=0 sha256=$(stream_sha "$MR_STREAM")" \
            "peer: got $MR_OPENS x 92" \
            "peer: got 98 name=exit-status want=0 rest=00000000" 'peer: got 96' \
            'peer: got 97' 'peer: sent 97')" ]]; then
            mr_ok=0
        fi
    done
    check "pause-mid-rekey: the peer flooded, rekeyed, then read all $MR_STREAM bytes byte-exact and every reply, and closed (rc 0)" \
        test "$mr_rc" = 0 -a "$mr_ok" = 0
    check "pause-mid-rekey: the stream ran to its end (SERVER: STREAM-DONE $MR_STREAM)" \
        test "$(since_lines "$PP_M" "SERVER: STREAM-DONE $MR_STREAM")" = 1
    check "pause-mid-rekey: no idle drop line, no disconnect: line" \
        test "$(idle_count "$PP_M" pwuser "($INACTIVE|$SILENT)")" = 0 \
            -a "$(since_re "$PP_M" "$DISC_RE")" = 0
    check "pause-mid-rekey: paused at most 5000 ms x host load $mr_factor = $mr_bound ms on the server's clock ($mr_ms)" \
        test "$mr_resumed" = 0 -a "$mr_ms" != none -a "${mr_ms/none/99999}" -le "$mr_bound"
    took pause-mid-rekey
fi

hostile_finish
