# shellcheck shell=bash
# ssh-hostile-cases.sh -- the body of the parts below: everything that was
# test-ssh-hostile-qemu.sh -- its doc, budgets, boot, scenarios and finish.
# Each part sets HOSTILE_SUMMARY and HOSTILE_SCENARIOS and sources this
# file; lib/ssh-hostile-lib.sh checks that a part's scenarios follow
# HOSTILE_SCENARIOS_ALL in its order and that the parts together run all
# of them. One boot is the unit the runner's pool parallelises, so the
# scenarios that dominate the time each have a boot of their own:
#
#   test-ssh-hostile-qemu.sh               bad-ident bad-length bad-padding flood auth-cap auth-forge penalty
#   test-ssh-hostile-stall-qemu.sh         auth-stall
#
# What follows is the original header, unchanged.
#
# test-ssh-hostile-qemu.sh — AxlSsh survives malformed input, a connection
# flood, an ABUSIVE AUTHENTICATION PHASE and a penalised source, from a peer
# that never authenticates.
#
# ONE OF TWO HALVES. The P3 password-hook scenarios (auth-pending-drop,
# auth-hook-reenter, auth-hook-nest, auth-hook-free) are
# test-ssh-hostile-hooks-qemu.sh; the setup, helpers and teardown both run are
# lib/ssh-hostile-lib.sh, which also says why there are two and where the seam
# falls. Scenarios here, in run order:
#
#   bad-ident bad-length bad-padding flood auth-cap auth-forge penalty
#   auth-stall
#
# WHY THIS EXISTS, and why test-ssh-transport-qemu.sh does not cover it: that
# test drives one SUCCESSFUL ssh(1) session, which only ever takes the passive
# close path (the peer FINs first). ssh(1) is the right oracle for the happy
# path and useless here — it never sends anything malformed. A security review
# of P1 found a pre-auth remote denial of service in exactly that gap: an
# ACTIVE graceful close issued from inside the receive callback never returned,
# and ONE 18-byte connection wedged the entire event loop until reboot.
#
# THE ASSERTION IS LIVENESS AFTER ABUSE, and it is made two ways, because
# either alone can lie:
#   - the server still speaks first on a NEW connection (a wedged loop leaves
#     the port bound, so "connect succeeded" alone proves nothing);
#   - and the guest's loop ends on a timer and prints SERVER: DONE (a loop
#     that stopped dispatching never fires its timer): the finish timer the
#     host arms once every scenario is over, or, unsignalled, the deadline.
#
# Refusing a connection over a cap is CORRECT behaviour, not a failure — so the
# flood scenario asserts nothing about how many are accepted, only that the
# server is alive afterwards.
#
# P2 ADDS THE AUTHENTICATION PHASE, and it needs a different lever. ssh-hostile.py
# says in its own docstring that it speaks "just enough SSH to be RUDE. No
# crypto", so every scenario there is refused BEFORE key exchange and none of
# them can reach userauth at all. Getting there hostilely would mean writing
# curve25519 and chacha20-poly1305 in Python. A fake ssh-agent
# (ssh-fake-agent.py) gets there for free: the agent protocol is
# length-prefixed messages over a Unix socket with no crypto in it, and a real
# ssh(1) performs the whole handshake on our behalf before asking the agent to
# sign. The agent then forges the signature or never answers.
#
# THE THREE AUTH SCENARIOS, and what each one would miss if it were dropped:
#   auth-cap   — 25 unauthorized keys offered on ONE connection. The cap is
#                MAX_AUTH_REQUESTS (20, src/net/axl-ssh-server.c); without it
#                ssh walks all 25 and ends with a clean "Permission denied"
#                instead of a reset, which is what the assertion reads.
#   auth-forge — a signature over a key the server has ALREADY answered PK_OK
#                to. A server that skipped verification would authenticate it.
#                An unauthorized key is refused just before it, so the audit
#                line it must still leave at INFO proves forgeries are not
#                rate-limited together with ordinary refusals.
#   auth-stall — key exchange completed, then silence. This is the ONLY thing
#                asserting that the pre-auth deadline covers the window AFTER
#                NEWKEYS: if the deadline were lifted there, the connection would
#                live forever and nothing else in the suite would notice.
#
# AND penalty (Task P) -- per-source penalties, on a SECOND server (one
#                     boot, two servers, two penalty tables): a forged
#                     signature and a wrong password, then a right-password
#                     login refused at once, 40 sequential knocks at the
#                     refused source, and the same login retried until the
#                     penalty has burned down -- timed, so the whole penalty
#                     is enforced, not the part above min. Then one silent
#                     connection reaped by that server's short grace, which
#                     must accrue grace-exceeded. Then the same forgery and
#                     wrong password against the FIRST server, which exempts
#                     the host: served. Without it nothing in the suite
#                     refuses anyone across connections, and nothing proves
#                     a refusal decays rather than deafening the server.
#
# EVERY auth scenario ends by requiring a good login to still succeed. That is
# the only assertion that separates "refused the attack" from "died refusing
# it".
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_FLOOD_N sets the flood's size;
# SSH_HOSTILE_KEEP=<dir> keeps every ssh(1) transcript.
#
#
# Usage (a part): ./test/integration/test-ssh-hostile-qemu.sh [--arch X64|AARCH64]

HOSTILE_SCENARIOS_ALL="bad-ident bad-length bad-padding flood auth-cap auth-forge penalty auth-stall"
PROBE_SCENARIOS="bad-ident bad-length bad-padding"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh, "the fixture's deadline"). Each is the scenario's
# measured time on the slower arch, standalone, 2026-09-18, plus what its own
# pass bands still allow it:
#   probes    ~0.7 s each; nothing in them waits on a clock
#   flood     2 s measured
#   auth-cap  1 s measured (25 ssh-keygen runs and one connection)
#   auth-forge  under 1 s measured
#   penalty   37 s measured, 36 on aa64 -- dominated by the ~24 s refusal it
#             waits out, which is wall-clock time and does not grow on a slow
#             box. +3 s for the far edge of the whole-penalty band, +2 s for
#             knocks that use their full KNOCK_BUDGET_MS instead of the ~1 s an
#             unloaded run needs
#   auth-stall  44 s measured on both arches; the reap band allows 50 s after
#             PK_OK, plus the handshake before it and the liveness login after
declare -A HOSTILE_BUDGET=(
    [bad-ident]=2 [bad-length]=2 [bad-padding]=2 [flood]=3
    [auth-cap]=3 [auth-forge]=2 [penalty]=42 [auth-stall]=55
)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

# How many sockets the flood holds. Below the pre-auth cap (max_startups,
# 10) every connection is ACCEPTED; above it each arrival EVICTS the oldest
# silent one (admission by eviction), and the two are different code.
# test-ssh-admission-qemu.sh grades the eviction itself; this only requires
# the server to survive it.
FLOOD_N=${SSH_HOSTILE_FLOOD_N:-70}

# Task G: axl_ssh_server_set_login_grace() lets auth-stall stop waiting out the
# SHIPPED 60 s default (AXL_SSH_LOGIN_GRACE_DEFAULT_SEC,
# include/axl/axl-ssh-server.h). Before this API existed the grace could only
# be shortened by compiling a different constant into the fixture, which the
# auth-stall comment below used to reject outright as "grading a constant we
# do not ship" -- setting it through the same public call every consumer gets
# removes that objection.
#
# IN THIS BOOT THE GRACE BINDS auth-stall AND NOTHING ELSE. Every other
# connection to this server completes in about a second, and the penalty
# scenario's reaped connection is on the second server, under GRACE2_SEC. The
# floor that used to hold this at 40 -- auth-hook-nest's three hooks in series
# (~18 s) in the same boot -- moved to test-ssh-hostile-hooks-qemu.sh with that
# scenario, so nothing here needs 40 any more. It stays 40 anyway, on purpose:
# auth-stall's pass band [25, 50] and both sabotages that prove it (the
# deadline lifted at NEWKEYS; a setter that ignores its argument, 57-65 s)
# were measured against 40, and this half meets its time target without
# touching them. A shorter grace (10 s or more keeps the 5 s reap tick, see
# axl_ssh_server_set_login_grace's Granularity paragraph) would save its
# difference in seconds here and re-open that band and both proofs.
GRACE_SEC=40

# Task P: a SECOND server, for the penalty scenario alone. Its own login grace
# (ssh-server-selftest.c's argv[9]) is short, so the scenario can let one
# connection's grace run out cheaply (ruling P7). Every other connection to
# that server completes in well under a second, measured, on both arches.
HOST_PORT2=$(test_port 1)
GUEST_PORT2=2223
GRACE2_SEC=3
HOSTILE_EXTRA_ARGS="${GUEST_PORT2} ${GRACE2_SEC}"
HOSTILE_EXTRA_FWD=("$HOST_PORT2" "$GUEST_PORT2")
# Sequential knocks at the penalised source (review M-3): at most KNOCK_N,
# starting none after KNOCK_BUDGET_MS, so that even under host load the last
# one ends inside the ~17 s second penalty the knocks are aimed at.
KNOCK_N=40
KNOCK_BUDGET_MS=8000

hostile_boot "AxlSsh hostile input"

# Each abuse scenario is followed by a liveness probe, so a failure names the
# input that killed the server rather than just "it died somewhere".
for scenario in $PROBE_SCENARIOS; do
    if ! wants "$scenario"; then continue; fi
    python3 "$PROBE" "$scenario" 127.0.0.1 "$HOST_PORT" --count 5 >/dev/null 2>&1 || true
    check "alive after 5x $scenario" \
        python3 "$PROBE" alive 127.0.0.1 "$HOST_PORT"
done

# More connections than AXL_MAX_SOURCES (64) can hold sources for.
if wants flood; then
    python3 "$PROBE" flood 127.0.0.1 "$HOST_PORT" --count "$FLOOD_N" >/dev/null 2>&1 || true
    check "alive after a $FLOOD_N-connection flood" \
        python3 "$PROBE" alive 127.0.0.1 "$HOST_PORT"
fi

# --- the authentication phase, reached through a real ssh(1) ---------------

CAP_LOG="$TEST_TMPDIR/ssh-auth-cap.log"
FORGE_LOG="$TEST_TMPDIR/ssh-auth-forge.log"
STALL_LOG="$TEST_TMPDIR/ssh-auth-stall.log"

# 1) auth-cap -- MAX_AUTH_REQUESTS, offered on ONE connection.
#
# ssh(1) sends a "none" request first and then one query per identity, so 25
# keys is 26 requests against a cap of 20: the server must cut the list off
# partway. No Python and no agent needed -- the keys are unauthorized, so the
# query form alone drives the counter.
if wants auth-cap && [[ "$HAVE_SSH" == 1 ]]; then
    CAP_KEYS=25
    CAP_IDS=()
    for i in $(seq 1 "$CAP_KEYS"); do
        ssh-keygen -q -t ed25519 -N '' -f "$TEST_TMPDIR/cap$i" </dev/null
        CAP_IDS+=(-i "$TEST_TMPDIR/cap$i")
    done
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o IdentityAgent=none -o PreferredAuthentications=publickey \
        "${CAP_IDS[@]}" root@127.0.0.1 true > "$CAP_LOG" 2>&1 || true
    DUMP_LOGS+=("$CAP_LOG")

    # `|| true` and the ${:-0} default together: grep -c EXITS NON-ZERO on a
    # count of zero, which under `set -e` would kill the script from inside
    # the command substitution before the assertion below ever ran.
    cap_offered=$(grep -c "Offering public key" "$CAP_LOG" 2>/dev/null || true)
    cap_offered=${cap_offered:-0}
    echo "  auth-cap: ssh offered $cap_offered of $CAP_KEYS keys on one connection"
    report_ending auth-cap "$CAP_LOG"

    # PINNED EXACTLY, not as "fewer than 25". ssh(1) sends one "none" request
    # before any key (RFC 4252 section 5.2), so the 20-request cap trips on
    # the 21st message -- after exactly MAX_AUTH_REQUESTS key offers.
    # Measured: 20. A range would still pass if the cap silently moved to 12,
    # and this line is the only place in the suite that records the value at
    # all. A mismatch means either MAX_AUTH_REQUESTS changed (update this) or
    # ssh's pre-auth message count did (worth knowing either way).
    #
    # The falsifier: delete the cap and ssh walks all 25, then prints its own
    # clean "Permission denied (publickey)".
    cap_offered_expected() { [[ "$cap_offered" -eq 20 ]]; }
    check "auth-cap: the server cut the list at MAX_AUTH_REQUESTS ($cap_offered of $CAP_KEYS offered, expected 20)" \
        cap_offered_expected

    # Delivered since P5, so pinned whole rather than as "some ending".
    check "auth-cap: ssh(1) printed the server's DISCONNECT, exactly (:14: Too many authentication failures)" \
        ended_by_cap "$CAP_LOG"

    # AND THE SERVER SAYS SO, in sshd's words: its MaxAuthTries line,
    # "maximum authentication attempts exceeded for %s%.100s from %.200s port
    # %d ssh2" (/usr/libexec/openssh/sshd-session, OpenSSH 9.9p1), logged
    # there at ERROR. Until 2026-09-20 this cut-off reached nothing above
    # DEBUG, so a peer that probed twenty keys left one rate-limited "key not
    # authorized" line and no record that it was thrown out. It is the first
    # line of its kind on this server in any selection, so it is at INFO
    # with no suppressed count.
    check "auth-cap: the audit log records the cut-off, with user and source" \
        audit_line 'maximum authentication attempts exceeded for root'

    check "auth-cap: the server still serves a good login afterwards" \
        good_login_works auth-cap
elif wants auth-cap; then
    echo "SKIP: auth-cap needs ssh(1) on the host"
fi

# 2) auth-forge -- a signature over a key the server already said PK_OK to.
#
# PROBED, not assumed: OpenSSH 9.9p1 forwards whatever the agent returns
# without checking it (`sign_and_send_pubkey: signing using ssh-ed25519`
# straight into `send packet: type 50`). If a future OpenSSH validates the
# agent's answer locally, this scenario stops reaching the server and the
# transcript says so -- the "never authenticated" assertion would still hold
# but would be graded by the client, so re-probe before trusting it.
if wants auth-forge && [[ "$HAVE_SSH" == 1 ]]; then
    # AN ORDINARY REFUSAL FIRST, on purpose: an unauthorized key, which is a
    # thing any peer can produce for free. Failure lines are rate-limited to
    # one INFO a minute per server, and until 2026-09-20 there was ONE limiter
    # for every kind -- so this refusal, a moment before the forgery, pushed
    # the forgery's line down to DEBUG, and with nothing after it the forgery
    # never reached INFO at all, not even as a count. That is the evasion a
    # holder of a stolen .pub has against anyone alerting on "bad signature".
    # With it here the scenario proves the forgery has a limiter of its own
    # in ANY selection, not only in a full run where auth-cap happens to have
    # taken the window first.
    DECOY_LOG="$TEST_TMPDIR/ssh-auth-forge-decoy.log"
    ssh-keygen -q -t ed25519 -N '' -f "$TEST_TMPDIR/decoy" </dev/null
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o IdentityAgent=none -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/decoy" root@127.0.0.1 true > "$DECOY_LOG" 2>&1 || true
    DUMP_LOGS+=("$DECOY_LOG")
    # OFFERED AND REFUSED, not merely "Permission denied" -- ssh(1) prints
    # that for a key it never sent too (a missing -i file does it), and then
    # nothing took the ordinary window: a review pointed the decoy at a
    # missing file AND moved the forgery back onto that window, and this
    # scenario still passed 13/0. Pinned from a transcript of this scenario
    # (OpenSSH 9.9p1):
    #   debug1: Offering public key: <tmp>/decoy ED25519 SHA256:... explicit
    #   debug1: Authentications that can continue: publickey,password
    # -- the second line AFTER the offer is the server's USERAUTH_FAILURE for
    # it, which is the refusal that must precede the forgery. The path ends
    # in "/decoy " before " ED25519", so no other key's line can match.
    decoy_refused() {
        awk '/Offering public key: .*\/decoy ED25519/ { offered = 1; next }
             offered && /Authentications that can continue/ { refused = 1 }
             END { exit !refused }' "$DECOY_LOG" \
            && grep -q "Permission denied" "$DECOY_LOG"
    }
    check "auth-forge PRECONDITION: an ordinary refusal came first (the decoy key was offered and refused)" \
        decoy_refused

    FORGE_SOCK="$TEST_TMPDIR/agent-forge.sock"
    FORGE_AGENT_LOG="$TEST_TMPDIR/agent-forge.log"
    python3 "$AGENT" --pub "$TEST_TMPDIR/good.pub" --socket "$FORGE_SOCK" \
        --mode forge --lifetime 40 > "$FORGE_AGENT_LOG" 2>&1 &
    FORGE_AGENT_PID=$!
    BG_PIDS+=("$FORGE_AGENT_PID")
    forge_ready=0
    for _ in $(seq 1 40); do
        if grep -q "agent: ready" "$FORGE_AGENT_LOG" 2>/dev/null; then
            forge_ready=1; break
        fi
        sleep 0.25
    done
    check "auth-forge: the fake agent bound its socket" \
        test "$forge_ready" -eq 1

    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o IdentityAgent="$FORGE_SOCK" -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/good.pub" root@127.0.0.1 true > "$FORGE_LOG" 2>&1 || true
    DUMP_LOGS+=("$FORGE_LOG" "$FORGE_AGENT_LOG")
    kill "$FORGE_AGENT_PID" 2>/dev/null || true
    wait "$FORGE_AGENT_PID" 2>/dev/null || true

    # PRECONDITION, not a server assertion: without this the two checks below
    # pass against a server that was never sent a forged signature at all --
    # which is exactly what a locally-validating OpenSSH would produce.
    check "auth-forge PRECONDITION: the agent was asked to sign and forged it" \
        grep -q "agent: returned a forged signature" "$FORGE_AGENT_LOG"

    # The falsifier: skip axl_pk_key_verify's result and this authenticates.
    check "auth-forge: the forged signature did not authenticate" \
        never_authenticated "$FORGE_LOG"

    # And the response is to END the connection, not to answer FAILURE and
    # let the peer try again -- spec section 9.
    report_ending auth-forge "$FORGE_LOG"
    check "auth-forge: the server ended the connection rather than refusing cleanly" \
        ended_by_server "$FORGE_LOG"

    # THE AUDIT LOG'S RECORD OF IT -- the refusal an operator most wants in
    # it: almost every other failure is something a legitimate user can
    # produce by accident, while a bad signature under an AUTHORIZED key is a
    # peer that does not hold the private half. It was logged nowhere above
    # DEBUG until 2026-09-20, and then only when no ordinary failure had
    # taken the server's one-a-minute window first -- which in a full run
    # auth-cap always had (19 "key not authorized" refusals a second
    # earlier), so for one round this check had to live at the end of the
    # file, where the penalty scenario's forgery against a SECOND server
    # satisfied it. The forgery now has its own limiter, and the check is
    # back where the forgery happens, after the decoy above.
    check "auth-forge: the audit log records the forgery, with user and source" \
        audit_line 'Failed publickey \(bad signature\) for root'

    check "auth-forge: the server still serves a good login afterwards" \
        good_login_works auth-forge
elif wants auth-forge; then
    echo "SKIP: auth-forge needs ssh(1) on the host"
fi

# penalty -- per-source penalties (Task P), end to end with a real ssh(1).
#
# What the owner asked for, in his words: "the real OpenSSH server kills the
# connection after 3 failed attempts, but if the client reconnects, it will
# allow them to retry possibly after a period of time". So, on the SECOND
# server (HOST_PORT2), which exempts nothing:
#   1. a FORGED signature (review I-3): the key is authorized on this server,
#      the fake agent forges the signature, the server ends the connection
#      without ever sending a failure reply -- and must still charge authfail.
#      Charged noauth instead (1 s), step 2 would leave 13 s, not over
#      PEN_MIN, and step 3's refusal would not happen;
#   2. a wrong-password connection, ended by the server at its third guess --
#      proving ONE failed connection is not refused (deferred until the
#      penalty exceeds PEN_MIN);
#   3. a login with the RIGHT password, refused at once: no identification
#      line, no prompt -- the whole point, and exactly what sshd does;
#   4. the same right-password login, retried until it succeeds -- proving the
#      penalty DECAYED rather than the server going deaf -- and timed: a
#      penalty is enforced until it burns down to nothing, so the refusal
#      lasts about as long as the whole penalty (ruling P1);
#   5. after it expired, one more failed connection is deferred again, so the
#      next connection is served;
#   6. one connection that takes the identification line and says nothing
#      until GRACE2_SEC reaps it (review M-8, ruling P7). That must accrue
#      grace-exceeded (10 s): on top of what step 5 left it activates a
#      second penalty, whose INFO line names the cause, and refuses the
#      connection after it. Charged noauth instead, it would activate nothing;
#   7. while that second penalty refuses, up to KNOCK_N connections at it ONE
#      AT A TIME for KNOCK_BUDGET_MS, each classified, with the heartbeat
#      sampled across them (review M-3): none may get through, most must be
#      refused by the server, and the guest's loop must keep pace with the
#      wall clock. SEQUENTIAL, not a burst, and LAST on this server, both for
#      reasons measured here. A burst overruns the guest's accept backlog, so
#      its connections never reach on_accept to be refused; and QEMU's
#      user-mode network RETRANSMITS the SYNs it could not deliver, so part of
#      a burst reaches the guest tens of seconds later, after a penalty has
#      expired, where each is accepted, finds its host side gone and accrues
#      noauth -- three such stragglers once activated a penalty of their own
#      in the middle of this scenario. Under host load even a sequential
#      knock can go unanswered for seconds and straggle the same way, so the
#      knocks come after every assertion that reads this server's table. The
#      flood scenario above keeps the burst, against the exempt server;
# Then steps 1-3 exactly -- the forgery, the wrong password, the right
# password -- against the FIRST server, which exempts HOST_AS_SEEN: the third
# connection is served. Step 3 on the second server is the control that proves
# the same sequence is refused without the exemption.
#
# WHY THIS CANNOT POISON THE SCENARIO AFTER IT, which shares the boot:
# penalties are per server, and every refusal happens on the second server --
# nothing after this scenario connects to it (steps 6 and 7 deliberately
# leave it refusing). The first server, which auth-stall uses, exempts the
# only address it can come from, so nothing it sees accrues at all.
# auth-stall, still green after this, is the proof. The hook scenarios in
# test-ssh-hostile-hooks-qemu.sh never share a boot with this one.
if wants penalty && [[ "$HAVE_SSH" == 1 ]]; then
    now_ms() { date +%s%3N; }

    # One password login as pwuser. NumberOfPasswordPrompts=5, as
    # test-ssh-auth-qemu.sh does, so a wrong-password connection ends
    # because the SERVER ended it at three, not because ssh stopped asking.
    pen_login() {
        local port="$1" pass="$2" log="$3"
        : > "$log.count"
        SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
        ASKPASS_PASSWORD="$pass" ASKPASS_COUNT="$log.count" \
        timeout 60 ssh -vvv -p "$port" \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            -o ConnectTimeout=20 -o PubkeyAuthentication=no \
            -o KbdInteractiveAuthentication=no \
            -o PreferredAuthentications=password \
            -o NumberOfPasswordPrompts=5 \
            pwuser@127.0.0.1 true > "$log" 2>&1 < /dev/null || true
        DUMP_LOGS+=("$log")
    }
    pen_prompts() { wc -l < "$1.count" | tr -d ' '; }
    pen_logged_in() { grep -q "$SUCCESS_PW_RE" "$1"; }

    # A forged signature over the authorized key, through the fake agent
    # auth-forge uses: the transcript to $2, the agent's log to $2.agent.
    pen_forge() {
        local port="$1" log="$2" sock="$2.sock" pid
        python3 "$AGENT" --pub "$TEST_TMPDIR/good.pub" --socket "$sock" \
            --mode forge --lifetime 40 > "$log.agent" 2>&1 &
        pid=$!
        BG_PIDS+=("$pid")
        for _ in $(seq 1 40); do
            if grep -q "agent: ready" "$log.agent" 2>/dev/null; then break; fi
            sleep 0.25
        done
        timeout 60 ssh -vvv -p "$port" \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
            -o IdentityAgent="$sock" -o PreferredAuthentications=publickey \
            -i "$TEST_TMPDIR/good.pub" root@127.0.0.1 true > "$log" 2>&1 || true
        DUMP_LOGS+=("$log" "$log.agent")
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    }
    # The forgery reached the server and was ended by it, unauthenticated.
    pen_forged() {
        grep -q "agent: returned a forged signature" "$1.agent" \
            && never_authenticated "$1" && ended_by_server "$1"
    }

    PEN_F1="$TEST_TMPDIR/ssh-penalty-forge.log"
    PEN_F2="$TEST_TMPDIR/ssh-penalty-fail2.log"
    PEN_REFUSED="$TEST_TMPDIR/ssh-penalty-refused.log"

    # 1. The forgery.
    pen_forge "$HOST_PORT2" "$PEN_F1"
    pen_t1=$(now_ms)

    # 2 and 3.
    pen_login "$HOST_PORT2" "$PW_WRONG" "$PEN_F2"
    pen_t2=$(now_ms)
    pen_login "$HOST_PORT2" "$PW_OK" "$PEN_REFUSED"
    pen_refused_ms=$(( $(now_ms) - pen_t2 ))
    pen_gap_ms=$(( pen_t2 - pen_t1 ))
    echo "  penalty: forge -> fail2 took ${pen_gap_ms} ms; the refused attempt took ${pen_refused_ms} ms"

    # 4. Retry the right password once a second until it gets in. Timed from
    # the START of the attempt that succeeded, against fail2's end. Two
    # PEN_AUTHFAIL accruals (the forgery's and fail2's), less the time between
    # them, is what fail2 left, so that is when it should get in; retrying
    # stops a little past the band the assertion below allows, so a penalty
    # that never decays fails here, inside the scenario's own time budget,
    # rather than dragging the run past the fixture's deadline.
    pen_expect_ms=$(( 2 * PEN_AUTHFAIL * 1000 - pen_gap_ms ))
    pen_ok_log=""
    pen_tries=0
    pen_allowed_ms=-1
    while [[ $(( $(now_ms) - pen_t2 )) -le $(( pen_expect_ms + 5000 )) ]]; do
        pen_tries=$((pen_tries + 1))
        pen_try_t=$(now_ms)
        pen_try_log="$TEST_TMPDIR/ssh-penalty-poll-$pen_tries.log"
        pen_login "$HOST_PORT2" "$PW_OK" "$pen_try_log"
        if pen_logged_in "$pen_try_log"; then
            pen_ok_log="$pen_try_log"
            pen_allowed_ms=$(( pen_try_t - pen_t2 ))
            break
        fi
        sleep 1
    done
    echo "  penalty: allowed again ${pen_allowed_ms} ms after fail2 ended, on try $pen_tries"

    # 5. Expired means forgotten: one failed connection is deferred again.
    PEN_F3="$TEST_TMPDIR/ssh-penalty-fail3.log"
    PEN_AFTER="$TEST_TMPDIR/ssh-penalty-after.log"
    pen_login "$HOST_PORT2" "$PW_WRONG" "$PEN_F3"
    pen_login "$HOST_PORT2" "$PW_OK" "$PEN_AFTER"

    # 6. Say nothing until GRACE2_SEC reaps it, then try the right password.
    pen_hold=$(python3 "$PROBE" hold 127.0.0.1 "$HOST_PORT2" --count 20 2>/dev/null || true)
    echo "  penalty: $pen_hold"
    PEN_AFTER_GRACE="$TEST_TMPDIR/ssh-penalty-after-grace.log"
    pen_login "$HOST_PORT2" "$PW_OK" "$PEN_AFTER_GRACE"

    # 7. Knocks, while step 6's penalty refuses, with the heartbeat sampled
    # across them. The window is at least 6 s so an unloaded run -- 40 knocks
    # in under a second -- still spans three heartbeats.
    pen_tick0=$(tick_now); pen_tick0=${pen_tick0:-0}
    pen_knock_t0=$(now_ms)
    pen_knock=$(python3 "$PROBE" knock 127.0.0.1 "$HOST_PORT2" --count "$KNOCK_N" \
                    --deadline-ms "$KNOCK_BUDGET_MS" 2>/dev/null || true)
    while [[ $(( $(now_ms) - pen_knock_t0 )) -lt 6000 ]]; do sleep 0.25; done
    pen_tick1=$(tick_now); pen_tick1=${pen_tick1:-0}
    pen_knock_win_ms=$(( $(now_ms) - pen_knock_t0 ))
    echo "  penalty: $pen_knock"
    echo "  penalty: heartbeat $pen_tick0 -> $pen_tick1 across ${pen_knock_win_ms} ms of knocks"
    check "penalty: the other server still speaks after the knocks at the penalised one" \
        python3 "$PROBE" alive 127.0.0.1 "$HOST_PORT"

    # The exempt phase, on the FIRST server: steps 1-3 exactly -- the
    # forgery, the wrong password, then the right one.
    EX_F1="$TEST_TMPDIR/ssh-exempt-forge.log"
    EX_F2="$TEST_TMPDIR/ssh-exempt-fail2.log"
    EX_OK="$TEST_TMPDIR/ssh-exempt-ok.log"
    pen_forge "$HOST_PORT" "$EX_F1"
    pen_login "$HOST_PORT" "$PW_WRONG" "$EX_F2"
    pen_login "$HOST_PORT" "$PW_OK" "$EX_OK"

    # --- the assertions ------------------------------------------------------
    #
    # PINNED FROM REAL TRANSCRIPTS (OpenSSH 9.9p1, captured with
    # SSH_HOSTILE_KEEP in this change's own runs). A refused connection
    # connects, then ends during the identification exchange, in one of
    # two forms depending on how QEMU's user-mode network relays our RST:
    #   kex_exchange_identification: Connection closed by remote host
    #   kex_exchange_identification: read: Connection reset by peer
    # -- the same two a real sshd's refusals produced when measured (sshd
    # writes "Not allowed at this time" first, visible only at -v). Every
    # SERVED connection in the same run prints "Remote protocol version",
    # and each wrong-password one exactly 3 prompts, which is what makes the
    # negatives below mean something.
    PEN_REFUSED_RE='kex_exchange_identification: (Connection closed by remote host|read: Connection reset by peer)'
    pen_served_3() { [[ "$(pen_prompts "$1")" == 3 ]] && grep -q 'Remote protocol version' "$1"; }
    pen_no_ident() { ! grep -q 'Remote protocol version' "$1"; }
    pen_no_prompt() { [[ "$(pen_prompts "$1")" == 0 ]]; }
    pen_never_auth() { ! grep -q "$SUCCESS_PW_RE" "$1"; }
    pen_refused() { grep -qE "$PEN_REFUSED_RE" "$1" && pen_no_ident "$1" && pen_no_prompt "$1"; }

    check "penalty PRECONDITION: the agent forged a signature, the server ended that connection, and it never authenticated" \
        pen_forged "$PEN_F1"
    check "penalty: after a forgery the reconnect is still served -- deferred (3 prompts)" \
        pen_served_3 "$PEN_F2"
    check "penalty PRECONDITION: the refused attempt reached TCP (Connection established)" \
        grep -q 'Connection established' "$PEN_REFUSED"
    check "penalty: the next connection is refused during identification, even with the right password" \
        grep -qE "$PEN_REFUSED_RE" "$PEN_REFUSED"
    check "penalty: the refused connection saw no identification line" \
        pen_no_ident "$PEN_REFUSED"
    check "penalty: the refused connection was never asked for a password" \
        pen_no_prompt "$PEN_REFUSED"
    check "penalty: the refused connection never authenticated" \
        pen_never_auth "$PEN_REFUSED"
    pen_at_once() { [[ "$pen_refused_ms" -lt 5000 ]]; }
    check "penalty: refused at once, no delay (${pen_refused_ms} ms, want < 5000)" \
        pen_at_once


    # The guest's activation lines: the audit signal the header promises;
    # the evidence that HOST_AS_SEEN -- which the first server exempts -- is
    # the address the guest really sees; and, second, the cause step 7's
    # reaped connection accrued.
    test_clean_log
    pen_act_lines=$(grep -aoE 'ssh: activating penalty of [0-9]+ seconds for [0-9.]+: [a-z ]+' \
                        "$TEST_CLEAN_LOG" || true)
    pen_act_n=$(printf '%s' "$pen_act_lines" | grep -c . || true)
    pen_act1=$(printf '%s\n' "$pen_act_lines" | sed -n 1p)
    pen_act2=$(printf '%s\n' "$pen_act_lines" | sed -n 2p)
    echo "  penalty: guest logged: ${pen_act_lines:-<no activation line>}"
    pen_act_is() {
        [[ "$1" =~ ^ssh:\ activating\ penalty\ of\ [0-9]+\ seconds\ for\ ([0-9.]+):\ (.*)$ ]] \
            && [[ "${BASH_REMATCH[1]}" == "$HOST_AS_SEEN" && "${BASH_REMATCH[2]}" == "$2" ]]
    }
    pen_two_activations() { [[ "${pen_act_n:-0}" == 2 ]]; }
    check "penalty: exactly two activations in the run" \
        pen_two_activations
    check "penalty: the first, for $HOST_AS_SEEN (the address the first server exempts), was for failed authentication" \
        pen_act_is "$pen_act1" "failed authentication"

    # The WHOLE penalty, not the part above PEN_MIN (ruling P1): retries are
    # a second apart, so the success lands in the second after
    # pen_expect_ms. The band is wide enough for scheduling and narrow
    # enough that the old model -- refusing only while remaining > PEN_MIN,
    # so for PEN_MIN seconds less -- and any accrual by the knocks' refusals
    # both fall outside it.
    pen_whole() { [[ "$pen_allowed_ms" -ge $(( pen_expect_ms - 2000 )) && "$pen_allowed_ms" -le $(( pen_expect_ms + 3000 )) ]]; }
    check "penalty: allowed again once the penalty burned down (try $pen_tries)" \
        test -n "$pen_ok_log"
    check "penalty: refused for the WHOLE penalty (${pen_allowed_ms} ms, want $(( pen_expect_ms - 2000 ))-$(( pen_expect_ms + 3000 )))" \
        pen_whole
    check "penalty: after it expired, one failed connection is deferred again (3 prompts)" \
        pen_served_3 "$PEN_F3"
    check "penalty: ...and the connection after it is served and logs in" \
        pen_logged_in "$PEN_AFTER"

    # Step 7 (ruling P7). The hold must be ended by the SERVER, at its short
    # grace -- not before, and not at the probe's own 20 s limit.
    pen_hold_ms=$(sed -nE 's/^hold: closed after_ms=([0-9]+)$/\1/p' <<< "$pen_hold")
    pen_hold_ms=${pen_hold_ms:--1}
    # The floor is half a second under the grace: the reaper cannot drop a
    # connection before its deadline, and a server that ignored GRACE2_SEC
    # for its reaper's first tick (1.5 s) would land under it. The ceiling
    # allows 5 s past the grace: one 1.5 s reap tick plus room for a loaded
    # host, which measured up to 4.75 s against the grace's 3 s.
    pen_reaped() { [[ "$pen_hold_ms" -ge $(( GRACE2_SEC * 1000 - 500 )) && "$pen_hold_ms" -le $(( GRACE2_SEC * 1000 + 5000 )) ]]; }
    check "penalty PRECONDITION: the silent connection was reaped by the second server's ${GRACE2_SEC} s grace (${pen_hold_ms} ms)" \
        pen_reaped
    check "penalty: the reaped connection accrued grace-exceeded -- the second activation names it" \
        pen_act_is "$pen_act2" "login grace exceeded"
    # AND THE REAP IS IN THE AUDIT LOG, at INFO, in sshd's words. OpenSSH
    # 9.9p1's listener, measured against a throwaway sshd with
    # LoginGraceTime 3 and a silent client:
    #   Timeout before authentication for connection from 127.0.0.1 to 127.0.0.1, pid = 1089169
    # AxlSsh has no pid and names the source port instead, as its other audit
    # lines do. Until 2026-09-20 the reap reached nothing above DEBUG. This is
    # the first grace drop on the SECOND server in any selection (every other
    # connection to it completes in well under a second, and nothing else
    # ever connects to it), so the line is at INFO with no suppressed count --
    # and nothing earlier in a full run reaps anything on the FIRST server
    # either (the flood closes its sockets after a second; auth-stall runs
    # after this).
    # NO REASON TAIL, and audit_line's anchoring is what asserts that: this
    # connection never sent a password, so it never joined the resume queue,
    # and the reason NEW-1 added names only a connection that was still
    # WAITING ITS TURN when the reaper took it. A reason appended
    # unconditionally would blame the server for a peer's silence, and this
    # line is what would catch it.
    check "penalty: the audit log records the reaped connection's timeout, with source and no reason" \
        audit_line 'Timeout before authentication for connection'
    check "penalty: ...and the connection after it is refused" \
        pen_refused "$PEN_AFTER_GRACE"

    # Step 7, the knocks (review M-3). A knock that got an identification
    # line got THROUGH the penalty: none may. A knock the server closed
    # without a byte reached on_accept and was refused there: most must, and
    # sequential knocks are how they reach it. Latency is reported, not
    # graded: under host load it is dominated by QEMU's user network (median
    # 19 ms unloaded on both arches, ~500 ms with six suite workers).
    #
    # The loop's health is graded on the guest's heartbeat, and it is worth
    # being exact about WHAT that sees, because "a timer that falls behind
    # the wall clock" is not it. The heartbeat is an EFI PERIODIC timer: a
    # stall shorter than its 2 s period, between two waits, costs it no tick
    # -- the next wait finds it signalled and dispatches it. What it does
    # catch is STARVATION. A refusal that stalled AFTER its RST (in
    # close_wait_inline, the only place on_accept waits) lets the host's next
    # knock arrive during the stall; when on_accept returns, AxlTcp re-arms
    # Accept(), which completes at once on the connection already waiting;
    # and WaitForEvent hands back the LOWEST-index signalled event, where the
    # second server's accept sits ahead of the heartbeat. Stall after every
    # refusal, then, and the accept wins every wait while knocks keep coming:
    # the heartbeat is not dispatched, and this pace check fails.
    #
    # That rests on the heartbeat's slot staying AFTER the listeners' in the
    # loop's source array -- see its registration in ssh-server-selftest.c.
    # Its honest limits: it cannot see an INTERMITTENT stall (one slow
    # refusal among fast ones does not starve anything), nor a delay BEFORE
    # the RST (the peer does not start its next knock until it is refused,
    # so nothing queues behind the stall); those show only as knock latency,
    # which is reported above, not graded.
    pen_knock_field() { sed -nE "s/.* $1=([0-9]+).*/\1/p" <<< "$pen_knock"; }
    pen_knock_done=$(pen_knock_field count); pen_knock_done=${pen_knock_done:-0}
    pen_knock_closed=$(pen_knock_field closed); pen_knock_closed=${pen_knock_closed:-0}
    pen_knock_ident=$(pen_knock_field ident); pen_knock_ident=${pen_knock_ident:--1}
    pen_knock_want=$(( (pen_knock_done + 1) / 2 ))
    # Three quarters of the nominal pace (one tick per 2 s): slack for host
    # jitter, and still far above what a loop blocked for a real fraction of
    # the window could manage. The window is at least 6 s, so at least 2.
    pen_tick_want=$(( pen_knock_win_ms * 3 / 8000 ))
    pen_none_through() { [[ "$pen_knock_done" -ge 1 && "$pen_knock_ident" == 0 ]]; }
    pen_knocks_refused() { [[ "$pen_knock_closed" -ge "$pen_knock_want" && "$pen_knock_closed" -ge 1 ]]; }
    pen_ticked() { [[ $(( pen_tick1 - pen_tick0 )) -ge "$pen_tick_want" && "$pen_tick_want" -ge 2 ]]; }
    check "penalty: no knock got through the second penalty ($pen_knock_done knocks, ident=$pen_knock_ident)" \
        pen_none_through
    check "penalty: the server itself refused most knocks ($pen_knock_closed of $pen_knock_done closed without a byte, want >= $pen_knock_want)" \
        pen_knocks_refused
    check "penalty: the guest's heartbeat kept pace across the knocks ($pen_tick0 -> $pen_tick1 in ${pen_knock_win_ms} ms, want +$pen_tick_want)" \
        pen_ticked

    pen_exempt_served() { pen_forged "$EX_F1" && pen_served_3 "$EX_F2"; }
    check "penalty exempt PRECONDITION: an exempt source's forgery and wrong password were both served" \
        pen_exempt_served
    check "penalty exempt: the same sequence the second server refused leaves an exempt source logging in" \
        pen_logged_in "$EX_OK"
elif wants penalty; then
    echo "SKIP: penalty needs ssh(1) on the host"
fi

# 3) auth-stall -- key exchange completed, then nothing.
#
# WHY THE ASSERTION IS A SOCKET COUNT AND NOT THE TRANSCRIPT: OpenSSH's agent
# RPC is SYNCHRONOUS. While ssh waits for a signature it never reads the
# server socket, so it does not notice the reap; when the agent finally lets
# go it prints "Permission denied" and exits, saying exactly the same thing
# whether the server reaped the connection or not. Measured, not assumed. The
# connection LEAVING the ESTABLISHED table is the observable that differs.
if wants auth-stall && [[ "$HAVE_SSH" == 1 ]]; then
    STALL_SOCK="$TEST_TMPDIR/agent-stall.sock"
    STALL_AGENT_LOG="$TEST_TMPDIR/agent-stall.log"
    # THE AGENT MUST OUTLIVE THE POLL WINDOW, not merely the grace, and the
    # difference is the whole assertion. When the agent lets go, ssh prints
    # Permission denied, EXITS, and takes its socket out of the ESTABLISHED
    # table -- which the detector below cannot tell apart from a server-side
    # reap. Caught by sabotage: with the deadline lifted at NEWKEYS (the Task 4
    # revert this scenario exists to catch) an earlier 95 s agent and 100 s
    # window -- against the 60 s grace this fixture shipped before Task G --
    # scored the drop at 94 s and passed, having measured ssh's own exit.
    # 100 s here against the 70 s window below leaves 30 s in which only the
    # server can be the cause -- the same 30 s margin, now against GRACE_SEC
    # (40 s, see above) instead of the old fixed 60 s.
    STALL_AGENT_LIFETIME=100
    STALL_POLL_TICKS=140          # x 0.5 s = 70 s, comfortably inside it
    python3 "$AGENT" --pub "$TEST_TMPDIR/good.pub" --socket "$STALL_SOCK" \
        --mode stall --lifetime "$STALL_AGENT_LIFETIME" > "$STALL_AGENT_LOG" 2>&1 &
    STALL_AGENT_PID=$!
    BG_PIDS+=("$STALL_AGENT_PID")
    stall_ready=0
    for _ in $(seq 1 40); do
        if grep -q "agent: ready" "$STALL_AGENT_LOG" 2>/dev/null; then
            stall_ready=1; break
        fi
        sleep 0.25
    done
    check "auth-stall: the fake agent bound its socket" \
        test "$stall_ready" -eq 1

    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o IdentityAgent="$STALL_SOCK" -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/good.pub" root@127.0.0.1 true > "$STALL_LOG" 2>&1 &
    STALL_SSH_PID=$!
    BG_PIDS+=("$STALL_SSH_PID")
    DUMP_LOGS+=("$STALL_LOG" "$STALL_AGENT_LOG")

    # "Server accepts key" is PK_OK coming back: proof the connection is past
    # NEWKEYS and still unauthenticated, which is precisely the state the
    # deadline is supposed to cover. Starting the clock anywhere earlier would
    # be timing the boot as well as the grace.
    stall_pk_ok=0
    for _ in $(seq 1 120); do
        if grep -q "Server accepts key" "$STALL_LOG" 2>/dev/null; then
            stall_pk_ok=1; break
        fi
        sleep 0.5
    done
    check "auth-stall: ssh reached the auth phase (the server answered PK_OK)" \
        test "$stall_pk_ok" -eq 1

    # THE DETECTOR'S CONTROL. A count that is zero because it is looking at
    # the wrong thing reads identically to a connection that was reaped, so
    # prove it SEES the connection while it is provably alive.
    stall_live=$(est_to_host_port)
    check "auth-stall CONTROL: the held connection is visible ($stall_live established)" \
        test "$stall_live" -ge 1

    stall_t0=$(date +%s)
    stall_dropped=0
    stall_ssh_alive=0
    stall_ssh_state=""
    for _ in $(seq 1 "$STALL_POLL_TICKS"); do
        if [[ "$(est_to_host_port)" -eq 0 ]]; then
            stall_dropped=1
            # Sampled AT the drop, not after. NOT `kill -0`: that succeeds on
            # a ZOMBIE -- a process that has exited but has not been waited
            # for -- which is exactly what a just-exited ssh is at this
            # instant, since nothing waits for it until after this loop. So
            # read the state field: anything but Z means ssh was genuinely
            # still holding the connection when the socket vanished.
            #
            # Positional awk over /proc/PID/stat is safe HERE and not in
            # general: field 2 is the parenthesised comm and MAY contain
            # spaces, which would shift $3 -- "ssh" contains none. Do not copy
            # this line to a process whose name you do not control.
            #
            # `|| true` because the file disappears entirely once the process
            # is reaped, and an unreadable /proc entry must read as "not
            # confirmed alive", never kill the script.
            stall_ssh_state=$(awk '{print $3}' "/proc/$STALL_SSH_PID/stat" \
                2>/dev/null || true)
            if [[ -n "$stall_ssh_state" && "$stall_ssh_state" != "Z" ]]; then
                stall_ssh_alive=1
            fi
            break
        fi
        sleep 0.5
    done
    stall_elapsed=$(( $(date +%s) - stall_t0 ))

    # The falsifier: lift the deadline at NEWKEYS (or lose the reaper) and this
    # connection is never reaped -- it is still ESTABLISHED when the 70 s
    # poll window runs out. Verified by sabotage.
    check "auth-stall: the post-kex unauthenticated connection was reaped (${stall_elapsed}s)" \
        test "$stall_dropped" -eq 1

    # PRECONDITION, and the one this scenario got WRONG first time round: the
    # socket leaving the table proves the server did something only if the
    # CLIENT was still holding it. ssh exiting -- because the agent gave up,
    # because it was killed -- empties the table just as effectively and reads
    # identically.
    #
    # READ THIS BEFORE TOUCHING THE TIMINGS ABOVE. This check is the SECOND
    # layer, not the guarantee. The guarantee is the 30 s margin between the
    # agent's lifetime and the poll window (STALL_AGENT_LIFETIME vs
    # STALL_POLL_TICKS): inside the window ssh cannot have been released, so
    # only the server can have ended the connection. Shrink that margin to
    # save wall clock and this check does NOT save you -- it is a same-instant
    # sample, and a client that exits a tick before the socket vanishes still
    # reads alive. This file has already had one timing tuned without
    # rereading the reasoning behind it; the margin is the design, and this
    # confirms it rather than replacing it.
    check "auth-stall PRECONDITION: ssh still held the connection when it vanished (state ${stall_ssh_state:-gone})" \
        test "$stall_ssh_alive" -eq 1

    # And it lasted the GRACE, and no longer. Without a floor, a server that
    # dropped every post-kex connection immediately -- a different and much
    # worse bug -- would satisfy the check above; without a ceiling, anything
    # that eventually closed the socket would. Measured at 44 s on x64 and
    # 41 s on aa64: GRACE_SEC (40) plus at most one reap tick (5 s --
    # unchanged from the library's REAP_INTERVAL_MS default, since GRACE_SEC
    # is >= 10 s, see axl_ssh_server_set_login_grace's Granularity
    # paragraph), less the couple of seconds between accept and the PK_OK
    # this clock starts from.
    #
    # THE UPPER BOUND IS THE SABOTAGE, not a guess. This scenario's band was
    # [45, 85] against the shipped 60 s grace (the ceiling was G + one reap
    # tick, the floor G - 15). Rescaling that shape to GRACE_SEC (40) gave
    # [25, 65] -- and a setter that ignores its argument
    # (`s->login_grace_sec = s->login_grace_sec` for `= seconds`,
    # scripts/sabotage.sh) leaves the server at its old 60 s default and
    # this scenario measured 64 s under it: INSIDE [25, 65], so that band
    # passed a server that never applied GRACE_SEC at all. 50 is
    # comfortably above every genuine GRACE_SEC measurement above and
    # comfortably below every sabotaged one (57-65, by the same arithmetic
    # against a 60 s grace).
    stall_in_grace_band() {
        [[ "$stall_elapsed" -ge 25 && "$stall_elapsed" -le 50 ]]
    }
    check "auth-stall: reaped at the login grace, not before or long after (${stall_elapsed}s, want 25-50)" \
        stall_in_grace_band

    kill "$STALL_AGENT_PID" 2>/dev/null || true
    wait "$STALL_AGENT_PID" 2>/dev/null || true
    # ssh is released by the agent going away, and it must be given the chance
    # to flush its transcript -- that log IS the diagnosis on a failure. A
    # BOUNDED wait, not a bare `wait`: this whole scenario exists because ssh
    # can block indefinitely on an agent, so hanging the test on it would be
    # the same mistake one level up.
    for _ in $(seq 1 40); do
        if ! kill -0 "$STALL_SSH_PID" 2>/dev/null; then break; fi
        sleep 0.25
    done
    kill "$STALL_SSH_PID" 2>/dev/null || true
    wait "$STALL_SSH_PID" 2>/dev/null || true

    check "auth-stall: the server still serves a good login afterwards" \
        good_login_works auth-stall
elif wants auth-stall; then
    echo "SKIP: auth-stall needs ssh(1) on the host"
fi

hostile_finish
