#!/bin/bash
# test-lint-gate-selftest.sh — scripts/lint.sh, the x64-lint CMake preset: it
# refuses to be scoped by file, its coverage guard can SEE, and its plain run
# is the gate verify.sh and CI read.
#
# Its own file, and NOT in run-selftests.sh's tree lane, because it touches
# only build/x64-lint: the lane exists for build/x64-debug, which the runner
# self-test reconfigures and test-gate-scoping-selftest.sh reads. These cases
# were that file's section 2 and made it the slowest member of the lane --
# a COLD x64-lint tree is a full clang-tidy pass, and CI's checkout wipes the
# tree every run -- so the lane, run-selftests.sh's critical path, waited for
# a lint that never needed to wait for it.
#
# NOT SAFE TO RUN CONCURRENTLY WITH verify.sh's lint job, OR WITH ITSELF: two
# Ninja runs in one tree race. run-selftests.sh runs it once, and CI runs that
# after the suite -- not beside a developer's own verify.sh.
#
# WHY A SELF-TEST AND NOT AN INTEGRATION TEST: it checks a GATE, so a defect
# makes other gates lie rather than failing a build. Excluded from discovery
# by name (*-selftest.sh), for the reason run-selftests.sh's header gives.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
REPO="$(cd ../.. && pwd)"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$REPO/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

# COUNTS, not a flag: each block compares $fail against its own entry value.
fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=$((fail + 1)); }
need() {   # need <regex> <text> <what>
    grep -qE -- "$1" <<<"$2" || bad "$3 (expected /$1/)"
}
needi() {  # need, case-insensitively -- for wording we do not want to pin
    grep -qiE -- "$1" <<<"$2" || bad "$3 (expected /$1/i)"
}
nonope() { # nonope <regex> <text> <what>
    grep -qE -- "$1" <<<"$2" && bad "$3 (did NOT expect /$1/)"
}

echo "=== lint gate self-test ==="
echo ""

# lint.sh is a wrapper over the x64-lint CMake preset (cmake/AxlLint.cmake):
# clang-tidy beside every compile, re-checking only what a change can reach.
# Scoping by file existed to dodge a 55s full run and proved less than it
# seemed; it is REFUSED now, and these assert the refusal, that the coverage
# guard can SEE, and that the plain run is the gate verify.sh and CI read.
# TOOLS FIRST, AND A LOUD SKIP RATHER THAN A RED. scripts/run-selftests.sh
# runs inside CI's INTEGRATION job, whose image need not carry cmake, ninja or
# clang-tidy (the lint job is a different job with a different image). The
# self-hosted box has them, so this is where they are really checked -- and
# the skip SAYS what went unchecked.
LINT_TOOLS=1
for _t in cmake ninja; do command -v "$_t" >/dev/null 2>&1 || LINT_TOOLS=0; done
{ command -v clang-tidy-21 || command -v clang-tidy; } >/dev/null 2>&1 || LINT_TOOLS=0
if [[ $LINT_TOOLS -eq 0 ]]; then
    echo "  SKIP: lint.sh -- cmake, ninja and/or clang-tidy are not on PATH, so"
    echo "        NOTHING about the lint gate's refusal, its coverage guard or"
    echo "        its default run was checked."
    echo ""
    echo "lint-gate: SKIPPED (no lint tools)"
    exit 0
fi

# NICED: a cold tree is ~70 s of clang-tidy on every core, every CI run, and it
# runs beside the tree lane, whose own cases hold wall-clock bounds of 2 s
# (gate-scoping's which-gates latency, the runner's --timeout 2). Lower
# priority lets those keep their CPU; the lint only finishes later.
# --- a. a file argument is REFUSED, and nothing prints the gate's summary.
before=$fail
lo=$(cd "$REPO" && timeout 600 nice -n 15 ./scripts/lint.sh src/util/axl-hexdump.c 2>&1); rc=$?
[[ $rc -eq 2 ]] || bad "lint.sh with a file argument exited $rc, expected 2"
needi 'incremental' "$lo" "the refusal does not say why a file list is not needed"
nonope '^lint: clean' "$lo" "a refused lint printed the summary line verify.sh reads"
[[ $fail -eq $before ]] && pass "lint.sh refuses a file argument, and says why"

# --- b. the coverage guard can SEE: a .c under src/ that no CMake target
# lists is in no linted target, and the lint must stop, naming it -- not
# skip it and report clean. A real blind spot, not a hypothetical:
# tools/axl.c was once in exactly that state under the old lint.
before=$fail
probe="$REPO/src/util/axl-unbuilt-lint-probe-$$.c"
# SWEPT, not just trapped: the EXIT trap does not survive SIGKILL, and a
# probe left in src/util/ stops EVERY later lint -- so any stale probe from a
# killed run is removed first. The cleanup is on EXIT, which still runs when
# INT or TERM kills the script; those two only EXIT -- a handler that cleans up
# and does not exit RESUMES the script (scripts/run-selftests.sh checks this).
cleanup_probe() { rm -f "$REPO"/src/util/axl-unbuilt-lint-probe-*.c; }
cleanup_probe
trap cleanup_probe EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'int axl_unbuilt_lint_probe(void);\nint axl_unbuilt_lint_probe(void) { return 0; }\n' \
    > "$probe"
lo4=$(cd "$REPO" && timeout 600 nice -n 15 ./scripts/lint.sh 2>&1); rc=$?
cleanup_probe
[[ $rc -ne 0 ]] || bad "a src/ file in no CMake target did NOT stop the lint"
need 'axl-unbuilt-lint-probe' "$lo4" "the coverage guard did not name the unlisted file"
nonope '^lint: clean' "$lo4" "the lint reported clean with a file it could not see"
[[ $fail -eq $before ]] && pass "the lint's coverage guard SEES a source no target lists"

# --- c. THE DEFAULT RUN is the gate verify.sh and CI read: it passes, prints
# the summary verify.sh quotes, and the preset really carries the test that
# summary claims (a summary naming a test that is not registered would
# be a claim with nothing behind it).
before=$fail
lo3=$(cd "$REPO" && timeout 600 nice -n 15 ./scripts/lint.sh 2>&1); rc=$?
[[ $rc -eq 0 ]] || { bad "lint.sh exited $rc"; printf '%s\n' "$lo3" | tail -5 | sed 's/^/      /'; }
need '^lint: clean \(clang-tidy over the x64-lint preset' "$lo3" \
     "the summary line changed -- verify.sh quotes it"
tl=$(cd "$REPO" && ctest --preset x64-lint -N 2>&1)
need 'lint\.clang-warnings' "$tl" "the x64-lint preset does not register the clang -Wall -Wextra test"
[[ $fail -eq $before ]] && pass "lint.sh with no arguments is the gate, and runs what it claims"

echo ""
if [[ $fail -eq 0 ]]; then
    echo "lint-gate: all passed"
else
    echo "lint-gate: $fail check(s) FAILED"
fi
[[ $fail -eq 0 ]]
