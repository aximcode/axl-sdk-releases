#!/bin/bash
# test-meta: arch=both needs=ssh est=59 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-disconnect-qemu.sh -- AxlSsh P5's disconnect hygiene: a DISCONNECT
# sent on a healthy stream is DELIVERED, or the connection is reset within
# AXL_SSH_DISCONNECT_LINGER_MS (1 s).
#
# Before P5 every DISCONNECT was followed by an abortive close on the same
# stack, and the reset raced the packet: ssh(1) printed the reason or
# "Connection reset", and the suites that read it had to accept either. Now
# the server sends nothing after the DISCONNECT, keeps reading (and
# discarding) until the peer hangs up, and resets it then -- or at the
# bound, if it never does. So the reason is pinned here EXACTLY.
#
# Scenarios, in run order:
#   cap        a wrong password at a cap of 1: ssh(1) prints `Received
#              disconnect ...:14: Too many authentication failures` -- sshd's
#              own text (auth.c:343) -- and the guest logs the delivery
#   change     a CHANGE_FIRST verdict at a cap of 1: the banner AND the
#              DISCONNECT both reach ssh(1) (contract review H2: the banner run
#              and the DISCONNECT behind it)
#   norecv     a raw peer provokes a DISCONNECT, reads it, never closes: the
#              server resets it at the bound -- not before 1 s, not long after
#   flood      the same peer writing junk the whole time: input does not move
#              the deadline
#   close      a raw peer that hangs up 200 ms after reading the DISCONNECT:
#              the guest logs it delivered
#   earlyfin   the provoking bytes and a FIN in one go: the server still sends
#              its DISCONNECT, sees the hang-up, and resets at once -- not at
#              the bound, and not with a FIN (contract review H3)
#   chandata   an authenticated session streaming output, ended by a protocol
#              error: the DISCONNECT arrives behind the output already in
#              flight, with its reason, and NOTHING follows it (H2 rule 1)
#   evict      16 lingering connections -- the whole connection cap -- then a
#              login: the lingerers are the first eviction candidates (M6), so
#              it is served at once rather than refused for a second
#   penalty    on the SECOND server, which penalises this host: a protocol
#              error that lingers, then a knock inside its linger second is
#              refused -- the penalty accrued at the DISCONNECT, not when the
#              linger ended (M6)
#   free       the server freed (from a password hook) while connections
#              linger: they are reset, and the leak verdict stays clean
#
# WHICH LINE A HANG-UP GETS. The guest logs `linger: peer closed after N ms,
# disconnect sent` only when the send carrying the DISCONNECT had completed
# before the peer's FIN was dispatched -- "sent", not "read": EDK2 completes a
# Transmit when it takes the data; a peer that hangs up within a millisecond of reading it can
# race that completion. So the scenarios whose peer hangs up at once accept
# either hang-up line and pin the delivery from the CLIENT's side; `close`,
# whose peer waits 200 ms, pins "delivered". earlyfin's FIN is in the
# server's receive queue before the DISCONNECT is even sent, and still races
# the same completion -- EDK2 completes a Transmit when it takes the data, not
# when the peer ACKs it -- so the server cannot tell it from a fast client
# either, and it is graded the same way. What H3 promised is kept: the line
# is never "delivered" unless the send had completed first, and the hang-up
# ends in a reset, never a FIN.
#
# RESET, NEVER FIN, IS BY CONSTRUCTION, NOT GRADED. Every exit of the linger
# is conn_drop(..., AXL_TEARDOWN_RESET) (src/net/axl-ssh-server.c,
# linger_recv and on_linger_timer), and QEMU's user network can surface a
# guest RST to the host as either a reset or an EOF, so the raw peers -- which
# read both as "ended" -- cannot tell them apart. A graceful close there would
# pass every check here; the guarantee is the code's, held by review.
#
# AUTHENTICATED LINGERERS ARE NOT A SCENARIO OF THEIR OWN (review T6(f)).
# linger_victim and preauth_count never look at authentication state, so
# what evict proves for pre-authentication lingerers holds for any; the work
# that IS authentication-specific -- releasing channels and the principal at
# the DISCONNECT -- is chandata's (an authenticated session) and its settle.
#
#   brokenflush  a "txfail" hook (cap 1) arms the fixture's seam, which then
#              refuses the run carrying the cap's DISCONNECT: a stream that
#              lost its last run is reset at once -- no linger -- since the
#              peer could not decrypt anything after the hole (review M7).
#   cfail      a "txcfail" login arms the seam that reports the COMPLETION of
#              the next lingering connection's run as failed; a raw peer then
#              provokes a DISCONNECT and never hangs up. The linger ends at
#              once in a reset, with no linger line of its own -- a stream
#              that lost bytes TCP had accepted is never "delivered". NOT
#              driven by ssh(1) at the cap, as the review suggested: ssh(1)
#              hangs up the moment it reads the DISCONNECT, its FIN can be
#              dispatched before the completion (see WHICH LINE A HANG-UP
#              GETS), and the first full run lost exactly that race -- the
#              seam stayed armed and fired on norecv's connection instead
#
# ONE GUARD, AT THE DROP (final review F2). Whether a DISCONNECT lingers is
# decided in ONE place: conn_drop's axl_ssh_linger_due(..., tx_broken).
# disconnect_as marks the connection ST_LINGER whenever the DISCONNECT was
# queued, and every lost run -- refused (tx_flush), or failed at completion
# (on_sent) -- sets tx_broken and nothing else. MP1 had two guards, and each
# hid the other's removal. Now brokenflush fails if that argument is
# dropped, and cfail fails if on_sent stops treating a failed completion on
# a lingering connection as broken. MP1's own window -- a run AHEAD of the
# DISCONNECT that fails between disconnect() and a DEFERRED drop -- passes
# through that same conn_drop call; no fixture stages the deferred drop on
# demand (at cap 1 the banner and the DISCONNECT seal into ONE run), so the
# predicate's full truth table is AxlTestSsh's (test_ssh_linger_due).
#
# MP2 IS HELD BY CODE REVIEW, NOT GRADED (final review F13). conn_linger
# sets the deadline BEFORE conn_release_app, which may dispatch the loop; a
# lingerer read there with a deadline of 0 would be the first one reset or
# evicted. No scenario stages a release that dispatches the loop at a linger
# start, so a reordering would pass every check here.
#
# The guest runs with sshloglevel=debug: the `linger:` lines are DEBUG, and
# each one is graded exactly. They carry their own prefix so that no
# `disconnect:` matcher in another suite sees them (contract review M5).
#
# Usage: ./test/integration/test-ssh-disconnect-qemu.sh [--arch X64|AARCH64]
#        SSH_HOSTILE_ONLY=<scenario> runs one.

HOSTILE_SUMMARY="ssh disconnect"
HOSTILE_SCENARIOS="cap change brokenflush cfail norecv flood close earlyfin chandata evict penalty free"

# Seconds each scenario may take, summed into the fixture's deadline. The raw
# peers wait at most their --hold; the logins are one key exchange each.
declare -A HOSTILE_BUDGET=(
    [cap]=5 [change]=5 [brokenflush]=6 [cfail]=6 [norecv]=8 [flood]=8 [close]=4 [earlyfin]=8
    [chandata]=20 [evict]=12 [penalty]=10 [free]=14
)

PW_CHANGE=$(od -An -tx1 -N8 /dev/urandom | tr -d ' \n')
PW_BROKEN=$(od -An -tx1 -N8 /dev/urandom | tr -d ' \n')
PW_CFAIL=$(od -An -tx1 -N8 /dev/urandom | tr -d ' \n')
HOSTILE_EXTRA_CREDS="changeme:${PW_CHANGE}:change
brokenuser:${PW_BROKEN}:txfail
cfailuser:${PW_CFAIL}:txcfail"

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
# AUTHFAIL:NOAUTH:GRACE:MIN:MAX. NOAUTH at or above MIN: ONE connection that
# ends unauthenticated activates a penalty -- on the second server, which is
# the only one that does not exempt this host.
PEN_SPEC="12:15:10:14:40"
PORT_B=$(test_port 1)
# maxstartups=16: evict needs all sixteen of its peers ADMITTED, so that the
# connection cap -- not MaxStartups' eviction of silent peers -- is what the
# login meets.
HOSTILE_EXTRA_ARGS="2223 ${GRACE_SEC} maxpwfail=1 maxstartups=16 channels=1 sshloglevel=debug"
HOSTILE_EXTRA_FWD=("$PORT_B" 2223)

hostile_boot "AxlSsh disconnect hygiene"

source "$(dirname "$0")/lib/ssh-channel-lib.sh"

# The guest's DEBUG lines carry "ssh:<function>:<line>: " ahead of the text.
DBG='.*\[DEBUG\] ssh:[a-z_]+:[0-9]+: '
LINGER_DELIVERED="${DBG}linger: peer closed after [0-9]+ ms, disconnect sent"
LINGER_RESET="${DBG}linger: no close within 1000 ms; reset"
LINGER_EVICTED="${DBG}linger: evicted for a new arrival; reset"
LINGER_FREED="${DBG}linger: server freed; reset"
ANY_LINGER="${DBG}linger: .*"
# A peer that hangs up as soon as it has read the DISCONNECT -- ssh(1) does,
# within a millisecond -- can have its FIN dispatched in the same loop turn
# as the completion of the send that carried it, and in either order. The
# server says "delivered" only when the completion came FIRST (contract
# review H3: never claim a delivery it did not see), so for such a peer
# either line is right, and the CLIENT's transcript is the delivery proof.
# What is never right is the reset at the bound.
PEER_CLOSED="${DBG}linger: peer closed (after [0-9]+ ms, disconnect sent|before the disconnect was sent)"

# ssh(1)'s line for our cap DISCONNECT: lib/ssh-guest-lib.sh's CAP_DISC_RE,
# the one copy, without its anchors -- `change` puts the banner in front of
# it on the same line, and line_count matches whole lines (-x) anyway.
CAP_LINE=${CAP_DISC_RE#^}
CAP_LINE=${CAP_LINE%\$}

# A password login with ssh(1) as $2/$3, transcript to $1 (CRs dropped: the
# lines are compared whole). The prompt count is in $1.count. PW_TRY_V=1
# adds -v, for the "Authenticated to" line the teardown's login count reads
# from an ssh-*.log.
pw_try() {
    : > "$1.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$3" ASKPASS_COUNT="$1.count" \
    timeout 30 ssh ${PW_TRY_V:+-v} -p "${4:-$HOST_PORT}" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=15 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password -o NumberOfPasswordPrompts=3 \
        "$2"@127.0.0.1 true 2>&1 < /dev/null | tr -d '\r' > "$1" || true
    DUMP_LOGS+=("$1")
}
prompts() { wc -l < "$1.count" | tr -d ' '; }
has_line()  { grep -qxE -- "$2" "$1"; }
line_count() { local n; n=$(grep -cxE -- "$2" "$1" || true); echo "${n:-0}"; }

# A raw linger peer (ssh-hostile.py linger-<mode>), transcript to $2.
linger_peer() {
    timeout 30 python3 "$PROBE" "linger-$1" 127.0.0.1 "${3:-$HOST_PORT}" \
        --hold "${4:-6}" > "$2" 2>&1 || true
    DUMP_LOGS+=("$2")
}
# Seconds the raw peer in $1 reported until the server closed, as an integer
# number of milliseconds, or -1.
ended_ms() {
    local s
    s=$(sed -n 's/^linger: ended after \([0-9.]*\) s$/\1/p' "$1" | head -1)
    if [[ -z "$s" ]]; then echo -1; return; fi
    awk -v s="$s" 'BEGIN { printf "%d\n", s * 1000 }'
}
in_band() { local v; v=$(ended_ms "$1"); [[ "$v" -ge "$2" && "$v" -le "$3" ]]; }

# The bound is 1000 ms from the DISCONNECT; the peer measures from the write
# that provoked it, through QEMU's user network both ways. Not before 900 ms
# (a reset at once is the pre-P5 behaviour); not after 2500 ms (a 1 s bound
# that is not enforced, or a deadline that input moved).
BAND_LO=900
BAND_HI=2500

# --- cap ------------------------------------------------------------------------
if wants cap; then
    m=$(mark)
    pw_try "$TEST_TMPDIR/cap.log" pwuser "$PW_WRONG"
    check "cap: one prompt at a cap of 1" test "$(prompts "$TEST_TMPDIR/cap.log")" = 1
    check "cap: ssh(1) prints the reason, exactly (CAP_DISC_RE)" \
        test "$(line_count "$TEST_TMPDIR/cap.log" "$CAP_LINE")" = 1
    check "cap: the guest saw the peer hang up, not the bound" \
        wait_since "$m" 1 10 "$PEER_CLOSED"
    check "cap: and no other linger line" \
        test "$(since_re "$m" "$ANY_LINGER")" = 1
fi

# --- change ---------------------------------------------------------------------
if wants change; then
    m=$(mark)
    pw_try "$TEST_TMPDIR/change.log" changeme "$PW_CHANGE"
    # The banner has no final newline, so ssh(1) prints the DISCONNECT's line
    # straight after it: one line, banner first, is both arriving in order.
    check "change: the CHANGE_FIRST banner, then the DISCONNECT, reach ssh(1), exactly" \
        test "$(line_count "$TEST_TMPDIR/change.log" "password change required: use the web UI$CAP_LINE")" = 1
    check "change: the guest saw the peer hang up, not the bound" \
        wait_since "$m" 1 10 "$PEER_CLOSED"
    check "change: and no other linger line" \
        test "$(since_re "$m" "$ANY_LINGER")" = 1
fi

# --- brokenflush ---------------------------------------------------------------
if wants brokenflush; then
    m=$(mark)
    pw_try "$TEST_TMPDIR/brokenflush.log" brokenuser "$PW_BROKEN"
    check "brokenflush: the seam refused the run carrying the DISCONNECT (SERVER: TX-REFUSED)" \
        wait_since "$m" 1 10 'SERVER: TX-REFUSED'
    check "brokenflush: the guest queued the cap's DISCONNECT" \
        wait_since "$m" 1 5 "${DBG}disconnect: Too many authentication failures"
    # Two seconds: a linger would end at its 1 s bound and say so.
    sleep 2
    check "brokenflush: no linger on a broken stream" \
        test "$(since_re "$m" "$ANY_LINGER")" = 0
    check "brokenflush: ssh(1) never read the DISCONNECT" \
        test "$(line_count "$TEST_TMPDIR/brokenflush.log" "$CAP_LINE")" = 0
fi

# --- cfail ----------------------------------------------------------------------
if wants cfail; then
    m=$(mark)
    # The login arms the seam and ends normally; no connection lingers yet.
    # Named ssh-*.log, with -v: the teardown counts its principal release.
    PW_TRY_V=1 pw_try "$TEST_TMPDIR/ssh-cfail-login.log" cfailuser "$PW_CFAIL"
    check "cfail: the arming login was let in" \
        grep -q '^Authenticated to 127\.0\.0\.1 ' "$TEST_TMPDIR/ssh-cfail-login.log"
    check "cfail: the seam was still armed when the raw peer came (no TX-COMPLETE-FAILED yet)" \
        test "$(since_lines "$m" 'SERVER: TX-COMPLETE-FAILED')" = 0
    m=$(mark)
    # A raw peer that never hangs up: the completion of its DISCONNECT's run
    # cannot be beaten by a FIN, so the seam fires on THIS connection.
    linger_peer norecv "$TEST_TMPDIR/cfail.log"
    check "cfail: the seam failed the completion of the DISCONNECT's run (SERVER: TX-COMPLETE-FAILED)" \
        wait_since "$m" 1 10 'SERVER: TX-COMPLETE-FAILED'
    check "cfail: the server took the completion as a lost run, exactly" \
        wait_since "$m" 1 5 "${DBG}tx: send failed \\(-1\\)"
    # Whether the peer read the DISCONNECT is not graded: EDK2 completed the
    # Transmit when it TOOK the bytes, and the reset that follows at once
    # can discard them before they leave -- which is the point.
    check "cfail: and was reset at once, not at the bound ($(ended_ms "$TEST_TMPDIR/cfail.log") ms)" \
        in_band "$TEST_TMPDIR/cfail.log" 0 $((BAND_LO - 1))
    check "cfail: no linger line" \
        test "$(since_re "$m" "$ANY_LINGER")" = 0
fi

# --- norecv ---------------------------------------------------------------------
if wants norecv; then
    m=$(mark)
    linger_peer norecv "$TEST_TMPDIR/norecv.log"
    check "norecv: the peer read the DISCONNECT, exactly" \
        has_line "$TEST_TMPDIR/norecv.log" 'linger: disconnect 2 "protocol error"'
    check "norecv: the server closed it inside [$BAND_LO, $BAND_HI] ms ($(ended_ms "$TEST_TMPDIR/norecv.log") ms)" \
        in_band "$TEST_TMPDIR/norecv.log" "$BAND_LO" "$BAND_HI"
    check "norecv: the guest logged the reset at the bound" \
        wait_since "$m" 1 5 "$LINGER_RESET"
fi

# --- flood ----------------------------------------------------------------------
if wants flood; then
    m=$(mark)
    linger_peer flood "$TEST_TMPDIR/flood.log"
    check "flood: the peer read the DISCONNECT" \
        has_line "$TEST_TMPDIR/flood.log" 'linger: disconnect 2 "protocol error"'
    check "flood: input does not move the deadline: closed inside [$BAND_LO, $BAND_HI] ms ($(ended_ms "$TEST_TMPDIR/flood.log") ms)" \
        in_band "$TEST_TMPDIR/flood.log" "$BAND_LO" "$BAND_HI"
    check "flood: the guest logged the reset at the bound" \
        wait_since "$m" 1 5 "$LINGER_RESET"
    check "flood: the server still answers" python3 "$PROBE" alive 127.0.0.1 "$HOST_PORT"
fi

# --- close ----------------------------------------------------------------------
if wants close; then
    m=$(mark)
    linger_peer close "$TEST_TMPDIR/close.log"
    check "close: the peer read the DISCONNECT and closed" \
        file_is "$TEST_TMPDIR/close.log" 'linger: disconnect 2 "protocol error"' \
        'linger: closed by us'
    check "close: the guest logged it delivered (the peer waited 200 ms to hang up)" \
        wait_since "$m" 1 5 "$LINGER_DELIVERED"
fi

# --- earlyfin -------------------------------------------------------------------
if wants earlyfin; then
    m=$(mark)
    linger_peer earlyfin "$TEST_TMPDIR/earlyfin.log"
    check "earlyfin: the guest saw the hang-up, not the bound" \
        wait_since "$m" 1 5 "$PEER_CLOSED"
    check "earlyfin: and no other linger line" \
        test "$(since_re "$m" "$ANY_LINGER")" = 1
    check "earlyfin: the peer still read the DISCONNECT, exactly" \
        has_line "$TEST_TMPDIR/earlyfin.log" 'linger: disconnect 2 "protocol error"'
fi

# --- chandata -------------------------------------------------------------------
if wants chandata; then
    m=$(mark)
    # 4 MB of output, then DATA to a channel never opened while it is still
    # streaming: the DISCONNECT is sealed behind output already queued or in
    # flight, and nothing may be sealed after it.
    peer chandata "send:90:$OPEN,expect:91,send:98:$(exec_req 'stream 4000000'),hold:0.5,send:94:$(hexof chan-data-hex --recipient 999 --text x),idle:15" \
        --report-disconnect
    chandata_tail() {
        tail -3 "$TEST_TMPDIR/ssh-chandata.log" | file_is /dev/stdin \
            'peer: disconnect 2 "channel: nonexistent channel"' \
            'peer: 0 packet(s) after the disconnect' 'peer: closed'
    }
    check "chandata: the DISCONNECT arrives behind the streamed output, and nothing after it" \
        chandata_tail
    check "chandata: the peer's run ended as a close (exit 4)" test "$PEER_RC" = 4
    # The peer reads on after the DISCONNECT to count what follows it, so
    # it waits for the SERVER to close: the bound ends it.
    check "chandata: the guest reset the draining peer at the bound" \
        wait_since "$m" 1 10 "$LINGER_RESET"
    settle "$m" 1 chandata
fi

# --- evict ----------------------------------------------------------------------
if wants evict; then
    m=$(mark)
    timeout 30 python3 "$PROBE" linger-hold 127.0.0.1 "$HOST_PORT" --count 16 \
        --hold 4 > "$TEST_TMPDIR/evict-hold.log" 2>&1 &
    BG_PIDS+=("$!")
    holder=$!
    DUMP_LOGS+=("$TEST_TMPDIR/evict-hold.log")
    # The arrival must come while all sixteen linger -- inside a second -- so
    # the holder makes it itself, the moment it has provoked them. It is
    # served only if a lingerer makes room: at the connection cap with no
    # other candidate it would be refused at accept.
    wait_line "$TEST_TMPDIR/evict-hold.log" 'linger-hold: knock ident' 15 || true
    # How many had read their DISCONNECT before the knock is printed for the
    # reader, not graded: under load one can still be in flight, and the
    # eviction line below is what says a lingerer made the room.
    echo "  evict: $(grep -h 'disconnected' "$TEST_TMPDIR/evict-hold.log" || true)"
    check "evict: an arrival at the connection cap is served (the server speaks first)" \
        grep -qxF 'linger-hold: knock ident' "$TEST_TMPDIR/evict-hold.log"
    wait "$holder" 2>/dev/null || true
    check "evict: and a login still works afterwards" good_login_works evict
    check "evict: the guest evicted at least one lingerer to make room" \
        test "$(since_re "$m" "$LINGER_EVICTED")" -ge 1
    check "evict: no arrival was refused" \
        test "$(since_lines "$m" "Refused connection from 10.0.2.2")" = 0
fi

# --- penalty --------------------------------------------------------------------
if wants penalty; then
    m=$(mark)
    # A lingerer on the SECOND server, held open for 3 s, so its linger runs
    # the whole second; the knock goes in while it does.
    timeout 30 python3 "$PROBE" linger-norecv 127.0.0.1 "$PORT_B" --hold 3 \
        > "$TEST_TMPDIR/pen-linger.log" 2>&1 &
    BG_PIDS+=("$!")
    pen_holder=$!
    wait_line "$TEST_TMPDIR/pen-linger.log" 'linger: disconnect 2 "protocol error"' 10 || true
    python3 "$PROBE" knock 127.0.0.1 "$PORT_B" --count 1 \
        > "$TEST_TMPDIR/pen-knock.log" 2>&1 || true
    DUMP_LOGS+=("$TEST_TMPDIR/pen-knock.log" "$TEST_TMPDIR/pen-linger.log")
    wait "$pen_holder" 2>/dev/null || true
    # BY ORDER IN THE GUEST LOG, not by the clock (midpoint review MP4): the
    # knock's refusal must come BEFORE the lingerer's reset. A knock that
    # landed after the reset would be refused by an accrual at the END of
    # the linger too, and prove nothing.
    pen_order() {
        local refused reset
        refused=$(since "$m" | grep -naE 'drop connection from .* penalty: ' | head -1 | cut -d: -f1)
        reset=$(since "$m" | grep -naE "$LINGER_RESET" | head -1 | cut -d: -f1)
        [[ -n "$refused" && -n "$reset" && "$refused" -lt "$reset" ]]
    }
    check "penalty: the knock was refused INSIDE the linger (before its reset, in the guest log)" \
        pen_order
    check "penalty: ...and was refused: the penalty accrued at the DISCONNECT" \
        grep -qE '^knock: count=1 closed=1 ident=0 ' "$TEST_TMPDIR/pen-knock.log"
fi

# --- free -----------------------------------------------------------------------
# LAST: the server is gone afterwards. freeuser's hook dispatches the loop for
# 3 s and then frees the server from inside itself; two lingerers are
# provoked inside that window, so the free finds them lingering.
if wants free; then
    m=$(mark)
    : > "$TEST_TMPDIR/free.log.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_FREE" ASKPASS_COUNT="$TEST_TMPDIR/free.log.count" \
    timeout 60 ssh -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=15 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        freeuser@127.0.0.1 true > "$TEST_TMPDIR/free.log" 2>&1 < /dev/null &
    BG_PIDS+=("$!")
    freer=$!
    # The hook frees the server FREE_AFTER_MS (1 s) into its dispatch: two
    # lingerers provoked inside that second, the first at once.
    wait_since "$m" 1 20 'SERVER: PW-LEN 16' || true
    linger_peer norecv "$TEST_TMPDIR/free-a.log" "$HOST_PORT" 4 &
    free_a=$!
    BG_PIDS+=("$free_a")
    sleep 0.3
    linger_peer norecv "$TEST_TMPDIR/free-b.log" "$HOST_PORT" 4
    # By PID: a bare `wait` would wait for QEMU too.
    wait "$free_a" 2>/dev/null || true
    wait "$freer" 2>/dev/null || true
    test_wait_for "SERVER: FREE-" 30 || true
    check "free: the server was freed inside the hook (SERVER: FREED-IN-HOOK)" \
        test "$(since_lines "$m" 'SERVER: FREED-IN-HOOK')" = 1
    check "free: the free found a connection lingering and reset it" \
        test "$(since_re "$m" "$LINGER_FREED")" -ge 1
fi

hostile_finish
