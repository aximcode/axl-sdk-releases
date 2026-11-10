#!/bin/bash
# test-meta: arch=none needs=jq est=5 local-only=0
# test-release-suite.sh — scripts/release-suite.sh is the ONE list of what
# certifies a release, and only its complete success writes the stamp.
#
# WHY (review ruling R-A, AXL-CI-Release-Speed-Design.md §14). The release
# gate accepts a local stamp in place of a dispatched CI suite run. The stamp
# used to come from run-integration.sh alone, while CI's integration job ALSO
# ran the unit tests in QEMU, staged the SDK for both arches and ran the
# harness self-tests -- so the local path certified less than the CI path it
# stood in for. Both now call scripts/release-suite.sh, and the stamp is its.
#
# The steps are stubbed: this checks the script's ORDER, ARGUMENTS and STAMP
# rules in a fixture repo, not the suite (which is what the script runs).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# Run INSIDE ctest --preset x64-ci, this inherits the preset's environment
# (AXL_IT_CACHE=0, AXL_IT_TIMEOUT=240), and the stubs below record exactly
# those variables: the first ctest-vs-runner comparison failed here on it.
unset AXL_IT_DIR
# shellcheck source=lib/preset-env.sh
source "$SCRIPT_DIR/lib/preset-env.sh"
preset_env_unset "$PROJECT_DIR" || exit 1
export TEST_SKIP_RATCHET=1
# CI exports TEST_MAX_DURATION=190 to the whole release suite; the unit
# step's declared budget is pinned below, so it is this test's to set.
unset TEST_MAX_DURATION

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
REPO="$TMP/repo"; CALLS="$TMP/calls"; BASELINE_SEEN="$TMP/baseline-seen"
REQUIRE_SEEN="$TMP/require-seen"; COST_SEEN="$TMP/cost-seen"
export CALLS BASELINE_SEEN REQUIRE_SEEN COST_SEEN
mkdir -p "$REPO/scripts" "$REPO/test/integration/lib" "$TMP/bin"
cp "$PROJECT_DIR/scripts/release-suite.sh" "$PROJECT_DIR/scripts/ci-plan.sh" \
   "$PROJECT_DIR/scripts/axl-common.sh" "$PROJECT_DIR/scripts/ctest-report.py" "$REPO/scripts/"
# A DEBUG OVMF for every run below but the refusal's: empty stand-ins, as
# find_debug_ovmf only checks that they are there and readable.
mkdir -p "$TMP/dbgfv"; : > "$TMP/dbgfv/OVMF_CODE.fd"; : > "$TMP/dbgfv/OVMF_VARS.fd"
export AXL_OVMF_DEBUG_DIR="$TMP/dbgfv"
cp "$PROJECT_DIR/test/integration/lib/release-gate.sh" "$PROJECT_DIR/test/integration/lib/discover.sh" \
   "$PROJECT_DIR/test/integration/lib/ctest-session.sh" "$REPO/test/integration/lib/"
# The pinned CMake's helper, and a cmake that passes as the pinned one and
# fails any other call (lib/cmake-stub.sh).
# shellcheck source=lib/cmake-stub.sh
source "$SCRIPT_DIR/lib/cmake-stub.sh"
cmake_stub_repo "$REPO" "$TMP/bin"
# The measured-times store, private to this run (never the real one in
# ~/.cache), and the tree file ctest reads and rewrites.
export AXL_CI_COST_DIR="$TMP/coststore"
COST_TREE_REL="build/x64-debug/Testing/Temporary/CTestCostData.txt"

# One stub for every step: it logs its name and arguments, fails when
# STUB_FAIL_<name> is set, and -- for the dirty-mid-run case -- edits a
# tracked file when STUB_DIRTY names it.
stub() {
    # --version first: the ctest stub must pass as the pinned one
    # (lib/cmake-stub.sh), or release-suite.sh would run the real ctest.
    { echo '#!/bin/bash'; cmake_stub_version_line; cat <<'STUB'; } > "$1"
n=$(basename "$0"); echo "$n${*:+ $*}" >> "$CALLS"
[[ "${STUB_DIRTY:-}" == "$n" ]] && echo dirt >> tracked.txt
# What the suite is told about a test that needs a DEBUG OVMF and finds none,
# and whether the skip cache is on for it.
# The ctest stub also stands in for ctest's cost data: it records what the
# tree held when it started (COST_SEEN) and writes a new file, as ctest does
# at the end of every parallel run.
if [[ "$n" == ctest ]]; then
    echo "AXL_REQUIRE_DEBUG_OVMF=${AXL_REQUIRE_DEBUG_OVMF-unset}" > "$REQUIRE_SEEN"
    echo "cache=${AXL_IT_CACHE-unset}" >> "$CALLS"
    cf=build/x64-debug/Testing/Temporary/CTestCostData.txt
    cat "$cf" > "$COST_SEEN" 2>/dev/null || echo "(none)" > "$COST_SEEN"
    mkdir -p "${cf%/*}"; printf 'integration.test-a 2 %s\n---\n' "${STUB_COST:-9.5}" > "$cf"
    # Its JUnit report, where the real ctest writes one: an --output-junit
    # (relative to the build dir), or the x64-ci preset's outputJUnitFile.
    # x64-integration has none of its own. STUB_JUNIT_<k> is the k-th ctest
    # call's, so a suite run and the quiet re-run after it can report
    # differently; STUB_JUNIT_DEFAULT otherwise.
    k=$(grep -c '^ctest' "$CALLS")
    j=""; [[ " $* " == *" --preset x64-ci "* ]] && j=build/x64-debug/ctest-junit.xml
    for ((a = 1; a <= $#; a++)); do [[ "${!a}" == --output-junit ]] && { b=$((a + 1)); j="build/x64-debug/${!b}"; }; done
    v="STUB_JUNIT_$k"; src="${!v:-${STUB_JUNIT_DEFAULT:-}}"
    [[ -n "$src" && -n "$j" ]] && { mkdir -p "${j%/*}"; cp "$src" "$j"; }
    v="STUB_FAIL_ctest_$k"; [[ -n "${!v:-}" ]] && exit 1
fi
# The failure comes AFTER the cost data: ctest writes it whatever the verdict.
v="STUB_FAIL_${n//[.-]/_}"; [[ -n "${!v:-}" ]] && exit 1
# The unit step's ratchet (verify.sh's x64 job: the pooled per-binary units'
# ratchet, or test-axl.sh's one boot): record the baseline it was handed,
# and -- on a green run whose count rose -- write the new count to it, as
# the real one does.
if [[ "$n" == verify.sh ]]; then
    echo "verify-env: agent=${AXL_VERIFY_POOL_AGENT-unset} budget=${AXL_VERIFY_UNIT_BUDGET-unset} print=${AXL_VERIFY_PRINT_FAILURES-unset}" >> "$CALLS"
    b="${TEST_BASELINE_FILE:-test/integration/.last-pass-count}"
    cat "$b" > "$BASELINE_SEEN" 2>/dev/null
    [[ -n "${STUB_RAISE:-}" ]] && echo "$STUB_RAISE" > "$b"
fi
exit 0
STUB
    chmod +x "$1"
}
FXR="$PROJECT_DIR/test/integration/fixtures/ctest-report/stubs.xml"
sed -e '/<testcase name="integration.test-cr-fail"/,/<\/testcase>/d' \
    -e '/<testcase name="integration.test-cr-hang"/,/<\/testcase>/d' "$FXR" > "$TMP/green.xml"
# The JUnit reports the ctest stub writes: real ctest output (stubs.xml) with
# lines rewritten, as test-ctest-report.sh does -- green by default.
sed -e 's|<system-out>  test-cr-skip.sh SKIP 0s (SKIP: no widget on this box)|<system-out>  test-cr-skip.sh UNJUDGED 0s (after retry; UNJUDGED: starved)|' \
    "$TMP/green.xml" > "$TMP/uj.xml"
sed -e 's|<system-out>  test-cr-skip.sh SKIP 0s (SKIP: no widget on this box)|<system-out>  test-cr-skip.sh PASS 0s|' \
    -e 's|<testcase name="integration.test-cr-skip" classname="integration.test-cr-skip" time="0.0121095" status="notrun">|<testcase name="integration.test-cr-skip" classname="integration.test-cr-skip" time="0.0121095" status="run">|' \
    -e '/<skipped message="SKIP_RETURN_CODE=77"\/>/d' "$TMP/green.xml" > "$TMP/judged.xml"
export STUB_JUNIT_DEFAULT="$TMP/green.xml"
stub "$REPO/scripts/verify.sh"
# The suite is CTest's (slice 5a step 6): a ctest on PATH, and the est gate
# CI runs before it.
stub "$TMP/bin/ctest"
stub "$REPO/scripts/check-test-est.py"
export PATH="$TMP/bin:$PATH"
# -j is a CPU BUDGET that each test draws its cpu= from (CTest's
# PROCESSORS), with no load-average gate: --test-load reads the one-minute
# load, which lags a burst by a minute, and counts every other process on the
# box. The budget, measured: AXL-CI-Release-Speed-Design.md §15.
JOBS=$(( $(nproc) + 4 ))
stub "$REPO/scripts/install.sh"
stub "$REPO/scripts/run-selftests.sh"
# The build step's tool: the wrapper every test builds through, so the
# suite reads the tree its build step made.
stub "$REPO/scripts/axl-build.sh"

cd "$REPO" || exit 1
echo tracked > tracked.txt
echo 100 > test/integration/.last-pass-count
printf 'test/integration/.last-run-stamp\n' > .gitignore
git init -q .; git config user.email t@t; git config user.name t
git add -A >/dev/null; git commit -qm fixture
SHA=$(git rev-parse HEAD)
STAMP="test/integration/.last-run-stamp"
NP=$(nproc)

run() { rm -f "$CALLS"; ./scripts/release-suite.sh "$@" > "$TMP/out" 2>&1; }

echo "=== release-suite: the steps, in CI's order ==="
run; rc=$?
check "a clean tree, every step green -> 0" "$rc" "0"
check "the steps are CI's integration job, in order (local mode)" "$(cat "$CALLS")" \
"verify.sh --only=x64
verify-env: agent=tree budget= print=1
axl-build.sh -j$NP ARCH=x64 all tests tools axl-busybox
install.sh --arch all --cpp
ctest --preset x64-integration -j$JOBS --output-junit ctest-junit.xml
cache=0
run-selftests.sh"
check "...and it stamps HEAD" "$(sed -n 's/^sha=//p' "$STAMP")" "$SHA"
check "...as the release suite" "$(sed -n 's/^suite=//p' "$STAMP")" "release-suite"
# A test whose DEBUG OVMF is missing SKIPs, and a skip passes the suite: in a
# certifying run it must FAIL instead, or a release is certified without it.
check "the suite treats a missing DEBUG OVMF as a FAILURE (local mode)" \
    "$(cat "$REQUIRE_SEEN" 2>/dev/null)" "AXL_REQUIRE_DEBUG_OVMF=1"

# --ci differs in the suite step only: the x64-ci preset (CI's limit, the
# local-only exclusion, the cache off -- the preset's, not this script's),
# after the est gate, which refuses a test the limit cannot hold before any
# test runs.
rm -f "$STAMP"
run --ci; rc=$?
check "--ci -> 0" "$rc" "0"
check "--ci grades est first, then runs the x64-ci preset, and nothing else differs" \
    "$(sed -n '5,7p' "$CALLS")" "check-test-est.py
ctest --preset x64-ci -j$JOBS --output-junit ctest-junit.xml
cache=unset"
check "--ci runs the same five steps (and the est gate)" "$(grep -vcE '^cache=|^verify-env:' "$CALLS")" "6"
# CI's TEST_MAX_DURATION (ci.yml: its slower runner) is DECLARED to the unit
# step: verify scrubs an inherited one, so without this CI's headroom would
# silently not reach the units.
rm -f "$STAMP"
TEST_MAX_DURATION=190 run --ci
check "CI's TEST_MAX_DURATION is declared to the unit step as its budget" \
    "$(grep '^verify-env:' "$CALLS")" "verify-env: agent=tree budget=190 print=1"
rm -f "$STAMP"
TEST_MAX_DURATION=190 run
check "...and only in CI: a local certifying run judges the default budget" \
    "$(grep '^verify-env:' "$CALLS")" "verify-env: agent=tree budget= print=1"
rm -f "$STAMP"
STUB_FAIL_check_test_est_py=1 run --ci; rc=$?
check "an est the limit cannot hold fails --ci -> non-zero" "$([[ $rc -ne 0 ]] && echo yes)" "yes"
check "...before the suite runs a test" "$(grep -c '^ctest' "$CALLS")" "0"
check "...and a missing DEBUG OVMF is a FAILURE there too" \
    "$(cat "$REQUIRE_SEEN" 2>/dev/null)" "AXL_REQUIRE_DEBUG_OVMF=1"

echo ""
echo "=== release-suite: no DEBUG OVMF, no certification ==="
# A hosted runner has none, and its run would only FAIL hours in, at the
# tests that need one -- so the suite refuses before any step, and says why
# and what to do.
rm -f "$STAMP"
AXL_OVMF_DEBUG_DIR="$TMP/nofv" run --ci; rc=$?
check "--ci with no DEBUG OVMF refuses (2)" "$rc" "2"
check "...before any step runs" "$(cat "$CALLS" 2>/dev/null)" ""
check "...and writes no stamp" "$([[ -f "$STAMP" ]] && echo stamped || echo none)" "none"
check "...saying exactly why, and what to do" "$(cat "$TMP/out")" \
"release-suite: refusing to certify -- no DEBUG OVMF -- $TMP/nofv: missing
release-suite: the integration tests that need one (test-stderr-stale-qemu.sh among them) would FAIL without it, and a skip of them would certify nothing.
release-suite: run CI on the self-hosted runner (workflow_dispatch input runner: self-hosted), or set AXL_OVMF_DEBUG_DIR to a DEBUG OVMF build."
AXL_OVMF_DEBUG_DIR="$TMP/nofv" run; rc=$?
check "a local run with no DEBUG OVMF refuses (2) too" "$rc" "2"
check "...pointing at the knob" "$(tail -1 "$TMP/out")" \
    "release-suite: set AXL_OVMF_DEBUG_DIR to a DEBUG OVMF build (docs/DEBUGGING.md)."
AXL_OVMF_DEBUG_DIR="$TMP/nofv" run --step integration; rc=$?
check "--step integration with none refuses (2)" "$rc" "2"
AXL_OVMF_DEBUG_DIR="$TMP/nofv" run --step unit; rc=$?
check "control: --step unit needs none, and runs (0)" "$rc" "0"

echo ""
echo "=== release-suite: only COMPLETE success stamps ==="
rm -f "$STAMP"
STUB_FAIL_ctest=1 run; rc=$?
check "the suite step fails -> non-zero" "$([[ $rc -ne 0 ]] && echo yes)" "yes"
check "...writes NO stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
check "...and stops there (the self-tests do not run)" \
    "$(grep -c '^run-selftests.sh' "$CALLS")" "0"
check "...and says which step failed" "$(tail -1 "$TMP/out")" \
    "release-suite: FAILED at step 4/5 (integration suite) -- no stamp written"
STUB_FAIL_verify_sh=1 run
check "the unit-test step fails -> no stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
STUB_FAIL_run_selftests_sh=1 run
check "the LAST step fails -> no stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"

echo ""
echo "=== release-suite: the stamp is bound to a CLEAN sha ==="
echo edit >> tracked.txt
run; rc=$?
check "tracked changes -> refuses (2)" "$rc" "2"
check "...before running anything" "$([[ -s "$CALLS" ]] && echo ran || echo nothing)" "nothing"
check "...and says why" "$(tail -1 "$TMP/out")" \
    "release-suite: refusing -- the tree has tracked changes, and a stamp names a commit, not a working tree"
git checkout -q -- tracked.txt
# A tree dirtied DURING the run certifies nothing either.
STUB_DIRTY=run-selftests.sh run; rc=$?
check "the tree changes mid-run -> non-zero" "$([[ $rc -ne 0 ]] && echo yes)" "yes"
check "...and no stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
git checkout -q -- tracked.txt

echo ""
echo "=== release-suite: the unit ratchet does not dirty the tree ==="
# test-axl.sh raises test/integration/.last-pass-count (TRACKED) on a green run
# whose count rose -- which made the end-of-run check fail "the tree moved"
# on every commit that added unit assertions, locally and in CI. The step now
# hands it a temp copy of the COMMITTED baseline: the ratchet still enforces
# the committed value, and a rise is a NOTE to commit, not a failure.
rm -f "$STAMP" "$BASELINE_SEEN"
STUB_RAISE=105 run; rc=$?
check "a rising unit count -> still green (0)" "$rc" "0"
check "...the ratchet was handed the COMMITTED baseline" "$(cat "$BASELINE_SEEN")" "100"
check "...the tracked baseline is untouched" "$(cat test/integration/.last-pass-count)" "100"
check "...the run stamps" "$(sed -n 's/^sha=//p' "$STAMP")" "$SHA"
check "...and it says to commit the new baseline" \
    "$(grep -cxF 'release-suite: NOTE -- the unit-test count rose 100 -> 105; commit the new baseline (test/integration/.last-pass-count)' "$TMP/out")" "1"
# Control: an unchanged count prints no NOTE, so the line above is about the rise.
run
check "control: an unchanged count prints no baseline NOTE" "$(grep -c 'count rose' "$TMP/out")" "0"

echo ""
echo "=== release-suite: a dirty tree is NAMED, and could-not-check is not clean ==="
STUB_DIRTY=run-selftests.sh run
check "a tree dirtied mid-run names the file (git status --short)" \
    "$(grep -cxF ' M tracked.txt' "$TMP/out")" "1"
git checkout -q -- tracked.txt
# A git that cannot answer is "could not check", not "clean" -- the same empty
# porcelain output, opposite facts.
mkdir -p "$TMP/badgit"
printf '#!/bin/bash\n[[ "$1" == status ]] && exit 128\nexec %q "$@"\n' "$(command -v git)" > "$TMP/badgit/git"
chmod +x "$TMP/badgit/git"
rm -f "$CALLS"
PATH="$TMP/badgit:$PATH" ./scripts/release-suite.sh > "$TMP/out" 2>&1; rc=$?
check "git status FAILS -> refuses (2)" "$rc" "2"
check "...says it could not check" "$(tail -1 "$TMP/out")" \
    "release-suite: could not check the tree (git status failed) -- refusing"
check "...before running anything" "$([[ -s "$CALLS" ]] && echo ran || echo nothing)" "nothing"

echo ""
echo "=== release-suite: a run starting on HEAD retires HEAD's old stamp ==="
# Otherwise a failed re-run leaves the LAST success's stamp standing for the
# same commit, and the gate reads it as if nothing had happened since.
release_stamp() { printf 'sha=%s\nsuite=release-suite\narch=X64\nwhen=x\n' "$1" > "$STAMP"; }
release_stamp "$SHA"
STUB_FAIL_verify_sh=1 run
check "a failing run deletes HEAD's earlier stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
release_stamp deadbeef
STUB_FAIL_verify_sh=1 run
check "control: a stamp for ANOTHER commit is left alone" "$(sed -n 's/^sha=//p' "$STAMP")" "deadbeef"
rm -f "$STAMP"

echo ""
echo "=== release-suite: one step, never a stamp ==="
run --step unit; rc=$?
check "--step unit -> 0" "$rc" "0"
check "...runs that step alone" "$(cat "$CALLS")" "verify.sh --only=x64
verify-env: agent=tree budget= print=1"
check "...and does not stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
check "...and says it is partial" "$(tail -1 "$TMP/out")" \
    "release-suite: PARTIAL -- ran step 'unit' only; this certifies nothing"
run --step bogus; rc=$?
check "--step <unknown> -> 2" "$rc" "2"
run --timeout 5; rc=$?
check "an unknown flag (e.g. --timeout, which would override CI's limit) -> 2" "$rc" "2"

echo ""
echo "=== release-suite: the measured test times outlive the checkout ==="
# CI's workspace is recreated for every run, so ctest's CTestCostData.txt was
# always absent and the order was est='s. The suite step seeds a tree that
# has none from a store outside it, keyed by preset, and keeps the new file.
STORE_CI="$AXL_CI_COST_DIR/ctest-cost-x64-ci.txt"
STORE_LOCAL="$AXL_CI_COST_DIR/ctest-cost-x64-integration.txt"
rm -rf "$AXL_CI_COST_DIR" build
run --ci --step integration; rc=$?
check "no store, no tree file: the run goes ahead (0)" "$rc" "0"
check "...ctest found no cost data" "$(cat "$COST_SEEN")" "(none)"
check "...and says it orders by est=" \
    "$(grep -c "^release-suite: no measured test times yet ($STORE_CI) -- ordering by est=$" "$TMP/out")" "1"
check "...and keeps what ctest wrote, under the x64-ci key" "$(cat "$STORE_CI" 2>/dev/null)" \
"integration.test-a 2 9.5
---"
check "...and says so" "$(grep -c "^release-suite: kept the measured test times in $STORE_CI$" "$TMP/out")" "1"
rm -rf build
STUB_COST=11 run --ci --step integration
check "a fresh tree (CI's) is seeded from the store before ctest" "$(cat "$COST_SEEN")" \
"integration.test-a 2 9.5
---"
check "...saying where from" \
    "$(grep -c "^release-suite: ordering by the measured times kept in $STORE_CI$" "$TMP/out")" "1"
check "...and the store takes the new times" "$(head -1 "$STORE_CI")" "integration.test-a 2 11"
# A developer tree's own file drifts low (ctest rewrites it after -j1 runs
# and cache hits), so the store, which holds only cache-off suite runs, wins.
mkdir -p "${COST_TREE_REL%/*}"; printf 'integration.test-a 7 0.1\n---\n' > "$COST_TREE_REL"
run --ci --step integration
check "the store wins over a tree's own (drifted) cost data" \
    "$(head -1 "$COST_SEEN")" "integration.test-a 2 11"
rm -rf "$AXL_CI_COST_DIR"
mkdir -p "${COST_TREE_REL%/*}"; printf 'integration.test-a 7 3\n---\n' > "$COST_TREE_REL"
run --ci --step integration
check "with no store yet, the tree's own cost data is used, not discarded" \
    "$(head -1 "$COST_SEEN")" "integration.test-a 7 3"
check "...and it says so" \
    "$(grep -c "^release-suite: no measured test times kept yet ($STORE_CI) -- ordering by the tree's own ($COST_TREE_REL)$" "$TMP/out")" "1"
rm -rf build
STUB_FAIL_ctest=1 STUB_COST=13 run --ci --step integration; rc=$?
check "a FAILING suite still keeps its times (non-zero)" \
    "$([[ $rc -ne 0 ]] && echo yes) $(head -1 "$STORE_CI")" "yes integration.test-a 2 13"
rm -rf build
run --step integration
check "a local run keys its store by its own preset, x64-integration" \
    "$(head -1 "$STORE_LOCAL" 2>/dev/null)" "integration.test-a 2 9.5"
check "...and never seeds from CI's" "$(cat "$COST_SEEN")" "(none)"
rm -rf build
: > "$TMP/ro"; AXL_CI_COST_DIR="$TMP/ro/nope" run --ci --step integration; rc=$?
check "a store it cannot write is a WARNING, not a failure (0)" "$rc" "0"
check "...said by name" \
    "$(grep -c "^release-suite: WARNING -- could not keep the measured test times in $TMP/ro/nope/ctest-cost-x64-ci.txt$" "$TMP/out")" "1"
rm -rf build "$AXL_CI_COST_DIR"

echo ""
echo "=== release-suite: an UNJUDGED timing test is re-run ALONE, and certifies only judged ==="
# A timing test whose guest the suite's own load starved reads UNJUDGED (run-one
# prints it; ctest saw a 77 skip, so ctest itself exits 0). A skip that passes
# would certify a release on a bound nobody judged -- so the integration step
# re-runs each UNJUDGED test alone after the suite (the quiet slot, as verify's
# qloop runs last and alone), and only a JUDGED result counts. Still UNJUDGED
# alone: no stamp, exit 3, named -- in --ci too, since the dispatched suite IS
# the release certificate.
rm -f "$STAMP"
STUB_JUNIT_1="$TMP/green.xml" run; rc=$?
check "control: no UNJUDGED test, no re-run (0, one ctest call)" "$rc $(grep -c '^ctest' "$CALLS")" "0 1"
rm -f "$STAMP"
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/judged.xml" run; rc=$?
check "UNJUDGED in the suite, a judged PASS alone -> certified (0)" "$rc" "0"
check "...re-run alone, by name, into its own report" "$(grep '^ctest' "$CALLS" | sed -n 2p)" \
    "ctest --preset x64-integration -j1 -R ^integration\\.test-cr-skip\$ --output-junit ctest-junit-quiet.xml"
check "...saying so" "$(grep -cxF 'release-suite: test-cr-skip.sh was UNJUDGED under the suite'"'"'s load -- re-running it alone (the quiet slot)' "$TMP/out")" "1"
check "...and it stamps" "$(sed -n 's/^sha=//p' "$STAMP")" "$SHA"
rm -f "$STAMP"
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/uj.xml" run; rc=$?
check "UNJUDGED even alone -> not certified, exit 3" "$rc" "3"
check "...no stamp" "$([[ -e "$STAMP" ]] && echo stamped || echo none)" "none"
check "...the self-tests do not run" "$(grep -c '^run-selftests.sh' "$CALLS")" "0"
check "...and it names the test and what to do" "$(tail -1 "$TMP/out")" \
    "release-suite: UNJUDGED at step 4/5 (integration suite) -- test-cr-skip.sh could not judge its timing bounds even run alone (the host starved its guest of CPU); no stamp written. Re-run on a quiet box."
rm -f "$STAMP"
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/uj.xml" run --ci; rc=$?
check "...the same in --ci: the dispatched suite is the release certificate (3)" "$rc" "3"
check "...re-run there under the x64-ci preset" "$(grep '^ctest' "$CALLS" | sed -n 2p)" \
    "ctest --preset x64-ci -j1 -R ^integration\\.test-cr-skip\$ --output-junit ctest-junit-quiet.xml"
rm -f "$STAMP"
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/judged.xml" STUB_FAIL_ctest=1 run; rc=$?
check "a FAILED suite is not re-run for its UNJUDGED test (one ctest call, non-zero)" \
    "$([[ $rc -ne 0 && $rc -ne 3 ]] && echo failed) $(grep -c '^ctest' "$CALLS")" "failed 1"
rm -f "$STAMP"; rm -rf build
STUB_JUNIT_DEFAULT= run; rc=$?
check "no JUnit report at all -> not certified (an unread report is not 'none unjudged')" \
    "$rc $([[ -e "$STAMP" ]] && echo stamped || echo none)" "1 none"
check "...saying why" "$(grep -cxF "release-suite: cannot read the suite's JUnit report (build/x64-debug/ctest-junit.xml) -- nothing certified" "$TMP/out")" "1"
# A report left by an EARLIER run must never stand as this one's: the suite's
# own is removed before ctest runs, so a ctest that writes none is caught.
mkdir -p build/x64-debug; cp "$TMP/uj.xml" build/x64-debug/ctest-junit.xml
STUB_JUNIT_DEFAULT= run; rc=$?
check "a stale report from an earlier run is not read as this run's (1, no re-run)" \
    "$rc $(grep -c '^ctest' "$CALLS")" "1 1"
rm -rf build
# Alone, the test SKIPPED (a resource gone): that judged nothing, so it is not
# a PASS -- nothing is certified, and the verdict is named.
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/green.xml" run; rc=$?
check "UNJUDGED, then a plain SKIP alone -> not certified (1)" \
    "$rc $([[ -e "$STAMP" ]] && echo stamped || echo none)" "1 none"
check "...naming what the lone run read" \
    "$(grep -cxF 'release-suite: test-cr-skip.sh alone read SKIP, not a judged PASS -- nothing certified' "$TMP/out")" "1"
# Green suite, then the lone re-run FAILS: a judged failure, named.
STUB_JUNIT_1="$TMP/uj.xml" STUB_FAIL_ctest_2=1 run; rc=$?
check "UNJUDGED, then a FAIL alone -> not certified (1)" \
    "$rc $([[ -e "$STAMP" ]] && echo stamped || echo none)" "1 none"
check "...naming it" \
    "$(grep -cxF 'release-suite: test-cr-skip.sh alone FAILED (ctest exit 1) -- nothing certified' "$TMP/out")" "1"
# --step integration names an UNJUDGED test too, not just PARTIAL.
STUB_JUNIT_1="$TMP/uj.xml" STUB_JUNIT_2="$TMP/uj.xml" run --step integration; rc=$?
check "--step integration, still UNJUDGED -> 3, naming it" \
    "$rc $(grep -c '^release-suite: UNJUDGED -- test-cr-skip.sh could not judge' "$TMP/out")" "3 1"
rm -rf build

echo ""
echo "=== release-suite: a tree another cmake configured is redone before ctest ==="
# --step integration skips the build step, so nothing else would move a
# registration the box's cmake wrote. A logging cmake that passes as pinned.
mv "$TMP/bin/cmake" "$TMP/cmake.versiononly"
{ echo '#!/bin/bash'; cmake_stub_version_line; echo 'echo "cmake $*" >> "$CALLS"'; } > "$TMP/bin/cmake"
chmod +x "$TMP/bin/cmake"
mkdir -p build/x64-debug
printf 'CMAKE_COMMAND:INTERNAL=/usr/bin/cmake\n' > build/x64-debug/CMakeCache.txt
run --step integration; rc=$?
check "a foreign tree: configured --fresh by the pinned cmake, then ctest (0)" \
    "$rc|$(grep -E '^(cmake|ctest)' "$CALLS" | cut -d' ' -f1-4)" \
    "0|cmake --fresh --preset x64-debug
ctest --preset x64-integration -j$JOBS"
check "...saying why" \
    "$(grep -c "^release-suite: $REPO/build/x64-debug was configured by /usr/bin/cmake, not the pinned $TMP/bin/cmake -- reconfiguring it --fresh$" "$TMP/out")" "1"
printf 'CMAKE_COMMAND:INTERNAL=%s\n' "$TMP/bin/cmake" > build/x64-debug/CMakeCache.txt
run --step integration; rc=$?
check "control: a tree the pinned cmake configured goes straight to ctest" \
    "$rc|$(grep -cE '^cmake' "$CALLS")" "0|0"
rm -rf build
mv "$TMP/cmake.versiononly" "$TMP/bin/cmake"

# Control for every "no stamp" above: a passing run DOES stamp, so their
# assertions are about the failure and not about stamping being broken.
run
check "control: a green run stamps" "$(sed -n 's/^sha=//p' "$STAMP")" "$SHA"
cd "$PROJECT_DIR" || exit 1

echo ""
echo "=== run-integration.sh no longer certifies a release ==="
RI="$PROJECT_DIR/test/integration/run-integration.sh"
check "run-integration.sh writes no release stamp" \
    "$(sed 's/#.*//' "$RI" | grep -c 'release_gate_write')" "0"
check "...and says what does, where it used to stamp" \
    "$(grep -cF 'integration: this run does not certify a release -- scripts/release-suite.sh does' "$RI")" "1"
# Whether CI runs the suite under the limit the est gate grades -- release-suite.sh
# --ci, reaching ctest --preset x64-ci -- is test-check-test-est.sh's, against the
# real gate.


echo ""
echo "release-suite: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
