#!/bin/bash
# run-one.sh -- run ONE integration test the way the suite does: the skip
# cache, a pooled run with its booted fallback, the per-test timeout, the
# SKIP (77) convention, and the one-line verdict.
#
# One copy, two callers, both with --retry: run-integration.sh's worker pool
# runs every test through it, and so does the CTest registration of the
# integration tests (docs/AXL-Build-System-Design.md §10.8). The retry is HERE
# and not ctest's `--repeat`, which repeats every test -- the unit ratchet and
# gate.qloop too, whose retries erased real failures. Moved here verbatim from
# the runner, so its history explains the code below.
#
# Usage: lib/run-one.sh [--retry] TEST_PATH
#
# Configuration, from the environment (the runner exports these):
#   AXL_ONE_LOGDIR        required -- <name>.log and the run's tally files go here
#   AXL_ONE_ARCH          X64 (default) | AARCH64. NOT `ARCH`: make reads ARCH
#                     from the environment, and an exported ARCH=X64 would
#                     name a build prefix nobody builds.
#   AXL_TEST_TIMEOUT  per-test limit in seconds (default 900)
#   AXL_TEST_CACHE    the skip cache's directory; unset = no cache
#   AXL_ONE_POOL_SOCK     the pool service's socket; unset = every test boots
#   AXL_ONE_PROJECT_DIR   the tree (default: this checkout)
#
# Exit: 0 PASS or CACHED, 77 SKIP or UNJUDGED, anything else FAIL (124 when
# the limit killed it, 128 + N when signal N stopped the run -- SIGNALS below).
#
# UNJUDGED: a test that exits 77 having printed a line `UNJUDGED: <why>` ran,
# and every check it failed was a timing bound missed while the host starved
# its guest of CPU (test-net-latency-qemu.sh; the rule is
# scripts/qloop-verdict.sh's). That is no verdict, so unlike a SKIP it is
# RETRIED like a failure and the retry's verdict stands; it reads UNJUDGED
# only when no attempt judged. Never cached. To ctest it is the 77 skip, so
# nothing goes red; ctest-report.py tells it from a skip by the line below.
set -uo pipefail

_RO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# A CHILD SUBREAPER (lib/subreaper.py), so the runner line's cpu= sees the
# test's whole tree: a `run-qemu.sh --background` guest's watchdog outlives the
# run-qemu.sh that started it, and would otherwise be reparented to init with
# its QEMU's CPU. Marked by an ARGUMENT, not the environment: an exported mark
# would reach a run-one.sh the test itself runs and skip it there.
if [[ "${1:-}" == --subreaped ]]; then
    shift
else
    # PYTHONCOERCECLOCALE=0: python3 would otherwise set LC_CTYPE=C.UTF-8 in
    # a C/unset locale and exec would pass it to every test (subreaper.py
    # drops the variable again). -P -s: nothing from lib/ or the user site
    # shadows its imports. Not -I: it implies -E, which ignores the opt-out.
    PYTHONCOERCECLOCALE=0 exec python3 -P -s "$_RO_DIR/subreaper.py" \
        "$BASH" "${BASH_SOURCE[0]}" --subreaped "$@"
fi
# SIGNALS. A TERM (ctest's TIMEOUT backstop sends one to this process, and
# only to it), INT or HUP is passed on at once to the attempt in flight -- its
# `timeout`, which forwards it to the test's whole group, or the lease client
# -- and ends the run with a line that says so: no retry, no booted rerun,
# nothing cached (lib/fwd-signal.sh says why it has to be passed on).
# shellcheck source=fwd-signal.sh
source "$_RO_DIR/fwd-signal.sh" || { echo "run-one.sh: cannot load fwd-signal.sh" >&2; exit 2; }
fwd_signal_init
# shellcheck source=discover.sh
source "$_RO_DIR/discover.sh"
# shellcheck source=pool-or-boot.sh
source "$_RO_DIR/pool-or-boot.sh"
# shellcheck source=test-cache.sh
source "$_RO_DIR/test-cache.sh"

RETRY=0
if [[ "${1:-}" == --retry ]]; then RETRY=1; shift; fi
if [[ $# -ne 1 ]]; then
    echo "usage: run-one.sh [--retry] TEST_PATH" >&2
    exit 2
fi
LOGDIR="${AXL_ONE_LOGDIR:?run-one.sh: AXL_ONE_LOGDIR must name the log directory}"
# A shell variable, never exported: cache_key reads it, make must not.
ARCH="${AXL_ONE_ARCH:-X64}"
# ...but the cache's FILE names carry the arch, and the test process records
# its inputs into them: it learns the arch from this (lib/test-cache.sh).
export AXL_TEST_CACHE_ARCH="$ARCH"
TIMEOUT="${AXL_TEST_TIMEOUT:-900}"
CACHE_DIR="${AXL_TEST_CACHE:-}"
POOL_SOCK="${AXL_ONE_POOL_SOCK:-}"
PROJECT_DIR="${AXL_ONE_PROJECT_DIR:-$(cd "$_RO_DIR/../../.." && pwd)}"
mkdir -p "$LOGDIR"

# A test's attempt-1 and pool-miss logs are this call's alone: --retry and a
# pool miss write them only when they happen, so one left from an earlier run
# (the log directory can outlive a run) would read as this run's.
rm -f "$LOGDIR/$(basename "$1").attempt1.log" "$LOGDIR/$(basename "$1").pool-miss.log"

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

# The runner's TIMING line (format: the "Wall-time accounting" block of
# scripts/axl-common.sh), appended to the log after every run
# of the test -- the pooled attempt too, before a miss copies its log aside.
# wall is the whole run as this script saw it: a pooled one's lease and
# release included, which the test cannot see from inside.
ATTEMPT=0
_ro_now() {   # microseconds; no fork on bash 5
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        printf -v "$1" '%s' "${EPOCHREALTIME//[!0-9]/}"
    else
        printf -v "$1" '%s' "$(date +%s%6N)"
    fi
}
# _ro_cpu VAR: the CPU (user + system, ms) of every child this shell has
# REAPED so far -- the test, and through it each QEMU, build and tool it
# waited for -- from /proc/$$/stat's cutime + cstime (fields 16-17, after the
# parenthesised comm). No fork. Orphans are reaped here too (the subreaper
# above) -- counted in the attempt during which they END, so one that
# outlives a failed first attempt lands in the retry's figure. What it cannot
# see: a process still running when the last line is written, and a pooled
# guest, whose QEMU is the pool's.
_RO_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)
_ro_cpu() {   # VAR: ms, or -1 when /proc cannot say (then cpu=- is printed)
    local st f
    printf -v "$1" '%s' -1
    read -r st < "/proc/$$/stat" 2>/dev/null || return 0
    read -r -a f <<< "${st##*) }"
    [[ ${#f[@]} -gt 14 ]] || return 0
    printf -v "$1" '%s' $(( (f[13] + f[14]) * 1000 / _RO_TCK ))
}
_ro_timing() {   # <name> <via> <start us> <rc> <log> <cpu ms at start>
    local now ms cpu
    _ro_now now
    _ro_cpu cpu
    ms=$(( (now - $3) / 1000 ))
    if [[ $cpu -lt 0 || $6 -lt 0 || $cpu -lt $6 ]]; then
        cpu=-
    else
        cpu=$(( cpu - $6 ))
        printf -v cpu '%d.%03d' $(( cpu / 1000 )) $(( cpu % 1000 ))
    fi
    printf 'TIMING: runner test=%s attempt=%d via=%s wall=%d.%03d cpu=%s rc=%s\n' \
        "$1" "$ATTEMPT" "$2" $(( ms / 1000 )) $(( ms % 1000 )) "$cpu" "$4" >> "$5"
}

# launch_test <test_path> <name> -- run it once into $LOGDIR/<name>.log; its
# status. With --pool, a guest= test runs on a pooled guest under `axlpool
# run`; a POOL failure there (77: a miss, the service gone) is not the test's
# verdict, so it is run booted instead -- the pool's log kept beside it as
# <name>.pool-miss.log, and the reason said. Never a quiet SKIP: that would
# drop the test's coverage whenever the pool hiccupped.
launch_test() {
    local t="$1" name="$2" g="" rc t0 c0
    ATTEMPT=$(( ATTEMPT + 1 ))
    # Every TIMING line a test's processes write lands in its own log: the
    # log is truncated once and then opened O_APPEND by everything, so a line
    # appended by path (run-qemu.sh's, from inside a captured $(...)) goes
    # between the test's output rather than under its file offset.
    export AXL_TEST_TIMING_LOG="$LOGDIR/$name.log"
    _ro_now t0; _ro_cpu c0
    [[ -n "$POOL_SOCK" && "${AXL_TEST_CACHE_MODE:-}" == pool ]] && g=$(test_meta_pool_class "$t")
    if [[ -n "$g" ]]; then
        local abs; abs="$(cd "$(dirname "$t")" && pwd)/$(basename "$t")"
        # The pooled attempt and its miss test are lib/pool-or-boot.sh's (one
        # copy, shared with the pooled unit.<App> registration): a 77 from
        # the TEST is its own SKIP, a POOL failure is marked AXLPOOL-MISS:.
        pool_attempt "$POOL_SOCK" "$PROJECT_DIR" "$g" "$ARCH" "$LOGDIR/$name.log" "$TIMEOUT" \
            bash "$abs" --arch "$ARCH"
        rc=$?
        _ro_timing "$name" pool "$t0" "$rc" "$LOGDIR/$name.log" "$c0"
        [[ -z "$POOL_MISS" || -n "$FWD_SIGNAL" ]] && return $rc
        cp -f "$LOGDIR/$name.log" "$LOGDIR/$name.pool-miss.log"
        echo "  $name: pool miss ($POOL_MISS); running it booted"
        # From here it is a BOOTED run: its inputs and its cache key are
        # those, and a retry stays booted rather than missing again.
        export AXL_TEST_CACHE_MODE=
        [[ -n "$CACHE_DIR" ]] && cache_begin_inputs "$name"
        _ro_now t0; _ro_cpu c0
    fi
    : > "$LOGDIR/$name.log"
    fwd_run timeout "$TIMEOUT" bash "$t" --arch "$ARCH" >> "$LOGDIR/$name.log" 2>&1
    rc=$?
    _ro_timing "$name" boot "$t0" "$rc" "$LOGDIR/$name.log" "$c0"
    return $rc
}

# unjudged_reason <log> -- the test's `UNJUDGED: <why>` line (the first), or
# nothing.
unjudged_reason() {
    grep -m1 -aE '^UNJUDGED: ' "$1" 2>/dev/null | tr -d '\r' | cut -c1-120 || true
}

# report_unjudged <name> <start> <reason-line> <prefix> -- the UNJUDGED verdict
# line. Never cached: the cache remembers green runs only.
report_unjudged() {
    local dur=$(( SECONDS - $2 ))
    _prof_test_total "$1" "$dur"
    [[ -n "$CACHE_DIR" ]] && cache_invalidate "$1"
    echo "  $1 UNJUDGED ${dur}s (${4}${3})"
}

# print_excerpt <name> -- the end of a failed test's output, inline: logs
# live in an ephemeral dir that CI discards. Tail keeps the job log readable
# when many tests fail at once.
print_excerpt() {
    echo "    --- $1 output (last 25 lines) ---"
    tail -n 25 "$LOGDIR/$1.log" 2>/dev/null | sed 's/^/    | /'
    echo "    --- end $1 ---"
}

# end_orphans -- what a stopped attempt left running, ended before this
# script exits. They are this script's children by now: it is their
# subreaper (top of file), so whatever outlived its parent -- a background
# guest's run-qemu.sh owner and watchdog, caught mid-launch by the TERM --
# was reparented HERE. Exiting with them alive would hand them on to init,
# out of the tree ctest's KILL reaches -- the KILL that, before this script
# acted on the TERM, took them down with it. A TERM to each, every second,
# then 10 s on a KILL to what is left -- re-read until nothing is: a KILLed
# orphan's own children (a watchdog's QEMU) are reparented here in turn.
_ro_children() {   # VAR: this shell's children, space-separated
    # A builtin read, not $(cat ...): the subshell that would fork is a
    # child of this shell too, and would be listed.
    local _k=""
    { read -r -d '' _k < "/proc/$$/task/$$/children"; } 2>/dev/null
    printf -v "$1" '%s' "${_k//[[:space:]]/ }"
}
end_orphans() {
    local kids killed="" i
    for (( i = 0; i < 50; i++ )); do
        _ro_children kids
        [[ -n "${kids// /}" ]] || return 0
        # shellcheck disable=SC2086  # a list of PIDs
        (( i % 5 == 0 )) && kill -TERM $kids 2>/dev/null
        sleep 0.2
    done
    for (( i = 0; i < 25; i++ )); do
        _ro_children kids
        [[ -n "${kids// /}" ]] || break
        killed+=" $kids"
        # shellcheck disable=SC2086
        kill -KILL $kids 2>/dev/null
        sleep 0.2
    done
    [[ -n "$killed" ]] && echo "    (killed what the stopped run left behind after 10 s: PIDs$killed)"
    return 0
}

# report_stopped <name> <start> -- a run a signal ended (ctest's TIMEOUT
# backstop, or a front end taking the run down) is no verdict on the test:
# said, its output shown, never cached. Its status is the signal's, 128 + N.
report_stopped() {
    local dur
    end_orphans
    dur=$(( SECONDS - $2 ))
    _prof_test_total "$1" "$dur"
    [[ -n "$CACHE_DIR" ]] && cache_invalidate "$1"
    echo "  $1 FAIL ${dur}s (stopped by SIG$FWD_SIGNAL from outside -- not its own ${TIMEOUT}s limit, and not a verdict)"
    print_excerpt "$1"
    fwd_signal_status
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
    # Pooled or booted decides which cache this test's run belongs to.
    export AXL_TEST_CACHE_MODE=
    if [[ -n "$POOL_SOCK" && -n "$(test_meta_pool_class "$t")" ]]; then
        export AXL_TEST_CACHE_MODE=pool
    fi
    if [[ -n "$CACHE_DIR" ]] && cache_is_fresh "$name"; then
        dur=$(( SECONDS - start ))
        echo "  $name CACHED ${dur}s (inputs identical to its last green run)"
        echo "$name" >> "$LOGDIR/_cached.txt"
        return 0
    fi
    # A real run rewrites the input list from scratch, so a test that stages
    # FEWER things than last time does not inherit stale entries.
    [[ -n "$CACHE_DIR" ]] && cache_begin_inputs "$name"
    launch_test "$t" "$name"
    rc=$?
    [[ -n "$FWD_SIGNAL" ]] && { report_stopped "$name" "$start"; return; }
    local uj1=""
    [[ $rc -eq 77 ]] && uj1=$(unjudged_reason "$LOGDIR/$name.log")
    # 77 = SKIPPED (the automake convention). Handled BEFORE the retry, because
    # a test that cannot run will not run any better the second time, and
    # before the cache, because the reason it skipped -- an absent corpus, a
    # missing host tool -- is not in the key, so committing one would skip it
    # forever afterwards.
    if [[ $rc -eq 77 && -z "$uj1" ]]; then
        dur=$(( SECONDS - start ))
        _prof_test_total "$name" "$dur"
        [[ -n "$CACHE_DIR" ]] && cache_invalidate "$name"
        echo "$name" >> "$LOGDIR/_skipped.txt"
        echo "  $name SKIP ${dur}s ($(grep -m1 -oE 'SKIP[:( ].{0,60}' "$LOGDIR/$name.log" 2>/dev/null || echo 'declined to run'))"
        return 77
    fi
    if [[ -n "$uj1" && $RETRY -eq 0 ]]; then
        report_unjudged "$name" "$start" "$uj1" ""
        return 77
    fi
    if [[ $rc -ne 0 && $RETRY -eq 1 ]]; then
        # Retry once. Most failures under the parallel pool are transient
        # resource-contention flakes (a QEMU starved during boot emits empty
        # serial output -> the test's greps miss). A genuine failure fails
        # again. Preserve the first attempt's log for post-mortem and let the
        # host drain briefly before the retry so the contention can ease.
        cp -f "$LOGDIR/$name.log" "$LOGDIR/$name.attempt1.log" 2>/dev/null
        fwd_run sleep 3
        [[ -n "$FWD_SIGNAL" ]] && { report_stopped "$name" "$start"; return; }
        launch_test "$t" "$name"
        rc=$?
        [[ -n "$FWD_SIGNAL" ]] && { report_stopped "$name" "$start"; return; }
        if [[ $rc -eq 0 ]]; then
            dur=$(( SECONDS - start ))
            _prof_test_total "$name" "$dur"
            [[ -n "$CACHE_DIR" ]] && cache_commit "$name"
            if [[ -n "$uj1" ]]; then
                echo "  $name PASS ${dur}s (retry; attempt 1 UNJUDGED -- see $name.attempt1.log)"
            else
                echo "  $name PASS ${dur}s (retry; attempt 1 failed — see $name.attempt1.log)"
            fi
            return 0
        fi
        # Neither attempt judged -- the retry UNJUDGED too, or a plain skip
        # -- is UNJUDGED. A judged FAIL in either stands (below), and so
        # does attempt 1's FAIL over a retry that skipped.
        if [[ -n "$uj1" && $rc -eq 77 ]]; then
            local uj2; uj2=$(unjudged_reason "$LOGDIR/$name.log")
            if [[ -n "$uj2" ]]; then
                report_unjudged "$name" "$start" "$uj2" "after retry; "
            else
                report_unjudged "$name" "$start" "$uj1" "after retry, which skipped; "
            fi
            return 77
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
    # Failure/timeout (twice): replay the test's captured output.
    if [[ $rc -eq 124 ]]; then
        # Name the declared est: a TIMEOUT is ambiguous between "the test hung"
        # and "the limit was too low for it", and the metadata already knows.
        local est; est=$(test_meta_field "$t" est 2>/dev/null)
        if [[ "$est" =~ ^[0-9]+$ ]]; then
            # A variable, not $(...): command substitution strips the trailing
            # space "after retry; " needs.
            _to_pre=" ("; ((RETRY)) && _to_pre=" (after retry; "
            echo "  $name TIMEOUT ${dur}s${_to_pre}test declares est=${est}s, limit was ${TIMEOUT}s)"
        else
            echo "  $name TIMEOUT ${dur}s$( ((RETRY)) && echo ' (after retry)')"
        fi
    else
        echo "  $name FAIL ${dur}s$( ((RETRY)) && echo ' (after retry)')"
    fi
    print_excerpt "$name"
    [[ $rc -eq 124 ]] && return 124
    return 1
}

run_one 0 "$1"
