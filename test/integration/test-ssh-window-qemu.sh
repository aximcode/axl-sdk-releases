#!/bin/bash
# test-meta: arch=both needs=ssh est=56 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-window-qemu.sh -- AxlSsh inbound flow control that follows the
# APPLICATION: a channel in AXL_SSH_CHANNEL_WINDOW_ON_CONSUME reopens its
# peer's window only as axl_ssh_channel_consumed() reports bytes, so a reader
# that holds input stops its peer after one window -- sshd's backpressure.
#
# The unit tests pin the window table's arithmetic (AxlTestSsh "chan
# consume"); what only a running server shows is the WIRING: that a report
# made from a loop timer, long after the data hook, reaches the wire as ONE
# WINDOW_ADJUST from the loop; that switching back to ON_ARRIVAL releases a
# peer stuck at window 0; and that a real ssh(1) really does stop.
#
# The guest runs ssh-server-selftest.efi with `channels=1`; the fixture's
# exec "hoard N ACT" (see THE FIXTURE SHELL in that file) is the application
# under test. Setup and teardown checks are lib/ssh-hostile-lib.sh's; the
# peer helpers and message bodies are lib/ssh-channel-lib.sh's.
#
# Scenarios, in run order:
#   held-then-consumed  a full window poured blind, nothing consumed: no
#                       WINDOW_ADJUST at all until the timer reports the
#                       2 MB, then exactly one for all of it
#   held-then-arrival   the same, released by switching back to ON_ARRIVAL
#                       (the zero-window deadlock an abandoning application
#                       must be able to end); then ON_ARRIVAL's own adjust
#   split-one-adjust    the 2 MB reported in eight calls from one timer:
#                       ONE adjust of 2 MB, not eight
#   overrun-empty       window + grace unconsumed, an EMPTY data packet
#                       (which restores sshd's grace), then one byte: the
#                       direct UNCONSUMED_MAX bound ends the connection
#   real-client-blocks  a real ssh(1) piping 6 MB + 1000 into a channel that
#                       consumes 2 MB at a time: every timer finds EXACTLY
#                       one window held -- ssh sent nothing past it -- and
#                       every byte arrives
#   inline-consume      consumed() from inside the data hook, for its own
#                       buffer: 8 MB flows as ON_ARRIVAL would
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-window-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh channel window"
HOSTILE_SCENARIOS="held-then-consumed held-then-arrival split-one-adjust overrun-empty real-client-blocks inline-consume"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh). Each pours 2-8 MB, which the host's pure-Python
# crypto seals at about a millisecond a packet and the guest decrypts under
# TCG on aa64; about three times the measurement.
declare -A HOSTILE_BUDGET=(
    [held-then-consumed]=12 [held-then-arrival]=14 [split-one-adjust]=12
    [overrun-empty]=14 [real-client-blocks]=20 [inline-consume]=16
)

if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host -- real-client-blocks needs a real session"
    exit 77
fi

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
HOSTILE_EXTRA_ARGS="channels=1 sshloglevel=debug"

hostile_boot "AxlSsh consume-driven channel windows"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"

# --- helpers ------------------------------------------------------------------

# The server's DEBUG disconnect line with reason ERE $2, after mark $1 (see
# test-ssh-channel-hostile-qemu.sh's since_disc).
since_disc() {
    local n
    n=$(since "$1" | grep -cE -- "ssh:[a-z_]+:[0-9]+: disconnect: $2\$" || true)
    echo "${n:-0}"
}

# The `sink` report for N bytes of 'x', as `expectdata` prints it, and its
# length on the wire.
xsum() { nsum x "$@"; }
sink_line() { printf '%s\n' "peer: data out \"\\nsink $1 $(xsum "$1")\\n\""; }
sink_len() { local l; l=$'\n'"sink $1 $(xsum "$1")"$'\n'; echo "${#l}"; }

# What the exec hook prints for every "hoard": ON_CONSUME set (0), set again
# (0), an unlisted mode refused (-4 AXL_INVALID), the idle count read as 0, a
# NULL output refused (-4), a report of 0 accepted (0) and of 1 with nothing
# held refused (-4).
HOARD_LINE='SERVER: HOARD mode=0 same=0 bad=-4 idle=0,0 nullout=-4 zero=0 one=-4'

# The timer that found 2 MB held and released it: the over-count and SIZE_MAX
# reports refused (-4) before the act, the act accepted, nothing left after.
TIMER_2M='SERVER: HOARD-TIMER unconsumed=0,2097152 over=-4 huge=-4 act=0 after=0,0'

# One real ssh(1) run of exec $2 as root, stdin from $3; stdout to NAME.out.
SSH_PK=(-p "$HOST_PORT" -o StrictHostKeyChecking=no
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes
        -o ConnectTimeout=20 -o IdentitiesOnly=yes -o IdentityAgent=none
        -o PreferredAuthentications=publickey -i "$TEST_TMPDIR/good")
sess() {
    SESS_RC=0
    timeout 90 ssh -v -E "$TEST_TMPDIR/ssh-$1.log" "${SSH_PK[@]}" \
        root@127.0.0.1 "$2" > "$TEST_TMPDIR/$1.out" \
        2> "$TEST_TMPDIR/$1.err" < "$3" || SESS_RC=$?
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$1.log")
}

# The `sink` report for file $1, exactly as the channel carries it.
file_sink() { printf '\nsink %s %s\n' "$(stat -c %s "$1")" "$(sha256sum < "$1" | awk '{print $1}')"; }

# --- held-then-consumed -------------------------------------------------------
#
# Exactly one window (2,097,152 bytes, 64 packets) poured without reading.
# The fixture consumes none of it until all of it is held, then reports it
# from a timer 200 ms later. If any byte had been credited on arrival, the
# server would have sent an adjust DURING the pour -- ON_ARRIVAL sends one
# once the window is more than three maximum packets below its size -- and
# it would be the first message the peer reads.
# So the 93 that follows the pour carrying exactly 2,097,152, and the timer's
# count of 2,097,152 held, together say no credit left before the report.
if wants held-then-consumed; then
    m=$(mark)
    peer held-then-consumed "send:90:$OPEN,expect:91,send:98:$(exec_req 'hoard 2097152 all'),expect:99,expect:93,pour:2097152:32768,expect:93,send:96:$RCPT,expectdata:$(sink_len 2097152),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "held-then-consumed: no adjust while held; one for the whole window after the report" \
        transcript held-then-consumed 0 "$OPENED" "$(got91 held-then-consumed)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 2097152 in 64 packets seqs=4..67' \
            'peer: got 93 bytes=2097152' 'peer: sent 96 seq=68' \
            "$(sink_line 2097152)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=69'
    settle "$m" 1 held-then-consumed
    check "held-then-consumed: the hook's window calls, the timer's 2 MB held, nothing at EOF" \
        test "$(since_lines "$m" "$HOARD_LINE")" = 1 \
            -a "$(since_lines "$m" "$TIMER_2M")" = 1 \
            -a "$(since_re "$m" 'SERVER: HOARD-TIMER .*')" = 1 \
            -a "$(since_lines "$m" 'SERVER: HOARD-EOF unconsumed=0,0')" = 1
fi

# --- held-then-arrival -------------------------------------------------------
#
# The same hold, released by axl_ssh_channel_set_window_mode(ON_ARRIVAL) from
# the timer: every unconsumed byte is credited at once, so the stuck peer
# gets its whole window back without a single report. After it the channel
# is ON_ARRIVAL: 1 MB + 1 more draws ON_ARRIVAL's own adjusts -- 128 KB
# every fourth packet, eight of them -- with nothing consumed by the
# application at all.
if wants held-then-arrival; then
    m=$(mark)
    peer held-then-arrival "send:90:$OPEN,expect:91,send:98:$(exec_req 'hoard 2097152 arrival'),expect:99,expect:93,pour:2097152:32768,expect:93,pour:1048577:32768,expect:93,expect:93,expect:93,expect:93,expect:93,expect:93,expect:93,expect:93,send:96:$RCPT,expectdata:$(sink_len 3145729),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "held-then-arrival: switching back releases the window; then ON_ARRIVAL's own adjust" \
        transcript held-then-arrival 0 "$OPENED" "$(got91 held-then-arrival)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 2097152 in 64 packets seqs=4..67' \
            'peer: got 93 bytes=2097152' \
            'peer: poured 1048577 in 33 packets seqs=68..100' \
            'peer: got 93 bytes=131072' 'peer: got 93 bytes=131072' \
            'peer: got 93 bytes=131072' 'peer: got 93 bytes=131072' \
            'peer: got 93 bytes=131072' 'peer: got 93 bytes=131072' \
            'peer: got 93 bytes=131072' 'peer: got 93 bytes=131072' \
            'peer: sent 96 seq=101' \
            "$(sink_line 3145729)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=102'
    settle "$m" 1 held-then-arrival
    check "held-then-arrival: the timer found 2 MB held and left none; none at EOF" \
        test "$(since_lines "$m" "$TIMER_2M")" = 1 \
            -a "$(since_lines "$m" 'SERVER: HOARD-EOF unconsumed=0,0')" = 1
fi

# --- split-one-adjust -----------------------------------------------------------
#
# Eight reports from one timer, between two loop iterations: the calls only
# count, and the flush sends what they made due -- ONE adjust of 2 MB. Eight
# adjusts would put a second 93 where the transcript expects the reply to
# the EOF.
if wants split-one-adjust; then
    m=$(mark)
    peer split-one-adjust "send:90:$OPEN,expect:91,send:98:$(exec_req 'hoard 2097152 split'),expect:99,expect:93,pour:2097152:32768,expect:93,send:96:$RCPT,expectdata:$(sink_len 2097152),expect:98,expect:96,expect:97,send:97:$RCPT"
    check "split-one-adjust: eight reports in one timer, one WINDOW_ADJUST" \
        transcript split-one-adjust 0 "$OPENED" "$(got91 split-one-adjust)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 2097152 in 64 packets seqs=4..67' \
            'peer: got 93 bytes=2097152' 'peer: sent 96 seq=68' \
            "$(sink_line 2097152)" \
            'peer: got 98 name=exit-status want=0 rest=00000000' \
            'peer: got 96' 'peer: got 97' 'peer: sent 97 seq=69'
    settle "$m" 1 split-one-adjust
    check "split-one-adjust: the timer's eight reports all accepted" \
        test "$(since_lines "$m" "$TIMER_2M")" = 1
fi

# --- overrun-empty -------------------------------------------------------------
#
# The window and the whole 10% grace (2,306,867 bytes) poured into a channel
# that consumes nothing: within sshd's grace, so the connection is still up
# (the first keepalive). An EMPTY data packet then fits the window -- in
# sshd that resets its grace counter, so a peer alternating overruns with
# empty packets is never stopped -- and the connection is still up (the
# second keepalive). One byte more takes the bytes held past
# AXL_SSH_CHANNEL_UNCONSUMED_MAX, and the direct bound ends the connection
# although the grace counter says 1.
if wants overrun-empty; then
    m=$(mark)
    peer overrun-empty "send:90:$OPEN,expect:91,send:98:$(exec_req 'hoard 0 none'),expect:99,expect:93,pour:2306867:32768,$KEEP,send:94:$(data ''),$KEEP,pour:1:32768,expect:82"
    check "overrun-empty: window + grace held, an empty packet, then one byte ends it" \
        transcript overrun-empty 4 "$OPENED" "$(got91 overrun-empty)" \
            'peer: sent 98 seq=3' 'peer: got 99' 'peer: got 93 bytes=2097152' \
            'peer: poured 2306867 in 71 packets seqs=4..74' \
            'peer: sent 80 seq=75' 'peer: got 82' 'peer: sent 94 seq=76' \
            'peer: sent 80 seq=77' 'peer: got 82' \
            'peer: poured 1 in 1 packets seqs=78..78' 'peer: closed'
    settle "$m" 1 overrun-empty
    check "overrun-empty: the guest ended it for data past the window, once" \
        test "$(since_disc "$m" 'channel: data past the window')" = 1
fi

# --- real-client-blocks -------------------------------------------------------
#
# A real ssh(1) with 6 MB + 1000 bytes of stdin; the fixture consumes 2 MB at
# a time, 200 ms after it holds them. ssh(1) honours the window, so each
# timer must find EXACTLY 2,097,152 held: a client that could send past it
# would have had 200 ms to do so. Three such timers, then the last 1000
# bytes and EOF with 1000 still held -- and the sink's SHA-256 says every
# byte arrived, in order.
if wants real-client-blocks; then
    m=$(mark)
    head -c 6292456 /dev/urandom > "$TEST_TMPDIR/rcb.in"
    sess real-client-blocks 'hoard 2097152 all' "$TEST_TMPDIR/rcb.in"
    file_sink "$TEST_TMPDIR/rcb.in" > "$TEST_TMPDIR/rcb.want"
    check "real-client-blocks: ssh(1) exits 0 with every byte's sink report" \
        test "$SESS_RC" = 0
    check "real-client-blocks: the sink report is exact" \
        cmp -s "$TEST_TMPDIR/rcb.want" "$TEST_TMPDIR/real-client-blocks.out"
    settle "$m" 1 real-client-blocks
    check "real-client-blocks: three timers, each found exactly one window held" \
        test "$(since_lines "$m" "$TIMER_2M")" = 3 \
            -a "$(since_re "$m" 'SERVER: HOARD-TIMER .*')" = 3
    check "real-client-blocks: EOF with the last 1000 bytes held" \
        test "$(since_lines "$m" 'SERVER: HOARD-EOF unconsumed=0,1000')" = 1
fi

# --- inline-consume ----------------------------------------------------------------
#
# consumed() from inside the data hook, for that hook's own buffer, every
# time: the channel then behaves as ON_ARRIVAL -- 8 MB through a real
# ssh(1), nothing held at EOF, no report refused.
if wants inline-consume; then
    m=$(mark)
    head -c 8388608 /dev/urandom > "$TEST_TMPDIR/ic.in"
    sess inline-consume 'hoard 0 inline' "$TEST_TMPDIR/ic.in"
    file_sink "$TEST_TMPDIR/ic.in" > "$TEST_TMPDIR/ic.want"
    check "inline-consume: ssh(1) exits 0 with every byte's sink report" \
        test "$SESS_RC" = 0
    check "inline-consume: the sink report is exact" \
        cmp -s "$TEST_TMPDIR/ic.want" "$TEST_TMPDIR/inline-consume.out"
    settle "$m" 1 inline-consume
    check "inline-consume: nothing held at EOF, no report refused" \
        test "$(since_lines "$m" 'SERVER: HOARD-EOF unconsumed=0,0')" = 1 \
            -a "$(since_re "$m" 'SERVER: HOARD-INLINE -?[0-9]+')" = 0
fi

if [[ "$ABUSE" == 1 ]]; then
    check "every scenario: no channel hook nested inside another of its channel's" \
        test "$(since_lines 0 'SERVER: HOOK-NESTED')" = 0
    check "every scenario: no fixture write, exit or timer failed" \
        test "$(since_re 0 'SERVER: (WRITE|EXIT)-FAIL -?[0-9]+')" = 0 \
            -a "$(since_re 0 'SERVER: FAIL .*')" = 0 \
            -a "$(since_lines 0 'SERVER: HOARD-TIMER gone')" = 0
fi

hostile_finish
