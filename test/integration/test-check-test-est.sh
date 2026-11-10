#!/bin/bash
# test-meta: arch=none needs=cmake,ctest,jq est=40 local-only=0
# test-check-test-est.sh -- scripts/check-test-est.py, the est-vs-CI-limit
# gate, now that CI runs the suite as `ctest --preset x64-ci` (slice 5a step
# 6, docs/AXL-Build-System-Design.md §10.8).
#
# The gate keeps no copy of the limit or the selection: it reads both from
# `ctest --preset x64-ci --show-only=json-v1`. So this test never writes the
# limit down either -- it reads the figure the gate reports on a clean stub
# and builds every over/at-limit case from it.
#
# Stub selections come in through AXL_EST_IT_DIR (the gate hands it to the
# CTest include script as AXL_IT_DIR, which registers that directory instead
# of the real one; an AXL_IT_DIR merely exported in a shell is ignored), and
# stub workflows and a stub release-suite through AXL_CI_WORKFLOW_DIR /
# AXL_RELEASE_SUITE, so no
# verdict here depends on the real ci.yml or on the real tests' est values --
# except the last check, which is the gate on the real tree.
#
# Host-only; lists tests, runs none. Needs a configured build/x64-debug (the
# gate refuses an unconfigured tree rather than configure it).
#
# Usage: ./test/integration/test-check-test-est.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE="$PROJECT_DIR/scripts/check-test-est.py"
unset AXL_IT_DIR AXL_EST_IT_DIR AXL_EST_BUILD_DIR
# Run under a test preset (CI: ctest --preset x64-ci), this inherits the
# preset's environment; clear every key any preset sets (lib/preset-env.sh).
# shellcheck source=lib/preset-env.sh
source "$SCRIPT_DIR/lib/preset-env.sh"
preset_env_unset "$PROJECT_DIR" || exit 1

WORK="$(mktemp -d -t axl-estgate.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

S="$WORK/tests"; WF="$WORK/wf"; mkdir -p "$S/lib" "$WF"
# A stub directory is discovered by ITS OWN lib/discover.sh, as the real one is.
cp "$SCRIPT_DIR"/lib/discover.sh "$SCRIPT_DIR"/lib/test-cache.sh "$S/lib/"
stub() {   # stub NAME EST [LOCAL_ONLY]
    printf '#!/bin/bash\n# test-meta: arch=x64 needs= est=%s local-only=%s\nexit 0\n' \
        "$2" "${3:-0}" > "$S/$1"
    chmod +x "$S/$1"
}
wf() {     # wf LINE... -- the lines of a ci.yml's steps, from line 4
    printf '%s\n' "jobs:" "  suite:" "    steps:" "$@" > "$WF/ci.yml"
}
RS="$WORK/release-suite.sh"
rs() {     # rs LINE... -- a stub release-suite.sh
    printf '%s\n' '#!/bin/bash' "$@" > "$RS"
}
gate() {   # gate -> output, then "rc=N" as the last line
    AXL_EST_IT_DIR="$S" AXL_CI_WORKFLOW_DIR="$WF" AXL_RELEASE_SUITE="$RS" \
        python3 "$GATE" 2>&1
    echo "rc=$?"
}

echo "=== a clean stub: the gate reports the limit it graded ==="
stub test-one-qemu.sh 1
wf "      - run: ./scripts/release-suite.sh --ci"
rs './scripts/check-test-est.py && ctest --preset x64-ci -j "$jobs"'
JU="$PROJECT_DIR/build/x64-debug/ctest-junit.xml"
ju_before=$(stat -c %Y "$JU" 2>/dev/null || echo none)
sleep 1
out=$(gate)
check "a 1 s test under a clean workflow passes" "$(tail -1 <<< "$out")" "rc=0"
lim=$(sed -nE 's/^check-test-est: clean -- 1 test\(s\) \(x64-ci\) fit the ([0-9]+)s per-test limit.*/\1/p' <<< "$out")
check "...and names the limit it graded (read back, never written here)" \
    "$([[ "$lim" =~ ^[0-9]+$ ]] && echo yes || echo "no: $out")" "yes"
[[ "$lim" =~ ^[0-9]+$ ]] || lim=0
check "the clean line, exactly" "$(head -1 <<< "$out")" \
    "check-test-est: clean -- 1 test(s) (x64-ci) fit the ${lim}s per-test limit; largest est=1s (test-one-qemu.sh); 1 workflow invocation(s) take it"
# The listing is a ctest run against the real tree, and the x64-ci preset
# names a JUnit file: inside a running ctest (CI runs this test under that
# very preset) it would overwrite the outer run's results file.
check "the gate's listing leaves the tree's JUnit file alone" \
    "$(stat -c %Y "$JU" 2>/dev/null || echo none)" "$ju_before"

# run-integration.sh --ci keeps no copy of the limit since it became a ctest
# front end (step 6, decision 1): it reads the preset's AXL_IT_TIMEOUT, and
# test-runner-selftest.sh pins both that and the copy's absence.

echo "=== est against the limit ==="
stub test-over-qemu.sh "$((lim + 1))"
out=$(gate)
check "one second over fails" "$(tail -1 <<< "$out")" "rc=1"
check "...naming the test and both figures" \
    "$(grep -c "^    test-over-qemu.sh: est=$((lim + 1))s > ${lim}s$" <<< "$out")" "1"
stub test-over-qemu.sh "$lim"
check "exactly at the limit passes" "$(gate | tail -1)" "rc=0"
stub test-over-qemu.sh soon
out=$(gate)
check "an est that is not a number fails" "$(tail -1 <<< "$out")" "rc=1"
check "...by name" "$(grep -c '^    test-over-qemu.sh: est=soon is not a number$' <<< "$out")" "1"
# CI does not run a local-only test, so its est is not CI's business.
stub test-over-qemu.sh "$((lim + 100))" 1
check "a local-only test over the limit is not graded (CI does not run it)" "$(gate | tail -1)" "rc=0"
rm -f "$S/test-over-qemu.sh"

echo "=== an empty selection graded nothing ==="
mkdir -p "$WORK/empty/lib"
cp "$S"/lib/*.sh "$WORK/empty/lib/"
out=$(AXL_EST_IT_DIR="$WORK/empty" AXL_CI_WORKFLOW_DIR="$WF" AXL_RELEASE_SUITE="$RS" python3 "$GATE" 2>&1); rc=$?
check "an empty selection fails" "$rc" "1"
check "...and says EMPTY" "$(grep -c 'the x64-ci selection is EMPTY' <<< "$out")" "1"

echo "=== the workflow half: CI must run the suite under that limit ==="
wf "      - run: ./scripts/release-suite.sh --ci --timeout 180"
out=$(gate)
check "release-suite.sh with --timeout fails" "$(tail -1 <<< "$out")" "rc=1"
check "...by file:line" "$(grep -c '^    ci.yml:4 overrides the limit: ' <<< "$out")" "1"
wf "      - run: ./scripts/release-suite.sh"
out=$(gate)
check "release-suite.sh without --ci fails" "$(tail -1 <<< "$out")" "rc=1"
check "...by file:line" "$(grep -c '^    ci.yml:4 runs the suite without --ci' <<< "$out")" "1"
wf "      - run: |" "          ./scripts/release-suite.sh \\" "            --ci --timeout=180"
check "an override on a continuation line is still seen" "$(gate | grep -c '^    ci.yml:5 overrides the limit')" "1"
# ctest has no run-time refusal of an est its limit cannot hold, so a direct
# call must grade first -- or an est=250 test is killed at 240 s again.
wf "      - run: ctest --preset x64-ci -j6"
out=$(gate)
check "ctest --preset x64-ci run directly, ungraded, fails" "$(tail -1 <<< "$out")" "rc=1"
check "...saying the est gate must run first" \
    "$(grep -c '^    ci.yml:4 runs ctest --preset x64-ci without scripts/check-test-est.py before it' <<< "$out")" "1"
wf "      - run: ./scripts/check-test-est.py && ctest --preset x64-ci -j6"
check "...and passes with the gate before it" "$(gate | tail -1)" "rc=0"
# BEFORE it is not enough: the gate's failure must stop the run. With `;`
# or `|| true` a failing gate is ignored (release-suite.sh has no `set -e`).
wf "      - run: ./scripts/check-test-est.py; ctest --preset x64-ci -j6"
check "a gate joined by ';' (its failure ignored) does not count" \
    "$(gate | grep -c '^    ci.yml:4 runs ctest --preset x64-ci without scripts/check-test-est.py before it')" "1"
wf "      - run: ./scripts/check-test-est.py || true; ctest --preset x64-ci -j6"
check "...nor one whose failure is swallowed by '|| true'" \
    "$(gate | grep -c '^    ci.yml:4 runs ctest --preset x64-ci without scripts/check-test-est.py before it')" "1"
wf "      - run: ctest --preset x64-suite"
check "ctest with another preset fails" "$(gate | grep -c '^    ci.yml:4 runs ctest with a preset other than x64-ci')" "1"
wf "      # ./scripts/release-suite.sh --timeout 5" "      - name: Release suite (release-suite.sh)" "        run: true"
out=$(gate)
check "a workflow that runs no suite fails (a comment and a step NAME run nothing)" "$(tail -1 <<< "$out")" "rc=1"
check "...saying so" "$(grep -c 'runs the suite (release-suite.sh --ci, or check-test-est.py && ctest --preset x64-ci), so nothing shows CI uses its limit' <<< "$out")" "1"
wf "      - run: ./scripts/release-suite.sh --ci"

echo "=== release-suite.sh's own ctest call is what --ci reaches ==="
rs 'ctest --preset x64-suite -j "$jobs"'
check "a release-suite that never runs the x64-ci preset fails" \
    "$(gate | grep -c '^    release-suite.sh never runs ctest --preset x64-ci')" "1"
rs 'ctest --preset x64-ci -j "$jobs"'
check "a release-suite running x64-ci without grading first fails" \
    "$(gate | grep -c '^    release-suite.sh:2 runs ctest --preset x64-ci without scripts/check-test-est.py before it')" "1"
rs './scripts/check-test-est.py \' '    && ctest --preset x64-ci -j "$jobs"'
check "...and the gate before it is seen across a continuation" "$(gate | tail -1)" "rc=0"
rs './scripts/check-test-est.py && ctest --preset x64-ci -j "$jobs"'

echo "=== what the gate will not do ==="
# A shell that exported AXL_IT_DIR (a stub directory, while testing) must not
# make the real gate grade stubs and read clean.
out=$(AXL_IT_DIR="$S" python3 "$GATE" 2>&1)
check "an exported AXL_IT_DIR is ignored: the real selection is graded" \
    "$(sed -nE 's/^check-test-est: clean -- ([0-9]+) test.*/\1/p' <<< "$out" | awk '{ print ($1 > 100) ? "yes" : "no: " $1 }')" "yes"
# It is a lint gate, run beside builds (verify.sh's gates job beside the x64
# job): configuring the tree itself would race that job's own configure.
mkdir -p "$WORK/unconf"
out=$(AXL_EST_BUILD_DIR="$WORK/unconf" python3 "$GATE" 2>&1); rc=$?
check "an unconfigured tree is refused, not configured" \
    "$rc $(grep -c 'is not configured' <<< "$out") $(ls -A "$WORK/unconf" | wc -l)" "1 1 0"
# It lists with the ctest that CONFIGURED the tree (its CMakeCache.txt's
# CMAKE_CTEST_COMMAND), never whichever is first on PATH: run by hand from a
# shell with the box's ctest first, that one would read a tree the pinned
# CMake wrote (scripts/axl-cmake-pin.sh, §10.13).
mkdir -p "$WORK/own" "$WORK/pathbin"
: > "$WORK/own/CTestTestfile.cmake"
for d in own pathbin; do
    printf '#!/bin/bash\necho "%s $*" >> "%s"\necho "{\\"tests\\": []}"\n' "$d" "$WORK/ctest.log" > "$WORK/$d/ctest"
    chmod +x "$WORK/$d/ctest"
done
printf 'CMAKE_CTEST_COMMAND:INTERNAL=%s\n' "$WORK/own/ctest" > "$WORK/own/CMakeCache.txt"
PATH="$WORK/pathbin:$PATH" AXL_EST_BUILD_DIR="$WORK/own" python3 "$GATE" > /dev/null 2>&1
check "the tree's own ctest lists it, not the one first on PATH" \
    "$(cut -d' ' -f1 "$WORK/ctest.log" 2>/dev/null)" "own"
: > "$WORK/own/CMakeCache.txt"
out=$(PATH="$WORK/pathbin:$PATH" AXL_EST_BUILD_DIR="$WORK/own" python3 "$GATE" 2>&1); rc=$?
check "a tree whose cache names no ctest is refused, by name" "$rc|$(tail -1 <<< "$out")" \
    "1|check-test-est: FAIL -- $WORK/own/CMakeCache.txt names no CMAKE_CTEST_COMMAND: reconfigure it (scripts/axl-build.sh), then re-run"

echo "=== the real tree ==="
out=$(python3 "$GATE" 2>&1); rc=$?
check "the real tree passes its own gate" "$rc" "0"
check "...grading the real selection (more than a handful of tests)" \
    "$(sed -nE 's/^check-test-est: clean -- ([0-9]+) test.*/\1/p' <<< "$out" | awk '{ print ($1 > 100) ? "yes" : "no: " $1 }')" "yes"

echo ""
echo "check-test-est: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
