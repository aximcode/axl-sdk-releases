#!/bin/bash
# run-integration.sh — the integration suite's front end over CTest.
#
# It CHOOSES and REFUSES; CTest RUNS (slice 5a step 6, decision 1;
# docs/AXL-Build-System-Design.md §10.8). Selection is lib/discover.sh's, as
# it always was -- --arch, --only, --for, --ci, --only-local, --shard -- and
# every refusal below stands. The run itself is ONE ctest call on the arch's
# tree (build/<arch>-debug), whose registration runs each test through
# lib/run-one.sh --retry (cache, pool rerun, the retry). Its JUnit file is
# rendered by scripts/ctest-report.py.
#
# Usage:
#   run-integration.sh [--arch X64|AARCH64] [--timeout SECONDS] [-jN] [--list]
#                      (--arch also takes x64/x86_64 and aa64/aarch64, any
#                      case; anything else is refused)
#                      [--only a,b | --for paths] [--ci | --only-local]
#                      [--shard i/K] [--logdir DIR] [--no-build] [--no-cache]
#                      [--pool[=SOCK] | --no-pool] [--print-ctest]
#
# Concurrency: -j is a CPU budget. Each test holds the slots its test-meta
# cpu= names (CTest's PROCESSORS, default 1), and with no -j the budget is
# suite_cpu_slots (lib/discover.sh) -- more slots than CPUs, because a booted
# guest keeps about half a CPU busy. -jN is a budget of exactly N. There is no
# load-average gate: ctest's --test-load reads the one-minute load, which lags
# a burst by a minute and counts every other process on the box
# (docs/AXL-CI-Release-Speed-Design.md §15). Do not stack the suite on other
# heavy jobs -- nothing here backs off for them.
#
# --print-ctest prints the ctest environment, the selected tests and the
# argv, and runs nothing: the self-tests pin the translation through it.
#
# Output: one `  <name>.sh PASS|FAIL|TIMEOUT|SKIP|UNJUDGED|CACHED <n>s` line per
# test (sorted), the totals line, the PARTIAL banners and `logs: <dir>`. Exit 0
# when nothing failed, 1 when something did, 2 for a refusal, 3 when nothing
# failed but a timing test was UNJUDGED (lib/run-one.sh: its bounds missed
# while the host starved its guest of CPU, even on the retry).
#
# RUN_INTEGRATION_DIR (env) overrides the test directory -- the host-only
# self-test seam (no QEMU). Its tests are DISCOVERED as the real ones are
# (arch, local-only, test-meta), the same selection CTest registers from it.
# RUN_PROJECT_DIR and RUN_TEST_CACHE_DIR override the tree the pre-builds run
# against and where the cache lives, so that seam can drive the fixture
# pre-build against a stub CMake project without touching the real tree or cache.
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
# axl_test_tmpdir_check, and nothing else of it, is used here.
# shellcheck source=../../scripts/axl-common.sh
source "$SCRIPT_DIR/../../scripts/axl-common.sh"
# shellcheck source=lib/test-cache.sh
source "$SCRIPT_DIR/lib/test-cache.sh"

# THE CI PER-TEST LIMIT IS THE x64-ci TEST PRESET'S, read back from it -- not
# a copy. It was a literal in ci.yml that nothing compared with anything, then
# a copy here that a test held equal to the preset. CI runs that preset;
# `gate.check-test-est` grades against it; --ci applies it. One number.
ci_limit() {
    jq -er '.testPresets[] | select(.name == "x64-ci") | .environment.AXL_IT_TIMEOUT' \
        "$SCRIPT_DIR/../../CMakePresets.json" 2>/dev/null
}
ARCH="X64"; TIMEOUT=900; TIMEOUT_SET=0; LIST_ONLY=0; JOBS=0; SHARD=""; LOGDIR=""  # JOBS=0 => auto
PRINT_CTEST=0
CI_MODE=0
ONLY_TESTS=""   # --only: an EXPLICIT subset, named by the caller
FOR_PATHS=""    # --for: the same subset, derived from paths by which-gates.sh
# SEEN is tracked apart from the VALUE. `--only=` and `--only="$V"` with V
# unset are the same string, and the second is how it happens -- an empty
# value used to read as "flag absent" and select all 204 tests, so a run
# driven by an unset variable ran the WHOLE SUITE believing it had scoped.
# Silently widening is the failure this file is arranged against, and it was
# reachable through its own selection flags.
ONLY_SEEN=0; FOR_SEEN=0
# Local-only tests need a capability the GitHub runners lack (a patched QEMU
# for the SMBus/SPD memdev device, usb-mouse pointer delivery, ...). The dev
# box HAS those, so the local run includes them by default; --ci drops them.
INCLUDE_LOCAL="--include-local-only"
ONLY_LOCAL=0
# ON BY DEFAULT. An opt-in flag does not get typed: this cache was built and
# then not used once in the session that built it, while the full uncached gate
# was run after every change. `--no-cache` turns it off, and it is what a
# release run uses -- scripts/release-suite.sh passes it; the stamp is that
# script's now, not this one's (AXL-CI-Release-Speed-Design.md §12.16, §14).
# $SCRIPT_DIR, not a repo-relative path: the runner is invoked from wherever
# the caller happens to be, and a relative default resolved against THAT --
# test-runner-selftest.sh cd's into test/integration and created
# test/integration/test/integration/.test-cache. One cache per checkout,
# wherever it is run from.
CACHE_DIR="${RUN_TEST_CACHE_DIR:-$SCRIPT_DIR/.test-cache}"
POOL_SOCK=""
POOL_ASKED=0 NO_POOL=0 POOL_AUTO=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)    ARCH="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; TIMEOUT_SET=1; shift 2 ;;
        --list)    LIST_ONLY=1; shift ;;
        --shard)   SHARD="$2"; shift 2 ;;   # i/K (0-based)
        # --only=a,b — run exactly these, named by the caller.
        #
        # EXPLICIT, AND THE RUNNER DOES NOT CHOOSE. §12.5 is the reason:
        # `8af4e530` touched src/log/ and the Makefile, every relevance map
        # picks "the logging tests", and the real blast radius was every image
        # in the tree failing at LINK. So this buys the ability to run a subset
        # and no ability at all to decide which. The caller that matters
        # (scripts/ci-plan.sh, via the plan job) is itself fixture-tested and
        # its allowlist is sabotage-verified; the judgement stays there, where
        # it can be tested, instead of moving in here where it cannot.
        --only=*)  ONLY_TESTS="${1#--only=}"; ONLY_SEEN=1; shift ;;
        --only)    ONLY_TESTS="${2:?--only needs a comma-separated list}"; ONLY_SEEN=1; shift 2 ;;
        # --for=a.c,b.sh — the same subset, named by what you CHANGED rather
        # than by which tests cover it. The lookup is which-gates.sh's, asked
        # for machine-readably; see the resolution block below.
        --for=*)   FOR_PATHS="${1#--for=}"; FOR_SEEN=1; shift ;;
        --for)     FOR_PATHS="${2:?--for needs a comma-separated list of paths}"; FOR_SEEN=1; shift 2 ;;
        --logdir)  LOGDIR="$2"; shift 2 ;;  # keep per-test output here
        # EXPORTED: the pre-build is lib/suite-prep.sh, a child process.
        --no-build) export RUN_NO_BUILD=1; shift ;; # skip the one-time pre-build (CI: built in a prior job)
        # --ci also turns the cache OFF. CI is the backstop -- §11.2 records
        # five days of undetected red when it was not run at all -- and a
        # backstop that skips tests is not one. (A fresh checkout has no cache
        # dir anyway, so this is belt and braces, and it is the belt that
        # matters if anyone ever runs --ci on a working tree.)
        #
        # And it sets the per-test limit to the x64-ci preset's, unless --timeout says
        # otherwise (in either order: resolved after the loop).
        --ci)      INCLUDE_LOCAL=""; CACHE_DIR=""; CI_MODE=1; shift ;;
        # The est gate moved to scripts/check-test-est.py (make
        # check-test-est), which grades what CTest will run. Refused by name,
        # not as an unknown option, so an old habit learns where it went.
        --print-ctest) PRINT_CTEST=1; shift ;;
        --check-est)
            echo "run-integration.sh: --check-est moved -- run scripts/check-test-est.py" \
                 "(gate.check-test-est), which grades ctest --preset x64-ci." >&2
            exit 2 ;;
        # The inverse of --ci: ONLY what CI cannot run. For the inner loop,
        # where the other 78% of the wall clock is work a push repeats for
        # free. NOT a substitute for the full run before pushing -- see the
        # banner it prints.
        --only-local) INCLUDE_LOCAL="--only-local"; ONLY_LOCAL=1; shift ;;
        # Accepted and a no-op: caching is the default now, and this spelling
        # exists so an invocation written before that still works.
        --cache)      CACHE_DIR="${RUN_TEST_CACHE_DIR:-$SCRIPT_DIR/.test-cache}"; shift ;;
        # Turn the cache OFF. Required for a run that must certify the tree,
        # which is why scripts/release-suite.sh passes it. lib/test-cache.sh
        # documents what the key does and does not cover -- the host
        # environment is the gap, and it is why this switch exists at all.
        --no-cache)   CACHE_DIR=""; shift ;;
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        # Run guest= tests on a pooled guest (docs/AXL-Test-Guest-Pool-Design.md
        # §6) through the service on SOCK (default: the user unit's). THE
        # DEFAULT without either flag, when that service serves this arch;
        # --pool ASKS for it (the run refuses to start without it), --no-pool
        # boots everything.
        --pool)    POOL_SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/axlpool/pool.sock"; POOL_ASKED=1; shift ;;
        --pool=*)  POOL_SOCK="${1#--pool=}"; POOL_ASKED=1; shift ;;
        --no-pool) NO_POOL=1; shift ;;
        # REFUSE anything else. This arm used to be `*) shift ;;`, which
        # silently ate a typo and then ran the WHOLE suite: measured,
        # `--fro=src/net/axl-ssh-server.c --list` selected all 204 tests with
        # no diagnostic. That is the widening direction -- the same one the
        # empty-value refusals above exist for -- and it is how `--for`
        # itself was swallowed while it was being written. verify.sh has
        # always refused an unknown option; these two runners now agree.
        *)
            echo "run-integration.sh: unknown option '$1'." >&2
            echo "  known: --arch --timeout --list --only --for --shard -j" \
                 "--logdir --no-build --ci --only-local --cache --print-ctest" \
                 "--no-cache --pool[=SOCK] --no-pool" >&2
            exit 2 ;;
    esac
done
# ONE SPELLING PER ARCH from here on. Everything below compares ARCH against
# "X64"/"AARCH64" exactly, so `--arch aa64` -- the build trees' and
# axl-build.sh's spelling -- used to build and run the X64 tree, select a
# set matching neither arch, label it "(aa64)" and miss the pool's agent
# check, exiting 0 (test-run-integration-arch.sh). Normalise or refuse.
case "${ARCH,,}" in
    x64|x86_64)    ARCH=X64 ;;
    aa64|aarch64)  ARCH=AARCH64 ;;
    *)  echo "run-integration: --arch $ARCH: not an arch (X64 or AARCH64; x64, aa64 and aarch64 are accepted too)" >&2
        exit 2 ;;
esac
for _f in "--only:$ONLY_SEEN:$ONLY_TESTS" "--for:$FOR_SEEN:$FOR_PATHS"; do
    _n="${_f%%:*}"; _rest="${_f#*:}"; _seen="${_rest%%:*}"; _val="${_rest#*:}"
    if [[ "$_seen" == "1" && -z "$_val" ]]; then
        echo "run-integration: $_n= was given an EMPTY value. That is a typo" >&2
        echo "                 (or an unset variable), not a request to run the" >&2
        echo "                 whole suite. Drop the flag if that is what you want." >&2
        exit 2
    fi
done
if [[ "$CI_MODE" -eq 1 && "$ARCH" != X64 ]]; then
    echo "run-integration: --ci is CI's x64 run (the x64-ci test preset); there is" >&2
    echo "                 no aa64 CI run to stand in for. Drop --ci for AARCH64." >&2
    exit 2
fi
if [[ "$CI_MODE" -eq 1 && "$TIMEOUT_SET" -eq 0 ]]; then
    TIMEOUT=$(ci_limit) || {
        echo "run-integration: --ci cannot read the x64-ci preset's AXL_IT_TIMEOUT from" >&2
        echo "                 CMakePresets.json (jq), so it has no limit to apply." >&2
        exit 2
    }
fi
# Every test learns the limit it runs under. common-test.sh's _test_killed
# reads it, so a test the limit kills says so and names the limit, instead of
# leaving its last output -- often a wait that the kill cut short -- to read
# as the verdict.
export AXL_TEST_TIMEOUT="$TIMEOUT"
# Wall-clock origin for the totals line at the end. $SECONDS is shell
# uptime, so this is taken once, here, rather than inferred later.
_RUN_T0=$SECONDS

# The CPU budget when -j was not given (see the header). An explicit -jN is
# a budget of exactly N.
[[ "$JOBS" -le 0 ]] && JOBS=$(suite_cpu_slots)

# This used to `export AXL_TLS=1` for every test subprocess, and had to: the
# Makefile WIPED every object whenever the flag toggled, so ~64 plain tests and
# 5 TLS tests sharing one prefix made the wipe fire constantly and -- run
# concurrently -- clobber each other's artifacts mid-run. Forcing one value was
# the fix.
#
# mbedTLS is unconditional now, so there is one configuration, nothing to
# toggle and nothing to standardise. The strip contract that made this safe is
# still enforced, by test-tls-strippable.sh.
# EVERY PATH HANDED ON IS ABSOLUTE: ctest, the registration and the tests
# each run from a directory of their own, and the registration refuses a
# relative one (integration.bad-env). A relative --logdir is the caller's.
_abs() { [[ -z "$1" || "$1" == /* ]] && printf '%s' "$1" || printf '%s/%s' "$PWD" "$1"; }
LOGDIR=$(_abs "$LOGDIR")
[[ -n "$CACHE_DIR" ]] && CACHE_DIR=$(_abs "$CACHE_DIR")
[[ -n "${RUN_TEST_CACHE_DIR:-}" ]] && RUN_TEST_CACHE_DIR=$(_abs "$RUN_TEST_CACHE_DIR")
[[ -n "${RUN_INTEGRATION_DIR:-}" ]] && RUN_INTEGRATION_DIR=$(_abs "$RUN_INTEGRATION_DIR")
[[ -n "${RUN_PROJECT_DIR:-}" ]] && RUN_PROJECT_DIR=$(_abs "$RUN_PROJECT_DIR")
[[ -n "${AXL_QEMU_TMPDIR:-}" ]] && AXL_QEMU_TMPDIR=$(_abs "$AXL_QEMU_TMPDIR")
PROJECT_DIR="${RUN_PROJECT_DIR:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
# THE POOL. --pool: refuse a run that ASKED for it when it cannot serve it,
# rather than quietly booting everything. No flag: the DEFAULT -- the user
# service, when its socket accepts a connection and it serves this arch (the
# pool exists to cut boots and wall time; Mike, 2026-11-06), else booted.
# --no-pool boots. --ci pools as any run does (Mike, 2026-11-07: the runners
# on this host use the pool; a runner with no service boots, said). Every
# case SAYS which, and why. A defaulted run hands ctest AXL_IT_POOL=auto, NOT the
# socket: the registration re-decides at run time and keeps its pool check
# in AUTO mode, so a service that dies mid-run lets the tests boot rather
# than failing every one. lib/pool-check.sh is the one check; a listing or
# --print-ctest asks no service.
if (( POOL_ASKED && NO_POOL )); then
    echo "run-integration: --pool and --no-pool contradict each other" >&2
    exit 2
fi
# (The default's decision is _pool_decide, below, made after the last
# refusal: a refusal comes out alone, before the pool is asked anything.)
MAKE_ARCH=x64; [[ "$ARCH" == "AARCH64" ]] && MAKE_ARCH=aa64


if [[ -n "${RUN_INTEGRATION_DIR:-}" ]]; then
    # The stub directory, discovered by the same rules -- and the same
    # discover.sh -- the registration uses (AXL_DISCOVER_DIR), so the runner
    # and CTest select one set.
    mapfile -t TESTS < <(AXL_DISCOVER_DIR="$RUN_INTEGRATION_DIR" discover_tree_tests --arch "$ARCH" $INCLUDE_LOCAL)
else
    # What the arch's TREE registers (discover_tree_tests: no arch=none tests
    # on AARCH64), or the run would name tests its tree does not have.
    mapfile -t TESTS < <(discover_tree_tests --arch "$ARCH" $INCLUDE_LOCAL)
fi

# --for=<paths>: DERIVE the subset, then hand it to --only.
#
# THE LOOKUP IS ASKED FOR, NOT REIMPLEMENTED. scripts/which-gates.sh already
# answers "which test names this file?", and it answers it by grepping test/
# rather than from a table, precisely because a table drifts when a test is
# renamed. A copy of that grep here would be the second source of truth the
# original was written to avoid -- so this asks it, over --porcelain, and
# never parses its prose.
#
# THREE OUTCOMES, THREE MESSAGES. "the lookup could not RUN", "nothing covers
# these paths" and "something covers them but discovery selects none of it"
# are the same empty list and three different facts; only the first is a bug
# in the tooling and only the last has an obvious next step (run-selftests.sh,
# usually). None of them widens to the suite: a scoping flag that falls back
# to running everything when it is confused is worse than no flag, because the
# confusion is invisible in a green log.
#
# AXL_WHICH_GATES is the self-test seam, like RUN_INTEGRATION_DIR above.
if [[ -n "$FOR_PATHS" ]]; then
    if [[ -n "$ONLY_TESTS" ]]; then
        echo "run-integration: --for and --only both name a selection." >&2
        echo "                 They are two spellings of the same thing; pick one." >&2
        exit 2
    fi
    IFS=',' read -r -a _for_paths <<< "$FOR_PATHS"
    _wg="${AXL_WHICH_GATES:-$PROJECT_DIR/scripts/which-gates.sh}"
    if [[ ! -x "$_wg" ]]; then
        echo "run-integration: --for needs $_wg, which is not executable." >&2
        exit 2
    fi
    _wg_out=$("$_wg" --porcelain --files "${_for_paths[@]}" 2>&1); _wg_rc=$?
    if [[ $_wg_rc -ne 0 ]]; then
        echo "run-integration: --for: the covering-test lookup FAILED (exit $_wg_rc)." >&2
        echo "                 That is not 'no test covers this' -- the question" >&2
        echo "                 was never answered. Its output:" >&2
        printf '%s\n' "$_wg_out" | sed 's/^/                     /' >&2
        exit 2
    fi
    _for_missing=$(sed -n 's/^missing=//p' <<<"$_wg_out")
    if [[ -n "$_for_missing" ]]; then
        echo "run-integration: --for names path(s) that are not in the worktree:" >&2
        printf '%s\n' "$_for_missing" | sed 's/^/                     /' >&2
        echo "                 (a typo'd path covers nothing, which would" >&2
        echo "                  otherwise read as 'no test covers this')" >&2
        exit 2
    fi
    _for_cover=$(sed -n 's/^cover=//p' <<<"$_wg_out")
    # vague= IS THE FOURTH OUTCOME, and consuming only three of them made
    # this lie. which-gates declines to attribute a name matched by more than
    # twelve test files, because a list that broad is not coverage -- and
    # those are the tree's HIGHEST-coverage files (scripts/run-qemu.sh is
    # named by 78 tests, scripts/axl by 36, the Makefile by 23). Reading only
    # cover= turned "too common to attribute" into "no file under test/ names
    # any of those paths", which is false, about exactly the paths where
    # being wrong costs most.
    _for_vague=$(sed -n 's/^vague=//p' <<<"$_wg_out")
    if [[ -n "$_for_vague" ]]; then
        # Worded as which-gates words it, so the two tools describe one fact
        # the same way ("CANNOT ATTRIBUTE ... the name is too common").
        echo "run-integration: --for: CANNOT ATTRIBUTE a covering test for these" >&2
        echo "                 -- the name is too common to grep for, and a list" >&2
        echo "                 that broad would not be coverage:" >&2
        printf '%s\n' "$_for_vague" | sed 's/^/                     /' >&2
        echo "                 Find the real consumer by grepping the SYMBOL or" >&2
        echo "                 STRING you changed, and name it with --only." >&2
    fi
    if [[ -z "$_for_cover" ]]; then
        echo "run-integration: --for=$FOR_PATHS resolved to NO covering test." >&2
        if [[ -z "$_for_vague" ]]; then
            echo "                 No file under test/ names any of those paths." >&2
        fi
        echo "                 Refusing to widen to the ${#TESTS[@]}-test suite:" >&2
        echo "                 run it deliberately if that is what you want." >&2
        exit 2
    fi
    # Intersect with DISCOVERY, which is the authority on what this runner can
    # run -- which-gates deliberately reports lib/*.sh helpers and
    # *-selftest.sh files too, and neither is a discovered test.
    declare -a _for_run=() _for_drop=()
    while IFS= read -r _c; do
        [[ -n "$_c" ]] || continue
        _cb=$(discover_test_basename "$_c"); _hit=""
        for _t in "${TESTS[@]}"; do
            _tb=$(discover_test_basename "$_t")
            if [[ "$_tb" == "$_cb" ]]; then _for_run+=("$_cb"); _hit=1; break; fi
        done
        [[ -n "$_hit" ]] || _for_drop+=("$_c")
    done <<<"$_for_cover"
    if [[ ${#_for_run[@]} -eq 0 ]]; then
        # ONE LINE for the discriminating phrase. Split over two, it cannot
        # be grepped for -- and the assertion that tells this case apart from
        # "nothing covers it" is exactly such a grep.
        echo "run-integration: --for: covering file(s) found, but none is a runnable test (--arch $ARCH):" >&2
        printf '%s\n' "${_for_drop[@]}" | sed 's/^/                     /' >&2
        echo "                 A lib/ helper is not a test; a *-selftest.sh runs" >&2
        echo "                 under scripts/run-selftests.sh, not here. Run those," >&2
        echo "                 or name a real test with --only." >&2
        exit 2
    fi
    # PRINT WHAT WAS CHOSEN, and what was not. A derived selection the human
    # cannot see is a selection they cannot veto.
    echo "for: ${#_for_paths[@]} path(s) -> ${#_for_run[@]} covering test(s)," \
         "derived by $(basename "$_wg") --porcelain:"
    printf '    %s\n' "${_for_run[@]}"
    if (( ${#_for_drop[@]} )); then
        # NAMES THE CAUSES DISCOVERY ACTUALLY HAS, not two of five.
        # lib/discover.sh also drops test-axl.sh/test-all.sh, an arch=
        # mismatch, and local-only=1 under --ci -- so under --ci or
        # --arch AARCH64 a two-cause explanation names a reason that does
        # not apply.
        echo "for: ${#_for_drop[@]} covering file(s) DROPPED --"
        discover_drop_explanation "$ARCH" "${INCLUDE_LOCAL:+, local-only included}" | sed 's/^/     /'
        printf '    %s\n' "${_for_drop[@]}"
    fi
    # A vague path in a selection that PROCEEDS must still be reported, or
    # "2 path(s) -> 1 covering test(s)" reads as both accounted for and the
    # caller believes a green run covered a file nothing in it touched.
    if [[ -n "$_for_vague" ]]; then
        echo "for: and the path(s) above that could not be attributed are NOT covered by"
        echo "     this run -- it is narrower than the ${#_for_paths[@]} path(s) you named."
    fi
    # Hand off to --only, which already refuses a name discovery did not find,
    # already marks the run PARTIAL. (No run of this script writes the release
    # stamp any more; scripts/release-suite.sh does.)
    ONLY_TESTS=$(IFS=,; echo "${_for_run[*]}")
fi

# --only selection, before sharding: a caller naming tests wants those tests,
# not those tests intersected with a shard.
#
# AN UNKNOWN NAME IS FATAL. "The tool ran and matched nothing" and "you asked
# for something that is not here" are the same empty set and opposite facts,
# and only one of them is safe to run to completion: a CI caller passing a
# renamed test would otherwise verify NOTHING and report success. The `.sh` is
# optional because both spellings are what people write.
if [[ -n "$ONLY_TESTS" ]]; then
    declare -a _picked=() _missing=()
    IFS=',' read -r -a _want <<< "$ONLY_TESTS"
    for _w in "${_want[@]}"; do
        [[ -n "$_w" ]] || continue
        _w="${_w##*/}"; _w="${_w%.sh}"
        _found=""
        for _t in "${TESTS[@]}"; do
            _b="$(basename "$_t")"; _b="${_b%.sh}"
            if [[ "$_b" == "$_w" ]]; then _picked+=("$_t"); _found=1; break; fi
        done
        [[ -n "$_found" ]] || _missing+=("$_w")
    done
    if [[ ${#_missing[@]} -gt 0 ]]; then
        echo "run-integration: --only names test(s) that discovery did not find:" >&2
        printf '                 %s\n' "${_missing[@]}" >&2
        echo "                 (--arch $ARCH found ${#TESTS[@]} test(s); see --list)" >&2
        exit 2
    fi
    TESTS=("${_picked[@]}")
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
#
# UNDER CTEST this orders only --list and the printed selection: ctest
# schedules by each test's COST, which the registration sets from the same
# est=, so the run itself starts longest-first all the same.
if [[ ${#TESTS[@]} -gt 1 ]]; then
    mapfile -t TESTS < <(
        for _t in "${TESTS[@]}"; do
            _e=$(test_meta_field "$_t" est 2>/dev/null)
            [[ "$_e" =~ ^[0-9]+$ ]] || _e=20
            printf '%s\t%s\n' "$_e" "$_t"
        done | sort -s -k1,1nr | cut -f2-
    )
fi

# EVERY SELECTED TEST'S est= MUST FIT THE PER-TEST LIMIT. One rule, applied
# three ways:
#   - `gate.check-test-est` FAILS on it (scripts/check-test-est.py, against
#     the x64-ci preset CI now runs).
#   - A `--ci` run that takes CI's limit (no --timeout of its own) REFUSES to
#     start on it, so CI checks itself even on a push that skipped the gate:
#     the invocation that applies the limit also refuses what the limit
#     cannot hold, in seconds instead of after the suite has run.
#   - Any other run WARNS on stderr: advice for a hand-picked --timeout.
#
# THE BAR IS est ITSELF, NOT A MULTIPLE OF IT. This warning used to fire at
# 2 x est, on the premise that "under the parallel pool a test routinely takes
# well over its est". The estimates do not mean that any more: since
# a18c783c the convention is to set est from a POOLED run, x1.3 + 2 for
# headroom, so the pool is already inside the number. Measured across five CI
# runs (6 workers, 2026-09-05 to 09-18), no test with est >= 50 ran past
# 1.02 x its est; the largest overrun anywhere was 2 s. At 2 x, three tests CI
# has passed every time would fail the gate -- test-cpu-spike-qemu.sh (est
# 233, ran 169-170 s), test-host-toolchain-qemu.sh (180, ran 3-4 s) and
# test-consumer-install.sh (150, ran 43-51 s) -- which grades the estimates'
# padding, not the risk.
#
# IT GRADES WHAT A TEST DECLARES, NOT WHAT IT TAKES. An est that understates
# its test is invisible to it -- which is exactly how test-ssh-hostile-qemu.sh
# got killed: est=250 was its unpadded standalone time, against 240. The
# convention is the only defence; set est by it.
#
# Prints one line per selected test the limit cannot hold, including one
# whose est is not a number -- an estimate that cannot be read cannot be said
# to fit.
est_offenders() {
    local t e
    for t in "${TESTS[@]}"; do
        e=$(test_meta_field "$t" est 2>/dev/null)
        if [[ ! "$e" =~ ^[0-9]+$ ]]; then
            printf '%s: est=%s is not a number\n' "$(basename "$t")" "$e"
        elif [[ "$e" -gt "$TIMEOUT" ]]; then
            printf '%s: est=%ss > %ss\n' "$(basename "$t")" "$e" "$TIMEOUT"
        fi
    done
}

# The offender list, as the gate and the --ci refusal both print it.
est_report() {   # $1 = the heading's lead-in
    echo "$1 -- $(printf '%s\n' "$2" | grep -c .) test(s) declare an est= the ${TIMEOUT}s per-test limit cannot hold:"
    printf '%s\n' "$2" | sed 's/^/    /'
    echo "  Split the test, or measure it and correct its est (pooled time x1.3 + 2)."
    echo "  Raising the limit is a CI decision: AXL_IT_TIMEOUT in the x64-ci test preset (CMakePresets.json)."
}

if [[ ${#TESTS[@]} -gt 0 ]]; then
    _off=$(est_offenders)
    if [[ -n "$_off" && "$CI_MODE" -eq 1 && "$TIMEOUT_SET" -eq 0 ]]; then
        est_report "run-integration: --ci REFUSES to start" "$_off" >&2
        exit 2
    elif [[ -n "$_off" ]]; then
        echo "warning: the ${TIMEOUT}s per-test limit cannot hold:" >&2
        printf '%s\n' "$_off" | sed 's/^/           /' >&2
        echo "         Those tests will be killed at the limit, not failed by a check." >&2
    fi
fi

if [[ $LIST_ONLY -eq 1 ]]; then
    [[ ${#TESTS[@]} -gt 0 ]] && printf '%s\n' "${TESTS[@]}"
    exit 0
fi

# A caller's AXL_QEMU_TMPDIR is used as-is (see own_qtmp below), and tests make
# UNIX sockets under it. Check it ONCE, before the pre-build: too long, and
# every test's own check would refuse it one by one, which reads as a broken
# suite rather than one wrong setting.
if [[ -n "${AXL_QEMU_TMPDIR:-}" ]]; then
    axl_test_tmpdir_check "$AXL_QEMU_TMPDIR" || exit 1
fi

# THE POOL DECISION, for a run that did not ask (see "THE POOL" above).
# _pool_decide QUERY -- QUERY 0 (--print-ctest): ask no service; a defaulted
# run hands ctest AXL_IT_POOL=auto and the registration decides at run time.
# QUERY 1 (a real run): called after EVERY refusal -- the tree-busy check
# and owner drift included -- so a refusal comes out alone, before the pool
# is asked anything; prints one "run-integration: pool..." line. The service
# must ANSWER and serve this arch (lib/pool-check.sh, auto mode): a socket
# that accepts is not enough, the installed unit is socket-activated.
_pool_decide() {
    local query="$1" dsock say=""
    dsock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/axlpool/pool.sock"
    if (( NO_POOL )); then
        say="pool: off (--no-pool) -- guest= tests boot"
    elif (( ! query )); then
        [[ -S "$dsock" ]] && POOL_AUTO=1   # what a real run would hand ctest
    elif [[ ! -S "$dsock" ]]; then
        say="pool: none (no service at $dsock) -- guest= tests boot"
    elif _pool_status=$("$SCRIPT_DIR/lib/pool-check.sh" "$dsock" "$ARCH" "$PROJECT_DIR" \
                            "run-integration" auto) && [[ "$_pool_status" != "pool: unavailable"* ]]; then
        POOL_AUTO=1
        say="pool (default): $_pool_status"
    else
        say="$_pool_status"
    fi
    # Said by _pool_say, after the last refusal; an unexplained failure still
    # says something (every run says which, and why).
    if (( query )) && [[ -z "$say" ]] && (( ! POOL_AUTO )); then
        say="pool: unavailable (lib/pool-check.sh gave no reason) -- guest= tests boot"
    fi
    POOL_SAY="$say"
    return 0
}
POOL_SAY=""
_pool_say() { [[ -n "$POOL_SAY" ]] && echo "run-integration: $POOL_SAY"; return 0; }
# Absolute: ctest and its tests run from other directories than the caller.
[[ -n "$POOL_SOCK" && "$POOL_SOCK" != /* ]] && POOL_SOCK="$PWD/$POOL_SOCK"
if (( POOL_ASKED )) && [[ "${LIST_ONLY:-0}" != 1 && "$PRINT_CTEST" != 1 ]]; then
    _pool_status=$("$SCRIPT_DIR/lib/pool-check.sh" "$POOL_SOCK" "$ARCH" "$PROJECT_DIR" \
                       "run-integration: --pool") || exit 2
    POOL_SAY="$_pool_status"   # said after the other refusals (_pool_say)
fi

# ---------------------------------------------------------------------------
# THE RUN IS CTEST'S. Everything above chose what to run and refused what it
# should; from here this only translates that into ONE ctest call on the
# arch's tree, and renders the result. The worker pool, cache decision, retry
# and reaper this script used to carry are CTest's and lib/run-one.sh's.
# ---------------------------------------------------------------------------
# The REAL tree, always: RUN_PROJECT_DIR is the self-test's stub project for
# the prep's builds, handed on to the prep below -- it has no ctest tree.
# -P: the registration names the tree by its real path (CMake's binary
# dir), so a symlinked spelling here would read as owner-record drift.
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
TREE="$REPO_DIR/build/${MAKE_ARCH}-debug"
# The pinned CMake and ctest (scripts/axl-cmake-pin.sh), for the prep, ctest
# and every test it runs; cts_tree_current below redoes a tree another cmake
# configured before ctest reads its registration.
# shellcheck source=../../scripts/axl-cmake-pin.sh
source "$REPO_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 2
# ctest in a session of its own, taken down whole on an interrupt; the tree
# checks; the guest reap by path (lib/ctest-session.sh, shared with verify.sh).
# shellcheck source=lib/ctest-session.sh
source "$SCRIPT_DIR/lib/ctest-session.sh"
CTS_WHO=run-integration
# The tree's owner record (lib/ctest-tmpbase.sh; the registration's
# ${_logs}/.run-owner): computed, not read from the registration, so it can
# be asked BEFORE anything -- a reconfigure included -- touches a tree another
# run is using.
QOWNER="$TREE/integration-logs${RUN_INTEGRATION_DIR:+-stub}/.run-owner"
_print_logdir="${LOGDIR:-<a fresh mktemp -d>}"
[[ $PRINT_CTEST -eq 1 ]] || { [[ -z "$LOGDIR" ]] && LOGDIR=$(mktemp -d); }
_ld="${LOGDIR:-$_print_logdir}"

# WHAT THE RUNNER DOES NOT SET, IT DOES NOT PASS ON. A caller's run settings
# -- its shell's, or an outer ctest run's, whose tests carry them -- would
# override a flag (an inherited AXL_TEST_CACHE re-enables the cache under
# --no-cache; AXL_IT_POOL turns the pool on) or this run's prep. The old
# runner unset AXL_TEST_CACHE and AXL_ONE_POOL_SOCK for the same reason.
# The list is the library's (cts_inherited), shared with verify.sh.
declare -a CT_UNSET=()
mapfile -t CT_UNSET < <(cts_inherited)
declare -a CT_UNSET_ARGS=()
for _v in ${CT_UNSET[@]+"${CT_UNSET[@]}"}; do CT_UNSET_ARGS+=(-u "$_v"); done

# THE ARGV IS BUILT ONCE: --print-ctest prints exactly what would run.
declare -a CT_ENV=() CT_ARGS=()
CT_ENV+=("AXL_IT_LOGDIR=$_ld" "AXL_IT_TIMEOUT=$TIMEOUT")
if [[ -z "$CACHE_DIR" ]]; then
    CT_ENV+=("AXL_IT_CACHE=0")
elif [[ -n "${RUN_TEST_CACHE_DIR:-}" ]]; then
    CT_ENV+=("AXL_IT_CACHE_DIR=$CACHE_DIR")
fi
[[ -n "$POOL_SOCK" ]] && CT_ENV+=("AXL_IT_POOL=$POOL_SOCK")
[[ "${RUN_NO_BUILD:-0}" == 1 ]] && CT_ENV+=("RUN_NO_BUILD=1")
[[ -n "${RUN_INTEGRATION_DIR:-}" ]] && CT_ENV+=("AXL_IT_DIR=$RUN_INTEGRATION_DIR")
[[ -n "${RUN_PROJECT_DIR:-}" ]] && CT_ENV+=("RUN_PROJECT_DIR=$RUN_PROJECT_DIR")
[[ -n "${AXL_QEMU_TMPDIR:-}" ]] && CT_ENV+=("AXL_IT_QEMU_TMPDIR=$AXL_QEMU_TMPDIR")
CT_ARGS=(ctest --test-dir "$TREE" --tests-from-file "$_ld/_selected.txt" -j "$JOBS")
CT_ARGS+=(--no-tests=error --output-junit "$_ld/ctest-junit.xml"
          --test-output-size-passed 65536 --test-output-size-failed 65536)

_names() { local t; for t in ${TESTS[@]+"${TESTS[@]}"}; do echo "integration.$(basename "$t" .sh)"; done; }

if [[ $PRINT_CTEST -eq 1 ]]; then
    (( POOL_ASKED )) || _pool_decide 0
    (( POOL_AUTO )) && CT_ENV+=("AXL_IT_POOL=auto")
    # printf repeats its format for NO arguments too: guard the lists that
    # can be empty, or they print a bare "ctest-unset: " line.
    (( ${#CT_UNSET[@]} )) && printf 'ctest-unset: %s\n' "${CT_UNSET[@]}"
    printf 'ctest-env: %s\n' "${CT_ENV[@]}"
    (( ${#TESTS[@]} )) && printf 'ctest-prep-tests: %s\n' "${TESTS[@]}"
    _names | sed 's/^/ctest-test: /'
    printf 'ctest:'; printf ' %q' "${CT_ARGS[@]}"; echo
    exit 0
fi

# AN EMPTY SELECTION RUNS NOTHING (an empty shard, --only-local on an arch
# with none): never hand ctest an empty --tests-from-file, which ran the
# whole tree.
if (( ${#TESTS[@]} == 0 )); then
    echo "integration: 0 passed, 0 failed ($ARCH)"
    echo "integration: no test was selected -- nothing ran."
    echo "logs: $LOGDIR"
    exit 0
fi

# ONE RUN PER TREE, asked FIRST. The registration's temp-base fixture refuses
# a second concurrent run too, but that shows as every test NOT RUN -- and
# by then a reconfigure here could already have rewritten the tree under the
# live run.
# (lib/ctest-session.sh: the tree helpers shared with verify.sh.)
# The default's service query is made HERE, before the ownership check, so
# it does not widen the window between that check and this run's owner
# record; what it found is said after the last refusal (_pool_say).
(( POOL_ASKED )) || _pool_decide 1
if _owner=$(cts_tree_busy "$QOWNER"); then
    echo "run-integration: another ctest run (pid $_owner) is using $TREE" >&2
    echo "                 (owner record $QOWNER). One run per tree at a time;" >&2
    echo "                 wait for it, or use the other arch's tree." >&2
    exit 2
fi

mkdir -p "$LOGDIR"
cts_tree_current "$TREE" "${MAKE_ARCH}-debug" "$REPO_DIR" "$LOGDIR/_configure.txt" || exit 2

# The temp base, from the setup fixture's own command: where this run's
# guests and disks will live, for the reap below. Its owner record must be
# the one computed above, or the pre-check guarded nothing.
_CT_FULLENV=(${CT_UNSET_ARGS[@]+"${CT_UNSET_ARGS[@]}"} "${CT_ENV[@]}")
if ! cts_tree_base "$TREE" _CT_FULLENV; then
    echo "run-integration: cannot read $TREE's temp-base fixture (integration.tmp-setup)" >&2
    echo "                 -- is the tree's integration registration broken? ctest -N -V says:" >&2
    printf '%s\n' "${CTS_BASE_RAW:-<nothing>}" | sed 's/^/                     /' >&2
    exit 2
fi
QBASE="$CTS_BASE"
if [[ "$CTS_OWNER_REG" != "$QOWNER" ]]; then
    echo "run-integration: the registration's owner record ($CTS_OWNER_REG) is not the one" >&2
    echo "                 this script checked ($QOWNER) -- the two have drifted." >&2
    exit 2
fi

_pool_say
(( POOL_AUTO )) && CT_ENV+=("AXL_IT_POOL=auto")
_names > "$LOGDIR/_selected.txt"
# This run's report and tallies only: a reused --logdir must not lend a run
# that wrote none the previous run's JUnit file.
rm -f "$LOGDIR/ctest-junit.xml"
: > "$LOGDIR/_durations.txt"; : > "$LOGDIR/_skipped.txt"; : > "$LOGDIR/_cached.txt"
# The prep builds the SELECTION's fixtures, not every registered test's.
CT_ENV+=("AXL_PREP_TESTS=$(printf '%s\n' "${TESTS[@]}")")


# An interrupt takes the run down with the library's cts_take_down: it asks
# whether THIS run owns the tree (the owner record names its ctest) BEFORE the
# kill, kills, then -- only if it did -- reaps the guests and removes what the
# cleanup fixture would have (the temp base, never a caller's, and the owner
# record). A run that lost the race to another must not kill that run's
# guests or remove its base, which sits at the same path.
_on_signal() {
    local sig="$1"
    cts_take_down
    echo "run-integration: interrupted (SIG$sig) -- logs: $LOGDIR" >&2
    case "$sig" in INT) exit 130 ;; HUP) exit 129 ;; *) exit 143 ;; esac
}
trap '_on_signal INT' INT
trap '_on_signal TERM' TERM
# A dropped terminal (SSH) is a hangup: without this the run would carry on
# in its own session, guests and all, holding the tree's owner record.
trap '_on_signal HUP' HUP
# Any other way out while ctest runs (an error, `set -u`) takes it down too.
trap 'cts_kill_all' EXIT

_RUN_T0=$SECONDS
printf 'ctest:'; printf ' %q' "${CT_ARGS[@]}"; echo
_CT_FULLENV=(${CT_UNSET_ARGS[@]+"${CT_UNSET_ARGS[@]}"} "${CT_ENV[@]}")   # AXL_PREP_TESTS added since
cts_own run "$QOWNER" "$QBASE" "$CTS_BASE_FOREIGN"
cts_start run "$LOGDIR/_ctest.txt" _CT_FULLENV CT_ARGS
_CT_PID=${CTS_PID[run]:-}
# Progress, as ctest reports each test done -- not silence until the end.
tail -n +1 --pid="$_CT_PID" -F "$LOGDIR/_ctest.txt" 2>/dev/null \
    | grep --line-buffered -E '^ *[0-9]+/[0-9]+ Test +#' &
_TAIL_PID=$!
cts_wait run; _ct_rc=$?
wait "$_TAIL_PID" 2>/dev/null
# A guest that outlived its test, reaped -- only if this run owned the tree:
# ITS setup fixture ran (another run's base sits at the same path).
cts_reap_after "$LOGDIR/ctest-junit.xml" "$QBASE"

echo ""
python3 "$REPO_DIR/scripts/ctest-report.py" runner \
    --junit "$LOGDIR/ctest-junit.xml" --arch "$ARCH" --expect "$LOGDIR/_selected.txt"
_rep_rc=$?
if (( _rep_rc == 2 )); then
    echo "run-integration: ctest left no report it could read (ctest exit $_ct_rc) -- see $LOGDIR/_ctest.txt" >&2
    tail -12 "$LOGDIR/_ctest.txt" | sed 's/^/    /' >&2
    exit 1
fi
# 3: nothing failed, but a test could not judge (UNJUDGED) -- the runner exits
# 3 for it, as verify.sh does for its quiet slot: not red, never green.
failc=$(( _rep_rc == 0 || _rep_rc == 3 ? 0 : 1 ))
# ctest's own verdict, cross-checked: it also fails on a fixture -- a cleanup
# after the tests -- that the per-test lines above cannot show. Said, not
# just counted.
if (( _ct_rc != 0 )); then
    failc=1
    (( _rep_rc == 0 )) && echo "integration: ctest exited $_ct_rc although every test passed -- a fixture? see $LOGDIR/_ctest.txt"
fi

# WALL TIME, CONCURRENCY, and the work actually done -- without these the
# suite's cost is invisible and every conversation about it is a guess.
_wall=$(( SECONDS - _RUN_T0 ))
_work=$(awk '{ s += $1 } END { print s + 0 }' "$LOGDIR/_durations.txt" 2>/dev/null || echo 0)
printf 'integration: %ss wall, -j %s, %ss of test work' "$_wall" "$JOBS" "$_work"
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
# 2026-09-02. A push gate is the covering tests, cached; the whole suite runs
# uncached in CI on a DISPATCH (since 2026-10-21 a push runs only the cheap
# jobs -- AXL-CI-Release-Speed-Design.md §14), and --no-cache here belongs to
# a RELEASE cut (docs/RELEASING.md), where it is one of the two ways to
# certify the tagged commit.
if [[ -n "$CACHE_DIR" && "$_nc" -gt 0 ]]; then
    echo "integration: PARTIAL -- cache: $_nc test(s) SKIPPED as unchanged" \
         "since their last green run. Inputs only; the host environment is not" \
         "in the key. --no-cache is for certifying a release; the uncached" \
         "suite in CI runs on dispatch (gh workflow run ci.yml --ref main)."
fi
if [[ $ONLY_LOCAL -eq 1 ]]; then
    echo "integration: PARTIAL -- only-local: ran the $(printf %d "${#TESTS[@]}") test(s) CI cannot," \
         "SKIPPED everything CI does run. Not a pre-push gate."
elif [[ -z "$INCLUDE_LOCAL" ]]; then
    echo "integration: PARTIAL -- --ci: local-only tests were SKIPPED."
fi
[[ -n "$SHARD" ]] && echo "integration: PARTIAL -- shard $SHARD of the suite only."
if [[ -n "$FOR_PATHS" ]]; then
    # Named for the flag the human TYPED. --for sets ONLY_TESTS internally, so
    # without this branch a --for run reported itself as a --only run and the
    # log gave no way to tell a derived selection from a hand-named one.
    echo "integration: PARTIAL -- --for: ran the $(printf %d "${#TESTS[@]}") test(s) derived from" \
         "$FOR_PATHS, not the suite."
elif [[ -n "$ONLY_TESTS" ]]; then
    echo "integration: PARTIAL -- --only: ran $(printf %d "${#TESTS[@]}") named test(s), not the suite."
fi
echo "logs: $LOGDIR"

# THIS RUN NO LONGER WRITES THE RELEASE STAMP (review ruling R-A,
# AXL-CI-Release-Speed-Design.md §14). It certified the suite alone, while
# CI's integration job -- which the stamp stands in for -- also runs the unit
# tests in QEMU, stages the SDK and runs the harness self-tests. Both now go
# through scripts/release-suite.sh, and only its complete success stamps.
# Said where the stamp USED to be written -- a clean, complete, uncached run --
# because that is exactly when someone expects one.
if [[ $failc -eq 0 && -z "$SHARD" && $ONLY_LOCAL -eq 0 && -n "$INCLUDE_LOCAL" \
      && -z "$CACHE_DIR" && -z "$ONLY_TESTS" ]]; then
    echo "integration: this run does not certify a release -- scripts/release-suite.sh does"
fi

(( failc == 0 )) || exit 1
(( _rep_rc == 3 )) && exit 3
exit 0
