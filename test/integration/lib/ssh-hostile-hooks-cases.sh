# shellcheck shell=bash
# ssh-hostile-hooks-cases.sh -- the body of the parts below: everything that was
# test-ssh-hostile-hooks-qemu.sh -- its doc, budgets, boot, scenarios and finish.
# Each part sets HOSTILE_SUMMARY and HOSTILE_SCENARIOS and sources this
# file; lib/ssh-hostile-lib.sh checks that a part's scenarios follow
# HOSTILE_SCENARIOS_ALL in its order and that the parts together run all
# of them. One boot is the unit the runner's pool parallelises, so the
# scenarios that dominate the time each have a boot of their own:
#
#   test-ssh-hostile-hooks-qemu.sh         auth-pending-drop auth-hook-nest auth-hook-free
#   test-ssh-hostile-hooks-reenter-qemu.sh auth-hook-reenter
#
# What follows is the original header, unchanged.
#
# test-ssh-hostile-hooks-qemu.sh — AxlSsh survives the P3 password-hook
# hazards: a verdict that outlives its client, a hook that dispatches the loop
# while the grace runs out under it, hooks that would nest, and the server
# freed from inside a hook.
#
# ONE OF TWO HALVES. Malformed input, the flood, auth-cap, auth-forge,
# auth-stall and the per-source penalties are test-ssh-hostile-qemu.sh; the
# setup, helpers and teardown both run are lib/ssh-hostile-lib.sh, which also
# says why there are two and where the seam falls. Scenarios here, in run
# order, with the post-abuse login between the last two in a full run:
#
#   auth-pending-drop auth-hook-reenter auth-hook-nest auth-hook-free
#
# WHAT EACH ONE WOULD MISS IF IT WERE DROPPED:
#   auth-pending-drop -- a password verdict answered ~3 s after its hook,
#                     with the client killed inside that window. The late
#                     completion must be refused as stale, and the server
#                     must still log the next user in.
#   auth-hook-reenter -- a password hook that runs the server's loop from
#                     INSIDE itself (the nested wait include/axl/axl-loop.h
#                     documents as safe) while the login grace runs out, so
#                     the reaper fires against the connection whose hook is
#                     on the stack. Without the deferral that frees memory
#                     every frame below the hook still uses: the guest dies
#                     or corrupts its heap, and nothing else in the suite
#                     makes a hook dispatch the loop. It also carries THE
#                     WHOLE HOOK WATCHDOG (NEW-1): a 20 s hook is the only
#                     thing in the suite that holds the one-hook slot long
#                     enough to be reported, a second client queued behind
#                     it is reaped in the one state the reaper used to be
#                     silent about, and a login ahead of both BLOCKS without
#                     dispatching -- the case the watchdog cannot see,
#                     asserted as unseen with the 20 s hook as its control.
#   auth-hook-nest -- three clients whose hooks each dispatch the loop, their
#                     passwords delivered so the hooks would nest. The server
#                     runs one hook at a time and the rest in arrival order;
#                     without that the fixture sees three hooks on the stack.
#   auth-hook-free -- the server freed from inside a hook's dispatch. Without
#                     the deferred free it spins for ever, or frees memory
#                     that every frame under the hook still uses. It ends the
#                     server, so it runs LAST, after the post-abuse login.
#
# EVERY scenario but auth-hook-free ends by requiring a good login to still
# succeed. That is the only assertion that separates "refused the attack" from
# "died refusing it". auth-hook-free cannot -- it frees the server -- so its
# liveness evidence is the heartbeat, SERVER: DONE and the leak verdict at
# teardown (lib/ssh-hostile-lib.sh's hostile_finish).
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_KEEP=<dir> keeps every ssh(1)
# transcript.
#
#
# Usage (a part): ./test/integration/test-ssh-hostile-hooks-qemu.sh [--arch X64|AARCH64]

HOSTILE_SCENARIOS_ALL="auth-pending-drop auth-hook-reenter auth-hook-nest auth-hook-free"

# Seconds each scenario may take, summed into the fixture's deadline
# (lib/ssh-hostile-lib.sh, "the fixture's deadline"). Each is the scenario's
# measured time on the slower arch, standalone, 2026-09-18, plus what its own
# waits still allow it:
#   auth-pending-drop  4 s measured: a prompt, a 0.5 s pause, the ~3 s slow
#                      verdict, a second login
#   auth-hook-reenter  52 s measured on aa64, 51 on x64: the 30 s askpass
#                      sleep, the 20 s ITERATE_US dispatch, the login after --
#                      fixed waits, so +6 s covers a slow handshake either
#                      side of them. +8 for the watchdog's blocking login,
#                      which runs FIRST (BLOCK_MS is 6 s plus its
#                      handshake); the queued client costs nothing, since
#                      everything it does happens inside the 20 s hook
#   auth-hook-nest     19 s measured: three handshakes, 3 x NEST_US (6 s) in
#                      series, the host's 1.5 s between releases
#   auth-hook-free     4 s measured: a control login, then the 3 s FREE_US
#                      dispatch
# The post-abuse login (full runs only) takes about a second and is carried by
# the base margin rather than a budget of its own: it is not a scenario, and
# SSH_HOSTILE_ONLY cannot select it.
declare -A HOSTILE_BUDGET=(
    [auth-pending-drop]=8 [auth-hook-reenter]=66
    [auth-hook-nest]=25 [auth-hook-free]=8
)

# Every scenario here is a password login through a real ssh(1), so without
# one this file has nothing of its own to test: a SKIP (exit 77), not a PASS
# earned by booting an idle server.
if ! command -v ssh >/dev/null 2>&1; then
    echo "SKIP: no ssh(1) on the host — every scenario here logs in with it"
    exit 77
fi

source "$(dirname "$0")/lib/ssh-hostile-lib.sh"

# Task G: axl_ssh_server_set_login_grace(), and here the grace is LOAD-BEARING
# twice over, which is why it stays at 40 s after the split:
#   - auth-hook-reenter's margin arithmetic is written against it (below): the
#     password reaches the hook >= 7 s before the grace ends, and the first
#     reap tick past it lands >= 6 s inside the hook's 20 s dispatch. A
#     different grace moves both edges and needs REENTER_ASKPASS_SLEEP moved
#     with it.
#   - auth-hook-nest's three NEST_US hooks run in series (~18 s) while B and C
#     wait, and each of those logins must finish inside its own grace, counted
#     from its own accept. B connects first and waits through C's and A's
#     handshakes and A's whole hook before its own runs, so its login ends
#     about two hooks and three handshakes after its accept; a grace that did
#     not cover that with room would drop B -- the very thing the scenario
#     asserts does not happen.
# The reap tick stays the unchanged 5 s default (a grace of 10 s or more, see
# axl_ssh_server_set_login_grace's Granularity paragraph).
GRACE_SEC=40

# NEW-1: axl_ssh_server_set_auth_hook_budget_ms(), through the fixture's
# hookbudget= option, for this boot only.
#
# FIVE SECONDS, NOT THE SHIPPED ONE, and the number is load-bearing twice:
#   - it takes the RACE out of the mid-hook line. That line rides the
#     reaper's 5 s sweep, so with a 1 s budget the first sweep that can write
#     it lands anywhere in [hook + 1 s, hook + 6 s] -- possibly before the
#     queued client below has even sent its password, which would make the
#     depth that line carries a coin toss. At 5 s the earliest is hook + 5 s,
#     by which time that client has been queued for seconds.
#   - it is the floor the blocking hook has to clear. BLOCK_MS is 6 s
#     (ssh-server-selftest.c), so the SLOW-return line it must still produce
#     lands and the missing mid-hook line means something.
# The SHIPPED default is exercised by AxlTestSsh's hook-budget API test and
# by every other boot in the suite, which passes no hookbudget= at all.
HOOK_BUDGET_MS=5000

# ...and the ssh domain at DEBUG for this boot, which is what makes any
# assertion about HOW MANY lines of a kind a path writes possible at all.
#
# The audit log gives each KIND one INFO line a minute and writes the rest
# at DEBUG -- and at the library's default level a DEBUG line is not
# demoted in the guest log, it is ABSENT. So "at most ONE mid-hook line per
# hook call", which the header promises in those words, was unobservable: a
# version that wrote three wrote one INFO line and two nothings, and looked
# identical to the correct one. MEASURED, not suspected -- the independent
# review sabotaged the latch away and this file stayed green at 22/0.
#
# It also makes the 20 s hook's own slow-RETURN line assertable, which was a
# disclosed gap: that line is the second of its kind inside the minute and
# was therefore invisible. audit_text_count's regex already anticipates the
# `ssh:func:NNNN:` framing a DEBUG line carries, so no helper changed.
#
# The cost is small and bounded: the ssh domain has twelve axl_debug sites
# and none of them is on a per-packet path.
HOOK_LOG_LEVEL=debug
HOSTILE_EXTRA_ARGS="hookbudget=${HOOK_BUDGET_MS} sshloglevel=${HOOK_LOG_LEVEL}"

hostile_boot "AxlSsh hostile password hooks"

# 1) auth-pending-drop -- a client that leaves while its verdict is pending.
#
# The fixture answers slowuser's password ~3 s after on_password returns
# (TICKS_SLOW, test/integration/ssh-server-selftest.c); this kills the client
# well inside that window. The only evidence is in the guest: a completion
# that finds nobody waiting must be refused as stale rather than written
# through freed connection memory, and the server must go on to log the next
# user in normally.
#
if wants auth-pending-drop && [[ "$HAVE_SSH" == 1 ]]; then
    DROP_LOG="$TEST_TMPDIR/ssh-pending-drop.log"
    AFTER_LOG="$TEST_TMPDIR/ssh-after-drop.log"
    : > "$DROP_LOG.count"

    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_SLOW" ASKPASS_COUNT="$DROP_LOG.count" \
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        slowuser@127.0.0.1 true > "$DROP_LOG" 2>&1 < /dev/null &
    drop_pid=$!
    DUMP_LOGS+=("$DROP_LOG")

    # Kill only once the password has really been asked for -- otherwise no
    # request is pending, nothing can go stale, and the stale assertion below
    # fails for the wrong reason. The verdict is ~3 s away; this leaves margin.
    for _ in $(seq 1 100); do
        [[ "$(wc -l < "$DROP_LOG.count" | tr -d ' ')" -ge 1 ]] && break
        sleep 0.1
    done
    drop_asked=$(wc -l < "$DROP_LOG.count" | tr -d ' ')
    sleep 0.5
    kill "$drop_pid" 2>/dev/null || true
    wait "$drop_pid" 2>/dev/null || true

    # Wait for the stale completion itself rather than sleeping a fixed guess
    # and hoping it landed before test_clean_log samples the log below --
    # test_wait_for polls the RAW log directly every second, so this is safe
    # even before test_clean_log has run. TICKS_SLOW is ~3 s from when the
    # hook returned; 10 s leaves room for a slower box. `|| true`: a timeout
    # here must not abort the script under set -e -- the exact-count check
    # below is the real assertion and fails on its own if this never arrives.
    test_wait_for "SERVER: COMPLETE-STALE" 10 || true

    : > "$AFTER_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$AFTER_LOG.count" \
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 true > "$AFTER_LOG" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$AFTER_LOG")

    after_ok()       { grep -q "$SUCCESS_PW_RE" "$AFTER_LOG"; }

    test_clean_log
    check "auth-pending-drop PRECONDITION: the password was sent before the kill" \
        test "${drop_asked:-0}" = 1
    check "auth-pending-drop: the late verdict was refused as stale, once" \
        test "$(guest_count 'SERVER: COMPLETE-STALE')" = 1
    check "auth-pending-drop: no completion reported an error" \
        test "$(guest_count 'SERVER: COMPLETE-ERR')" = 0
    check "auth-pending-drop: the server still logs the next user in" \
        after_ok
elif wants auth-pending-drop; then
    echo "SKIP: auth-pending-drop needs ssh(1) on the host"
fi

# 2) auth-hook-reenter -- a hook that dispatches the server's loop while the
# login grace expires underneath it.
#
# include/axl/axl-loop.h documents a nested wait from inside any source
# callback as SAFE, and a password hook that checks a remote service does
# exactly that. While it waits, every other source fires -- including
# on_reap, which frees an unauthenticated connection past the login grace.
# If that connection is the one whose hook is on the stack, every frame below
# the hook (issue_request, pump, on_recv) resumes on freed memory. The server
# must instead mark it and drop it on the ordinary path once the hook returns.
#
# WHY THE REAPER AND NOT A RESET: a hook reached from a connection's own
# on_recv cannot see that connection's reset -- its receive is not re-armed
# until the callback returns -- and ssh(1) never pipelines a password behind
# a pending failure, which is the only other way in. The grace is the lever
# ssh(1) can pull.
#
# THE MARGIN ARITHMETIC, from the guest's accept (t = 0, deadline at GRACE_SEC
# = 40 s, set through axl_ssh_server_set_login_grace -- see above):
#   - ssh finishes kex and the "none" query in ~1-3 s, runs askpass, and
#     askpass sleeps REENTER_ASKPASS_SLEEP = 30 s: the password reaches the
#     hook at ~31-33 s. That is >= 7 s BEFORE the deadline, and the reaper
#     drops nothing until now > deadline, so no tick can take the connection
#     before its hook runs.
#   - the hook dispatches for ITERATE_US = 20 s (ssh-server-selftest.c,
#     unchanged -- shrinking it too is possible but was not needed once the
#     deadline itself moved earlier): window ~31-33 s -> ~51-53 s.
#   - on_reap ticks every 5 s (REAP_INTERVAL_MS, unchanged from the library
#     default: GRACE_SEC is >= 10 s, so axl_ssh_server_listen derives the
#     same tick it always has -- see axl_ssh_server_set_login_grace's
#     Granularity paragraph), so the first tick past the deadline lands in
#     (40, 45] s: >= 6 s inside the far end of the window, same margin the
#     60 s grace gave this scenario before Task G.
# Shrinking either number to save wall clock can move the tick outside the
# window, and then this scenario stops testing anything: the recorded ACCEPT
# is applied and ssh AUTHENTICATES, which the never-authenticated check below
# turns into a red rather than a silent pass.
#
# THE HOOK WATCHDOG RIDES THE SAME WINDOW (NEW-1), with a SECOND client, Q,
# and no extra wall clock -- everything Q does happens inside A's 20 s hook.
# Q connects alongside A and parks at its password prompt on a gate file;
# the host opens that gate the moment the guest reports A's hook ENTERED
# (SERVER: PW-ALLOC ... hookuser, printed by on_password before
# check_password), so Q's password lands a fraction of a second INSIDE the
# hook and can only be queued -- never parsed, never its own hook. From
# Q's accept (t = 0, its own grace also GRACE_SEC):
#   - Q is queued from ~t_hook + 0.5 s, i.e. ~32-34 s;
#   - the first sweep that can write the mid-hook line is t_hook + 5 s
#     (HOOK_BUDGET_MS), ~36-38 s, and the sweep does the watchdog report
#     BEFORE its drop loop -- so Q is on the queue for it either way, and
#     that line's depth is 1, not a coin toss;
#   - Q's own grace runs out at 40 s and the tick in (40, 45] reaps it while
#     it is STILL QUEUED, which is the exact state the reaper used to be
#     silent about. A is reaped by that same tick, but A was never queued,
#     so the two produce DIFFERENT lines -- and, because they are different
#     rate-limit kinds, both at INFO;
#   - A's hook returns at ~51-53 s, at least 6 s after Q is gone, so the
#     slow-RETURN line's depth is a real 0 rather than an unwritten field.
# A login AHEAD of all of this blocks for BLOCK_MS without dispatching: it
# produces the slow-RETURN line and, necessarily, no mid-hook line at all.
# It goes first for a rate-limiter reason spelled out at its own site.
#
# THE PROOF IS TWO OBSERVATIONS TOGETHER. ITERATE-DONE 0 says the connection
# was still LISTED when the hook answered, so the verdict was recorded; ssh
# never authenticating says that recorded ACCEPT was not applied -- which is
# only true if the connection was marked for its drop DURING the hook. Either
# alone is satisfied by a different server.
if wants auth-hook-reenter && [[ "$HAVE_SSH" == 1 ]]; then
    # FIRST, before anything else in this boot holds a hook: the login whose
    # hook BLOCKS. It holds the server for BLOCK_MS (6 s, over
    # HOOK_BUDGET_MS) with axl_msleep, which waits on one timer event and
    # dispatches no AxlLoop source -- so the reaper sweep that writes the
    # mid-hook line cannot run while it is held.
    #
    # WHY FIRST, and it is the rate limiter that decides it. Each audit KIND
    # gets one INFO line a minute and the rest go to DEBUG, which this guest
    # does not print at all -- so the SECOND line of a kind inside one
    # scenario is not merely demoted, it is INVISIBLE to any assertion.
    # Running this login first puts its slow-RETURN line first of the
    # HOOK_SLOW kind, and it leaves the HOOK_STALLED window untouched, so
    # the mid-hook line this hook must NOT produce would land at INFO if a
    # regression ever wrote one. Both halves of the pair are therefore
    # assertable, which they are not in the other order. (The 20 s hook
    # below writes a slow-return line too; it is inside the same minute and
    # invisible, which costs nothing -- it is the same app_call_end report,
    # and the mid-hook line it DOES produce is what needs a dispatching
    # hook.)
    BLOCK_LOG="$TEST_TMPDIR/ssh-hook-block.log"
    : > "$BLOCK_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_BLOCK" ASKPASS_COUNT="$BLOCK_LOG.count" \
    timeout 60 ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        blockuser@127.0.0.1 true > "$BLOCK_LOG" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$BLOCK_LOG")
    test_wait_for "SERVER: BLOCK-DONE" 30 || true

    REENTER_LOG="$TEST_TMPDIR/ssh-hook-reenter.log"
    REENTER_AFTER_LOG="$TEST_TMPDIR/ssh-after-reenter.log"
    REENTER_ASKPASS_SLEEP=30
    : > "$REENTER_LOG.count"

    # `timeout` bounds the client: on a server that died in the hook window
    # nothing ever closes its socket, and a bare `wait` below would hang the
    # whole file on it. 100 s is the 30 s sleep, the 20 s dispatch and the
    # same 50 s margin the fixture had before Task G; a healthy run exits at
    # ~52 s.
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_ITERATE" ASKPASS_COUNT="$REENTER_LOG.count" \
    ASKPASS_SLEEP="$REENTER_ASKPASS_SLEEP" \
    timeout 100 ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        hookuser@127.0.0.1 true > "$REENTER_LOG" 2>&1 < /dev/null &
    reenter_pid=$!
    BG_PIDS+=("$reenter_pid")
    DUMP_LOGS+=("$REENTER_LOG")

    # Q: the connection that gets STARVED behind A's hook and reaped in that
    # state. Connected now, so its grace runs from the same moment A's does;
    # its password waits on a gate the host opens once A's hook is on the
    # stack. pwuser, whose hook would answer at once -- which is the point:
    # if the server ever parsed this password instead of queueing it, Q
    # would AUTHENTICATE, and the never-authenticated check below turns that
    # into a red rather than a quiet pass.
    REENTER_Q_LOG="$TEST_TMPDIR/ssh-reenter-queued.log"
    REENTER_Q_GATE="$TEST_TMPDIR/reenter-gate-q"
    : > "$REENTER_Q_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$REENTER_Q_LOG.count" \
    ASKPASS_GATE="$REENTER_Q_GATE" \
    timeout 100 ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 true > "$REENTER_Q_LOG" 2>&1 < /dev/null &
    reenter_q_pid=$!
    BG_PIDS+=("$reenter_q_pid")
    DUMP_LOGS+=("$REENTER_Q_LOG")

    # A's hook ENTERED -- on_password prints PW-ALLOC before check_password
    # runs, so this is the earliest the host can know. Only then is Q's
    # password released, which is what makes "queued, never parsed"
    # structural instead of a race between two askpass sleeps. `|| true`: a
    # guest that never enters the hook fails the ITERATE preconditions
    # below, which is where that is diagnosed.
    test_wait_for "SERVER: PW-ALLOC .* hookuser" 90 || true
    : > "$REENTER_Q_GATE"

    # The hook's own report is the end of the window. Matches ITERATE-DONE
    # and ITERATE-FAIL alike, so a hook whose dispatch was cut short does not
    # cost the full timeout; the checks below tell the two apart. `|| true`:
    # a guest that died never prints it, and the exact counts below are the
    # assertions that say so.
    test_wait_for "SERVER: ITERATE-" 90 || true
    wait "$reenter_pid" 2>/dev/null || true
    wait "$reenter_q_pid" 2>/dev/null || true
    reenter_asked=$(wc -l < "$REENTER_LOG.count" | tr -d ' ')
    reenter_q_asked=$(wc -l < "$REENTER_Q_LOG.count" | tr -d ' ')

    : > "$REENTER_AFTER_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$REENTER_AFTER_LOG.count" \
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 true > "$REENTER_AFTER_LOG" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$REENTER_AFTER_LOG")

    reenter_after_ok()   { grep -q "$SUCCESS_PW_RE" "$REENTER_AFTER_LOG"; }
    reenter_never_auth() { ! grep -q "$SUCCESS_PW_RE" "$REENTER_LOG"; }
    reenter_q_never_auth() { ! grep -q "$SUCCESS_PW_RE" "$REENTER_Q_LOG"; }
    block_ok()           { grep -q "$SUCCESS_PW_RE" "$BLOCK_LOG"; }

    test_clean_log
    check "auth-hook-reenter PRECONDITION: the password was asked for once" \
        test "${reenter_asked:-0}" = 1
    check "auth-hook-reenter PRECONDITION: the hook dispatched the loop and answered, once" \
        test "$(guest_count 'SERVER: ITERATE-DONE')" = 1
    check "auth-hook-reenter: the in-hook verdict was recorded (SERVER: ITERATE-DONE 0)" \
        test "$(guest_lines 'SERVER: ITERATE-DONE 0')" = 1
    check "auth-hook-reenter: the connection reaped during its hook never authenticated" \
        reenter_never_auth
    check "auth-hook-reenter: the recorded principal was released exactly once" \
        test "$(guest_count 'SERVER: ITERATE-PRINCIPAL-RELEASED')" = 1
    check "auth-hook-reenter: no completion reported an error" \
        test "$(guest_count 'SERVER: COMPLETE-ERR')" = 0
    check "auth-hook-reenter: the server still logs the next user in" \
        reenter_after_ok

    # --- the hook watchdog, NEW-1 -------------------------------------------
    #
    # Every audit line asserted here is exact (audit_line anchors the whole
    # line and refuses a DEBUG copy or a rate-limited "(and N more ...)"
    # suffix) and is the FIRST of its KIND in this boot -- which is itself
    # an assertion, since three of these kinds exist only because a shared
    # window would let the cheaper event hide the other's first line.
    check "auth-hook-reenter PRECONDITION: the queued client asked for its password once" \
        test "${reenter_q_asked:-0}" = 1
    check "auth-hook-reenter PRECONDITION: the queued client never authenticated -- its password was queued, not parsed" \
        reenter_q_never_auth
    # DEPTH 1, pinned: this is the one place a real, non-zero queue depth is
    # observed. A field nothing ever writes reads 0 for ever and looks
    # perfectly healthy, which is why the 0 in the slow-return line below is
    # not enough on its own.
    check "auth-hook-reenter: the hook STILL RUNNING over budget is reported mid-hook, with the queue behind it" \
        audit_line 'Stalled password hook: authentication held [0-9]+ ms so far, 1 connections waiting, for hookuser'
    check "auth-hook-reenter: the reaper says the queued connection was starved behind a hook" \
        audit_line 'Timeout before authentication for connection' \
            " after waiting [0-9]+ ms behind another connection's authentication hook"
    # The hook's OWN connection was never queued -- it was the one being
    # served -- so its line is the one AxlSsh has always written, with no
    # reason at all. audit_line's anchoring is what asserts the absence.
    check "auth-hook-reenter: the connection that was NOT queued keeps the unchanged timeout line" \
        audit_line 'Timeout before authentication for connection'

    # THE UNDETECTABLE CASE, asserted as undetectable -- with its control in
    # the same run, because "no line" on its own is satisfied by a fixture
    # that ran nothing, by a watchdog that reports nothing, and by a hook
    # that was simply fast. Three assertions close those in turn:
    #   - the blocking hook's slow-RETURN line says the server MEASURED a
    #     hold over the budget for it and reported that hold -- so the
    #     watchdog saw this hook go over and still wrote no mid-hook line;
    #   - the 20 s hook's mid-hook line above says a hook that DOES dispatch
    #     gets one in this very boot;
    #   - and only then does the count of 0 below mean what it says. It is
    #     counted rather than asserted absent by grep alone so the number
    #     appears in the output.
    # audit_text_count, not audit_line: these read a COUNT, including a
    # count of zero, which audit_line cannot express.
    block_slow=$(audit_text_count 'Slow password hook: authentication was held [0-9]+ ms, 0 connections waiting, for blockuser')
    block_stalled=$(audit_text_count 'Stalled password hook: authentication held [0-9]+ ms so far, [0-9]+ connections waiting, for blockuser')
    # BLOCK-DONE 0 rather than the login alone: every OTHER mode in the
    # fixture also ends in a successful login, so "it logged in" would be
    # satisfied by a credential that fell through to the default branch and
    # never blocked at all.
    check "auth-hook-reenter PRECONDITION: the blocking hook ran its no-dispatch branch (SERVER: BLOCK-DONE 0)" \
        test "$(guest_lines 'SERVER: BLOCK-DONE 0')" = 1
    check "auth-hook-reenter CONTROL: ...and the client whose hook blocked still logged in" \
        block_ok
    check "auth-hook-reenter: a hook that blocks without dispatching is still reported when it RETURNS over budget ($block_slow)" \
        test "$block_slow" = 1
    check "auth-hook-reenter: ...and produces no mid-hook line at all, which nothing in this process can ($block_stalled)" \
        test "$block_stalled" = 0

    # --- and the two things only the DEBUG level makes observable ----------
    #
    # THE CONTROL FIRST: a line only the raised level can produce. Without
    # it every count below could read 1 or 0 because the domain was never
    # raised, and a coverage assertion that cannot SEE is worse than none --
    # which is the whole reason these assertions exist at all.
    ssh_debug_on() {
        grep -qaE "ssh:[a-z_]+:[0-9]+: listening on port [0-9]+" \
            "$TEST_CLEAN_LOG"
    }
    check "auth-hook-reenter CONTROL: the ssh domain really is at DEBUG for this boot" \
        ssh_debug_on

    # ONE mid-hook line per hook call -- the header's promise in its own
    # words. The 20 s hook spans three or four 5 s sweeps and every one of
    # them passes the budget test, so an unlatched version writes three or
    # four lines: one at INFO and the rest at DEBUG. At the default level
    # those extras are ABSENT rather than demoted, which is exactly how the
    # independent review's sabotage of the latch went undetected.
    hook_stalled=$(audit_text_count 'Stalled password hook: authentication held [0-9]+ ms so far, [0-9]+ connections waiting, for hookuser')
    check "auth-hook-reenter: ONE mid-hook line for the whole 20 s hook, not one per sweep ($hook_stalled)" \
        test "$hook_stalled" = 1

    # The dispatching hook's own slow-RETURN line: second of its kind inside
    # the rate limiter's minute, so it exists only at DEBUG. This is the
    # other gap the raised level closes. The 0 is a real 0 -- the queued
    # client was reaped at least 6 s before this hook returned.
    hook_slow=$(audit_text_count 'Slow password hook: authentication was held [0-9]+ ms, 0 connections waiting, for hookuser')
    check "auth-hook-reenter: the hook that DISPATCHED is reported on return too ($hook_slow)" \
        test "$hook_slow" = 1

    # The budget is LATCHED at hook entry. The blocking hook raised it to
    # the maximum from inside itself before holding the server, so a version
    # that read the server's CURRENT field at report time would have found
    # an hour's budget and written no slow-return line -- which makes
    # block_slow above the assertion that pins the latch. These two say the
    # raise really happened and was undone, so that argument is not resting
    # on an unexecuted line.
    check "auth-hook-reenter PRECONDITION: the blocking hook raised the budget from inside itself (SERVER: BUDGET-SET 0)" \
        test "$(guest_lines 'SERVER: BUDGET-SET 0')" = 1
    check "auth-hook-reenter PRECONDITION: ...and it was put back once that hook returned (SERVER: BUDGET-RESTORED 0)" \
        test "$(guest_lines 'SERVER: BUDGET-RESTORED 0')" = 1
elif wants auth-hook-reenter; then
    echo "SKIP: auth-hook-reenter needs ssh(1) on the host"
fi

# 3) auth-hook-nest -- three password hooks that each dispatch the loop,
# delivered so they would nest (P3 security review MED-3).
#
# A hook that dispatches the loop runs every other source meanwhile, including
# another connection's receive -- and that receive used to parse a password
# and call ITS hook inside the first. Frames unwind in LIFO order, so the
# first hook could not return until every hook above it had: remote peers
# could push a legitimate login past its grace, and each level cost ~1.9 KB
# of stack. The server now runs one hook at a time; authentication input that
# arrives meanwhile waits, still encrypted, and is read in arrival order.
#
# THE ARRANGEMENT. B and then C connect and stop at their password prompts
# (askpass waits on a gate file). A connects last and answers at once; its
# hook enters and dispatches for NEST_US (6 s, ssh-server-selftest.c). Inside
# that window the host releases B's password, and 1.5 s later C's.
#   - Without the one-hook rule B's hook runs inside A's and C's inside B's,
#     and the fixture reports HOOK-DEPTH-MAX 3.
#   - With it, B and C wait and run after A, one at a time: depth 1, and the
#     hooks ENTER as A, B, C. That order is the FIFO assertion, and it
#     discriminates: C connected after B, and on_accept pushes a new
#     connection at the HEAD of the server's list, so a queue served in list
#     order would run C before B.
#
# PRECONDITION: both passwords were released while A's hook was still on the
# stack -- no NEST-DONE in the serial log yet. A release that landed after A
# returned would make depth 1 prove nothing.
if wants auth-hook-nest && [[ "$HAVE_SSH" == 1 ]]; then
    NEST_PIDS=()
    NEST_GATE_B="$TEST_TMPDIR/nest-gate-b"
    NEST_GATE_C="$TEST_TMPDIR/nest-gate-c"

    # $1 = client letter, $2 = its password, $3 = gate file ("" answers at
    # once). `timeout` bounds each client, as in auth-hook-reenter.
    start_nest_client() {
        local log="$TEST_TMPDIR/ssh-nest-$1.log"
        : > "$log.count"
        SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
        ASKPASS_PASSWORD="$2" ASKPASS_COUNT="$log.count" ASKPASS_GATE="$3" \
        timeout 90 ssh -vvv -p "$HOST_PORT" \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            -o ConnectTimeout=20 -o PubkeyAuthentication=no \
            -o KbdInteractiveAuthentication=no \
            -o PreferredAuthentications=password \
            "nest$1@127.0.0.1" true > "$log" 2>&1 < /dev/null &
        NEST_PIDS+=("$!")
        BG_PIDS+=("$!")
        DUMP_LOGS+=("$log")
    }
    nest_prompts() { wc -l < "$TEST_TMPDIR/ssh-nest-$1.log.count" | tr -d ' '; }
    # Up to 30 s for client $1 to reach its password prompt. A client that
    # never does fails the prompt PRECONDITION below, not this.
    nest_wait_prompt() {
        for _ in $(seq 1 300); do
            if [[ "$(nest_prompts "$1")" -ge 1 ]]; then return 0; fi
            sleep 0.1
        done
        return 1
    }

    start_nest_client b "$PW_NEST_B" "$NEST_GATE_B"
    nest_wait_prompt b || true
    start_nest_client c "$PW_NEST_C" "$NEST_GATE_C"
    nest_wait_prompt c || true
    start_nest_client a "$PW_NEST_A" ""
    test_wait_for "SERVER: NEST-ENTER" 30 || true
    nest_done_at_b=$(raw_count 'SERVER: NEST-DONE')
    : > "$NEST_GATE_B"
    sleep 1.5
    nest_done_at_c=$(raw_count 'SERVER: NEST-DONE')
    : > "$NEST_GATE_C"

    # Three hooks report, DONE or FAIL: 3 x NEST_US in series plus margin.
    for _ in $(seq 1 60); do
        if [[ "$(raw_count 'SERVER: NEST-\(DONE\|FAIL\)')" -ge 3 ]]; then
            break
        fi
        sleep 1
    done
    for _p in "${NEST_PIDS[@]}"; do wait "$_p" 2>/dev/null || true; done

    test_clean_log
    nest_order=$(sed -n 's/^SERVER: NEST-ENTER //p' "$TEST_CLEAN_LOG" | paste -sd' ' -)
    echo "  auth-hook-nest: hooks entered as: ${nest_order:-<none>}"

    nest_all_prompted_once() {
        [[ "$(nest_prompts a)" == 1 && "$(nest_prompts b)" == 1 \
           && "$(nest_prompts c)" == 1 ]]
    }
    nest_released_inside_a() {
        [[ "$nest_done_at_b" == 0 && "$nest_done_at_c" == 0 ]]
    }
    nest_entered_in_order() { [[ "$nest_order" == "nesta nestb nestc" ]]; }
    nest_logged_in() { grep -q "$SUCCESS_PW_RE" "$TEST_TMPDIR/ssh-nest-$1.log"; }

    check "auth-hook-nest PRECONDITION: each client asked for its password once" \
        nest_all_prompted_once
    check "auth-hook-nest PRECONDITION: B's and C's passwords were released while A's hook ran (NEST-DONE lines then: $nest_done_at_b, $nest_done_at_c)" \
        nest_released_inside_a
    check "auth-hook-nest: the hooks entered in arrival order (nesta nestb nestc)" \
        nest_entered_in_order
    check "auth-hook-nest: all three hooks dispatched and answered (SERVER: NEST-DONE 0, x3)" \
        test "$(guest_lines 'SERVER: NEST-DONE 0')" = 3
    check "auth-hook-nest: A logged in" nest_logged_in a
    check "auth-hook-nest: B logged in after waiting, not dropped by the grace" \
        nest_logged_in b
    check "auth-hook-nest: C logged in after waiting, not dropped by the grace" \
        nest_logged_in c
elif wants auth-hook-nest; then
    echo "SKIP: auth-hook-nest needs ssh(1) on the host"
fi

# And a real client still completes the whole handshake AND authenticates
# after all of it — proving the server is not merely accepting sockets but
# still functioning. This was a SERVICE_ACCEPT grep in P1, when no key was
# authorized; now that one is, requiring a full login is strictly stronger and
# no more expensive.
if [[ "$ABUSE" == 1 && -z "$ONLY" ]]; then
    if [[ "$HAVE_SSH" == 1 ]]; then
        check "ssh(1) still authenticates after all of the above" \
            good_login_works after-all
    else
        echo "SKIP: no ssh(1) on the host for the post-abuse handshake"
    fi
fi

# 4) auth-hook-free -- the server freed from inside a hook's dispatch (P3
# security review MED-1). LAST, after the post-abuse login above: it ends the
# server, and nothing after it can log in.
#
# A handler that turns SSH off can run while a password check waits in the
# loop. axl_ssh_server_free then spun for ever: conn_drop defers the
# connection whose hook is on the stack, and the free's loop re-read that same
# head. Freeing at once instead would leave every frame under the hook on
# freed memory. The server now stops at once, and finishes the free from the
# loop once no application code is on the stack.
#
# The fixture's "free" hook arms a 1 s timer and dispatches for 3 s; the timer
# calls axl_ssh_server_free() -- which STOPS the server (removed from the
# live-server table) but cannot yet actually free it, since this hook's own
# call is still on the stack; after the dispatch the hook completes against
# that stopped-but-intact server. What proves what:
#   - SERVER: FREED-IN-HOOK: the free RETURNED inside the dispatch -- no spin;
#   - SERVER: BANNER-AFTER-FREE -1: axl_ssh_server_set_banner() on that same
#     address, made between the free and clearing the global, is refused with
#     AXL_ERR from the live-server table alone -- WITHOUT the server being
#     read. D9's setter is the first one that FREES heap a server path reads,
#     so "refused once the free has started" is load-bearing for it in a way
#     it is not for the others: a version that stored through the pointer
#     would also leak the copy it made, which the teardown leak verdict below
#     would then show;
#   - SERVER: FREE-DONE -5: the hook's completion after it was refused
#     (AXL_NOT_FOUND) against the now-stopped server, not applied to one being
#     freed. This is NOT a dangling-pointer read (P3 security re-review
#     NEW-6) -- the memory is still valid here, only removed from the live
#     table; the genuinely-freed case is covered by the unit test
#     test_ssh_auth_complete_after_free (test/unit/axl-test-ssh.c), which
#     runs against a poisoned, actually-freed block;
#   - the heartbeat and SERVER: DONE below: the loop kept running;
#   - MEM-CORRUPTION 0 below: no frame under the hook wrote freed memory;
#   - the leak verdict below: the rest of the free DID run. Only that deferred
#     step frees the server itself, so a free that never finished leaks it.
#
# The CONTROL login first proves SUCCESS_PW_RE matches in THIS run, so the
# never-authenticated check means something even when this scenario runs alone.
if wants auth-hook-free && [[ "$HAVE_SSH" == 1 ]]; then
    FREE_CONTROL_LOG="$TEST_TMPDIR/ssh-before-free.log"
    FREE_LOG="$TEST_TMPDIR/ssh-hook-free.log"

    : > "$FREE_CONTROL_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_OK" ASKPASS_COUNT="$FREE_CONTROL_LOG.count" \
    timeout 60 ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        pwuser@127.0.0.1 true > "$FREE_CONTROL_LOG" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$FREE_CONTROL_LOG")

    # `timeout`: against a server that spins inside the free, nothing ever
    # closes this client's socket.
    : > "$FREE_LOG.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$PW_FREE" ASKPASS_COUNT="$FREE_LOG.count" \
    timeout 60 ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        freeuser@127.0.0.1 true > "$FREE_LOG" 2>&1 < /dev/null || true
    DUMP_LOGS+=("$FREE_LOG")
    # The hook's own report ends the window (FREE-DONE or FREE-FAIL, not
    # FREED-IN-HOOK). `|| true`: a guest that spins never prints it, and the
    # exact counts below say so.
    test_wait_for "SERVER: FREE-" 30 || true
    free_asked=$(wc -l < "$FREE_LOG.count" | tr -d ' ')

    free_control_ok()  { grep -q "$SUCCESS_PW_RE" "$FREE_CONTROL_LOG"; }
    free_never_auth()  { ! grep -q "$SUCCESS_PW_RE" "$FREE_LOG"; }

    test_clean_log
    check "auth-hook-free CONTROL: a password login succeeds before the free" \
        free_control_ok
    check "auth-hook-free PRECONDITION: the password was asked for once" \
        test "${free_asked:-0}" = 1
    check "auth-hook-free: the free returned inside the hook's dispatch (SERVER: FREED-IN-HOOK)" \
        test "$(guest_lines 'SERVER: FREED-IN-HOOK')" = 1
    check "auth-hook-free: the hook's completion after the free was refused (SERVER: FREE-DONE -5)" \
        test "$(guest_lines 'SERVER: FREE-DONE -5')" = 1
    # -1 is AXL_ERR (include/axl/axl-macros.h), the "not live" answer. Pinned
    # exactly rather than "not 0": AXL_INVALID would also be non-zero and
    # would mean something else entirely.
    check "auth-hook-free: set_banner on the freed server is refused (SERVER: BANNER-AFTER-FREE -1)" \
        test "$(guest_lines 'SERVER: BANNER-AFTER-FREE -1')" = 1
    check "auth-hook-free: the connection whose hook freed the server never authenticated" \
        free_never_auth
elif wants auth-hook-free; then
    echo "SKIP: auth-hook-free needs ssh(1) on the host"
fi

hostile_finish
