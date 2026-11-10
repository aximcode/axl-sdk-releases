#!/bin/bash
# test-meta: arch=both needs= est=50 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-live-flood-qemu.sh — a password login gets in WHILE a connection
# flood is still running against the same server (spec
# docs/superpowers/specs/2026-08-21-axl-ssh-design.md section 12a.2, and
# axl_ssh_server_set_max_startups in include/axl/axl-ssh-server.h).
#
# test-ssh-admission-qemu.sh proves admission against floods that have
# STOPPED: its logins arrive after the silent connections are all open. Two
# mechanisms only show while a flood keeps going:
#
#   1. The victim clock. With age as the only rank, an honest client
#      survives about max_startups further arrivals, so a flood of R
#      connections a second evicts it unless its key exchange ends within
#      roughly max_startups / R. Ranking by protocol progress (eviction_victim)
#      takes that clock away from any client that has sent its KEXINIT.
#   2. Loop starvation. The accept is armed for the server's whole life, so
#      under a sustained flood the listener can be ready on every loop
#      iteration; with WaitForEvent's lowest-index-first choice it would win
#      every one and starve the login's own receives. AxlLoop's round-robin
#      wait (src/loop/axl-loop.c) is what gives them a turn.
#
# ONE SCENARIO, live-flood. ssh-hostile.py's `churn` keeps LIVE_FLOOD_WIDTH
# silent connections going, each closed LIVE_FLOOD_HOLD seconds after it
# connected and replaced at once. Once the server has started evicting --
# it is at max_startups and every arrival displaces someone -- a peer stops
# at the point where ssh(1) waits for its user to accept the host key, and
# five ssh(1) password logins run, 1.5 s apart, inside the flood. Every
# login must get in and the waiting peer must never be evicted.
#
# WHAT IT DISCRIMINATES, measured on both arches (2026-10-08, default shape):
#
#   26-28 connections a second achieved (32 nominal), 15-30 evictions a
#   second at the guest.
#   Shipped code: every login in, each in ~0.2 s, the waiting peer held (five
#     runs across both arches).
#   Victim ranked by age alone: the waiting peer evicted within 0.52 s in
#     all three runs (x64 twice, aa64 once); the LOGINS lost to the same
#     clock only once (1 of 5, on aa64) -- a login is exposed only for its
#     own ~0.2 s key exchange -- which is why the waiting peer is the
#     discriminating check. The waiting peer was evicted within 0.9 s at
#     the earlier ~60/s shape too (2026-09-27); that shape's lost logins are
#     no evidence either way, being above the ceiling below.
#   Loop without the round-robin wait: everything passed. Starvation did NOT
#     reproduce at this rate: the guest keeps up, so the listener is not
#     ready on every iteration. The loop fix rests on its unit test
#     (axl-test-loop.c's fairness checks), not on this one.
#
# THE CEILING OF THIS HARNESS IS ABOUT 40 A SECOND, not the ~60 first
# measured. Past it, a login's connection is accepted at once by QEMU's user
# network on the host side, but the SYN that network sends on to the guest
# is discarded while the firmware TCP's accept backlog of ten is full
# (TcpDxe logs `SockCreate: ... 10 ConnCnt and 10 BackLog`), and only the
# user network's own TCP retries it, seconds later. Measured 2026-10-08:
# the old default (32 wide, 64/s nominal) lost logins in every run offered
# >= 59/s; 24 wide (48/s nominal) achieved 35-43.5/s on x64 and lost one
# login in three runs, at 43.5/s, and 39-44/s on aa64, where it lost none
# but one run had two logins rescued only by a 6 s retry. Far past it
# (~230/s), new connections stop reaching the guest at all, the scenario
# overruns its budget, and hostile_finish says the deadline fired first. (At
# ~150/s the pre-P4a code stopped answering the same way, 2026-09-27.) So
# above the ceiling this test measures the network path, not admission, and
# the default (16 wide) stays well below it on both arches.
#
# THE FLOOD IS EXEMPT FROM PENALTIES. Every connection shares one source
# (QEMU's gateway, 10.0.2.2), and without the exemption the flood's `noauth`
# accruals would activate a penalty within seconds -- the logins would then be
# refused by the penalty ladder, and the outcome would say nothing about
# admission. So this runs on the fixture's FIRST server, which exempts that
# address (lib/ssh-hostile-lib.sh, the HOSTILE_EXEMPT default), with the
# shipped max_startups (10). The check that no penalty activated is the
# evidence the exemption held.
#
# sshloglevel=debug: the audit log writes one eviction line a minute at INFO
# and the rest at DEBUG, which at the default level is ABSENT, so counting the
# flood's evictions needs both levels. `Accepted` is never rate-limited, so
# each login's line is at INFO and is counted there.
#
# Usage: ./test/integration/test-ssh-live-flood-qemu.sh [--arch X64|AARCH64]
#        LIVE_FLOOD_WIDTH / LIVE_FLOOD_HOLD override the flood's shape.

HOSTILE_SUMMARY="ssh live flood"
HOSTILE_SCENARIOS="live-flood"

# Seconds the scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh, "the fixture's deadline"): up to ~2 s for the flood
# to reach the cap, then five logins 1.5 s apart, each ~0.2 s -- or ~6 s for
# one whose SYN the guest discarded (see THE CEILING above, and pw_login).
declare -A HOSTILE_BUDGET=([live-flood]=16)

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

GRACE_SEC=60
HOSTILE_EXTRA_ARGS="sshloglevel=debug"

FLOOD_WIDTH=${LIVE_FLOOD_WIDTH:-16}
FLOOD_HOLD=${LIVE_FLOOD_HOLD:-0.5}
# Logins spread across the flood rather than back to back: a login takes
# ~0.2 s, so five in a row would sample one moment of it.
LOGINS=5
LOGIN_GAP=1.5
# Inside the logins' ~8 s, so the flood outlasts it.
PROMPT_HOLD=4
PEER="$TESTS_DIR/ssh-peer.py"

hostile_boot "AxlSsh live flood"

# A password login with ssh(1), transcript to $1; sets LOGIN_MS to how long
# it took to authenticate, or to "-" if it never did, and CONNECT_MS to how
# much of that was the host-side TCP connect -- kept as the CONTROL that says
# where a slow login's time did NOT go. That connect completes at QEMU's user
# network at once (~0.1 s even for the slow logins), so a slow login's
# time goes AFTER it: the user network's SYN to the guest was discarded at a
# full accept backlog and is retried by that network's TCP (see THE CEILING
# in the header), not by the server. `-N`: no channel exists
# yet (P4b), so there is no command to run and ssh would sit logged in until
# `timeout` ended it; it is stopped as soon as its transcript says it is in
# instead, so each login costs what it takes and the flood around it stays
# short. `timeout 20` is the bound if it never gets in: generous, because
# under the flood a login's SYN toward the guest can be discarded and retried
# -- two logins of some eighty measured took 6 s, the rest ~0.2 s -- and the
# claim under test is that it gets in, not how fast.
pw_login() {
    local pid t0 i
    : > "$1.count"
    t0=$(date +%s%N)
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$1.count" \
    timeout 20 ssh -vvv -N -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=15 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 > "$1" 2>&1 < /dev/null &
    pid=$!
    BG_PIDS+=("$pid")
    LOGIN_MS=-
    CONNECT_MS=-
    for ((i = 0; i < 210; i++)); do
        if [[ "$CONNECT_MS" == - ]] \
            && grep -qF 'debug1: Connection established.' "$1" 2>/dev/null
        then
            CONNECT_MS=$(( ($(date +%s%N) - t0) / 1000000 ))
        fi
        if grep -q "$SUCCESS_PW_RE" "$1" 2>/dev/null; then
            LOGIN_MS=$(( ($(date +%s%N) - t0) / 1000000 ))
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 0.1
    done
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    DUMP_LOGS+=("$1")
}

EVICT_WHY=': past MaxStartups, key exchange unfinished'

# --- live-flood --------------------------------------------------------------
if wants live-flood; then
    if [[ "$HAVE_SSH" -eq 0 ]]; then
        echo "SKIP: live-flood needs ssh(1)"
    else
        LF_OUT="$TEST_TMPDIR/live-flood.out"
        LF_ERR="$TEST_TMPDIR/live-flood.err"
        python3 "$PROBE" churn 127.0.0.1 "$HOST_PORT" --count "$FLOOD_WIDTH" \
            --hold "$FLOOD_HOLD" --duration 60 > "$LF_OUT" 2> "$LF_ERR" &
        LF_PID=$!
        BG_PIDS+=("$LF_PID")
        wait_line "$LF_ERR" 'churn: started' 30 || true
        # The flood is PRESSING admission once the server has evicted: from
        # then on it holds max_startups unauthenticated connections and every
        # arrival displaces one. Logging in before that would test an idle
        # server. The raw log, as test_wait_for reads it: this is sampled
        # mid-scenario.
        lf_pressed=0
        for ((i = 0; i < 200; i++)); do
            if [[ "$(raw_count 'Evicted connection')" -ge 1 ]]; then
                lf_pressed=1
                break
            fi
            sleep 0.1
        done
        test_clean_log
        lf_ev0=$(audit_text_count 'Evicted connection' "$EVICT_WHY")
        # The client that makes the victim clock DETERMINISTIC: one that has
        # the server's KEX_ECDH_REPLY and then waits, as ssh(1) does while its
        # user answers the host-key prompt -- the server holds it in
        # ST_NEWKEYS. Ranked by age alone it would be evicted within about
        # max_startups arrivals (a fraction of a second at this rate); ranked
        # by progress, no silent peer can displace it. The logins below only
        # expose that clock for their own ~0.2 s key exchange, which is why
        # they alone lost to it only some of the time.
        LF_PROMPT="$TEST_TMPDIR/live-flood-prompt.out"
        python3 "$PEER" kexwait 127.0.0.1 "$HOST_PORT" --stage reply \
            --hold "$PROMPT_HOLD" > "$LF_PROMPT" 2>&1 &
        LF_PROMPT_PID=$!
        BG_PIDS+=("$LF_PROMPT_PID")
        lf_ok=0
        lf_ms=""
        for ((n = 1; n <= LOGINS; n++)); do
            sleep "$LOGIN_GAP"
            pw_login "$TEST_TMPDIR/ssh-live-flood-$n.log"
            lf_ms+="${lf_ms:+,}$LOGIN_MS/$CONNECT_MS"
            if [[ "$LOGIN_MS" != - ]]; then lf_ok=$((lf_ok + 1)); fi
        done
        LF_PROMPT_RC=0
        wait "$LF_PROMPT_PID" || LF_PROMPT_RC=$?
        # Still flooding when the last login and the prompt ended?
        lf_alive=0
        if kill -0 "$LF_PID" 2>/dev/null; then lf_alive=1; fi
        kill -TERM "$LF_PID" 2>/dev/null || true
        wait "$LF_PID" || true
        test_clean_log
        lf_acc=$(grep -caxE "[0-9:.]+ \[INFO\]  ssh: Accepted password for pwuser from ${HOST_AS_SEEN//./\\.} port [0-9]+" \
            "$TEST_CLEAN_LOG" || true)
        lf_ev=$(( $(audit_text_count 'Evicted connection' "$EVICT_WHY") - lf_ev0 ))
        echo "  live-flood: $(cat "$LF_OUT" 2>/dev/null); guest evictions" \
             "$lf_ev0 before the logins, $lf_ev during them; logins in" \
             "$lf_ok/$LOGINS (ms, login/connect: $lf_ms);" \
             "Accepted lines $lf_acc; host-key-prompt client" \
             "'$(tr '\n' '|' < "$LF_PROMPT")' rc=$LF_PROMPT_RC"

        check "live-flood PRECONDITION: the flood started" \
            grep -qxF 'churn: started' "$LF_ERR"
        check "live-flood PRECONDITION: the flood reached the cap before the logins" \
            test "$lf_pressed" = 1
        check "live-flood PRECONDITION: the flood outlasted every login" \
            test "$lf_alive" = 1
        check "live-flood CONTROL: the server kept evicting while the logins ran" \
            test "${lf_ev:-0}" -ge "$LOGINS"
        check "live-flood CONTROL: no penalty activated (the exemption held)" \
            test "$(guest_count 'activating penalty')" = 0
        check "live-flood: every password login got in during the flood" \
            test "$lf_ok" = "$LOGINS"
        check "live-flood: one INFO Accepted password line per login" \
            test "${lf_acc:-0}" = "$LOGINS"
        check "live-flood: the server still answers once the flood has stopped" \
            python3 "$PROBE" alive 127.0.0.1 "$HOST_PORT"
        check "live-flood: a client held at the host-key prompt was never evicted" \
            peer_is "$LF_PROMPT_RC" 0 "$LF_PROMPT" 'peer: host key verified' \
            'peer: held rx=0'
    fi
fi

hostile_finish
