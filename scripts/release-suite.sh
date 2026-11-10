#!/bin/bash
# release-suite.sh -- what certifies a release, as ONE list of steps.
#
# CI's "QEMU integration (full suite)" job runs this with --ci, and a local
# release run runs it bare. Only a COMPLETE success of every step, on a clean
# tree, writes the sha-bound stamp the release gate reads
# (test/integration/lib/release-gate.sh). AXL-CI-Release-Speed-Design.md §14.
#
# WHY ONE SCRIPT. The local release stamp used to come from run-integration.sh
# alone, while CI's job also ran the unit tests in QEMU, staged the SDK for both
# arches and ran the harness self-tests -- so the local path the gate accepts
# in place of a CI run certified LESS than that run (review ruling R-A). Two
# lists of steps drift; one cannot.
#
# Steps, in CI's order, fail-fast:
#   1 unit         verify.sh --only=x64 -- the unit tests in QEMU, selected as
#                  verify selects them: when the axlpool service serves x64,
#                  every unit binary its own CTest test on a leased guest
#                  (no boot), with the ratchet and budget summed over them;
#                  else, booted, test-axl.sh's one boot. One selection for
#                  both, owner decision 3 (docs/AXL-Build-System-Design.md
#                  §10.12) -- the planned two-guest one-boot split was dropped
#   2 build        axl-build.sh ARCH=x64 all tests tools axl-busybox, ONCE, so
#                  the per-test builds the suite invokes are no-ops. Through
#                  the wrapper, so it builds the tree every test then reads
#   3 sdk          install.sh --arch all --cpp. --cpp because two C++ tests need
#                  the staged glue objects; BOTH arches because
#                  test-host-deps-minimal.sh builds x64 AND aa64 through the
#                  staged axl-cc -- a dev box stages `--arch all`, and an
#                  x64-only CI stage once failed only there
#   4 integration  the integration tests through CTest (slice 5a step 6,
#                  docs/AXL-Build-System-Design.md §10.8), on a CPU budget
#                  of suite_cpu_slots (lib/discover.sh) that each test
#                  draws its cpu= from, ordered by the measured times kept
#                  across runs (cost_seed, below). In CI (--ci):
#                  scripts/check-test-est.py first -- a test the limit cannot
#                  hold is refused before any test runs -- then
#                  `ctest --preset x64-ci` (240 s per test, local-only tests
#                  excluded, cache off: the preset's, not this script's).
#                  Locally: `ctest --preset x64-integration` with the cache
#                  off (every test, local-only included).
#                  Then THE QUIET SLOT: a timing test the suite's own load
#                  starved reads UNJUDGED (lib/run-one.sh) -- a 77 to ctest,
#                  which would pass this step on a bound nobody judged. Each
#                  is re-run ALONE (`ctest -R`, its own JUnit report), as
#                  verify's qloop runs last and alone, and only a JUDGED
#                  result counts: a PASS certifies, a FAIL fails, and still
#                  UNJUDGED alone stops the run with exit 3 and no stamp --
#                  in --ci too, because the dispatched suite IS the release
#                  certificate (docs/RELEASING.md, "UNJUDGED")
#   5 selftests    run-selftests.sh -- the harness self-tests, OUTSIDE the pool and LAST:
#                  inside the suite's pool the runner self-test's SIGTERM case
#                  left two qemu processes and 108 MB of /dev/shm behind, and
#                  the suite went 121 passed / 59 failed (§13.13)
#
# Before any step it REFUSES (exit 2) when find_debug_ovmf finds no DEBUG
# OVMF -- always the case on a hosted runner -- saying why and what to do.
#
# Usage:
#   scripts/release-suite.sh              # a local release run; stamps on success
#   scripts/release-suite.sh --ci         # CI's integration job
#   scripts/release-suite.sh --step NAME  # one step, for debugging; never stamps
#
# There is deliberately no --timeout: CI's per-test limit lives once, in the
# x64-ci test preset (CMakePresets.json), and `gate.check-test-est` reads this
# script and its caller in ci.yml to hold that true.
set -uo pipefail

ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

# A test that needs a DEBUG OVMF and finds none SKIPs, and a skip passes the
# suite -- so a release could be certified by a run that never ran it
# (test-stderr-stale-qemu.sh is the only guard on writing through a freed
# Shell handle). Here a missing DEBUG OVMF is a FAILURE instead; point
# AXL_OVMF_DEBUG_DIR at one (find_debug_ovmf, scripts/axl-common.sh).
export AXL_REQUIRE_DEBUG_OVMF=1

CI=0
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ci)   CI=1 ;;
        --step)
            [[ $# -ge 2 ]] || { echo "release-suite: --step needs a name" >&2; exit 2; }
            ONLY="$2"; shift ;;
        *)      echo "release-suite: unknown argument '$1'" >&2
                echo "usage: $0 [--ci] [--step unit|build|sdk|integration|selftests]" >&2
                exit 2 ;;
    esac
    shift
done

# NO DEBUG OVMF, NO CERTIFICATION -- refused up front, not hours in. The
# integration step's DEBUG-OVMF tests FAIL without one (AXL_REQUIRE_DEBUG_OVMF
# above), and a hosted runner (ci.yml's `runner: hosted` fallback) has none:
# building EDK2 there is too costly for a fallback, so that run refuses with
# the way out instead of failing late or certifying a skip.
if [[ -z "$ONLY" || "$ONLY" == integration ]]; then
    # shellcheck source=axl-common.sh
    source "$ROOT/scripts/axl-common.sh"
    if ! find_debug_ovmf; then
        echo "release-suite: refusing to certify -- no DEBUG OVMF -- $DEBUG_OVMF_SEARCHED"
        echo "release-suite: the integration tests that need one (test-stderr-stale-qemu.sh among them) would FAIL without it, and a skip of them would certify nothing."
        if [[ $CI -eq 1 ]]; then
            echo "release-suite: run CI on the self-hosted runner (workflow_dispatch input runner: self-hosted), or set AXL_OVMF_DEBUG_DIR to a DEBUG OVMF build."
        else
            echo "release-suite: set AXL_OVMF_DEBUG_DIR to a DEBUG OVMF build (docs/DEBUGGING.md)."
        fi
        exit 2
    fi
fi

# The pinned CMake and ctest (scripts/axl-cmake-pin.sh) for every step; on CI
# ci.yml put it first on PATH already, and this does nothing.
# shellcheck source=axl-cmake-pin.sh
source "$ROOT/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 2
# cts_tree_current: before ctest reads build/x64-debug, a registration another
# cmake wrote (a `--step integration` on a tree the box's cmake configured) is
# redone by the pinned one. Only then: the build step configures it otherwise.
# shellcheck source=../test/integration/lib/ctest-session.sh
source "$ROOT/test/integration/lib/ctest-session.sh"
CTS_WHO=release-suite

STEPS=(unit build sdk integration selftests)
declare -A LABEL=([unit]="unit tests in QEMU" [build]="build" [sdk]="stage the SDK"
                  [integration]="integration suite" [selftests]="harness self-tests")

# THE UNIT RATCHET GETS A COPY. The unit ratchet (unit-ratchet.sh pooled,
# test-axl.sh's one boot booted) raises the TRACKED
# test/integration/.last-pass-count on a green run whose count rose, which
# dirtied the tree and failed the end-of-run check on every commit that added
# unit assertions -- locally and in every dispatched CI run. It is handed a
# temp copy of the COMMITTED baseline instead: the ratchet still enforces the
# committed value, and a rise becomes a NOTE to commit, not a failure.
BASELINE=test/integration/.last-pass-count
run_unit() {
    local tmp committed now rc
    tmp=$(mktemp) || return 1
    committed=$(git show "HEAD:$BASELINE" 2>/dev/null) || committed=""
    if [[ -n "$committed" ]]; then printf '%s\n' "$committed" > "$tmp"; else rm -f "$tmp"; fi
    # CI's runner budget is CI's: a local certifying run judges the default.
    local unit_budget=""
    [[ $CI -eq 1 ]] && unit_budget="${TEST_MAX_DURATION:-}"
    # The pooled guests run THIS tree's agent, as the suite presets' do
    # (AXL_IT_POOL_AGENT=tree), or a commit that changes the agent is
    # certified by units that never ran it. CI's budget for its slower runner
    # (ci.yml's TEST_MAX_DURATION) is DECLARED to verify, which scrubs an
    # inherited one; and in CI the log is all anyone sees, so a failing
    # test's output is printed there.
    TEST_BASELINE_FILE="$tmp" AXL_VERIFY_POOL_AGENT=tree \
        AXL_VERIFY_UNIT_BUDGET="$unit_budget" AXL_VERIFY_PRINT_FAILURES=1 \
        ./scripts/verify.sh --only=x64
    rc=$?
    now=$(cat "$tmp" 2>/dev/null)
    rm -f "$tmp"
    if [[ $rc -eq 0 && -n "$committed" && -n "$now" && "$now" != "$committed" ]]; then
        echo "release-suite: NOTE -- the unit-test count rose $committed -> $now; commit the new baseline ($BASELINE)"
    fi
    return "$rc"
}

# suite_cpu_slots: the suite's -j, beside the cpu= it budgets.
# shellcheck source=../test/integration/lib/discover.sh
source "$ROOT/test/integration/lib/discover.sh"

# THE MEASURED TEST TIMES OUTLIVE THE CHECKOUT. ctest orders a parallel run
# costliest-first by the averages it keeps in the tree's CTestCostData.txt,
# and only falls back to a test's est= (the registration's COST) for a test
# that file does not name. CI's workspace is recreated for every run, so the
# file was always absent there and the order was est='s -- which sums to 1.9x
# the real work and started two mis-ranked tests late. So it is kept outside
# the workspace, one file per preset, seeded into a tree that has none and
# written back after the run whatever its verdict (a failing run's times are
# still times; ctest also starts last run's failures first). The STORE WINS
# over a tree's own file: ctest rewrites that file after every run, -j1 and
# cache hits included (a cache hit is a ~0 s pass; at -j1 ctest writes back
# elapsed/(runs+1)), so a developer tree's record drifts low, while the store
# holds only this script's cache-off suite runs. A tree's own file is used
# only when there is no store yet. The store keeps the whole file, unit.* and
# gate.* lines included; ctest ignores lines for tests a run does not have.
# AXL_CI_COST_DIR moves the store (the self-test's seam).
COST_TREE="build/x64-debug/Testing/Temporary/CTestCostData.txt"   # x64-ci's and x64-integration's tree
cost_store() { printf '%s/ctest-cost-%s.txt' "${AXL_CI_COST_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/axl-ci}" "$1"; }
cost_seed() {
    local c; c=$(cost_store "$1")
    if [[ -s "$c" ]] && mkdir -p "${COST_TREE%/*}" && cp "$c" "$COST_TREE"; then
        echo "release-suite: ordering by the measured times kept in $c"
    elif [[ -e "$COST_TREE" ]]; then
        echo "release-suite: no measured test times kept yet ($c) -- ordering by the tree's own ($COST_TREE)"
    else
        echo "release-suite: no measured test times yet ($c) -- ordering by est="
    fi
}
cost_save() {
    local c tmp; c=$(cost_store "$1")
    [[ -s "$COST_TREE" ]] || return 0
    # Written beside the store and RENAMED, so a concurrent run never seeds
    # from a half-written file. Failing to keep it costs the next run its
    # order, not this run its verdict: said, not fatal.
    if mkdir -p "${c%/*}" && tmp=$(mktemp "$c.XXXXXX") && cp "$COST_TREE" "$tmp" && mv -f "$tmp" "$c"; then
        echo "release-suite: kept the measured test times in $c"
    else
        rm -f "${tmp:-}"
        echo "release-suite: WARNING -- could not keep the measured test times in $c"
    fi
}

# quiet_rerun -- after a green suite, re-run each UNJUDGED test alone
# (see the header, step 4). 0 none, or every one a judged PASS alone (its
# own line PASS under ctest's run status: a lone SKIP judged nothing); 1 a
# re-run failed or read anything else, or a report could not be read (an
# unread report is not "none unjudged"); 3 still UNJUDGED alone, with
# UNJUDGED_NAMES set.
UNJUDGED_NAMES=""
quiet_rerun() {
    local names n left="" again re rc qj=ctest-junit-quiet.xml
    names=$(python3 scripts/ctest-report.py unjudged --junit build/x64-debug/ctest-junit.xml) || {
        echo "release-suite: cannot read the suite's JUnit report (build/x64-debug/ctest-junit.xml) -- nothing certified"
        return 1; }
    for n in $names; do
        echo "release-suite: $n was UNJUDGED under the suite's load -- re-running it alone (the quiet slot)"
        re="^integration\\.${n%.sh}\$"
        # Under the suite's own preset, graded as the suite was (the est gate
        # reads every x64-ci call in this file). The last re-run's report is
        # removed first: it must never stand as this one's.
        rm -f "build/x64-debug/$qj"
        if [[ $CI -eq 1 ]]; then
            ./scripts/check-test-est.py && ctest --preset x64-ci -j1 -R "$re" --output-junit "$qj"
        else
            AXL_IT_CACHE=0 ctest --preset x64-integration -j1 -R "$re" --output-junit "$qj"
        fi
        rc=$?
        if (( rc != 0 )); then
            echo "release-suite: $n alone FAILED (ctest exit $rc) -- nothing certified"
            return 1
        fi
        again=$(python3 scripts/ctest-report.py verdict --junit "build/x64-debug/$qj" --test "$n" 2>&1)
        case "$again" in
            PASS)     ;;
            UNJUDGED) left+="${left:+, }$n" ;;
            *)        echo "release-suite: $n alone read ${again:-nothing}, not a judged PASS -- nothing certified"
                      return 1 ;;
        esac
    done
    UNJUDGED_NAMES="$left"
    [[ -z "$left" ]] || return 3
}

# unjudged_say [WHERE] -- the one line naming what stayed UNJUDGED alone.
unjudged_say() {
    local its=its
    [[ "$UNJUDGED_NAMES" == *,* ]] && its=their
    echo "release-suite: UNJUDGED ${1:-}-- $UNJUDGED_NAMES could not judge $its timing bounds even run alone (the host starved $its guest of CPU); no stamp written. Re-run on a quiet box."
}

run_step() {
    case "$1" in
        unit)        run_unit ;;
        build)       ./scripts/axl-build.sh -j"$(nproc)" ARCH=x64 all tests tools axl-busybox ;;
        sdk)         ./scripts/install.sh --arch all --cpp ;;
        integration)
            # -j IS A CPU BUDGET, NOT A WORKER COUNT. Each test holds the
            # slots its test-meta cpu= names (CTest's PROCESSORS; 1 for a
            # guest or a host script), so a compiling test takes the CPUs its
            # compilers use and nothing else has to guess. It replaced
            # `-j nproc+4 --test-load nproc-2`: ctest reads the ONE-minute
            # load average, which lags a burst by about a minute, so after
            # each one the gate stayed shut while workers idled (3,165-4,016
            # idle worker-seconds per CI run, concurrency swinging 16 -> 3-9
            # -> 16), and it counted every other process on the box -- an
            # agent's build made CI slower. docs/AXL-CI-Release-Speed-Design.md
            # §15 has the measurement behind the budget.
            local jobs preset=x64-integration rc
            jobs=$(suite_cpu_slots)
            [[ $CI -eq 1 ]] && preset=x64-ci
            cost_seed "$preset"
            # The ONLY difference between the two modes, and the one CI needs.
            # The JUnit report is asked for by name in both (x64-integration
            # writes none of its own), and an earlier run's is removed first:
            # the quiet slot below reads it, and a stale one must never
            # stand as this run's.
            if axl_cmake_tree_foreign build/x64-debug; then
                cts_tree_current "$ROOT/build/x64-debug" x64-debug "$ROOT" \
                    build/release-suite-configure.log || return 2
            fi
            rm -f build/x64-debug/ctest-junit.xml
            if [[ $CI -eq 1 ]]; then
                ./scripts/check-test-est.py && ctest --preset x64-ci -j"$jobs" --output-junit ctest-junit.xml
            else
                AXL_IT_CACHE=0 ctest --preset x64-integration -j"$jobs" --output-junit ctest-junit.xml
            fi
            rc=$?
            cost_save "$preset"
            (( rc == 0 )) || return "$rc"
            quiet_rerun ;;
        selftests)   ./scripts/run-selftests.sh ;;
    esac
}

if [[ -n "$ONLY" ]]; then
    [[ -n "${LABEL[$ONLY]:-}" ]] || {
        echo "release-suite: unknown step '$ONLY' (one of: ${STEPS[*]})" >&2; exit 2; }
    run_step "$ONLY"; rc=$?
    (( rc == 3 )) && [[ -n "$UNJUDGED_NAMES" ]] && unjudged_say
    echo "release-suite: PARTIAL -- ran step '$ONLY' only; this certifies nothing"
    exit "$rc"
fi

# A STAMP NAMES A COMMIT, so the tree must BE that commit -- before, and still
# after. Untracked files are fine (the build makes plenty); a tracked change is
# code the stamp would certify without it being in the commit it names.
#
# dirty: 0 dirty, 1 clean, 2 COULD NOT CHECK. A git that fails prints nothing,
# the same empty porcelain as a clean tree and the opposite fact.
dirty() {
    local out
    out=$(git status --porcelain --untracked-files=no 2>/dev/null) || return 2
    [[ -n "$out" ]]
}
dirty; case $? in
    0)  echo "release-suite: refusing -- the tree has tracked changes, and a stamp names a commit, not a working tree"
        exit 2 ;;
    2)  echo "release-suite: could not check the tree (git status failed) -- refusing"
        exit 2 ;;
esac
SHA=$(git rev-parse HEAD) || { echo "release-suite: cannot read HEAD" >&2; exit 2; }

# shellcheck source=../test/integration/lib/release-gate.sh
source "$ROOT/test/integration/lib/release-gate.sh"
# A RUN ON HEAD RETIRES HEAD'S OLD STAMP. Otherwise a failed re-run leaves the
# last success's stamp standing for the same commit, and the gate reads it as
# though nothing had happened since. A stamp for another commit is left alone.
if [[ -f "$AXL_STAMP_FILE" && "$(sed -n 's/^sha=//p' "$AXL_STAMP_FILE")" == "$SHA" ]]; then
    rm -f "$AXL_STAMP_FILE"
fi

n=${#STEPS[@]} i=0
for s in "${STEPS[@]}"; do
    i=$((i + 1))
    echo "=== release-suite: step $i/$n -- ${LABEL[$s]} ==="
    run_step "$s"; rc=$?
    if (( rc == 3 )) && [[ -n "$UNJUDGED_NAMES" ]]; then
        unjudged_say "at step $i/$n (${LABEL[$s]}) "
        exit 3
    fi
    if (( rc != 0 )); then
        echo "release-suite: FAILED at step $i/$n (${LABEL[$s]}) -- no stamp written"
        exit 1
    fi
done

dirty; drc=$?
if [[ $drc -ne 1 || "$(git rev-parse HEAD)" != "$SHA" ]]; then
    if [[ $drc -eq 2 ]]; then
        echo "release-suite: every step passed, but the tree could not be checked afterwards (git status failed) -- no stamp written"
    else
        echo "release-suite: every step passed, but the tree moved off $SHA during the run -- no stamp written"
        # NAME what moved: "the tree moved" alone sent people hunting.
        git status --short --untracked-files=no
    fi
    exit 1
fi
release_gate_write "$SHA" X64
echo "release-suite: PASSED -- all $n steps on $SHA; stamped $AXL_STAMP_FILE"
