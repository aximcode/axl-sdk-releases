#!/bin/bash
# test-meta: arch=both needs= est=126 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-admission-qemu.sh — AxlSsh's admission by eviction (spec
# docs/superpowers/specs/2026-08-21-axl-ssh-design.md section 12a.2, and
# axl_ssh_server_set_max_startups in include/axl/axl-ssh-server.h): the accept
# stays armed for as long as the server runs; a full server EVICTS an
# unauthenticated connection that has not finished key exchange -- the least
# far along in it, the oldest among equals -- and REFUSES a new connection only
# when there is no such victim.
#
# Scenarios, in run order:
#
#   silent-evict kexwait-refuse auth-safe rank-evict flood-churn
#   tx-backpressure penalty-first
#
# tx-backpressure is not admission: it is the OUTPUT limit (the server stops
# reading a peer whose replies it cannot send), and it lives here because this
# boot already has an authenticated-login server with DEBUG visible and room
# under CI's per-test limit. It authenticates, so it accrues no penalty and
# moves none of the arithmetic below.
#
# ONE BOOT, TWO SERVERS, because the scenarios need two pre-auth caps and the
# harness boots once per test (lib/ssh-hostile-lib.sh):
#
#   CAP10_PORT  the fixture's SECOND server: the shipped
#               AXL_SSH_MAX_STARTUPS_DEFAULT (10). silent-evict, flood-churn.
#   CAP2_PORT   the FIRST server, `maxstartups=2`. kexwait-refuse, auth-safe,
#               rank-evict, penalty-first:
#               two slots are cheap to fill with REAL key exchanges, and
#               ssh-peer.py's pure-Python one is the slow part of the test.
#
# PENALTIES ARE ON, AND ACTIVATE ONCE, DELIBERATELY, IN THE LAST SCENARIO.
# Every connection here comes from one
# address (QEMU's user-net gateway, 10.0.2.2), and the spec promises a
# penalty consequence for each admission outcome (12a.2): an EVICTED
# connection accrues its source `noauth`, like any connection that ends
# unauthenticated; a REFUSED one never became a connection and accrues
# nothing. Both halves are graded here, so neither server is exempt
# (HOSTILE_EXEMPT=- lifts the lib's default exemption of the first server --
# one variable, where giving the second server penalties of its own would be
# a new fixture option). PEN_SPEC gives NOAUTH 1 s and a threshold (MIN 600 s)
# no noauth total in this boot approaches -- about 90 accruals of 1 s,
# decaying meanwhile -- so until penalty-first nothing ACTIVATES, and every
# refusal the test sees is admission's, never the penalty ladder's.
# flood-churn asserts that no activation line appeared up to that point.
# AUTHFAIL (596 s) is paid only by penalty-first's one wrong password, and
# puts CAP2's source 4 s under the threshold; that scenario's evictions then
# activate it. Nothing connects to CAP2 afterwards.
#
# The accruals are counted from the guest's DEBUG line, one per connection
# that accrued (penalty_accrue), measured as:
#   [DEBUG] ssh:penalty_accrue:2981: penalty of 1 seconds for 10.0.2.2:
#           connections without attempting authentication
# (one line; the cause is sshd's wording for `noauth`). N is the source's
# running total, so only the kind and the source are pinned.
#
# sshloglevel=debug (the whole boot): the audit log writes one INFO line per
# kind per minute and the rest at DEBUG, which at the default level is ABSENT,
# not demoted. Counting eviction and refusal lines -- auth-safe's "no
# eviction happened" above all -- is only a real observation with DEBUG
# visible.
#
# THE LOGIN GRACE (60 s, both servers) is above every hold here (15 s at
# most), so no connection in this test is reaped; a reap would close a silent
# connection and move silent-evict's exact line for a reason that is not
# admission.
#
# Usage: ./test/integration/test-ssh-admission-qemu.sh [--arch X64|AARCH64]
#        SSH_HOSTILE_ONLY=<scenario> runs one.

HOSTILE_SUMMARY="ssh admission"
HOSTILE_SCENARIOS="silent-evict kexwait-refuse auth-safe rank-evict flood-churn tx-backpressure penalty-first"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh, "the fixture's deadline"). Each is dominated by a
# hold the scenario waits out, which is wall-clock time and does not grow on
# a slow box:
#   silent-evict   the 15 s hold, plus opening 16 connections one at a time
#   kexwait-refuse the holders' key exchanges and 8 s hold, plus the wait
#                  for their closes to accrue
#   auth-safe      the login's key exchange and 12 s hold
#   rank-evict     the KEXINIT peer's 8 s hold, which outlasts everything
#                  else in the scenario
#   flood-churn    the flood's 1 s hold, the liveness probe, and a password
#                  login that `timeout 10` ends
#   tx-backpressure two peer logins, measured 4-5 s and 2 s on both arches
#                  (the host's pure-Python crypto sealing and opening 5,050
#                  packets each way, plus 2 s holds); each peer's `timeout`
#                  (12 s, 6 s) is sized inside this, so a hang fails the
#                  scenario rather than outliving the fixture's deadline
#   penalty-first  one failed login's key exchange and failure delay, 20
#                  sequential connections (most refused, ~0.5 s each through
#                  QEMU's user network) and a 3 s hold
declare -A HOSTILE_BUDGET=(
    [silent-evict]=21 [kexwait-refuse]=16 [auth-safe]=18 [rank-evict]=11
    [flood-churn]=20 [tx-backpressure]=20 [penalty-first]=16
)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
# AUTHFAIL:NOAUTH:GRACE:MIN:MAX -- accrues, never activates; see the header.
PEN_SPEC="596:1:10:600:1200"
HOSTILE_EXEMPT=-

CAP2_PORT=$HOST_PORT
CAP10_PORT=$(test_port 1)
GUEST_PORT2=2223
# latency=firmware: every scenario here was designed and measured at the
# firmware's 10 ms pace, and tx-backpressure DEPENDS on it -- its pause comes
# from the server's parse outrunning AxlTcp's drain (see that section), and
# with an axl_net_set_latency policy polling every 1 ms the drain keeps up
# and no pause happens (measured 2026-11-03). Stated, not left to the
# library's default, so a change of default cannot move it. Under QEMU's user-mode network,
# which ACKs everything the guest sends, nothing else can fill the queue. The
# latency policy is test-net-latency-qemu.sh's to pin.
HOSTILE_EXTRA_ARGS="${GUEST_PORT2} ${GRACE_SEC} maxstartups=2 sshloglevel=debug latency=firmware"
HOSTILE_EXTRA_FWD=("$CAP10_PORT" "$GUEST_PORT2")

PEER="$TESTS_DIR/ssh-peer.py"

hostile_boot "AxlSsh admission"

# Both host files $2 and $3 hold the exact line $1. (One grep over both
# would succeed on EITHER.)
both_have() { grep -qxF -- "$1" "$2" && grep -qxF -- "$1" "$3"; }

# A password login with ssh(1) at port $1, transcript to $2. `-N`: no channel
# exists yet (P4b), so there is no command to run; `timeout 10` ends it once
# it has logged in. Graded by the caller from the guest's audit line and from
# the transcript.
pw_login() {
    : > "$2.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$2.count" \
    timeout 10 ssh -vvv -N -p "$1" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=15 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 > "$2" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$2")
}

EVICT_WHY=': past MaxStartups, key exchange unfinished'
REFUSE_WHY=': past MaxStartups'
evicted() { audit_text_count 'Evicted connection' "$EVICT_WHY"; }
refused() { audit_text_count 'Refused connection' "$REFUSE_WHY"; }

# noauth accruals against 10.0.2.2 so far, both servers (see the header).
NOAUTH_CAUSE='connections without attempting authentication'
accrued() {
    local n
    test_clean_log
    n=$(grep -caxE "[0-9:.]+ \[DEBUG\] ssh:penalty_accrue:[0-9]+: penalty of [0-9]+ seconds for ${HOST_AS_SEEN//./\\.}: $NOAUTH_CAUSE" \
        "$TEST_CLEAN_LOG" || true)
    echo "${n:-0}"
}

# Wait up to $2 seconds for accrued() to reach $1: an accrual is written when
# the server drops the connection, a moment after the peer has gone.
wait_accrued() {
    local i
    for ((i = 0; i < $2 * 2; i++)); do
        if [[ "$(accrued)" -ge "$1" ]]; then return 0; fi
        sleep 0.5
    done
    return 1
}

# --- silent-evict ------------------------------------------------------------
#
# 16 connections that never say a word, against CAP10_PORT, then a real
# password login. THE ARITHMETIC behind the exact line: arrivals 0-9 fill
# max_startups (10); arrivals 10-15 each find 10 unauthenticated and evict the
# OLDEST connection still in key exchange, 0-5 in turn (all are silent, so
# all are equally far along and age decides); the ssh(1) login is
# the 17th arrival and evicts 6. So 16 identified, 7 closed, and the closed
# ones are exactly 0-6 -- which is what makes the victim ORDER observable: a
# newest-first victim would have closed 9 first, and the ssh login's victim
# would have been 15.
#
# THE HOLD (15 s) only has to outlast the login, which `timeout 10` ends: the
# login's arrival is what evicts 6. The EVICTED half of the penalty clause is
# counted while the other nine are still held -- exactly 7 accruals then, one
# per victim, and none for the login, which authenticated -- and again once
# the hold has ended: 16, because the nine that the probe closes end
# unauthenticated too.
#
# PRE-P4a: 16 connections filled MAX_CONNS and PAUSED the accept, so ssh(1)
# never got an identification line, no `Accepted` line appeared, and nothing
# was closed (closed=0).
if wants silent-evict; then
    SE_OUT="$TEST_TMPDIR/silent-evict.out"
    SE_ERR="$TEST_TMPDIR/silent-evict.err"
    SE_LOGIN="$TEST_TMPDIR/ssh-silent-evict-login.log"
    # Before the probe: arrivals 10-15 evict as they are opened.
    se_acc0=$(accrued)
    python3 "$PROBE" silent 127.0.0.1 "$CAP10_PORT" --count 16 --hold 15 \
        > "$SE_OUT" 2> "$SE_ERR" &
    SE_PID=$!
    BG_PIDS+=("$SE_PID")
    # The probe says so once all 16 have been tried. Each was identified
    # before the next was opened, so by then the server has accepted every
    # one of them -- a host-side marker the guest log cannot give: the
    # server writes no line for an accept, even at DEBUG.
    wait_line "$SE_ERR" 'silent: opened 16' 30 || true
    pw_login "$CAP10_PORT" "$SE_LOGIN"
    se_acc_held=$(( $(accrued) - se_acc0 ))
    SE_RC=0
    wait "$SE_PID" || SE_RC=$?
    wait_accrued $(( se_acc0 + 16 )) 10 || true
    se_acc_all=$(( $(accrued) - se_acc0 ))
    echo "  silent-evict: $(cat "$SE_OUT" 2>/dev/null) (rc=$SE_RC);" \
         "noauth accruals $se_acc_held while held, $se_acc_all in all"

    check "silent-evict PRECONDITION: all 16 silent connections were tried" \
        grep -qxF 'silent: opened 16' "$SE_ERR"
    check "silent-evict: a password login gets in past 16 silent connections" \
        audit_line 'Accepted password for pwuser'
    check "silent-evict: ssh(1) saw itself authenticated" \
        grep -q "$SUCCESS_PW_RE" "$SE_LOGIN"
    check "silent-evict: an eviction is logged at INFO" \
        audit_line 'Evicted connection' "$EVICT_WHY"
    check "silent-evict: seven evictions logged in all (both levels)" \
        test "$(evicted)" = 7
    check "silent-evict: the OLDEST silent connections were evicted, in order" \
        file_is "$SE_OUT" 'silent: held=16 refused=0 closed=7 closed_idx=0,1,2,3,4,5,6'
    check "silent-evict: each of the 7 victims accrued noauth, the login nothing" \
        test "$se_acc_held" = 7
    check "silent-evict: all 16 silent connections accrued noauth by the end" \
        test "$se_acc_all" = 16
fi

# --- kexwait-refuse ----------------------------------------------------------
#
# CAP2_PORT holds two peers that have FINISHED key exchange and sit
# unauthenticated in ST_AUTH. A third arrival finds two unauthenticated and
# no victim -- neither holder is still in key exchange -- so it is REFUSED:
# an abortive close with nothing sent, which ssh-peer.py reports as
# `peer: refused` (rc 5). Both holders must be untouched.
#
# THE REFUSED HALF of the penalty clause: no accrual may appear for the
# refused connection. Counted while both holders are still held, so nothing
# else on either server can end in that window; and then, as the control that
# proves the count can see this server at all, the holders' own two closes
# must each accrue noauth. The holders' 8 s hold only has to outlast one
# refused attempt.
#
# PRE-P4a: max_startups was read by nothing; three connections are far under
# MAX_CONNS, so the third was accepted and completed key exchange.
if wants kexwait-refuse; then
    KW1="$TEST_TMPDIR/kw1.out"
    KW2="$TEST_TMPDIR/kw2.out"
    KW3="$TEST_TMPDIR/kw3.out"
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 8 > "$KW1" 2>&1 &
    KW1_PID=$!
    BG_PIDS+=("$KW1_PID")
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 8 > "$KW2" 2>&1 &
    KW2_PID=$!
    BG_PIDS+=("$KW2_PID")
    wait_line "$KW1" 'peer: kex complete' 30 || true
    wait_line "$KW2" 'peer: kex complete' 30 || true
    check "kexwait-refuse PRECONDITION: both holders finished key exchange" \
        both_have 'peer: kex complete' "$KW1" "$KW2"
    kr_acc0=$(accrued)
    KW3_RC=0
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 5 > "$KW3" 2>&1 \
        || KW3_RC=$?
    # An accrual is written as the refusal happens, before the peer sees its
    # EOF; one second more covers the serial line reaching the host.
    sleep 1
    kr_acc_refused=$(( $(accrued) - kr_acc0 ))
    echo "  kexwait-refuse: third peer printed '$(tr '\n' '|' < "$KW3")' rc=$KW3_RC"
    check "kexwait-refuse: a third peer is refused (peer: refused, rc 5)" \
        peer_is "$KW3_RC" 5 "$KW3" 'peer: refused'
    check "kexwait-refuse: the refusal is logged at INFO" \
        audit_line 'Refused connection' "$REFUSE_WHY"
    check "kexwait-refuse: exactly one refusal logged (both levels)" \
        test "$(refused)" = 1
    check "kexwait-refuse: the refused connection accrued nothing" \
        test "$kr_acc_refused" = 0
    KW1_RC=0; wait "$KW1_PID" || KW1_RC=$?
    KW2_RC=0; wait "$KW2_PID" || KW2_RC=$?
    wait_accrued $(( kr_acc0 + 2 )) 10 || true
    check "kexwait-refuse CONTROL: the holders' own closes each accrued noauth" \
        test "$(( $(accrued) - kr_acc0 ))" = 2
    check "kexwait-refuse: the first holder was not evicted (peer: held)" \
        peer_is "$KW1_RC" 0 "$KW1" 'peer: kex complete' 'peer: held rx=0'
    check "kexwait-refuse: the second holder was not evicted (peer: held)" \
        peer_is "$KW2_RC" 0 "$KW2" 'peer: kex complete' 'peer: held rx=0'
fi

# --- auth-safe ---------------------------------------------------------------
#
# An AUTHENTICATED connection is never a victim and never counts against
# max_startups. With one logged in on CAP2_PORT, two key-exchanged holders
# still fit, a third arrival is refused, and so is a connection that says
# nothing at all: both pre-auth slots are held by peers that have finished
# key exchange, so there is no victim. This is the case a naive "evict
# anything unauthenticated" would get wrong -- it would take a holder.
#
# WHY THE PRINCIPAL COUNT: ssh-peer.py's `login --hold` sleeps and cannot
# see a close, so "the authenticated peer is still open" is read from the
# guest instead. The fixture prints SERVER: PRINCIPAL-RELEASED when a logged
# in connection ends; the count must not move while the login is held.
#
# Neither refusal may accrue anything, counted the same way as in
# kexwait-refuse, while the login and both holders are still held.
#
# THE HOLDS: the holders' 8 s outlast one refusal and the probe; the login's
# 12 s outlast both holders' key exchanges as well.
#
# PRE-P4a: the third holder was accepted (no admission check), and the silent
# connection was accepted too (held=1).
if wants auth-safe; then
    AS_LOGIN="$TEST_TMPDIR/ssh-auth-safe-peer.log"
    KW4="$TEST_TMPDIR/kw4.out"
    KW5="$TEST_TMPDIR/kw5.out"
    KW6="$TEST_TMPDIR/kw6.out"
    AS_OUT="$TEST_TMPDIR/auth-safe-silent.out"
    python3 "$PEER" login 127.0.0.1 "$CAP2_PORT" --user pwuser \
        --password "$PW_OK" --hold 12 > "$AS_LOGIN" 2>&1 &
    AS_PID=$!
    BG_PIDS+=("$AS_PID")
    wait_line "$AS_LOGIN" 'peer: authenticated' 30 || true
    test_clean_log
    as_rel0=$(guest_count 'SERVER: PRINCIPAL-RELEASED')
    as_ev0=$(evicted)
    as_rf0=$(refused)
    as_acc0=$(accrued)

    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 8 > "$KW4" 2>&1 &
    KW4_PID=$!
    BG_PIDS+=("$KW4_PID")
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 8 > "$KW5" 2>&1 &
    KW5_PID=$!
    BG_PIDS+=("$KW5_PID")
    wait_line "$KW4" 'peer: kex complete' 30 || true
    wait_line "$KW5" 'peer: kex complete' 30 || true
    KW6_RC=0
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --hold 5 > "$KW6" 2>&1 \
        || KW6_RC=$?
    python3 "$PROBE" silent 127.0.0.1 "$CAP2_PORT" --count 1 --hold 10 \
        > "$AS_OUT" 2>/dev/null || true
    sleep 1
    as_acc1=$(accrued)
    test_clean_log
    as_rel1=$(guest_count 'SERVER: PRINCIPAL-RELEASED')
    as_ev1=$(evicted)
    as_rf1=$(refused)
    echo "  auth-safe: third peer '$(tr '\n' '|' < "$KW6")' rc=$KW6_RC;" \
         "$(cat "$AS_OUT" 2>/dev/null); evictions +$(( as_ev1 - as_ev0 ))," \
         "refusals +$(( as_rf1 - as_rf0 ))"

    check "auth-safe PRECONDITION: the login authenticated" \
        grep -qxF 'peer: authenticated' "$AS_LOGIN"
    check "auth-safe PRECONDITION: two holders finished key exchange beside it" \
        both_have 'peer: kex complete' "$KW4" "$KW5"
    check "auth-safe: a third holder is refused (peer: refused, rc 5)" \
        peer_is "$KW6_RC" 5 "$KW6" 'peer: refused'
    check "auth-safe: a silent connection is refused, not given a victim" \
        file_is "$AS_OUT" 'silent: held=0 refused=1 closed=0 closed_idx=-'
    check "auth-safe: nothing was evicted" \
        test "$as_ev1" = "$as_ev0"
    check "auth-safe: both refusals were logged (both levels)" \
        test "$(( as_rf1 - as_rf0 ))" = 2
    check "auth-safe: the authenticated connection is still open" \
        test "$as_rel1" = "$as_rel0"
    check "auth-safe: neither refused connection accrued anything" \
        test "$as_acc1" = "$as_acc0"
    AS_RC=0; wait "$AS_PID" || AS_RC=$?
    KW4_RC=0; wait "$KW4_PID" || KW4_RC=$?
    KW5_RC=0; wait "$KW5_PID" || KW5_RC=$?
    check "auth-safe: the login held to the end of its hold (exit 0, no more output)" \
        peer_is "$AS_RC" 0 "$AS_LOGIN" 'peer: authenticated'
    check "auth-safe: the first holder was left alone (peer: held)" \
        peer_is "$KW4_RC" 0 "$KW4" 'peer: kex complete' 'peer: held rx=0'
    check "auth-safe: the second holder was left alone (peer: held)" \
        peer_is "$KW5_RC" 0 "$KW5" 'peer: kex complete' 'peer: held rx=0'
fi

# --- rank-evict --------------------------------------------------------------
#
# The victim is ranked by PROTOCOL PROGRESS first and by age only among equals.
# CAP2_PORT holds an OLDER peer that has sent its KEXINIT (the server waits in
# ST_KEX for its KEX_ECDH_INIT) and a NEWER one that has said nothing
# (ST_IDENT). A third arrival must evict the silent one although it is the
# younger: it is the least far along. Pure oldest-first would take the KEXINIT
# peer instead -- which is the case this scenario exists for, because a real
# client is that peer (ssh(1) sends its KEXINIT one round trip after
# connecting) and a flood that never speaks is the other one. silent-evict
# cannot see the difference: all its victims are ST_IDENT, so ranking falls
# through to age there.
#
# The 1 s pause after "peer: kexinit sent" is the margin for the server to
# have READ that KEXINIT before the silent peer arrives; the marker is written
# when the peer has sent it.
#
# THE HOLDS: the KEXINIT peer's 8 s outlast the silent peer's open and the
# arrival's 2 s hold; the silent peer's 6 s outlast the arrival.
#
# PRE-RANKING (oldest-first), measured by sabotage: the KEXINIT peer is closed
# (`peer: closed after_ms=...`) and the silent one is held (closed=0).
if wants rank-evict; then
    RK_KEX="$TEST_TMPDIR/rank-kexinit.out"
    RK_SIL="$TEST_TMPDIR/rank-silent.out"
    RK_SIL_ERR="$TEST_TMPDIR/rank-silent.err"
    RK_NEW="$TEST_TMPDIR/rank-arrival.out"
    rk_ev0=$(evicted)
    python3 "$PEER" kexwait 127.0.0.1 "$CAP2_PORT" --stage kexinit --hold 8 \
        > "$RK_KEX" 2>&1 &
    RK_KEX_PID=$!
    BG_PIDS+=("$RK_KEX_PID")
    wait_line "$RK_KEX" 'peer: kexinit sent' 30 || true
    sleep 1
    python3 "$PROBE" silent 127.0.0.1 "$CAP2_PORT" --count 1 --hold 6 \
        > "$RK_SIL" 2> "$RK_SIL_ERR" &
    RK_SIL_PID=$!
    BG_PIDS+=("$RK_SIL_PID")
    wait_line "$RK_SIL_ERR" 'silent: opened 1' 30 || true
    python3 "$PROBE" silent 127.0.0.1 "$CAP2_PORT" --count 1 --hold 2 \
        > "$RK_NEW" 2>/dev/null || true
    RK_KEX_RC=0; wait "$RK_KEX_PID" || RK_KEX_RC=$?
    wait "$RK_SIL_PID" || true
    test_clean_log
    rk_ev=$(( $(evicted) - rk_ev0 ))
    echo "  rank-evict: KEXINIT peer '$(tr '\n' '|' < "$RK_KEX")' rc=$RK_KEX_RC;" \
         "silent peer $(cat "$RK_SIL" 2>/dev/null); arrival" \
         "$(cat "$RK_NEW" 2>/dev/null); evictions +$rk_ev"

    check "rank-evict PRECONDITION: the older peer sent its KEXINIT" \
        grep -qxF 'peer: kexinit sent' "$RK_KEX"
    check "rank-evict PRECONDITION: the newer silent peer was identified" \
        grep -qxF 'silent: opened 1' "$RK_SIL_ERR"
    check "rank-evict: exactly one eviction for the one arrival (both levels)" \
        test "$rk_ev" = 1
    check "rank-evict: the NEWER silent peer was the victim (least progress)" \
        file_is "$RK_SIL" 'silent: held=1 refused=0 closed=1 closed_idx=0'
    check "rank-evict: the OLDER peer past its KEXINIT was not evicted" \
        peer_is "$RK_KEX_RC" 0 "$RK_KEX" 'peer: kexinit sent' 'peer: held rx=0'
    check "rank-evict: the arrival was admitted and left alone" \
        file_is "$RK_NEW" 'silent: held=1 refused=0 closed=0 closed_idx=-'
fi

# --- flood-churn -------------------------------------------------------------
#
# ssh-hostile.py's 70-connection flood against CAP10_PORT: with the accept
# always armed, arrivals past the first ten each evict the oldest silent one --
# up to 60 evictions back to back (38 measured: the flood closes its early
# sockets as it goes). What must survive that churn is the server
# itself -- it still speaks first, and a real password login still gets in.
# (Memory is the unit leak gate's, and hostile_finish's leak verdict for this
# whole boot.)
#
# PRE-P4a: the flood filled MAX_CONNS and paused the accept; this passed then
# too, by a different path.
if wants flood-churn; then
    FC_ERR="$TEST_TMPDIR/flood-churn.err"
    FC_LOGIN="$TEST_TMPDIR/ssh-flood-churn-login.log"
    python3 "$PROBE" flood 127.0.0.1 "$CAP10_PORT" --count 70 \
        > /dev/null 2> "$FC_ERR" || true
    echo "  flood-churn: $(cat "$FC_ERR" 2>/dev/null)"
    check "flood-churn: alive after a 70-connection flood" \
        python3 "$PROBE" alive 127.0.0.1 "$CAP10_PORT"
    pw_login "$CAP10_PORT" "$FC_LOGIN"
    check "flood-churn: a password login still gets in" \
        grep -q "$SUCCESS_PW_RE" "$FC_LOGIN"
    # The header's claim that every refusal so far is admission's.
    test_clean_log
    check "flood-churn: no penalty activated in this boot so far" \
        test "$(guest_count 'activating penalty')" = 0
fi

# --- tx-backpressure ---------------------------------------------------------
#
# A peer that keeps sending requests and stops reading the replies must not
# make the server buffer them without bound: once more than TXQ_HIGH (128 KiB,
# src/net/axl-ssh-server.c) is queued to send, the server stops parsing and
# stops reading -- the peer's further input waits in TCP -- and nothing is
# dropped; when its sends bring the queue under TXQ_LOW it reads again.
#
# THE PEER: 8,000 CHANNEL_OPEN "nosuch" -- a type no server knows, carrying no
# type-specific data, so well-formed and refused in every phase (spec 12a.3),
# so each draws exactly one 44-byte CHANNEL_OPEN_FAILURE (type 92), about
# 350 KB of output -- written by ssh-peer.py's `flood`, a background thread,
# while the script holds 2 s without reading and then reads all 8,000
# replies. WHY IT CANNOT DEADLOCK: the server stops reading only while its
# output is backed up, and the peer's reader is not behind its writer -- the
# writer is blocked by design and the reader drains the replies that reopen
# the window (ssh-peer.py's module doc, "WHY `flood` NEEDS A THREAD").
# WHAT MAKES IT PAUSE is the server's own parse rate outrunning AxlTcp's
# drain into the guest's stack, not the peer: QEMU's user-mode network ACKs
# every byte the guest sends with a full window whatever the peer does, and
# the backlog sits in QEMU's host socket (measured, 2026-10-09: default
# buffers paused 18/18 on x64 and 5/5 on aa64, every B = peak = 131,076).
# So the peer runs with default buffers. It used `--rcvbuf 4096` until that
# was measured to cause a ~24 s delivery stall of its own on the HOST
# loopback leg: a 4 KB receive buffer locks the Linux sender into one
# 2028-byte segment per 200 ms probe (about 9.7 KB/s) -- a harness artifact,
# not the server. EIGHT thousand opens, not the five it was: 220 KB of
# replies only just outran the drain, and on aa64 whether the pause
# happened at all came to depend on the server's code layout (no pause in
# five consecutive runs of one build). 350 KB pauses on every run.
#
# THE BOUND. The pause is decided in pump, before each packet is parsed, and
# each parsed open queues one 44-byte reply, so the queue at the pause is
# past TXQ_HIGH by at most one frame: every pause line's B is in
# (131072, 131116]. While paused nothing more is parsed, and on an
# authenticated connection nothing else queues (no authentication reply is
# due), so the PEAK the server logs when the pause ends (`tx: resuming
# input, peak P bytes queued`) is EXACTLY its pause's B, and there is exactly
# one resume per pause --
# a pause never ended is a peer never read again. Nothing may be dropped --
# no "input backlog" disconnect -- and the peer must get all 8,000 replies on
# a connection that stayed up. THE CONTROL is 50 opens on the same peer
# settings: their 2 KB of replies never reach the mark, so a pause line there
# would mean the gate fires on something other than the queue.
TXBP_HIGH=131072
TXBP_FRAME=44
if wants tx-backpressure; then
    TXBP_HEX_RC=0
    # NOT x11-open-hex: an RFC 4254 x11 open carries an originator address
    # and port, which makes each request 68 bytes on the wire instead of
    # the 52 this flood was measured with, and the larger request changes
    # the parse-against-drain race the pause depends on -- measured on
    # aa64: x11 paused in 2 of 3 runs, "nosuch" in every run. "nosuch"
    # keeps the 52-byte request; the reply is the same 44-byte
    # OPEN_FAILURE either way.
    TXBP_HEX=$(python3 "$PEER" open-hex --type nosuch) || TXBP_HEX_RC=$?
    TXBP_LOG="$TEST_TMPDIR/ssh-tx-backpressure.log"
    TXBP_CTL="$TEST_TMPDIR/ssh-tx-backpressure-control.log"
    # One peer: $1 opens, transcript to $2, killed after $3 seconds. Sets
    # TXBP_PEER_RC and TXBP_PEER_SECS (printed, so the timeouts and this
    # scenario's budget can be checked against what a run took).
    txbp_peer() {
        local t0=$SECONDS
        TXBP_PEER_RC=0
        timeout "$3" python3 "$PEER" login 127.0.0.1 "$CAP10_PORT" \
            --user pwuser --password "$PW_OK" \
            --script "flood:$1:90:$TXBP_HEX,hold:2,expectn:$1:92" \
            > "$2" 2>&1 || TXBP_PEER_RC=$?
        TXBP_PEER_SECS=$((SECONDS - t0))
    }
    test_clean_log
    txbp_l0=$(wc -l < "$TEST_CLEAN_LOG")
    txbp_peer 8000 "$TXBP_LOG" 12
    TXBP_RC=$TXBP_PEER_RC
    TXBP_SECS=$TXBP_PEER_SECS
    sleep 1
    test_clean_log
    txbp_l1=$(wc -l < "$TEST_CLEAN_LOG")
    txbp_peer 50 "$TXBP_CTL" 6
    TXBP_CTL_RC=$TXBP_PEER_RC
    TXBP_CTL_SECS=$TXBP_PEER_SECS
    sleep 1
    test_clean_log
    txbp_l2=$(wc -l < "$TEST_CLEAN_LOG")
    # The figure of every line of one kind ($3: "pausing input," or
    # "resuming input, peak") in a slice of the clean log, one per line.
    txbp_figures() {
        sed -n "$(($1 + 1)),$2p" "$TEST_CLEAN_LOG" \
            | grep -aoE "ssh:[a-z_]+:[0-9]+: tx: $3 [0-9]+ bytes queued\$" \
            | grep -oE '[0-9]+ bytes' | grep -oE '[0-9]+' || true
    }
    # 1 when every figure given is in (TXQ_HIGH, TXQ_HIGH + one frame].
    txbp_in_band() {
        local v
        for v in "$@"; do
            if [[ "$v" -le "$TXBP_HIGH" || "$v" -gt $((TXBP_HIGH + TXBP_FRAME)) ]]; then
                echo 0
                return
            fi
        done
        echo 1
    }
    mapfile -t TXBP_B < <(txbp_figures "$txbp_l0" "$txbp_l1" 'pausing input,')
    mapfile -t TXBP_P < <(txbp_figures "$txbp_l0" "$txbp_l1" 'resuming input, peak')
    mapfile -t TXBP_CTL_B < <(txbp_figures "$txbp_l1" "$txbp_l2" 'pausing input,')
    txbp_backlog=$(sed -n "$((txbp_l0 + 1)),${txbp_l2}p" "$TEST_CLEAN_LOG" \
        | grep -caF 'disconnect: input backlog' || true)
    # The largest single send of the 8,000-open run (the "tx: F frames, B
    # bytes" DEBUG line). A send is the whole pending queue taken at once, so
    # it can never be larger than the PENDING part of the queue was, and so
    # never larger than the queue: with the limit in force no send exceeds
    # TXQ_HIGH plus one frame. A consequence of the bound, not the bound --
    # the resume peak above is that -- but it is what shows a limit that
    # never engaged at all. Without the limit: 160,680 bytes in one send
    # (measured when a refusal was 60 bytes, with 5,000 opens -- an
    # order of magnitude, not this run's figure).
    txbp_maxrun=$(sed -n "$((txbp_l0 + 1)),${txbp_l1}p" "$TEST_CLEAN_LOG" \
        | grep -aoE 'tx: [0-9]+ frames, [0-9]+ bytes$' \
        | awk '{print $4}' | sort -n | tail -1 || true)
    echo "  tx-backpressure: peer '$(tr '\n' '|' < "$TXBP_LOG")' rc=$TXBP_RC" \
         "in ${TXBP_SECS}s;" \
         "pauses ${#TXBP_B[@]} (B: ${TXBP_B[*]:-none});" \
         "resumes ${#TXBP_P[@]} (peak: ${TXBP_P[*]:-none});" \
         "largest send ${txbp_maxrun:-none} bytes;" \
         "control '$(tr '\n' '|' < "$TXBP_CTL")' rc=$TXBP_CTL_RC in" \
         "${TXBP_CTL_SECS}s," \
         "pauses ${#TXBP_CTL_B[@]}; backlog drops ${txbp_backlog:-0}"

    check "tx-backpressure PRECONDITION: open-hex printed the 22-byte body" \
        test "$TXBP_HEX_RC" = 0 -a "${#TXBP_HEX}" = 44
    check "tx-backpressure: a peer that stops reading still gets all 8000 replies, connection up" \
        peer_is "$TXBP_RC" 0 "$TXBP_LOG" 'peer: authenticated' \
            'peer: writing 8000 x 90 seqs=2..8001' 'peer: got 8000 x 92' \
            'peer: wrote 8000 x 90'
    check "tx-backpressure: the server paused its input at least once" \
        test "${#TXBP_B[@]}" -ge 1
    check "tx-backpressure: every pause came at the mark, within one frame past TXQ_HIGH ($TXBP_HIGH < B <= $((TXBP_HIGH + TXBP_FRAME)))" \
        test "$(txbp_in_band "${TXBP_B[@]}")" = 1
    check "tx-backpressure: every pause ended in exactly one resume" \
        test "${#TXBP_P[@]}" = "${#TXBP_B[@]}"
    check "tx-backpressure: nothing was parsed while paused (each resume's peak equals its pause's B: '${TXBP_P[*]:-}' = '${TXBP_B[*]:-}')" \
        test "${TXBP_P[*]:-}" = "${TXBP_B[*]:-}"
    check "tx-backpressure: the queue stayed bounded: no send larger than TXQ_HIGH + one frame (largest ${txbp_maxrun:-none})" \
        test -n "$txbp_maxrun" -a "${txbp_maxrun:-0}" -le $((TXBP_HIGH + TXBP_FRAME))
    check "tx-backpressure: nothing was dropped (no input backlog disconnect)" \
        test "${txbp_backlog:-0}" = 0
    check "tx-backpressure CONTROL: 50 requests get their replies" \
        peer_is "$TXBP_CTL_RC" 0 "$TXBP_CTL" 'peer: authenticated' \
            'peer: writing 50 x 90 seqs=2..51' 'peer: got 50 x 92' \
            'peer: wrote 50 x 90'
    check "tx-backpressure CONTROL: 50 requests never pause the input" \
        test "${#TXBP_CTL_B[@]}" = 0
fi

# --- penalty-first -----------------------------------------------------------
#
# The per-source penalty check runs BEFORE admission, so a source whose
# penalty is ACTIVE is refused without evicting anyone. The other order would
# let a source that is already being penalised keep knocking honest clients
# out of key exchange, one per knock, for as long as its penalty lasts.
#
# Making the order observable needs a full server that still has a victim at
# the moment a penalised arrival comes in -- and every connection here shares
# the penalised source, so the victims must be in place before the penalty
# activates, and the arrival that activates it must not leave a slot free.
# EVICTION DOES BOTH: a victim's noauth accrues as the arrival that evicted it
# takes its slot. So, against CAP2 (max_startups 2):
#   1. one wrong password (ssh-peer.py): AUTHFAIL, 596 s -- deferred, 4 s
#      under MIN, and nothing else is left on this source by now (its 1 s
#      noauth accruals from the earlier scenarios have long decayed);
#   2. `silent --count 20`, one at a time: 0 and 1 fill both slots, and each
#      later arrival evicts the oldest (+1 s noauth each) until one crosses
#      MIN and ACTIVATES the penalty -- with the server full of two silent
#      connections still in ST_IDENT, i.e. two victims;
#   3. every arrival after that must be refused by the PENALTY, with nothing
#      evicted: the two connections holding the slots at activation stay
#      open until the probe's hold ends.
# THE ARITHMETIC. Activation needs the total ABOVE 600 s, and it decays one
# second per second, so with d seconds between the wrong password and the
# n-th eviction it activates once 596 - d + n > 600, i.e. n > 4 + d. Twenty
# arrivals leave eighteen evictions, so the scenario tolerates about 13 s of
# decay in all -- the wait for step 2 to start plus the time its first
# evictions take. The first version paid 590 s (10 s under), needed about
# eleven evictions, and tolerated about 7 s: in the six-worker pool that
# budget ran out and nothing activated, a flake that said nothing about the
# code.
#
# Which arrival activates it depends on how long 1 and 2 are apart, so the
# test pins what does NOT: exactly one activation, and it is noauth's (an
# eviction's); no eviction logged after it; one penalty drop per refused
# arrival; and exactly two identified connections left unclosed.
#
# TWO DEFECTS, TWO CHECKS, each measured by sabotage:
#   - THE OTHER ORDER (admission first): the arrival whose eviction activates
#     the penalty is then refused by that same penalty, so the slot it
#     evicted for is left EMPTY and only ONE identified connection stays
#     open -- the holder check is what fails. That order never re-fills the
#     server, so no eviction follows the activation and the no-eviction
#     check below stays green under it.
#   - NO PENALTY REFUSAL AT ALL (the check deleted from on_accept): every
#     penalised arrival at the full server is admitted by evicting a holder,
#     which the reorder cannot show -- the no-eviction-after-activation check
#     is what fails. Its CONTROL counts this scenario's evictions BEFORE the
#     activation with the same pattern and requires at least two (the
#     activation itself needs five or more), so a pattern that matched
#     nothing could not pass it as "no eviction".
if wants penalty-first; then
    PF_WRONG="$TEST_TMPDIR/pf-wrong.out"
    PF_OUT="$TEST_TMPDIR/pf-silent.out"
    PF_RC=0
    # Only this scenario's lines count below: the earlier ones evicted too.
    test_clean_log
    pf_start=$(wc -l < "$TEST_CLEAN_LOG")
    python3 "$PEER" login 127.0.0.1 "$CAP2_PORT" --user pwuser \
        --password "$PW_WRONG" > "$PF_WRONG" 2>&1 || PF_RC=$?
    python3 "$PROBE" silent 127.0.0.1 "$CAP2_PORT" --count 20 --hold 3 \
        > "$PF_OUT" 2>/dev/null || true
    sleep 1
    test_clean_log
    pf_line=$(cat "$PF_OUT" 2>/dev/null)
    pf_held=$(sed -nE 's/^silent: held=([0-9]+) .*/\1/p' <<< "$pf_line")
    pf_refused=$(sed -nE 's/.* refused=([0-9]+) .*/\1/p' <<< "$pf_line")
    pf_closed=$(sed -nE 's/.* closed=([0-9]+) .*/\1/p' <<< "$pf_line")
    pf_act=$(grep -caxE "[0-9:.]+ \[INFO\]  ssh: activating penalty of [0-9]+ seconds for ${HOST_AS_SEEN//./\\.}: $NOAUTH_CAUSE" \
        "$TEST_CLEAN_LOG" || true)
    # This scenario's guest lines matching regex $2, at either level, on the
    # $1 side ("before" or "after") of the activation line. The regex goes
    # through the ENVIRONMENT: `awk -v` processes backslash escapes, so gawk
    # warned and turned the escaped dots of 10\.0\.2\.2 into bare ones that
    # match any character.
    pf_count() {
        PF_RE="$2" awk -v start="$pf_start" -v side="$1" '
            FNR <= start { next }
            /ssh: activating penalty of / { on = 1; next }
            (on ? "after" : "before") == side && $0 ~ ENVIRON["PF_RE"] { n++ }
            END { print n + 0 }' "$TEST_CLEAN_LOG"
    }
    PF_EVICT_RE="ssh(:[a-z_]+:[0-9]+)?: Evicted connection from "
    pf_ev_before=$(pf_count before "$PF_EVICT_RE")
    pf_ev_after=$(pf_count after "$PF_EVICT_RE")
    pf_drops=$(pf_count after "ssh:on_accept:[0-9]+: drop connection from ${HOST_AS_SEEN//./\\.}:[0-9]+ penalty: ")
    echo "  penalty-first: wrong password '$(tr '\n' '|' < "$PF_WRONG")' rc=$PF_RC;" \
         "$pf_line; activations $pf_act, evictions before it $pf_ev_before," \
         "after it $pf_ev_after, penalty drops after it $pf_drops"

    check "penalty-first PRECONDITION: the wrong password was refused" \
        peer_is "$PF_RC" 2 "$PF_WRONG" 'peer: auth failed'
    check "penalty-first PRECONDITION: one activation, by an eviction's noauth" \
        test "${pf_act:-0}" = 1
    check "penalty-first PRECONDITION: penalised arrivals met the full server" \
        test "${pf_drops:-0}" -ge 1
    check "penalty-first: every refused arrival was the penalty's" \
        test "${pf_drops:-0}" = "${pf_refused:-x}"
    check "penalty-first CONTROL: the eviction count sees this scenario's evictions (>= 2 before activation)" \
        test "${pf_ev_before:-0}" -ge 2
    check "penalty-first: a penalty refusal was applied at the full server (no eviction after activation)" \
        test "$pf_ev_after" = 0
    check "penalty-first: the two slot holders at activation stayed open" \
        test "$(( ${pf_held:-0} - ${pf_closed:-0} ))" = 2
fi

hostile_finish
