#!/bin/bash
# make-verify-fixtures.sh -- regenerate the verify-*.xml fixtures that
# test-ctest-report.sh renders verify.sh's table from.
#
# They come from REAL ctest runs, so the JUnit schema is ctest's, never a
# hand-written belief about it: a scratch project whose tests stand in for
# the real ones by NAME, LABEL and OUTPUT -- the one-boot unit run, the
# --apps binaries and their ratchet, the check gates (one labelled artifact),
# lint, docs, qloop -- each
# printing the line its real counterpart prints.
#
# Usage: test/integration/fixtures/ctest-report/make-verify-fixtures.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$HERE/../../../../scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

# gen NAME CMAKE-BODY -- one scratch project, one ctest run, one fixture.
gen() {
    mkdir -p "$W/$1"
    { echo 'cmake_minimum_required(VERSION 3.29)'; echo 'project(fx NONE)'; echo 'enable_testing()'; echo "$2"; } \
        > "$W/$1/CMakeLists.txt"
    cmake -S "$W/$1" -B "$W/$1/b" > /dev/null
    ctest --test-dir "$W/$1/b" --output-junit "$HERE/$1.xml" \
          --test-output-size-passed 65536 --test-output-size-failed 65536 > /dev/null 2>&1 || true
}
# t NAME LABELS EXIT OUTPUT... -- a test printing OUTPUT lines, exiting EXIT.
t() {
    local n="$1" l="$2" rc="$3"; shift 3
    printf 'add_test(NAME %s COMMAND bash -c [==[printf "%%s\\n" "$@"; exit %s]==] _' "$n" "$rc"
    printf ' [==[%s]==]' "$@"
    printf ')\nset_tests_properties(%s PROPERTIES LABELS "%s")\n' "$n" "$l"
}
oneboot() {   # oneboot EXIT RESULTS-LINE -- the one-boot unit run (verify's job)
    t unit.one-boot "arch:x64;unit-boot" "$1" "$2"
}
gates() {   # gates CHECK_B_EXIT
    t gate.lint "gate;lint" 0 "lint: clean (clang-tidy over the x64-lint preset)"
    t gate.docs "docs;gate" 0 "docs built, 0 warnings"
    t gate.check-a "gate;check" 0 "check-a: clean"
    t gate.check-b "gate;check" "$1" "check-b: FAILED -- 2 offenders"
    t gate.check-img "gate;check;artifact" 0 "check-img: clean"
}

# A full verify runs the ONE-BOOT unit run per arch; --apps runs per binary.
gen verify-x64 "$(oneboot 0 'Results: 14824 passed, 0 failed, 12 group(s) SKIPPED (84 assertions) (X64) in 105.504s'; gates 0)"
gen verify-aa64 "$(oneboot 0 'Results: 14841 passed, 0 failed, 16 group(s) SKIPPED (67 assertions) (AARCH64) in 107.490s')"
# The aa64 tree's own gates (an arch gate registers in each tree): one failing.
gen verify-aa64-gate-failed "$(oneboot 0 'Results: 14841 passed, 0 failed, 16 group(s) SKIPPED (67 assertions) (AARCH64) in 107.490s'; t gate.check-c "gate;check" 1 "check-c: FAILED")"
gen verify-aa64-diverged "$(oneboot 0 'Results: 14840 passed, 0 failed, 16 group(s) SKIPPED (67 assertions) (AARCH64) in 107.490s')"
# Output cut short (ctest keeps the HEAD of a test's output): passed, no Results line.
gen verify-x64-truncated "$(oneboot 0 'PASS: AxlTestA: something')"
gen verify-x64-failed "$(oneboot 1 'Results: 14823 passed, 1 failed, 12 group(s) SKIPPED (84 assertions) (X64) in 104.100s'; gates 1)"
# --apps AxlTestB: ONLY that binary runs (plus the inventory, and the
# ratchet and its reset as fixtures), as a real --apps run selects.
apps() {   # apps ARCH B_COUNT [BINARY...] -- the named binaries, B_COUNT each
    local arch="$1" b="$2"; shift 2
    local names=() x
    t unit.counts-reset unit 0 ""
    for x in "$@"; do
        t "unit.$x" "arch:x64;unit" 0 "Results: $b passed, 0 failed ($arch) in 1s"
        names+=("unit.$x")
    done
    t unit.inventory unit 0 "inventory: clean"
    local counts="unit-counts:"; for x in "$@"; do counts+=" $x=$b"; done
    t unit.ratchet unit 77 "PARTIAL: $# of 2 unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run" "$counts"
    echo 'set_tests_properties(unit.counts-reset PROPERTIES FIXTURES_SETUP unit.counts)'
    echo 'set_tests_properties(unit.ratchet PROPERTIES FIXTURES_CLEANUP unit.counts SKIP_RETURN_CODE 77)'
    (( $# )) && echo "set_tests_properties(${names[*]} PROPERTIES FIXTURES_REQUIRED unit.counts)"
    return 0
}
gen verify-x64-apps "$(apps X64 5 AxlTestB)"
gen verify-aa64-apps "$(apps AARCH64 5 AxlTestB)"
gen verify-aa64-apps-diverged "$(apps AARCH64 4 AxlTestB)"
# The tree registered no unit.AxlTestB: the inventory alone ran.
gen verify-x64-apps-none "$(apps X64 5)"
# A FULL run with the units POOLED: every binary its own test, the ratchet
# whole and green, its budget line after it (judged on x64, not on aa64).
pooled() {   # pooled ARCH BUDGET-LINE [RATCHET-EXIT [RATCHET-FIRST-LINE]]
    local arch="$1" rc="${3:-0}" first="${4:-unit ratchet: 10 assertions across 2 binaries (baseline 10; 0 red)}" x
    t unit.counts-reset unit 0 ""
    for x in AxlTestA AxlTestB; do
        t "unit.$x" "arch:x64;unit" 0 "Results: 5 passed, 0 failed ($arch) in 1s"
    done
    t unit.inventory unit 0 "inventory: clean"
    t unit.ratchet unit "$rc" "$first" "$2" "unit-counts: AxlTestA=5 AxlTestB=5"
    echo 'set_tests_properties(unit.counts-reset PROPERTIES FIXTURES_SETUP unit.counts)'
    echo 'set_tests_properties(unit.ratchet PROPERTIES FIXTURES_CLEANUP unit.counts SKIP_RETURN_CODE 77)'
    echo 'set_tests_properties(unit.AxlTestA unit.AxlTestB PROPERTIES FIXTURES_REQUIRED unit.counts)'
}
gen verify-x64-pooled "$(pooled X64 'unit budget: 61.2 s of pooled guest time across 2 binaries (budget 110 s)')"
gen verify-aa64-pooled "$(pooled AARCH64 'unit budget: not judged -- a judge-only tree (WRITE 0), as its one-boot run skips the budget too')"
# ...the budget NOT judged on x64 (a binary booted after a pool miss);
gen verify-x64-pooled-unjudged "$(pooled X64 'unit budget: not judged -- 1 of 2 binaries ran booted (its guest time includes its own boot)')"
# ...OVER budget: the ratchet fails, and its row must say why;
gen verify-x64-pooled-over "$(pooled X64 'FAIL: unit budget: 120.0 s of pooled guest time across 2 binaries is over the 110 s budget' 1)"
# ...a whole ratchet that printed no budget line at all (an older script);
gen verify-x64-pooled-nobudget "$(pooled X64 'unit-counts: AxlTestA=5 AxlTestB=5')"
# ...--apps naming EVERY binary: whole, booted, so its budget is not judged --
# which the --apps note already says;
gen verify-x64-apps-whole "$(pooled X64 'unit budget: not judged -- 2 of 2 binaries ran booted (its guest time includes its own boot)')"
# ...and a ratchet that judged no whole run (77): a full run that is NOT green.
gen verify-x64-pooled-partial "$(pooled X64 '  no count: AxlTestB' 77 'PARTIAL: 1 of 2 unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run')"
qloop() {   # qloop EXIT -- gate.qloop with its SKIP_RETURN_CODE 3
    t gate.qloop "gate;qloop" "$1" "qloop-verdict: see qloop-verdict.txt"
    echo 'set_tests_properties(gate.qloop PROPERTIES SKIP_RETURN_CODE 3)'
}
gen verify-qloop-judged "$(qloop 0)"
gen verify-qloop-unjudged "$(qloop 3)"
gen verify-qloop-failed "$(qloop 1)"
echo "regenerated: $(cd "$HERE" && ls verify-*.xml | paste -sd' ')"
