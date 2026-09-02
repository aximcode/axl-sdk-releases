#!/bin/bash
# run-integration.sh — the integration-suite execution engine, used both
# locally and in CI. A -jN job pool runs tests concurrently; --shard i/K splits
# the set across machines. Reads per-test metadata via lib/discover.sh.
#
# Usage:
#   run-integration.sh [--arch X64|AARCH64] [--timeout SECONDS] [-jN] [--list]
#
# Concurrency: -jN sets the worker count. With no -j, the default leaves two
# cores of headroom (nproc-2, min 1; 1 on hosts with <3 cores) — a saturated
# host can starve a QEMU during its boot/setup window, which surfaces as empty
# serial output and a spurious failure. For the same reason, do NOT stack the
# full -jN suite on top of other heavy jobs (a concurrent `lint.sh`, a build):
# the combined overcommit reintroduces those contention flakes. Run them
# sequentially, or lower -j, if you must overlap. A transient failure is
# retried once (see run_one) so a lone contention blip doesn't fail the suite.
#
# Output: one `  <name> PASS|FAIL|TIMEOUT <dur>s` line per test, then a totals
# line. Exit non-zero if any test failed or timed out.
#
# RUN_INTEGRATION_DIR (env) overrides the test directory and runs every
# test-*.sh in it verbatim — the host-only self-test seam (no QEMU).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# THE -delay 0 SHELL LAUNCHER, ON FOR THE SUITE AND OFF FOR CONSUMERS.
#
# Every guest boot otherwise pays the EDK2 Shell's "Press ESC in 5...1 seconds
# to skip startup.nsh" countdown -- five gBS->Stall(1s) busy-waits. Measured
# 2026-08-21, back-to-back uncached X64 runs on one machine, both 172 passed /
# 0 failed:  launcher OFF 489 s, launcher ON 368 s -- 121 s, 24.7%. Per boot it
# is ~4.6 s on BOTH arches (X64 6.2 -> 1.6 s, AARCH64 10.8 -> 6.1 s).
#
# WHY IT IS SET HERE AND NOT AS THE DEFAULT IN axl-common.sh. The launcher was
# made opt-in by 1ae66ccd because it "hangs some firmware": the Shell loads,
# starts, then produces no output. That is NOT fixed -- investigated 2026-08-21
# and the code was never the variable. The launcher works at 1ae66ccd itself
# (built that tree, ran its own test: passes), across three OVMF builds and
# both arches, and through a full uncached 172-test suite. What changed is the
# MACHINE's firmware: the custom OVMF of that era is gone and the box now runs
# distro stock. The trigger is absent, not repaired.
#
# That split (suite yes, standalone run-qemu.sh no) is no longer why this is
# set: run-qemu.sh's default is now ON and self-healing -- it notices a firmware
# that never reaches the Shell, records it, and retries without the launcher.
#
# =1 is kept here for a DIFFERENT reason: it means "always, ignore the cache,
# no fallback", so a genuine launcher regression fails LOUDLY across the suite
# instead of being absorbed into a slow-but-green run. If you would rather the
# suite self-heal on a box whose firmware cannot take the launcher, unset this
# -- the first test to hit it records the verdict and the rest skip it, while
# test-shell-launcher-qemu.sh (which forces =1 itself) stays the loud guard.
#
# Assignment-if-unset, so an explicit outer value still wins in both
# directions: AXL_SHELL_LAUNCHER=0 to bisect a suspected launcher fault.
: "${AXL_SHELL_LAUNCHER:=1}"
export AXL_SHELL_LAUNCHER

source "$SCRIPT_DIR/lib/discover.sh"
# shellcheck source=lib/test-cache.sh
source "$SCRIPT_DIR/lib/test-cache.sh"

ARCH="X64"; TIMEOUT=900; LIST_ONLY=0; JOBS=0; SHARD=""; LOGDIR=""  # JOBS=0 => auto
# Local-only tests need a capability the GitHub runners lack (a patched QEMU
# for the SMBus/SPD memdev device, usb-mouse pointer delivery, ...). The dev
# box HAS those, so the local run includes them by default; --ci drops them.
INCLUDE_LOCAL="--include-local-only"
ONLY_LOCAL=0
# ON BY DEFAULT. An opt-in flag does not get typed: this cache was built and
# then not used once in the session that built it, while the full uncached gate
# was run after every change. `--no-cache` turns it off, and it is what a
# pre-push or release run must use -- only an uncached run writes the
# release-gate stamp (see the stamp block at the bottom, and
# AXL-CI-Release-Speed-Design.md §12.16).
# $SCRIPT_DIR, not a repo-relative path: the runner is invoked from wherever
# the caller happens to be, and a relative default resolved against THAT --
# test-runner-selftest.sh cd's into test/integration and created
# test/integration/test/integration/.test-cache. One cache per checkout,
# wherever it is run from.
CACHE_DIR="$SCRIPT_DIR/.test-cache"
CACHE_HITS=0
TESTDIR="${RUN_INTEGRATION_DIR:-$SCRIPT_DIR}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)    ARCH="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --list)    LIST_ONLY=1; shift ;;
        --shard)   SHARD="$2"; shift 2 ;;   # i/K (0-based)
        --logdir)  LOGDIR="$2"; shift 2 ;;  # keep per-test output here
        --no-build) RUN_NO_BUILD=1; shift ;; # skip the one-time pre-build (CI: built in a prior job)
        # --ci also turns the cache OFF. CI is the backstop -- §11.2 records
        # five days of undetected red when it was not run at all -- and a
        # backstop that skips tests is not one. (A fresh checkout has no cache
        # dir anyway, so this is belt and braces, and it is the belt that
        # matters if anyone ever runs --ci on a working tree.)
        --ci)      INCLUDE_LOCAL=""; CACHE_DIR=""; shift ;;
        # The inverse of --ci: ONLY what CI cannot run. For the inner loop,
        # where the other 78% of the wall clock is work a push repeats for
        # free. NOT a substitute for the full run before pushing -- see the
        # banner it prints.
        --only-local) INCLUDE_LOCAL="--only-local"; ONLY_LOCAL=1; shift ;;
        # Accepted and a no-op: caching is the default now, and this spelling
        # exists so an invocation written before that still works.
        --cache)      CACHE_DIR="$SCRIPT_DIR/.test-cache"; shift ;;
        # Turn the cache OFF. Required for a run that must certify the tree:
        # only an uncached run writes the release-gate stamp. lib/test-cache.sh
        # documents what the key does and does not cover -- the host
        # environment is the gap, and it is why this switch exists at all.
        --no-cache)   CACHE_DIR=""; shift ;;
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        *) shift ;;
    esac
done
[[ -z "$LOGDIR" ]] && LOGDIR=$(mktemp -d)
mkdir -p "$LOGDIR"
# Wall-clock origin for the totals line at the end. $SECONDS is shell
# uptime, so this is taken once, here, rather than inferred later.
_RUN_T0=$SECONDS
: > "$LOGDIR/_durations.txt"
# The two tally files exist from the start, EMPTY, because their readers count
# them with `wc -l < FILE 2>/dev/null`. That 2>/dev/null is on `wc`, but the `<`
# redirect is performed by the SHELL, so a missing file prints
# "No such file or directory" to the script's stderr and nothing can suppress it
# at the call site. Every uncached run printed one; the counts were right (the
# reader defaults to 0), so it was pure noise -- the kind that trains a reader
# to skim a release run's stderr.
#
# Created here rather than guarded at each reader: there were TWO readers and
# only one was ever reported, so the fix that scales is to remove the
# precondition instead of defending it twice.
: > "$LOGDIR/_skipped.txt"
: > "$LOGDIR/_cached.txt"

# Auto job count when -j was not given: leave two cores of headroom so a guest
# isn't starved during boot/setup (starvation there -> empty serial output ->
# spurious failure). Only parallelize with >=3 cores; otherwise stay serial.
if [[ "$JOBS" -le 0 ]]; then
    _ncpu=$(nproc 2>/dev/null || echo 1)
    if [[ "$_ncpu" -ge 3 ]]; then JOBS=$(( _ncpu - 2 )); else JOBS=1; fi
fi

# This used to `export AXL_TLS=1` for every test subprocess, and had to: the
# Makefile WIPED every object whenever the flag toggled, so ~64 plain tests and
# 5 TLS tests sharing one prefix made the wipe fire constantly and -- run
# concurrently -- clobber each other's artifacts mid-run. Forcing one value was
# the fix.
#
# mbedTLS is unconditional now, so there is one configuration, nothing to
# toggle and nothing to standardise. The strip contract that made this safe is
# still enforced, by test-tls-strippable.sh.
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAKE_ARCH=x64; [[ "$ARCH" == "AARCH64" ]] && MAKE_ARCH=aa64

if [[ -n "$CACHE_DIR" ]]; then
    mkdir -p "$CACHE_DIR"
    # Absolute: tests run with their own cwd, and run-qemu.sh is a separate
    # process again.
    AXL_TEST_CACHE="$(cd "$CACHE_DIR" && pwd)"; export AXL_TEST_CACHE
fi

if [[ -n "${RUN_INTEGRATION_DIR:-}" ]]; then
    mapfile -t TESTS < <(ls "$TESTDIR"/test-*.sh 2>/dev/null)
else
    mapfile -t TESTS < <(discover_tests --arch "$ARCH" $INCLUDE_LOCAL)
fi

# Shard selection: keep only this shard's subset, balanced by est= so each
# shard's total estimated wall-clock is roughly equal (greedy
# longest-processing-time-first into the currently-lightest shard).
if [[ -n "$SHARD" ]]; then
    shard_i="${SHARD%%/*}"; shard_k="${SHARD##*/}"
    declare -a pairs=()
    for t in "${TESTS[@]}"; do
        e=$(test_meta_field "$t" est); [[ "$e" =~ ^[0-9]+$ ]] || e=20
        pairs+=("$e|$t")
    done
    declare -a load=() mine=()
    for ((s=0; s<shard_k; s++)); do load[$s]=0; done
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        e="${line%%|*}"; p="${line#*|}"
        min=0
        for ((s=1; s<shard_k; s++)); do
            [[ ${load[$s]} -lt ${load[$min]} ]] && min=$s
        done
        [[ $min -eq $shard_i ]] && mine+=("$p")
        load[$min]=$(( load[$min] + e ))
    done < <(printf '%s\n' "${pairs[@]}" | sort -t'|' -k1,1nr)
    TESTS=("${mine[@]}")
fi

# LONGEST FIRST (LPT), and this is worth more than it looks.
#
# The pool takes tests in discovery order, which is the glob's -- alphabetical,
# i.e. arbitrary with respect to cost. Feeding a fixed set of jobs to N workers
# is makespan scheduling, where longest-processing-time-first is the classic
# greedy and is provably within 4/3 of optimal; arbitrary order has no bound at
# all, because a long test that starts last runs alone while five workers idle.
#
# The FULL run barely notices: 164 tests over 6 workers already measured 99%
# packing (3,385 s of work, a 564 s floor, 569 s actual) because there is always
# small work left to fill a gap. The SCOPED run is where order decides
# everything -- `--only-local` is 17 tests of which six are >= 87 s, and it
# measured 194 s against a 125 s floor: 65%. Same tests, same workers, just
# started in the wrong order.
#
# `est=` is declared by every test and enforced by check-test-meta, so the key
# already exists and needs no new metadata. It is an ESTIMATE: being wrong
# costs some packing efficiency and nothing else, since order cannot change a
# result. Ties keep discovery order (`sort -s`), so a run stays reproducible.
if [[ ${#TESTS[@]} -gt 1 ]]; then
    mapfile -t TESTS < <(
        for _t in "${TESTS[@]}"; do
            _e=$(test_meta_field "$_t" est 2>/dev/null)
            [[ "$_e" =~ ^[0-9]+$ ]] || _e=20
            printf '%s\t%s\n' "$_e" "$_t"
        done | sort -s -k1,1nr | cut -f2-
    )
fi

# Warn when --timeout is too tight for what was selected. stderr only, so
# --list's stdout stays machine-parseable. Every test declares
# an `est=` in its test-meta header, and a test that blows the wall clock is
# reported as a TIMEOUT with no hint that the limit -- not the test -- was the
# problem. Under the parallel pool a test routinely takes well over its est
# (that is why run_one retries at all), so the bar is 2x rather than 1x.
if [[ ${#TESTS[@]} -gt 0 ]]; then
    _max_est=0; _max_est_name=""
    for t in "${TESTS[@]}"; do
        _e=$(test_meta_field "$t" est 2>/dev/null)
        [[ "$_e" =~ ^[0-9]+$ ]] || continue
        if [[ "$_e" -gt "$_max_est" ]]; then _max_est="$_e"; _max_est_name=$(basename "$t"); fi
    done
    if [[ "$_max_est" -gt 0 && "$TIMEOUT" -lt $(( _max_est * 2 )) ]]; then
        echo "warning: --timeout ${TIMEOUT}s is tight for $_max_est_name (est=${_max_est}s)." >&2
        echo "         Under -j${JOBS} a test commonly exceeds its est; consider --timeout $(( _max_est * 2 ))s or more." >&2
    fi
fi

if [[ $LIST_ONLY -eq 1 ]]; then
    [[ ${#TESTS[@]} -gt 0 ]] && printf '%s\n' "${TESTS[@]}"
    exit 0
fi

# Build ONCE up front (real mode only; the self-test seam uses stubs that need
# no build). This populates the shared prefix before the pool starts, so the
# per-test makes that follow are consistent-flag no-ops instead of a thundering
# herd of concurrent first-builds. --no-build skips it (CI builds in a prior job).
if [[ -z "${RUN_INTEGRATION_DIR:-}" && "${RUN_NO_BUILD:-0}" != "1" ]]; then
    echo "pre-building once (ARCH=$MAKE_ARCH all tests tools axl-busybox)..."
    if ! make -C "$PROJECT_DIR" ARCH="$MAKE_ARCH" all tests tools axl-busybox \
            > "$LOGDIR/_prebuild.log" 2>&1; then
        echo "pre-build FAILED — see $LOGDIR/_prebuild.log"; exit 1
    fi
fi

# A STALE STAGED SDK is a precondition, not a test result.
#
# Several tests assert that the staged include/axl-sdk/axl matches include/axl,
# because they exercise the STAGED copy and an unstaged header edit means they
# silently report on the previous one. That assertion is right, but as a
# per-test failure it is diagnosis-hostile: editing one public header surfaces
# as two unrelated-looking C++ tests failing at the END of a seven-minute run,
# and the reader has to open a log to learn the cause is one missing command.
# It happened twice in one evening, and cost a re-run each time.
#
# So it is checked ONCE, here, before anything runs. Same condition, same
# remedy, stated when it is cheap to act on. The per-test assertions stay --
# they are what protects a test run from a stage that goes stale MID-run, which
# this cannot see.
if [[ -d "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/include/axl-sdk/axl" ]]; then
    if ! diff -rq "$PROJECT_DIR/include/axl" \
                  "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/include/axl-sdk/axl" >"$LOGDIR/_stage.diff" 2>&1; then
        echo
        echo "*** STAGED SDK IS STALE — run: ./scripts/install.sh --arch all --cpp"
        echo "    include/axl and the staged copy differ, so any test that"
        echo "    exercises the staged SDK would report on the PREVIOUS build."
        sed 's/^/      /' "$LOGDIR/_stage.diff" | head -8
        echo
        exit 1
    fi
fi

# Run one test under a timeout. Host ports are NOT assigned here: each test
# claims its own from the shared allocator in scripts/axl-common.sh (see
# common-test.sh's test_port), which verifies each port is free right now and
# holds it for the test's lifetime.
#
# This used to be `TEST_PORT_BASE=$((20000 + idx*200))`. That kept ONE
# invocation's tests apart but nothing else: a second run-integration.sh —
# another developer, a consumer repo's suite, a second agent — derives the
# identical bases and collides on every one of them. Worse, the retry below
# reused the same base, so a genuine collision was retried straight into it.
# Leaving TEST_PORT_BASE unset is what routes both attempts through the
# allocator, so a retry draws different ports.
# Write the runner's OWN per-test duration to the profile. Not a second
# measurement of the same thing -- it IS the measurement run_one already makes
# and prints, written where profile-report.py can read it. The report needs
# totals for all 164 tests to derive seconds-per-boot for the ones instrumented
# by count only (see scripts/run-qemu.sh).
_prof_test_total() {
    # Always recorded, profile or not: the totals line at the end reports how
    # much test work the run actually did, and that number is what makes the
    # difference between "the suite is slow" and "there are 47 minutes of work
    # in it" a measurement rather than an argument. One append per test.
    printf '%s\n' "$2" >> "$LOGDIR/_durations.txt"
    [[ -n "${AXL_TEST_PROFILE:-}" ]] || return 0
    printf 'test|%s|%s\n' "$1" "$2" >> "$AXL_TEST_PROFILE"
}

run_one() {  # <idx> <test_path>
    local idx="$1" t="$2" name start dur rc
    name=$(basename "$t"); start=$SECONDS
    # Name the profile records for this test. run-qemu.sh runs as a separate
    # process and has no other way to know which test launched it.
    export AXL_TEST_PROFILE_NAME="$name"

    # SKIP CACHE. Decided here and nowhere else, because this is the only place
    # that sees every test exactly once -- common-test.sh's test_setup runs
    # three times in some tests, and the run-qemu-only tests never reach it.
    if [[ -n "$CACHE_DIR" ]] && cache_is_fresh "$name"; then
        dur=$(( SECONDS - start ))
        echo "  $name CACHED ${dur}s (inputs identical to its last green run)"
        echo "$name" >> "$LOGDIR/_cached.txt"
        return 0
    fi
    # A real run rewrites the input list from scratch, so a test that stages
    # FEWER things than last time does not inherit stale entries.
    [[ -n "$CACHE_DIR" ]] && cache_begin_inputs "$name"
    timeout "$TIMEOUT" bash "$t" --arch "$ARCH" > "$LOGDIR/$name.log" 2>&1
    rc=$?
    # 77 = SKIPPED (the automake convention). Handled BEFORE the retry, because
    # a test that cannot run will not run any better the second time, and
    # before the cache, because the reason it skipped -- an absent corpus, a
    # missing host tool -- is not in the key, so committing one would skip it
    # forever afterwards.
    if [[ $rc -eq 77 ]]; then
        dur=$(( SECONDS - start ))
        _prof_test_total "$name" "$dur"
        [[ -n "$CACHE_DIR" ]] && cache_invalidate "$name"
        echo "$name" >> "$LOGDIR/_skipped.txt"
        echo "  $name SKIP ${dur}s ($(grep -m1 -oE 'SKIP[:( ].{0,60}' "$LOGDIR/$name.log" 2>/dev/null || echo 'declined to run'))"
        return 0
    fi
    if [[ $rc -ne 0 ]]; then
        # Retry once. Most failures under the parallel pool are transient
        # resource-contention flakes (a QEMU starved during boot emits empty
        # serial output -> the test's greps miss). A genuine failure fails
        # again. Preserve the first attempt's log for post-mortem and let the
        # host drain briefly before the retry so the contention can ease.
        cp -f "$LOGDIR/$name.log" "$LOGDIR/$name.attempt1.log" 2>/dev/null
        sleep 3
        timeout "$TIMEOUT" bash "$t" --arch "$ARCH" > "$LOGDIR/$name.log" 2>&1
        rc=$?
        if [[ $rc -eq 0 ]]; then
            dur=$(( SECONDS - start ))
            _prof_test_total "$name" "$dur"
            [[ -n "$CACHE_DIR" ]] && cache_commit "$name"
            echo "  $name PASS ${dur}s (retry; attempt 1 failed — see $name.attempt1.log)"
            return 0
        fi
    fi
    dur=$(( SECONDS - start ))
    _prof_test_total "$name" "$dur"
    if [[ $rc -eq 0 ]]; then
        [[ -n "$CACHE_DIR" ]] && cache_commit "$name"
        echo "  $name PASS ${dur}s"; return 0
    fi
    # A red test must never be skippable next time, even if its inputs are
    # somehow identical again.
    [[ -n "$CACHE_DIR" ]] && cache_invalidate "$name"
    # Failure/timeout (twice): replay the test's captured output (logs live in
    # an ephemeral dir that CI discards, so surface the reason inline). Tail
    # keeps the job log readable when many tests fail at once.
    if [[ $rc -eq 124 ]]; then
        # Name the declared est: a TIMEOUT is ambiguous between "the test hung"
        # and "the limit was too low for it", and the metadata already knows.
        local est; est=$(test_meta_field "$t" est 2>/dev/null)
        if [[ "$est" =~ ^[0-9]+$ ]]; then
            echo "  $name TIMEOUT ${dur}s (after retry; test declares est=${est}s, limit was ${TIMEOUT}s)"
        else
            echo "  $name TIMEOUT ${dur}s (after retry)"
        fi
    else
        echo "  $name FAIL ${dur}s (after retry)"
    fi
    echo "    --- $name output (last 25 lines) ---"
    tail -n 25 "$LOGDIR/$name.log" 2>/dev/null | sed 's/^/    | /'
    echo "    --- end $name ---"
    return 1
}

# Give every run-qemu invocation a per-run temp base we own (on tmpfs when
# available) and remove it wholesale at exit. run-qemu honors AXL_QEMU_TMPDIR
# for its disk-image/pflash scratch, so this guarantees no per-run temp dir
# leaks regardless of which run-qemu mode a test uses — the background /
# --mount / --cpu-report paths have their own cleanup lifecycles that get
# skipped when the launcher is killed (8.6 GB of such leaks were found
# accumulating). Respect a caller-provided base (don't delete what we didn't
# create).
own_qtmp=0
if [[ -z "${AXL_QEMU_TMPDIR:-}" ]]; then
    qbase=/tmp; [[ -d /dev/shm && -w /dev/shm ]] && qbase=/dev/shm
    AXL_QEMU_TMPDIR=$(mktemp -d -p "$qbase" axl-itest.XXXXXX)
    export AXL_QEMU_TMPDIR
    own_qtmp=1
fi

# Job pool: at most JOBS tests in flight. Each finished test writes its exit
# status (0 = pass) to results/<idx>; we tally after the barrier.
results=$(mktemp -d)

# Cleaning up a signalled run has two halves, and both are needed.
#
# Half 1 -- the workers. They are background jobs, so the runner exiting does
# NOT stop them. Left alive they see their test "fail", hit run_one's
# retry-once path, and relaunch a guest INTO the tree that was just deleted,
# recreating the leak moments after it was cleaned. They cannot be matched by
# command line (a subshell shares this script's argv), so they are tracked by
# PID and killed by descent -- the test script, its `timeout` wrapper and the
# guest all sit below them.
WORKERS=()

# STOP before descending, KILL after. Signalling a worker's child first and the
# worker second loses a race: the retry path sees the child die and relaunches,
# and the replacement appears after the walk has already passed that branch. A
# stopped process cannot fork, which closes the race rather than narrowing it.
# KILL, not TERM, because a STOPped process does not act on TERM until it is
# continued.
_kill_tree() {
    local pid="$1" c
    kill -STOP "$pid" 2>/dev/null
    for c in $(pgrep -P "$pid" 2>/dev/null); do
        _kill_tree "$c"
    done
    kill -KILL "$pid" 2>/dev/null
}

kill_worker_trees() {
    local pid
    for pid in "${WORKERS[@]:-}"; do
        [[ -n "$pid" ]] || continue
        _kill_tree "$pid"
    done
}

# Half 2 -- the guests, reaped BEFORE their disks are removed and never after.
# A guest that outlives its disk keeps it open as a DELETED inode: the
# directory is gone, so every directory-based check reports clean while tmpfs
# cannot reclaim the pages. `du` cannot see it; only a `df` delta can. 365 MB
# accumulated that way from five orphans of a single interrupted run.
#
# This is a belt-and-braces pass for anything the worker walk missed (a guest
# already reparented to init, say). Two filters, and BOTH matter:
#
#   - the run's own temp dir, which every guest carries on its command line via
#     `-drive file=$TMPDIR/...`; and
#   - the process actually being a guest.
#
# The second is not redundant. `pgrep -f` matches any process whose command
# line merely CONTAINS the path, which in practice includes an editor, a
# grep, or an agent shell that happens to mention it -- killing one of those
# would be far worse than the leak. run-qemu.sh's sweeper can use the path
# alone because a false match there means "don't delete" (harmless); here a
# false match would mean "kill it", so the comm check is what keeps this from
# being the blanket `pkill qemu-system` it must never become.
_guest_pids() {
    local base="$1" pid comm
    for pid in $(pgrep -f -- "$base" 2>/dev/null); do
        comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
        case "$comm" in
            qemu-system*|virtiofsd) echo "$pid" ;;
        esac
    done
}

reap_pool() {
    local base="$1" i pids
    [[ -n "$base" ]] || return 0
    pids=$(_guest_pids "$base"); [[ -n "$pids" ]] || return 0
    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null
    # Bounded wait, then escalate. Without the wait, the rm below can still
    # beat a slow guest and recreate the very leak this exists to prevent.
    for ((i = 0; i < 40; i++)); do
        pids=$(_guest_pids "$base"); [[ -n "$pids" ]] || return 0
        sleep 0.25
    done
    # shellcheck disable=SC2086
    kill -KILL $pids 2>/dev/null
    sleep 0.2
}

# Note what is NOT here: kill_worker_trees. On a normal exit the pool has
# already been reaped by `wait`, so those PIDs are stale -- and a stale PID is
# not merely a no-op to signal, it may have been RECYCLED to an unrelated
# process by then (this box wraps its PID counter routinely). Killing by
# remembered PID is only safe on the signal path, where the workers are known
# to still be ours. reap_pool stays: it resolves live processes by path AND
# comm every time, so it has no stale-PID exposure and still catches a guest
# that somehow outlived a clean run.
_cleanup() {
    reap_pool "${AXL_QEMU_TMPDIR:-}"
    rm -rf "$results"
    [[ "$own_qtmp" == 1 ]] && rm -rf "$AXL_QEMU_TMPDIR"
    return 0
}

# One handler for both signals, so the selftest's SIGTERM case exercises the
# identical body the SIGINT path uses. (It cannot drive SIGINT directly: a
# background job of a non-interactive shell starts with SIGINT *ignored*
# -- /proc SigIgn carries bit 2 -- and bash cannot trap a signal ignored on
# entry, so `kill -INT` at the runner is swallowed. A terminal Ctrl-C is a
# different delivery: it reaches the whole foreground process group, where the
# runner is not a background job and this trap does fire.)
#
# The signal handlers exit; the EXIT handler preserves the script's own status.
# Without an explicit exit, bash runs the handler and then RESUMES the `wait`
# below, so a signalled runner would delete the tree and keep running.
_signal_exit() {
    trap - EXIT
    kill_worker_trees
    _cleanup
    exit "$1"
}
trap '_rc=$?; _cleanup; exit $_rc' EXIT
trap '_signal_exit 130' INT
trap '_signal_exit 143' TERM
# HUP and PIPE deliberately get no handler of their own. Measured: bash runs
# the EXIT trap even for untrapped fatal signals, so `... | head -3` (SIGPIPE)
# and a closed terminal (SIGHUP) already clean up through the line above.
# Adding traps for them would look protective while changing nothing but the
# exit status. SIGKILL is the one that truly escapes; run-qemu.sh's stale-dir
# sweeper is the net for it.

idx=0; running=0
for t in "${TESTS[@]}"; do
    ( run_one "$idx" "$t"; echo $? > "$results/$idx" ) &
    WORKERS+=($!)
    idx=$((idx+1)); running=$((running+1))
    if [[ $running -ge $JOBS ]]; then
        wait -n 2>/dev/null || true
        running=$((running-1))
    fi
done
wait

pass=0; failc=0
for f in "$results"/*; do
    if [[ "$(cat "$f")" == "0" ]]; then pass=$((pass+1)); else failc=$((failc+1)); fi
done

echo ""
_nskip=$(wc -l < "$LOGDIR/_skipped.txt" 2>/dev/null | tr -d ' ' || echo 0)
[[ -n "$_nskip" ]] || _nskip=0
# Skips are reported in the headline, not buried. A suite that skipped a third
# of itself and said "N passed" is the same failure shape as a gate that cannot
# see the change it was run for.
if [[ "$_nskip" -gt 0 ]]; then
    echo "integration: $pass passed, $failc failed, $_nskip SKIPPED ($ARCH)"
    sed 's/^/    skipped: /' "$LOGDIR/_skipped.txt"
else
    echo "integration: $pass passed, $failc failed ($ARCH)"
fi

# WALL TIME, WORKERS, and the work that was actually done. Without these the
# suite's cost is invisible and every conversation about it is a guess: the
# honest shape is ~47 minutes of test work packed into $JOBS workers, so the
# floor is (summed work / JOBS), not the slowest test. Printing all three shows
# which one you are up against, and whether the cache is doing its job.
#
# JOBS defaults to nproc-2 deliberately -- these are timing-sensitive QEMU
# guests, and a saturated host makes them flake. That is a correctness choice,
# not a tuning oversight; raise it only for a run whose failures you are
# prepared to re-triage.
_wall=$(( SECONDS - _RUN_T0 ))
_work=$(awk '{ s += $1 } END { print s + 0 }' "$LOGDIR/_durations.txt" 2>/dev/null || echo 0)
printf 'integration: %ss wall, %s worker(s), %ss of test work' \
       "$_wall" "$JOBS" "$_work"
if [[ "$_work" -gt 0 && "$_wall" -gt 0 ]]; then
    printf ' (%sx packing)' "$(( (_work + _wall / 2) / _wall ))"
fi
echo ""
# A filtered run NEVER reports as though it were a full one. Same rule
# verify.sh --only follows, and for the same reason: the failure this tree
# keeps meeting is a gate that could not see the change reporting the same
# green as one that could. AXL-CI-Release-Speed-Design.md §12.8.
_nc=$(wc -l < "$LOGDIR/_cached.txt" 2>/dev/null | tr -d ' ' || echo 0)
[[ -n "$_nc" ]] || _nc=0
# Only when something was ACTUALLY skipped. Printed on every run -- which is
# what a default-on cache would do -- it stops carrying information, and this
# tree already has the rule that a banner which always fires is not a banner.
# NOT "re-run with --no-cache for a pre-push gate", which this said until
# 2026-09-02. ci.yml runs the full uncached suite on EVERY push to main, on a
# self-hosted runner on this same machine, so a --no-cache run immediately
# before pushing is the same tests twice on the same hardware -- several
# minutes of duplicated wall time that also contend with each other.
# --no-cache belongs to a RELEASE cut (docs/RELEASING.md), where the tagged
# commit must be certified locally before the tag exists.
if [[ -n "$CACHE_DIR" && "$_nc" -gt 0 ]]; then
    echo "integration: PARTIAL -- cache: $_nc test(s) SKIPPED as unchanged" \
         "since their last green run. Inputs only; the host environment is not" \
         "in the key. Pushing? CI re-runs the whole suite uncached anyway --" \
         "--no-cache here is for cutting a release."
fi
if [[ $ONLY_LOCAL -eq 1 ]]; then
    echo "integration: PARTIAL -- only-local: ran the $(printf %d "${#TESTS[@]}") test(s) CI cannot," \
         "SKIPPED everything CI does run. Not a pre-push gate."
elif [[ -z "$INCLUDE_LOCAL" ]]; then
    echo "integration: PARTIAL -- --ci: local-only tests were SKIPPED."
fi
[[ -n "$SHARD" ]] && echo "integration: PARTIAL -- shard $SHARD of the suite only."
echo "logs: $LOGDIR"

# Stamp a clean, COMPLETE run so a release can skip dispatching CI for an
# answer this run already has (AXL-CI-Release-Speed-Design.md §10.3). Only a
# full run counts: a --shard or a filtered run did not test everything, so it
# must not be able to satisfy a release gate.
#
# --only-local and --ci are filtered runs by construction and are excluded for
# exactly that reason. --ci was NOT excluded before and could write the stamp
# having skipped every local-only test -- the same defect this flag would have
# introduced, one flag earlier.
if [[ $failc -eq 0 && -z "$SHARD" && $ONLY_LOCAL -eq 0 && -n "$INCLUDE_LOCAL" \
      && -z "$CACHE_DIR" ]]; then
    # shellcheck source=lib/release-gate.sh
    source "$SCRIPT_DIR/lib/release-gate.sh"
    release_gate_write "$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null)" \
                       "$ARCH" "$pass" "$failc"
fi

[[ $failc -eq 0 ]]
