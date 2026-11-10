#!/bin/bash
# suite-prep.sh -- prepare the tree for a set of integration tests, ONCE,
# before any of them runs: build the shared targets, restage the SDK when a
# selected test reads it, pre-build every selected test's own fixtures
# (builds=), refuse a stale staged SDK, and list what was built in the
# manifest scripts/axl-build.sh reads to skip a test's identical build call.
#
# One copy, two callers: run-integration.sh before its pool starts, and the
# CTest build fixture (docs/AXL-Build-System-Design.md §10.8). Moved here
# verbatim from the runner, so its history explains the code below.
#
# Usage: lib/suite-prep.sh TEST_PATH...
#
# Configuration, from the environment:
#   AXL_PREBUILT_MANIFEST  required -- the manifest to (re)write
#   AXL_PREP_ARCH          x64 (default) | aa64 -- axl-build.sh's spelling
#   AXL_PREP_LOGDIR        where _prebuild.log / _stage.log go (default:
#                          the manifest's directory)
#   RUN_NO_BUILD=1         the shared targets were built elsewhere (CI)
#   AXL_PREP_SKIP_LOCAL=1  the run excludes local-only tests (the x64-ci test
#                          preset sets it): their fixtures are not built
#   AXL_PREP_TESTS         the scripts the run SELECTED, one per line, from
#                          a front end: only their
#                          fixtures are built, instead of every registered
#                          test's. Set but EMPTY: no integration test was
#                          selected (a unit-only run) -- the shared targets,
#                          no fixture, no restage
#   RUN_INTEGRATION_DIR, RUN_PROJECT_DIR  the runner self-test's seams
#
# Exit: 0 prepared; 1 a build, the staging, or the stale-SDK check failed;
# 2 it could not ask the build what it needed to.
set -uo pipefail

_SP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=discover.sh
source "$_SP_DIR/discover.sh"

TESTS=("$@")
# A front end knows the selection, which ctest cannot hand a fixture.
if [[ -n "${AXL_PREP_TESTS+set}" ]]; then
    mapfile -t TESTS < <(printf '%s\n' "$AXL_PREP_TESTS" | sed '/^$/d')
fi
# CTest hands the prep EVERY registered test -- it cannot hand a fixture the
# selection -- so a run that excludes local-only tests says so, and their
# fixtures are not built: one of them breaking would otherwise fail every test
# of a CI run over a test CI never runs.
if [[ "${AXL_PREP_SKIP_LOCAL:-0}" == 1 ]]; then
    mapfile -t TESTS < <(discover_drop_local_only ${TESTS[@]+"${TESTS[@]}"})
fi
: "${AXL_PREBUILT_MANIFEST:?suite-prep.sh: AXL_PREBUILT_MANIFEST must name the manifest}"
MAKE_ARCH="${AXL_PREP_ARCH:-x64}"
LOGDIR="${AXL_PREP_LOGDIR:-$(dirname "$AXL_PREBUILT_MANIFEST")}"
PROJECT_DIR="${RUN_PROJECT_DIR:-$(cd "$_SP_DIR/../../.." && pwd)}"
mkdir -p "$LOGDIR"
# A run starts its tallies empty. The runner makes them itself; under
# CTest the log directory persists in the build tree, and run-one.sh only
# appends, so without this they would grow across runs.
: > "$LOGDIR/_durations.txt"; : > "$LOGDIR/_skipped.txt"; : > "$LOGDIR/_cached.txt"

# What the builds below made, for scripts/axl-build.sh to skip a test's own
# identical build of (see its "PRE-BUILT BY THE RUNNER"). A line is added
# only once its build SUCCEEDED; --no-build adds none for the shared targets.
: > "$AXL_PREBUILT_MANIFEST"
# Goals that leave no reusable artifact (a report) never go in it: the
# project's NON_ARTIFACT_GOALS (scripts/axl-lists.conf; the CMake build
# refuses to configure if one names no target), and every print-* goal.
# Read with THIS tree's reader from the project's file, so a stub project (the
# runner self-test's) needs only the list.
# An answer that could not be had is not "none": refuse, or every report
# goal would be skipped as pre-built, silently.
if ! _NON_ARTIFACT=" $(AXL_LISTS_FILE="$PROJECT_DIR/scripts/axl-lists.conf" \
        "$_SP_DIR/../../../scripts/axl-list.sh" NON_ARTIFACT_GOALS 2>&1) "; then
    echo "run-integration: cannot read the project's non-artifact goals:$_NON_ARTIFACT" >&2
    exit 2
fi
_artifacts() {   # _artifacts TARGET... -> the ones a later call may reuse
    local t out=()
    for t in "$@"; do
        [[ "$t" == print-* || "$_NON_ARTIFACT" == *" $t "* ]] || out+=("$t")
    done
    printf '%s\n' "${out[*]:-}"
}

# Build ONCE up front (real mode only; the self-test seam uses stubs that need
# no build). This populates the shared prefix before the pool starts, so the
# per-test builds that follow are consistent-flag no-ops instead of a thundering
# herd of concurrent first-builds. --no-build skips it (CI builds in a prior job).
if [[ -z "${RUN_INTEGRATION_DIR:-}" && "${RUN_NO_BUILD:-0}" != "1" ]]; then
    echo "pre-building once (ARCH=$MAKE_ARCH $TEST_PREBUILT_TARGETS)..."
    # shellcheck disable=SC2086
    if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$MAKE_ARCH" $TEST_PREBUILT_TARGETS \
            > "$LOGDIR/_prebuild.log" 2>&1; then
        echo "pre-build FAILED — see $LOGDIR/_prebuild.log"; exit 1
    fi
    # shellcheck disable=SC2086
    printf '%s||%s|%s\n' "$MAKE_ARCH" "$(_artifacts $TEST_PREBUILT_TARGETS)" \
        "$("$PROJECT_DIR/scripts/axl-build.sh" --env-fingerprint ARCH="$MAKE_ARCH" $TEST_PREBUILT_TARGETS)" \
        >> "$AXL_PREBUILT_MANIFEST"
    # ...AND THE STAGED SDK. Every test that links through the staged
    # axl-cc / axl-c++ measures the staged libaxl.a and glue objects, and
    # nothing above refreshes them: the header check below catches a stale
    # PUBLIC header, but a src/-only edit left every such test grading the
    # previous build, green (found 2026-11-02 while writing
    # test-heap-release-qemu.sh). Restaging is install.sh's job, under its own
    # build lock and with its own environment, so it is run rather than
    # imitated; when nothing changed it rewrites nothing (install -C).
    # Real tree only: RUN_PROJECT_DIR is the runner self-test's stub tree,
    # which has no SDK to stage. And only when a SELECTED test reads the
    # stage -- it costs ~14 s even when nothing changed, which a one-test
    # --only run should not pay. A test reaches the shared stage through
    # test_sdk_dir (common-test.sh) or scripts/sdk-prefix.sh; tests that run
    # install.sh themselves stage into a private --prefix and are not users.
    # Two runners on ONE tree can still race here: install.sh locks its build
    # but not its copies into the stage. That belongs in install.sh.
    _stage_users=""
    if (( ${#TESTS[@]} )); then   # no files: grep would read stdin
        _stage_users=$(grep -lE 'test_sdk_dir|sdk-prefix\.sh' "${TESTS[@]}" \
                           2>/dev/null || true)
    fi
    if [[ -z "${RUN_PROJECT_DIR:-}" && -n "$_stage_users" ]]; then
        echo "restaging the SDK for $(grep -c . <<< "$_stage_users") test(s)" \
             "that read it (install.sh --arch all --cpp)..."
        if ! "$PROJECT_DIR/scripts/install.sh" --arch all --cpp \
                > "$LOGDIR/_stage.log" 2>&1; then
            echo "staging FAILED — see $LOGDIR/_stage.log"; exit 1
        fi
    fi
fi

# ...AND THE FIXTURES THE SELECTED TESTS BUILD THEMSELVES, which the line
# above does not reach. 81 make targets across 79 tests are in that position, and leaving
# them to the tests cost two distinct defects:
#
#   THE CACHE GOES BLIND. cache_is_fresh (run_one, below) hashes the STAGED
#   ARTIFACTS a test recorded on its last green run. All five ssh tests
#   record exactly three inputs -- two firmware blobs and
#   ssh-server-selftest.efi -- and libaxl.a is in NO key at all. So after a
#   change to src/net/axl-ssh-*.c the line above rebuilds the library, the
#   fixture is NOT rebuilt because only the test rebuilds it, all three
#   inputs hash the same, and every ssh test reports CACHED. It does not
#   self-heal: a test that is skipped never rebuilds the artifact that would
#   miss the key next time. Measured 2026-09-26 -- a sabotage of
#   axl-ssh-server.c ran green through the pre-push gate, untested.
#
#   AND IT NARROWS A RACE IT DOES NOT OWN. Four of five aa64 ssh tests once
#   failed attempt 1 with "ssh-server-selftest.so is empty". That was first
#   read here as two workers colliding on one `ld -o`, and that reading was
#   WRONG: the cause was in the (since retired) Makefile, whose build-state
#   signature was hashed from a file every concurrent make rewrote at parse
#   time, so one make read it truncated and wiped the whole tree mid-link.
#   What this pre-build does for it is real but partial -- it removes the
#   case where five workers rebuild libaxl.a and the fixture at once -- and
#   it is NOT why that race is
#   closed. Two builds in one tree at once are now serialised where every
#   build goes through (scripts/axl-build.sh, test-axl-build-lock.sh).
#
# NOT gated on --no-build. That flag says the shared build happened in a
# prior job, and CI's prior job builds exactly $TEST_PREBUILT_TARGETS -- the
# fixtures are precisely what it does NOT build, so skipping here would leave
# CI building them under the pool. An already-built fixture makes this a
# no-op.
# Grouped by the builds= variables, one build call per group.
_fixture_targets() {   # echo "VARS|TARGETS" groups, one per line
    local t p vars tgts key
    declare -A groups=()
    for t in "${TESTS[@]}"; do
        vars=""; tgts=""
        while IFS= read -r p; do
            [[ -n "$p" ]] || continue
            if [[ "$p" == *=* ]]; then vars+=" $p"; else tgts+=" $p"; fi
        done < <(test_meta_builds "$t")
        [[ -n "$tgts" ]] || continue
        # A leading space keeps the no-variables group from indexing the
        # associative array with the empty string, which bash rejects
        # outright ("bad array subscript") -- and that group is the common
        # case, so the failure would be immediate and total.
        key=" ${vars# }"
        groups["$key"]="${groups[$key]:-}$tgts"
    done
    for key in "${!groups[@]}"; do
        # shellcheck disable=SC2086
        printf '%s|%s\n' "${key# }" "$(printf '%s\n' ${groups[$key]} | sort -u | paste -sd' ' -)"
    done
}
while IFS='|' read -r _fx_vars _fx_tgts; do
    [[ -n "$_fx_tgts" ]] || continue
    # The stub seam runs STUB tests whose builds= name stub targets. Without
    # RUN_PROJECT_DIR those builds would land in the REAL tree, which is not
    # what a self-test asked for and not something it could notice.
    [[ -n "${RUN_INTEGRATION_DIR:-}" && -z "${RUN_PROJECT_DIR:-}" ]] && continue
    _fx_arch="$MAKE_ARCH"; _fx_pass=()
    for _fx_w in $_fx_vars; do
        if [[ "$_fx_w" == ARCH=* ]]; then _fx_arch="${_fx_w#ARCH=}"; else _fx_pass+=("$_fx_w"); fi
    done
    echo "pre-building fixtures (ARCH=$_fx_arch ${_fx_pass[*]:+${_fx_pass[*]} }$_fx_tgts)..."
    # shellcheck disable=SC2086
    if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_fx_arch" \
            ${_fx_pass[@]+"${_fx_pass[@]}"} $_fx_tgts \
            >> "$LOGDIR/_prebuild.log" 2>&1; then
        echo "fixture pre-build FAILED (ARCH=$_fx_arch $_fx_tgts) — see $LOGDIR/_prebuild.log"
        tail -12 "$LOGDIR/_prebuild.log" | sed 's/^/    /'
        exit 1
    fi
    # The variables exactly as passed, sorted,
    # so a test's identical call -- and only that -- matches.
    printf '%s|%s|%s\n' "$_fx_arch" \
        "$(printf '%s\n' ${_fx_pass[@]+"${_fx_pass[@]}"} | sed '/^$/d' | sort | paste -sd' ' -)" \
        "$(_artifacts $_fx_tgts)|$("$PROJECT_DIR/scripts/axl-build.sh" --env-fingerprint \
            ARCH="$_fx_arch" ${_fx_pass[@]+"${_fx_pass[@]}"} $_fx_tgts)" \
        >> "$AXL_PREBUILT_MANIFEST"
done < <(_fixture_targets)

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
if [[ -z "${RUN_INTEGRATION_DIR:-}" \
      && -d "$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)/include/axl-sdk/axl" ]]; then
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
