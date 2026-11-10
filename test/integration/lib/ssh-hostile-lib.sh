# shellcheck shell=bash
# ssh-hostile-lib.sh -- the setup, helpers and teardown shared by the AxlSsh
# hostile-peer tests. Sourced by them; never run on its own.
#
#   lib/ssh-hostile-cases.sh        abuse from a peer that never gets in:
#                                   malformed input, a connection flood, the
#                                   auth phase driven hostilely (auth-cap,
#                                   auth-forge, auth-stall), and per-source
#                                   penalties on a second server (penalty).
#                                   Run as test-ssh-hostile-qemu.sh and
#                                   test-ssh-hostile-stall-qemu.sh (auth-stall)
#   lib/ssh-hostile-hooks-cases.sh  the P3 password-hook hazards: a verdict
#                                   that outlives its client, a hook that
#                                   dispatches the loop, hooks that would
#                                   nest, and the server freed from inside one.
#                                   Run as test-ssh-hostile-hooks-qemu.sh and
#                                   test-ssh-hostile-hooks-reenter-qemu.sh
#                                   (auth-hook-reenter)
#
# (Each half below was later split once more, its longest scenario into a
# boot of its own; the case libraries' headers say so. The idle suites,
# lib/ssh-idle-cases.sh, use this file the same way.)
#
# WHY TWO FILES. It was one boot running all twelve scenarios, and it outgrew
# CI's per-test limit: 245 s on x64 and 249 s on aa64 standalone, against the
# 240 s CI runs each test under, so CI run 35348243557 killed it. The
# time was not all work. The fixture's deadline has to outlast every scenario
# in its boot, and the test then waited for that deadline because SERVER: DONE
# is an assertion -- 164 s of scenarios, then ~66 s of idle margin (since
# cut: see THE EARLY FINISH above hostile_finish). And the runner's
# pool parallelises ACROSS tests, never within one. Two boots run side by side
# in the pool, each well under the limit.
#
# THE SEAM IS BY MEASURED TIME. Standalone, 2026-09-18, seconds of scenario on
# the slower arch (x64 and aa64 agree to within 1 s -- these scenarios are
# dominated by wall-clock waits, not by the guest's speed):
#
#   bad-ident, bad-length, bad-padding  2 (all three)
#   flood 2, auth-cap 1, auth-forge 1, penalty 37, auth-stall 44       =  87
#   auth-pending-drop 4, auth-hook-reenter 52, auth-hook-nest 19,
#   the post-abuse login 1, auth-hook-free 4                           =  80
#
# The pre-P3 abuse against P3's hooks would have been 50 s against 117 s;
# penalty goes with the abuse, where it belongs anyway -- a refusal happens at
# accept, before any hook can run -- and the halves come out 87 against 80.
#
# NOT A TEST. lib/discover.sh schedules test/integration/test-*.sh and nothing
# under lib/, so this is never run as one and carries no test-meta header.
# lib/test-cache.sh keys every test on every lib/*.sh, so an edit here
# invalidates both halves' cache entries, as it must.
#
# HOW A HALF USES IT:
#
#   HOSTILE_SUMMARY="ssh hostile"      the verdict line's label
#   HOSTILE_SCENARIOS="a b c"          its scenario names, in run order
#   HOSTILE_SCENARIOS_ALL="..."        optional: a case library's whole list,
#                                      which HOSTILE_SCENARIOS must follow
#   declare -A HOSTILE_BUDGET=([a]=2 ...)  seconds each may take (below)
#   source "$(dirname "$0")/lib/ssh-hostile-lib.sh"   (with "$@" intact)
#   GRACE_SEC=...                      its own login grace
#   PEN_SPEC=... HOSTILE_EXEMPT=-      optional: its own penalty spec, and
#                                      "-" to exempt nothing on the first
#                                      server (default: exempt HOST_AS_SEEN)
#   hostile_boot "<banner>"            deadline, build, stage, boot, listen
#   ... its scenarios ...
#   hostile_finish                     liveness + teardown checks; exits
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) -- integration tests.

export TEST_SKIP_RATCHET=1

# One host port past whatever the test claims for itself (TEST_PORT_SLOTS,
# set before this file is sourced; common-test.sh's default is 4) is the
# fixture's CONTROL port: hostile_finish's early-finish signal. Claimed here,
# before common-test.sh takes the run of ports, so no test has to know.
HOSTILE_CTL_SLOT=${TEST_PORT_SLOTS:-4}
TEST_PORT_SLOTS=$(( HOSTILE_CTL_SLOT + 1 ))

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common-test.sh"
source "$(dirname "${BASH_SOURCE[0]}")/ssh-guest-lib.sh"

test_parse_args "$@"

# The probe (ssh-hostile.py) speaks raw sockets and needs neither ssh(1) nor
# ssh-keygen(1) -- but listen() refuses an empty key set (P2), and ssh-keygen
# is how the key below is staged, so both halves need it. Exit 77, the
# runner's SKIP: this was `exit 0`, which scored both halves PASS with nothing
# checked. In the abuse half ssh(1) itself is optional: the scenarios that
# need it announce a SKIP without it, rather than taking the probes down with
# them. The hooks half has nothing to run without it and skips whole, before
# sourcing this file.
if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh-keygen on the host — needed to stage an authorized key"
    exit 77
fi

test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=2222
CTL_HOST_PORT=$(test_port "$HOSTILE_CTL_SLOT")
CTL_GUEST_PORT=2299

# --- scenario selection, read BEFORE the fixture's deadline is chosen -------
#
# SSH_HOSTILE_ABUSE=0 runs the CONTROL: boot, touch nothing, and assert only
# that the loop reaches its own deadline. It never sends the early-finish
# signal (hostile_finish), so it also proves, on demand, that the deadline
# ceiling still fires on its own: SERVER: DEADLINE, and no SERVER: FINISH.
# (test-ssh-policy-qemu.sh, on the same fixture, proves it on every suite
# run.) Without it, "the loop is wedged" and
# "the loop never exits under any circumstances" look identical -- which cost
# one wrong diagnosis and one fix applied to the wrong defect.
ABUSE=${SSH_HOSTILE_ABUSE:-1}
# SSH_HOSTILE_ONLY=<scenario> narrows to one input, which is how you find
# WHICH one wedged the loop rather than that one of several did.
ONLY=${SSH_HOSTILE_ONLY:-}

# Naming every scenario in HOSTILE_SCENARIOS, rather than inline at each use,
# is what lets an unknown SSH_HOSTILE_ONLY be REFUSED. Without this check a
# typo selects nothing, scores 0 passed / 0 failed, and the file exits GREEN
# -- a run that tested nothing and said so nowhere. A scenario that belongs to
# the OTHER half is refused the same way: this file cannot run it.
#
# Every scenario must also have a budget. A missing one would count as zero
# and quietly shorten the deadline under the scenario it belongs to.
for _s in $HOSTILE_SCENARIOS; do
    if [[ ! "${HOSTILE_BUDGET[$_s]:-}" =~ ^[0-9]+$ ]]; then
        echo "FAIL: scenario $_s has no HOSTILE_BUDGET entry in $(basename "$0")"
        exit 1
    fi
done
# A suite split into parts over one case library (lib/ssh-idle-cases.sh)
# names the whole list as HOSTILE_SCENARIOS_ALL, and each part's
# HOSTILE_SCENARIOS must be a subsequence of it IN ITS ORDER: a part that
# reordered them, or named one the library does not have, would move which
# scenario writes a server's first rate-limited INFO line, and the checks
# that pin that line would then grade a different boot from the one they
# were written for.
if [[ -n "${HOSTILE_SCENARIOS_ALL:-}" ]]; then
    _prev=-1
    for _s in $HOSTILE_SCENARIOS; do
        _i=0; _at=-1
        for _a in $HOSTILE_SCENARIOS_ALL; do
            if [[ "$_a" == "$_s" ]]; then _at=$_i; fi
            _i=$((_i + 1))
        done
        if [[ "$_at" -le "$_prev" ]]; then
            echo "FAIL: $(basename "$0"): scenario $_s is not in HOSTILE_SCENARIOS_ALL"
            echo "      after the one before it -- the parts must keep the library's order:"
            echo "      $HOSTILE_SCENARIOS_ALL"
            exit 1
        fi
        _prev=$_at
    done
    # ...and the parts together must run ALL of them. A scenario no part
    # names is never run, and nothing else would say so: every part would
    # stay green. The parts are the tests beside this one that source the
    # same case library, read from their HOSTILE_SCENARIOS= lines.
    _lib=$(basename "${BASH_SOURCE[1]}")
    _union=" "
    for _w in "$(dirname "$0")"/test-*.sh; do
        if grep -qE "^source .*lib/${_lib//./\\.}\"\$" "$_w"; then
            _union+="$(sed -nE 's/^HOSTILE_SCENARIOS="([^"]*)"$/\1/p' "$_w") "
        fi
    done
    for _a in $HOSTILE_SCENARIOS_ALL; do
        if [[ "$_union" != *" $_a "* ]]; then
            echo "FAIL: scenario $_a of lib/$_lib is in no part's HOSTILE_SCENARIOS"
            echo "      (the parts: the test-*.sh that source lib/$_lib)"
            exit 1
        fi
    done
fi
if [[ -n "$ONLY" ]]; then
    _known=0
    for _s in $HOSTILE_SCENARIOS; do
        if [[ "$ONLY" == "$_s" ]]; then _known=1; fi
    done
    if [[ "$_known" -eq 0 ]]; then
        echo "FAIL: SSH_HOSTILE_ONLY=$ONLY is not a scenario of $(basename "$0")"
        echo "      its scenarios: $HOSTILE_SCENARIOS"
        if [[ -n "${HOSTILE_SCENARIOS_ALL:-}" ]]; then
            echo "      (its case library's whole list, across its parts:" \
                 "$HOSTILE_SCENARIOS_ALL)"
        else
            echo "      (the hostile halves: test-ssh-hostile-qemu.sh," \
                 "test-ssh-hostile-hooks-qemu.sh)"
        fi
        exit 1
    fi
fi

# Is scenario $1 selected? Only one of THIS file's: a name from the other half
# is never selected here, so a check guarded by it cannot run -- and pass --
# in a boot where its scenario never happened. Written as explicit ifs rather
# than `&&` chains because this runs under `set -e` from outside a condition
# context.
wants() {
    local s known=0
    if [[ "$ABUSE" != 1 ]]; then return 1; fi
    for s in $HOSTILE_SCENARIOS; do
        if [[ "$s" == "$1" ]]; then known=1; fi
    done
    if [[ "$known" -eq 0 ]]; then return 1; fi
    if [[ -n "$ONLY" && "$ONLY" != "$1" ]]; then return 1; fi
    return 0
}

# Task P: per-source penalties, as AUTHFAIL:NOAUTH:GRACE:MIN:MAX seconds
# (ssh-server-selftest.c's argv[6]), handed to every server through
# axl_ssh_server_set_penalties. Seconds rather than OpenSSH's minutes-scale
# defaults so the penalty scenario costs seconds; the SHIPPED defaults are
# exercised by test-ssh-auth-qemu.sh, which passes none of this. Both halves'
# first server gets the same spec and exempts HOST_AS_SEEN, so this is the
# configuration every scenario but the penalty's own ran against before the
# split, unchanged.
PEN_AUTHFAIL=12
PEN_MIN=14
PEN_SPEC="${PEN_AUTHFAIL}:1:10:${PEN_MIN}:40"

# EVERY connection in these tests reaches the guest from ONE address: QEMU's
# user-mode network forwards the host's port from its own guest-visible host
# address. That is why the penalty scenario gets a SECOND server, and why the
# first one -- which every other scenario uses -- exempts this address
# (ssh-server-selftest.c's argv[7]): with penalties on, the flood alone (70
# connections, each accruing noauth) would otherwise have every later scenario
# refused. The hook half keeps the same exemption so that its scenarios run
# against exactly the server they ran against before the split. The value is
# MEASURED, not assumed: the penalty scenario asserts that the guest's
# activation lines name exactly this address.
HOST_AS_SEEN=10.0.2.2

# --- the fixture's deadline ---------------------------------------------------
#
# The fixture quits its loop on a timer and prints SERVER: DONE, and that is
# an assertion, not a teardown convenience: a loop that stopped dispatching
# never fires its timer. Since THE EARLY FINISH (above hostile_finish) that
# timer is normally the finish timer the host's signal arms, and the deadline
# is the CEILING: it fires only when the host never signals (the
# SSH_HOSTILE_ABUSE=0 control, a host that died) or a scenario overran. So it
# must still outlast every selected scenario plus the 6 s heartbeat window,
# but its margin is no longer paid on a passing run.
#
# It is summed from the scenarios actually selected, so a full run, an
# SSH_HOSTILE_ONLY run and the ABUSE=0 control each get what they need and no
# more:
#
#   HOSTILE_BASE_SEC (30)    the 6 s heartbeat window, the <= 1 s it takes to
#                            see "ssh: listening", and 23 s of margin -- the
#                            margin the probe-only bucket (30 s) always had
#   + each HOSTILE_BUDGET    the scenario's longest PASSING run, not its
#                            typical one: its measured time on the slower arch
#                            plus whatever its own bands still let it take
#                            (each half's budget table says how)
#
# A scenario that overruns its budget borrows from the ones after it; only the
# sum has to hold. Before the split this was a table of hand-sized buckets, one
# per selection, that grew by special case as scenarios were added; a sum
# cannot fall out of step with the list it is summed over.
HOSTILE_BASE_SEC=30
SERVER_DEADLINE_MS=0

hostile_deadline() {
    local s sum=0
    for s in $HOSTILE_SCENARIOS; do
        if wants "$s"; then sum=$(( sum + HOSTILE_BUDGET[$s] )); fi
    done
    SERVER_DEADLINE_MS=$(( (HOSTILE_BASE_SEC + sum) * 1000 ))
}

# The auth scenarios run a fake agent and deliberately-stalled ssh(1) clients
# in the background. Both are children of this script, so an early exit -- a
# failed assertion, a Ctrl-C, the runner's SIGTERM -- would orphan them: the
# agent would sit on its lifetime and ssh would block on it. The runner's
# SIGTERM does NOT reach them all by itself: `timeout N ssh ...` puts ssh in a
# process group of its own, outside the one the runner signals. So reap them
# here, ahead of test_cleanup; on a kill, common-test.sh's handler runs this
# EXIT trap before the script dies.
BG_PIDS=()
_hostile_cleanup() {
    if [[ "${#BG_PIDS[@]}" -gt 0 ]]; then
        for _p in "${BG_PIDS[@]}"; do kill "$_p" 2>/dev/null || true; done
    fi
    # SSH_HOSTILE_KEEP=<dir> keeps every ssh(1) transcript. Repo rule: an
    # assertion on what ssh prints must be pinned from a REAL transcript
    # captured in a run, and this is how one is captured.
    if [[ -n "${SSH_HOSTILE_KEEP:-}" && -n "${TEST_TMPDIR:-}" ]]; then
        mkdir -p "$SSH_HOSTILE_KEEP"
        cp -f "$TEST_TMPDIR"/*.log "$SSH_HOSTILE_KEEP"/ 2>/dev/null || true
    fi
    test_cleanup
}

# hostile_boot <banner>
#
# Build and stage the fixture, boot it, and wait for it to listen. A half that
# starts the second (penalty) server sets HOSTILE_EXTRA_ARGS (the fixture's
# argv[8] onward) and HOSTILE_EXTRA_FWD (more host/guest port pairs) first.
hostile_boot() {
    local banner="$1"
    local -A native_arch_map=([X64]=x64 [AARCH64]=aa64)
    local native_arch="${native_arch_map[$TEST_ARCH]:-x64}"

    hostile_deadline

    TEST_BUILD_DIR="$(test_build_dir)"
    "$PROJECT_DIR/scripts/axl-build.sh" \
        ARCH="$native_arch" all ssh-server-selftest 2>&1 | tail -3
    test_add_efi "$TEST_BUILD_DIR/ssh-server-selftest.efi"

    # listen() refuses an empty key set as of P2, so stage one. The PRIVATE
    # half is KEPT: the scenarios end by requiring a good login to still work,
    # which is the only observation that separates "the server refused the
    # attack" from "the server died refusing it". Generated per run into
    # TEST_TMPDIR; only the public half ever reaches the guest and nothing
    # here is committed.
    #
    # The .pub is chmod 600 because it is also passed as `ssh -i` for the
    # agent scenarios: ssh(1) tries an IdentityFile as a PRIVATE key first and
    # refuses a world-readable one, and while the agent path does not reach
    # that check on 9.9p1, a future version taking the other branch would fail
    # for a reason that has nothing to do with this server.
    ssh-keygen -q -t ed25519 -N '' -C 'axl-hostile-good' \
        -f "$TEST_TMPDIR/good" </dev/null
    chmod 600 "$TEST_TMPDIR/good.pub"
    {
        echo "# generated per run by $(basename "$0")"
        cat "$TEST_TMPDIR/good.pub"
    } > "$TEST_STAGING/authorized_keys"

    # The fixture's password hooks install only when it is given a 4th
    # argument, and that argument is not scenario-scoped, so EVERY scenario in
    # both halves gets it. Concretely: the server advertises
    # "publickey,password" instead of "publickey", and every publickey login
    # (good_login_works) is answered through on_publickey instead of the
    # pre-P3 default -- so it acquires a principal and prints SERVER:
    # PRINCIPAL-RELEASED when its connection closes. The teardown check in
    # hostile_finish counts every one of them.
    #
    # Same staging as test-ssh-auth-qemu.sh: hex from /dev/urandom, no ':' (the
    # fixture's field separator) and no shell metacharacters. Every user
    # either half needs, staged in both so there is one list: "ok" for the
    # logins that must still succeed, "slow" for the verdict auth-pending-drop
    # times out from under, "iterate" for the hook auth-hook-reenter makes
    # dispatch the loop, "nest" for auth-hook-nest's three, "free" for the
    # hook auth-hook-free frees the server under, and "block" for the hook
    # that holds the server WITHOUT dispatching it -- the case the hook
    # watchdog cannot report mid-hook, which auth-hook-reenter asserts is
    # undetectable. The four "open*" users are the channel suite's: the
    # fixture's open hook writes, closes, exits, or closes and refuses from
    # inside itself, keyed on the user name. Twelve of the fixture's
    # CREDS_MAX (16); one past it fails the boot with SERVER: FAIL
    # creds-overflow rather than dropping a user in silence.
    rand_hex() { od -An -tx1 -N8 /dev/urandom | tr -d ' \n'; }
    PW_OK=$(rand_hex)
    PW_SLOW=$(rand_hex)
    PW_ITERATE=$(rand_hex)
    PW_NEST_A=$(rand_hex)
    PW_NEST_B=$(rand_hex)
    PW_NEST_C=$(rand_hex)
    PW_FREE=$(rand_hex)
    PW_BLOCK=$(rand_hex)
    PW_OPENWRITE=$(rand_hex)
    PW_OPENCLOSE=$(rand_hex)
    PW_OPENCLOSEREFUSE=$(rand_hex)
    PW_OPENEXIT=$(rand_hex)
    # Matches no credential: the penalty scenario's wrong password.
    PW_WRONG=$(rand_hex)
    {
        echo "pwuser:${PW_OK}:ok"
        echo "slowuser:${PW_SLOW}:slow"
        echo "hookuser:${PW_ITERATE}:iterate"
        echo "nesta:${PW_NEST_A}:nest"
        echo "nestb:${PW_NEST_B}:nest"
        echo "nestc:${PW_NEST_C}:nest"
        echo "freeuser:${PW_FREE}:free"
        echo "blockuser:${PW_BLOCK}:block"
        echo "openwrite:${PW_OPENWRITE}:ok"
        echo "openclose:${PW_OPENCLOSE}:ok"
        echo "opencloserefuse:${PW_OPENCLOSEREFUSE}:ok"
        echo "openexit:${PW_OPENEXIT}:ok"
        # A half's own users, one "name:password:mode" per line, set before
        # hostile_boot (test-ssh-disconnect-qemu.sh's CHANGE_FIRST user).
        if [[ -n "${HOSTILE_EXTRA_CREDS:-}" ]]; then
            printf '%s\n' "$HOSTILE_EXTRA_CREDS"
        fi
    } > "$TEST_STAGING/credentials"

    # ssh(1) reads the password from SSH_ASKPASS when SSH_ASKPASS_REQUIRE=force,
    # regardless of DISPLAY. Each invocation appends one line to a counter
    # file: the count of password PROMPTS is the precondition evidence that
    # the password was really sent. ASKPASS_SLEEP, when set for one
    # invocation, delays the ANSWER (the prompt is counted first):
    # auth-hook-reenter uses it to deliver a password late in the login grace.
    # ASKPASS_GATE, when set, holds the answer until that file exists:
    # auth-hook-nest parks clients at their prompts and releases each password
    # at a moment of its choosing. Bounded at 60 s, so a run that aborts
    # before releasing a gate leaves no askpass looping behind the ssh that
    # spawned it.
    cat > "$TEST_TMPDIR/askpass.sh" <<'ASKPASS'
#!/bin/sh
echo x >> "$ASKPASS_COUNT"
if [ -n "${ASKPASS_GATE:-}" ]; then
    n=0
    while [ ! -e "$ASKPASS_GATE" ] && [ "$n" -lt 600 ]; do
        sleep 0.1
        n=$((n + 1))
    done
fi
if [ -n "${ASKPASS_SLEEP:-}" ]; then sleep "$ASKPASS_SLEEP"; fi
printf '%s\n' "$ASKPASS_PASSWORD"
ASKPASS
    chmod +x "$TEST_TMPDIR/askpass.sh"

    # GENERATED, so test_use_nsh_text rather than a checked-in .nsh: the
    # fixture's arguments differ per test. The staged files are named
    # relatively -- booted the startup.nsh enters fs0:\, pooled the agent runs
    # it from the lease's share directory, and fs0: there is not this run's.
    test_use_nsh_text hostile << NSHEOF
@echo -off
echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo Starting SSH server...
ssh-server-selftest.efi ${GUEST_PORT} ${SERVER_DEADLINE_MS} authorized_keys credentials ${GRACE_SEC} ${PEN_SPEC} ${HOSTILE_EXEMPT:-$HOST_AS_SEEN}${HOSTILE_EXTRA_ARGS:+ $HOSTILE_EXTRA_ARGS} ctl=${CTL_GUEST_PORT}
NSHEOF

    test_build_image

    echo "=== $banner ($TEST_ARCH) ==="

    test_build_qemu_cmd
    test_add_port_forward "$HOST_PORT" "$GUEST_PORT" "$CTL_HOST_PORT" "$CTL_GUEST_PORT" \
        ${HOSTILE_EXTRA_FWD[@]+"${HOSTILE_EXTRA_FWD[@]}"}
    test_run_background

    # EXIT alone: common-test.sh's INT/TERM handlers report a kill, run this
    # EXIT trap themselves, and end the script. Trapping them here as well
    # would make a killed test resume after its cleanup and report whatever
    # its interrupted wait concluded.
    trap '_hostile_cleanup' EXIT

    echo "  QEMU PID: $(test_guest_qemu_pid || echo none), host :$HOST_PORT -> guest :$GUEST_PORT (control :$CTL_HOST_PORT -> :$CTL_GUEST_PORT)"
    echo "  fixture deadline: $(( SERVER_DEADLINE_MS / 1000 ))s"

    if ! test_wait_boot "ssh: listening" 90; then
        echo "FAIL: SSH server did not start listening within ${TEST_WAITED_S} seconds"
        test_clean_log
        echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
        exit 1
    fi
    # The fixture arms its deadline just before it prints that line, so this
    # is the host's best reading of when the deadline clock started; the
    # final wait is measured from it.
    HOSTILE_LISTEN_T=$SECONDS
    echo "  Server is listening"
}

PROBE="$(dirname "$0")/ssh-hostile.py"
AGENT="$(dirname "$0")/ssh-fake-agent.py"
pass=0; fail=0

HAVE_SSH=0
if command -v ssh >/dev/null 2>&1; then HAVE_SSH=1; fi

check() {
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then
        echo "PASS: $name"; pass=$((pass+1))
    else
        echo "FAIL: $name"; fail=$((fail+1))
    fi
}

# Wait up to $3 seconds for host file $1 to hold the exact line $2. In the
# foreground and bounded: nothing is left polling after the test.
wait_line() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        if grep -qxF -- "$2" "$1" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

# The whole of host file $1 is exactly the lines $2... -- nothing more.
file_is() {
    local f="$1"; shift
    [[ "$(cat "$f" 2>/dev/null)" == "$(printf '%s\n' "$@")" ]]
}

# A peer's exit code $1 is $2, and its transcript $3 is exactly the lines
# $4... -- a right line with the wrong exit, or the reverse, is a failure.
peer_is() {
    local rc="$1" want="$2" f="$3"
    shift 3
    [[ "$rc" == "$want" ]] && file_is "$f" "$@"
}

# `|| true` is load-bearing: common-test.sh installs `set -euo pipefail`, so a
# grep that matches nothing kills the whole script from inside a command
# substitution. That exact trap already cost this phase one silently-aborting
# test (see test-ssh-transport-qemu.sh's check()); this is the second time.
tick_now() {
    grep -oE 'SERVER: TICK [0-9]+' "$TEST_LOG" 2>/dev/null | tail -1 \
        | grep -oE '[0-9]+$' || true
}

# A file of $1 bytes at path $2 holding the fixture's "stream" pattern:
# byte k is k % 251. Shared by the rekey and idle suites.
stream_file() {
    local n="$1" out="$2" i
    for ((i = 0; i < 251; i++)); do
        printf "\\$(printf %03o "$i")"
    done > "$out.p"
    cp "$out.p" "$out.x"
    while [[ "$(stat -c %s "$out.x")" -lt "$n" ]]; do
        cat "$out.x" "$out.x" > "$out.y"
        mv "$out.y" "$out.x"
    done
    head -c "$n" "$out.x" > "$out"
    rm -f "$out.p" "$out.x"
}

# Transcripts to print if anything fails. The auth scenarios are graded
# ENTIRELY from ssh(1) output, so on a red run these ARE the diagnosis --
# without them a reader has a verdict and no way to separate a server
# regression from an OpenSSH change.
DUMP_LOGS=()

# OpenSSH 9.9p1 (this repo's tested version) does NOT print "Authentication
# succeeded (publickey)" -- checked directly against real transcripts, in
# neither a successful nor a refused run. What it prints on success is the
# summary line `Authenticated to <host> ([<host>]:<port>) using "publickey".`
# Spelled once here for both hostile halves; test-ssh-auth-qemu.sh carries the
# same constant with the same warning, and an OpenSSH bump has to move BOTH.
SUCCESS_RE='Authenticated to .* using "publickey"'

# SUCCESS_PW_RE: SECOND COPY. The first is in test-ssh-auth-qemu.sh. An
# OpenSSH upgrade that changes this string must change BOTH, or the negative
# checks built on the stale copy silently become tautologies.
SUCCESS_PW_RE='Authenticated to .* using "password"'

# ONE spelling of "the server still works", used by every scenario.
# IdentitiesOnly=yes and IdentityAgent=none keep the DEVELOPER's ~/.ssh keys
# and running agent out of it -- without them this would be asserting
# something about the host's key store rather than about the server.
good_login_works() {
    local log="$TEST_TMPDIR/ssh-live-$1.log"
    ssh -vvv -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o IdentityAgent=none -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/good" root@127.0.0.1 true > "$log" 2>&1 || true
    # Registered even on success: `check` runs this in the CURRENT shell (a
    # redirection is not a subshell), so the append survives, and a liveness
    # login that failed is precisely the transcript a reader needs.
    DUMP_LOGS+=("$log")
    grep -q "$SUCCESS_RE" "$log"
}

# How many ESTABLISHED connections this host holds TO the forwarded port.
#
# /proc/net/tcp rather than ss(1): ss lives in /usr/sbin, which is not on
# every box's PATH for a non-root user, and reading /proc needs no privilege
# and no package. Field 3 is the REMOTE address, so filtering on it counts the
# CLIENT side of the forward only -- QEMU's own listening and accepted sockets
# carry the port on the LOCAL side and are not double counted. State 01 is
# ESTABLISHED, and awk always exits 0, so this is safe under `set -e`.
est_to_host_port() {
    local hex
    hex=$(printf ':%04X' "$HOST_PORT")
    awk -v p="$hex" '$3 ~ (p "$") && $4 == "01" { n++ } END { print n+0 }' \
        /proc/net/tcp
}

# THE SERVER ENDED IT, rather than answering and letting the peer try again.
#
# Two forms, and the disjunction is deliberate WHERE IT IS STILL USED (the
# forgery scenarios): it says only that the server ended the connection, not
# how. Before P5 a DISCONNECT raced the reset behind it, so either ending
# could be printed; since P5 a DISCONNECT on a healthy stream is delivered
# (axl-ssh-server.h, "How a connection ends"), and the CAP scenarios pin the
# exact line instead -- CAP_DISC_RE below. What is never accepted is ssh
# reaching its own clean refusal, which is what a server with no cap and no
# signature check produces.
#
# SECOND COPY of this exact regex is test-ssh-auth-qemu.sh's ENDED_RE --
# change both, the same convention as SUCCESS_RE above.
ENDED_RE='Received disconnect from|Connection reset|Connection closed by|Broken pipe|connection unexpectedly closed'

# CAP_DISC_RE is lib/ssh-guest-lib.sh's.
ended_by_cap() { grep -qaE "$CAP_DISC_RE" "$1"; }

ended_by_server() {
    local log="$1"
    if grep -q "Permission denied" "$log"; then return 1; fi
    grep -qE "$ENDED_RE" "$log"
}

# What actually ended it, for the reader. Not graded -- see above.
report_ending() {
    local what="$1" log="$2" line
    line=$(grep -m1 -E "$ENDED_RE|Permission denied" "$log" 2>/dev/null || true)
    echo "  $what ended with: ${line:-<no ending line in the transcript>}"
}

never_authenticated() {
    ! grep -q "$SUCCESS_RE" "$1"
}

# ONE LINE of the server's authentication audit log, read from the GUEST --
# no client can see a server's own record. $1 is the line between "ssh: " and
# " from": an ERE, so a literal parenthesis must come escaped. $2, optional,
# is an ERE for what follows the PORT -- the login-grace line now carries a
# reason there (NEW-1), and without this argument such a line is
# unmatchable rather than merely unasserted, because of the anchoring below.
#
# THE WHOLE LINE, anchored at both ends (-x): the console timestamp, the INFO
# level, the source the guest really sees ($HOST_AS_SEEN, measured by the
# penalty scenario) and the port. So a DEBUG copy -- which prints as
# `ssh:func:NNNN: ...`, not `ssh: ...` -- cannot satisfy it, and neither can a
# line that carries the rate limiter's "(and N more ...)" suffix, which would
# mean an EARLIER line of the same kind took the window. Leaving $2 out
# therefore ASSERTS THERE IS NO TAIL, which is what pins the unchanged
# no-reason form of the timeout line.
#
# Shared by both halves since the hook watchdog gave the hooks half lines of
# its own to read.
audit_line() {
    test_clean_log
    grep -qaxE "[0-9:.]+ \[INFO\]  ssh: $1 from ${HOST_AS_SEEN//./\\.} port [0-9]+${2:-}" \
        "$TEST_CLEAN_LOG"
}

# How many audit lines carry this TEXT, at EITHER level.
#
# Why not audit_line: the rate limiter gives each KIND one INFO line a
# minute and writes the rest at DEBUG, so the second line of a kind in one
# scenario is at DEBUG through no fault of the code under test. When what is
# being asserted is the line's CONTENT rather than its level -- "this hook
# produced a slow-return line", "that one produced no stalled line at all"
# -- counting both copies is the honest question. Still anchored at the end
# and still exact about every field; only the level framing is relaxed,
# which is why it is a separate helper and not a loosening of audit_line.
audit_text_count() {
    test_clean_log
    local n
    n=$(grep -caE "ssh(:[a-z_]+:[0-9]+)?: $1 from ${HOST_AS_SEEN//./\\.} port [0-9]+${2:-}\$" \
        "$TEST_CLEAN_LOG" || true)
    echo "${n:-0}"
}

# How many guest lines CONTAIN $1, and how many ARE exactly $1. Both read
# TEST_CLEAN_LOG, so call test_clean_log first. The exact form is for a line
# that carries a value: the assertion then pins the whole line, value
# included, rather than whatever happens to share its prefix.
# guest_count is a FIXED-STRING match (grep -F): the patterns are log text,
# and several carry '(' or '.' that must not act as regex. A log it cannot
# READ (grep exit 2) prints "unreadable" and returns 2 -- never a count that
# could pass `= 0` -- and an assignment from it ends the script under set -e.
guest_count() {
    local n rc=0
    n=$(grep -cF -- "$1" "$TEST_CLEAN_LOG") || rc=$?
    if [[ "$rc" -gt 1 ]]; then
        echo "guest_count: cannot read $TEST_CLEAN_LOG" >&2
        echo unreadable
        return 2
    fi
    echo "${n:-0}"
}
guest_lines()    { local n; n=$(grep -cxF "$1" "$TEST_CLEAN_LOG" || true); echo "${n:-0}"; }

# How many RAW serial lines match $1 (a grep BRE). The raw log, not
# TEST_CLEAN_LOG: this is sampled mid-scenario, at the instant a decision is
# taken, and test_wait_for reads the same file.
raw_count() { local n; n=$(grep -ac "$1" "$TEST_LOG" 2>/dev/null || true); echo "${n:-0}"; }

# The guest log's last lines, for a heartbeat failure. Dumped HERE too, not
# only on the deadline failure in hostile_finish: a wedge caught by the
# heartbeat is exactly when its last lines matter, and having them only on the
# other branch cost a diagnostic round trip. An overrun needs them as much --
# which login timed out, and when the deadline fired.
hostile_dump_tail() {
    test_clean_log
    echo "--- guest serial log (tail) ---"
    tail -30 "$TEST_CLEAN_LOG" | sed 's/^/      /'
}

# THE EARLY FINISH. Every scenario and every check that reads the live server
# is over by the time hostile_finish calls this (the heartbeat window is the
# last of them), so the rest of the deadline would be idle. The host connects
# to the fixture's control port (ctl=, a plain AxlTcp listener: the SSH
# servers are freed and filled to their caps by the scenarios); the fixture
# prints SERVER: FINISH-ARMED and arms a FINISH_MS (2 s) one-shot on its
# loop, and THAT timer quits the loop (SERVER: FINISH). So what the deadline
# proved is still proved -- the loop ran a one-shot timer and returned, and
# the teardown ran -- and a fixture whose finish timer never fires does not
# pass: the wait is bounded by the finish timer, not by the deadline, and the
# deadline ending the loop instead is a failure of its own.
#
# WHAT IT NO LONGER SHOWS: that a one-shot armed at BOOT survives every
# scenario's churn of sources and still fires -- the deadline did, on every
# run. The finish timer is armed after the churn; the heartbeat, armed at
# boot, is a periodic source. That is left to test-ssh-policy-qemu.sh, whose
# boot ends on its deadline after its own scenarios, rather than made a
# second chain of one-shots here: a re-arming source takes the first free
# slot of the loop's source array, and test-ssh-hostile-qemu.sh's penalty
# knocks depend on what sits ahead of the second server's accept there
# (ssh-server-selftest.c, the heartbeat's comment).
#
# QEMU's user-mode forward accepts the host's connection itself, whether or
# not the guest is listening, so a connect that succeeds says nothing; the
# fixture's FINISH-ARMED line is the acknowledgement, and the connect is
# retried until it appears.
HOSTILE_FINISH_SEC=2   # ssh-server-selftest.c's FINISH_MS
hostile_end_early() {
    local i j armed=0 n_done n_fin n_dl
    # The deadline got there first: the heartbeat check has already failed
    # the run as an overrun, and a signal now would only add a second,
    # misleading failure about the control port. Collect the teardown the
    # deadline way instead, for the checks after this one.
    if [[ "$(raw_count 'SERVER: DEADLINE')" -gt 0 ]]; then
        hostile_wait_deadline
        return 0
    fi
    for ((i = 0; i < 5 && armed == 0; i++)); do
        # A guest that is gone, or a loop already tearing down, will never
        # answer: say so now rather than after every retry.
        if ! test_guest_running \
            || [[ "$(raw_count 'SERVER: REAPING')" -gt 0 ]]; then
            break
        fi
        timeout 5 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1" && sleep 0.5' \
            _ "$CTL_HOST_PORT" 2>/dev/null || true
        for ((j = 0; j < 30; j++)); do
            if [[ "$(raw_count 'SERVER: FINISH-ARMED')" -gt 0 ]]; then
                armed=1
                break
            fi
            sleep 0.1
        done
    done
    if [[ "$armed" == 0 ]]; then
        echo "FAIL: the fixture never acknowledged the finish signal (no SERVER: FINISH-ARMED"
        echo "      after $i connect(s) to host :$CTL_HOST_PORT; CTL-FAIL lines" \
             "$(raw_count 'SERVER: CTL-FAIL'), finish-timer failures" \
             "$(raw_count 'SERVER: FAIL finish-timer'), REAPING $(raw_count 'SERVER: REAPING'))"
        fail=$((fail+1))
        hostile_dump_tail
        return 0
    fi
    # The timer's own period, plus the 15 s for the teardown and a slow guest
    # the deadline wait allows past its deadline.
    test_wait_for "SERVER: DONE" $(( HOSTILE_FINISH_SEC + 15 )) || true
    test_clean_log
    n_done=$(guest_lines 'SERVER: DONE')
    n_fin=$(guest_lines 'SERVER: FINISH')
    n_dl=$(guest_lines 'SERVER: DEADLINE')
    if [[ "$n_done" == 1 && "$n_fin" == 1 && "$n_dl" == 0 ]]; then
        echo "PASS: the event loop quit on its finish timer and tore down (never wedged)"
        pass=$((pass+1))
    elif [[ "$n_dl" != 0 ]]; then
        echo "FAIL: the loop ended on its deadline, not on the finish timer armed"
        echo "      ${HOSTILE_FINISH_SEC}s after the signal (FINISH $n_fin, DEADLINE $n_dl, DONE $n_done)"
        fail=$((fail+1))
        hostile_dump_tail
    else
        echo "FAIL: the event loop never ran its finish timer to SERVER: DONE — it is wedged"
        echo "      (FINISH $n_fin, DONE $n_done, $(( HOSTILE_FINISH_SEC + 15 ))s after the signal)"
        fail=$((fail+1))
        hostile_dump_tail
    fi
}

# The unsignalled ending (SSH_HOSTILE_ABUSE=0, or an overrun): the deadline
# itself is the timer, and the grading pins that it was -- SERVER: DEADLINE
# once, and no SERVER: FINISH. Waited for as long as the deadline has LEFT, plus 15 s for the
# teardown and a slow guest -- not for a fixed guess. Sized too short, the
# wait gives up before a healthy deadline fires. Sized from the deadline
# itself, it would declare a real wedge a whole deadline late.
hostile_wait_deadline() {
    local done_wait
    done_wait=$(( SERVER_DEADLINE_MS / 1000 - (SECONDS - HOSTILE_LISTEN_T) + 15 ))
    if [[ "$done_wait" -lt 15 ]]; then done_wait=15; fi
    test_wait_for "SERVER: DONE" "$done_wait" || true
    test_clean_log
    if [[ "$(guest_lines 'SERVER: DONE')" == 1 \
          && "$(guest_lines 'SERVER: DEADLINE')" == 1 \
          && "$(guest_lines 'SERVER: FINISH')" == 0 ]]; then
        echo "PASS: the event loop reached its deadline (never wedged)"
        pass=$((pass+1))
    else
        echo "FAIL: the event loop never reached its deadline — it is wedged"
        echo "      (DEADLINE $(guest_lines 'SERVER: DEADLINE'), FINISH $(guest_lines 'SERVER: FINISH')," \
             "DONE $(guest_lines 'SERVER: DONE'))"
        fail=$((fail+1))
        hostile_dump_tail
    fi
}

# hostile_finish -- the liveness and teardown checks every boot ends with,
# then the verdict. Exits: 0 only if every check in the file passed.
hostile_finish() {
    # THE HEARTBEAT IS THE FAST DETECTOR. The fixture ticks every 2 s; if the
    # loop is stuck inside one callback the tick number stops advancing, and a
    # 6 s window sees it. Waiting out the deadline instead costs far longer
    # per failing run, which is a real cost on a shared box -- this check
    # exists so a regression is caught cheaply and the timer ending below (the
    # finish timer, or the deadline) is only confirmation.
    #
    # The window must close WELL BEFORE the deadline quits the loop: after
    # that the ticks legitimately stop, and a check still sampling then reads
    # it as a stall (which it did, twice, and I believed it). The deadline is
    # summed above from the selected scenarios' budgets plus a base that
    # includes this window and 23 s of margin, so 6 s here sits clear of both
    # ends.
    #
    # WHAT THIS DOES NOT GUARD, measured rather than assumed: reverting the
    # AxlTcp abortive-close change is NOT detected here. With MAX_CONNS at 16
    # the stall is under one tick, and the tick census is identical either way
    # (14 ticks clean, 14 reverted). This test measures the connection CAP,
    # not that change.
    #
    # A SCENARIO THAT OVERRAN ITS BUDGET ends the window the same way: the
    # deadline fires, the loop quits and the ticks stop, so a stuck heartbeat
    # alone cannot tell a wedge from a run that was simply too slow. The
    # fixture says which: SERVER: DEADLINE is printed by the deadline's own
    # timer, which a wedged loop never runs. And the deadline is sized to
    # fire only AFTER this window (the base above includes it), so that line
    # in the log at ANY point of this check is an overrun -- read before the
    # window, so none is sampled after a deadline, and again after it, so a
    # deadline that fired mid-window cannot pass as a tick. Still a failure,
    # since every scenario after the deadline ran against a server being torn
    # down, but not one that sends a reader hunting a stuck callback.
    # Measured: the live flood at LIVE_FLOOD_WIDTH=128 (~256/s nominal) lost
    # three logins to 20 s timeouts and was reported here as a stall, with
    # DEADLINE and DONE in the log.
    local t0=0 t1=0 overran=0
    if [[ "$(raw_count 'SERVER: DEADLINE')" -gt 0 ]]; then
        overran=1
    else
        t0=$(tick_now); t0=${t0:-0}
        sleep 6
        t1=$(tick_now); t1=${t1:-0}
        if [[ "$(raw_count 'SERVER: DEADLINE')" -gt 0 ]]; then overran=1; fi
    fi
    if [[ "$overran" == 1 ]]; then
        echo "FAIL: the fixture's deadline fired before the heartbeat window closed"
        echo "      (SERVER: DEADLINE is in the log): the scenarios overran their"
        echo "      summed HOSTILE_BUDGET -- not a stall"
        fail=$((fail+1))
        hostile_dump_tail
    elif [[ "$t1" -gt "$t0" ]]; then
        echo "PASS: the event loop is still dispatching (heartbeat $t0 -> $t1 in 6s)"
        pass=$((pass+1))
    else
        echo "FAIL: the event loop stopped dispatching (heartbeat stuck at $t0)"
        echo "      it is inside a callback that never returned"
        fail=$((fail+1))
        hostile_dump_tail
    fi

    # And it must end on a TIMER. A loop that is dispatching can still be
    # losing its timers, which the heartbeat alone would not catch.
    if [[ "$ABUSE" == 1 ]]; then
        hostile_end_early
    else
        hostile_wait_deadline
    fi

    # Principal accounting, checked once at teardown against every login the
    # HOST saw succeed across the WHOLE boot -- not just one scenario's.
    # Staging credentials installs the fixture's hooks unconditionally, so
    # every publickey good_login_works call goes through on_publickey and
    # adopts a principal, and so does every password login either server
    # accepts; a count scoped to one scenario would miss a leak or a double
    # release anywhere else in the boot. auth-pending-drop's stale slow login
    # must NOT count: its principal was never adopted, only ever held by the
    # fixture as a static. Nor does auth-hook-reenter's: that login never
    # authenticates, and its principal prints the DIFFERENT line SERVER:
    # ITERATE-PRINCIPAL-RELEASED, which this pattern does not match ("SERVER:
    # I...", not "SERVER: P..."). Too few here is a leak; too many is a double
    # release.
    if [[ "$HAVE_SSH" == 1 && "$ABUSE" == 1 ]]; then
        local host_ok=0 f n
        # This glob also picks up logins that must never authenticate (the
        # forged, refused and reaped ones). Each is checked never to have
        # authenticated where it runs, so each contributes zero "Authenticated
        # to " matches here. If one ever did, this would show up only as a
        # mismatch against the guest's PRINCIPAL-RELEASED count below -- not
        # as a check naming the real defect.
        #
        # A login made by ssh-peer.py counts the same way: its transcript
        # goes to an ssh-*.log too, and it prints `peer: authenticated`
        # where ssh(1) prints "Authenticated to".
        for f in "$TEST_TMPDIR"/ssh-*.log; do
            [[ -e "$f" ]] || continue
            n=$(grep -c -e 'Authenticated to ' -e '^peer: authenticated$' \
                "$f" || true)
            host_ok=$((host_ok + ${n:-0}))
        done
        test_clean_log
        echo "  teardown: the host saw $host_ok successful login(s)"
        check "teardown: one principal release per successful login" \
            test "$(guest_count 'SERVER: PRINCIPAL-RELEASED')" = "$host_ok"
    fi

    # The heap's verdict on the whole boot, printed by the fixture after the
    # servers and the loop are freed and the quarantine is drained. Every
    # selection gets it: a use-after-free WRITE that happens not to crash is
    # invisible to every other check. What it can and cannot see is argued in
    # src/mem/axl-mem.c's quarantine comment: writes into a block still held
    # when it is evicted or drained, and frees of a block still held.
    test_clean_log
    check "teardown: the heap recorded no corruption (SERVER: MEM-CORRUPTION 0)" \
        test "$(guest_lines 'SERVER: MEM-CORRUPTION 0')" = 1

    ssh_check_no_reentry teardown

    # The depth is printed at teardown, so it is graded here rather than in
    # the scenario. Only when auth-hook-nest ran: that is the selection whose
    # PRECONDITION proves hooks were delivered to overlap.
    if wants auth-hook-nest && [[ "$HAVE_SSH" == 1 ]]; then
        echo "  $(grep -a 'SERVER: HOOK-DEPTH-MAX' "$TEST_CLEAN_LOG" || echo 'no HOOK-DEPTH-MAX line')"
        check "auth-hook-nest: one hook on the stack at a time (SERVER: HOOK-DEPTH-MAX 1)" \
            test "$(guest_lines 'SERVER: HOOK-DEPTH-MAX 1')" = 1
    fi

    # The fixture's teardown leak verdict, printed after main returns -- so
    # wait for it, since SERVER: DONE precedes it. Two counts, as
    # test-cxx-seam-qemu.sh does: exactly one clean verdict proves accounting
    # was ON and the fixture reached its teardown, and no leak report proves
    # nothing leaked. For auth-hook-free this is the evidence that the
    # deferred half of the free ran: the server's own allocation is freed
    # nowhere else.
    test_wait_for 'mem: no leaks detected\|=== AxlMem leak report:' 15 || true
    test_clean_log
    local n_leak n_ok
    n_leak=$(grep -acF '=== AxlMem leak report:' "$TEST_CLEAN_LOG" || true)
    n_ok=$(grep -acF 'mem: no leaks detected' "$TEST_CLEAN_LOG" || true)
    check "teardown: the fixture leaked nothing (${n_leak:-0} leak reports)" \
        test "${n_leak:-0}" = 0
    check "teardown: the fixture printed exactly one leak verdict, clean (${n_ok:-0})" \
        test "${n_ok:-0}" = 1

    if [[ "$fail" -ne 0 && "${#DUMP_LOGS[@]}" -gt 0 ]]; then
        for f in "${DUMP_LOGS[@]}"; do
            [[ -f "$f" ]] || continue
            echo "--- $(basename "$f") (tail) ---"
            tail -25 "$f" | sed 's/^/      /'
        done
    fi

    echo
    echo "$HOSTILE_SUMMARY: $pass passed, $fail failed ($TEST_ARCH)"
    [[ "$fail" -eq 0 ]]
}
