#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-ctest-report.sh -- scripts/ctest-report.py, the report helper the
# CTest front ends render their output with (slice 5a step 6, decision 1;
# docs/AXL-Build-System-Design.md §10.8).
#
# It is a pure function of a ctest JUnit file, so it is pinned on FIXTURES
# CAPTURED FROM REAL ctest RUNS (test/integration/fixtures/ctest-report/),
# never hand-written ones -- the schema is ctest's, and a hand-written file
# would encode my belief about it:
#   stubs.xml        five stub integration tests through the real
#                    registration and run-one.sh --retry: pass, fail, a
#                    timeout, a 77 skip, a fail-then-pass flake
#   prep-failed.xml  the prep fixture failing, its dependents Not Run
#
# Exact output, line for line (CLAUDE.md: substrings lie).
#
# Usage: ./test/integration/test-ctest-report.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
R="$PROJECT_DIR/scripts/ctest-report.py"
FX="$SCRIPT_DIR/fixtures/ctest-report"

WORK="$(mktemp -d -t axl-ctreport.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:"; sed 's/^/      |/' <<< "$2"
        echo "    want:"; sed 's/^/      |/' <<< "$3"; FAIL=$((FAIL + 1))
    fi
}

echo "=== runner: the per-test lines and the headline ==="
out=$(python3 "$R" runner --junit "$FX/stubs.xml" --arch X64); rc=$?
check "each test's own run-one line, sorted by name, a failure's excerpt under it; skips count as passed and are named" "$out" \
"  test-cr-fail.sh FAIL 3s (after retry)
    --- test-cr-fail.sh output (last 25 lines) ---
    | stub fail
    --- end test-cr-fail.sh ---
  test-cr-flaky.sh PASS 3s (retry; attempt 1 failed — see test-cr-flaky.sh.attempt1.log)
  test-cr-hang.sh TIMEOUT 9s (after retry; test declares est=1s, limit was 3s)
    --- test-cr-hang.sh output (last 25 lines) ---
    --- end test-cr-hang.sh ---
  test-cr-pass.sh PASS 0s
  test-cr-skip.sh SKIP 0s (SKIP: no widget on this box)

integration: 3 passed, 2 failed, 1 SKIPPED (X64)
    skipped: test-cr-skip.sh"
check "a failure exits 1" "$rc" "1"

echo "=== runner: a fixture that failed is the cause, not 'N failed' ==="
out=$(python3 "$R" runner --junit "$FX/prep-failed.xml" --arch X64); rc=$?
check "the failed fixture is named with its output, and its dependents read NOT RUN" "$out" \
"integration: integration.prep FAILED:
    | pre-build FAILED -- see _prebuild.log
  test-fx-a.sh NOT RUN (Fixture dependency failed: integration.prep)
  test-fx-b.sh NOT RUN (Fixture dependency failed: integration.prep)

integration: 0 passed, 2 failed (X64)"
check "...and exits 1" "$rc" "1"

echo "=== runner: ctest's verdict and the test's own line must agree ==="
# A passing ctest status with a FAIL line (or a line missing altogether) is a
# disagreement, and a disagreement is a FAIL -- never resolved in the test's
# favour.
sed -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>  test-cr-pass.sh FAIL 0s|' \
    "$FX/stubs.xml" > "$WORK/disagree.xml"
out=$(python3 "$R" runner --junit "$WORK/disagree.xml" --arch X64 | grep 'test-cr-pass')
check "status passed + a FAIL line reads FAIL, saying both" "$out" \
"  test-cr-pass.sh FAIL (ctest: passed; the test's own line: test-cr-pass.sh FAIL 0s)"
sed -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>nothing useful|' \
    "$FX/stubs.xml" > "$WORK/noline.xml"
out=$(python3 "$R" runner --junit "$WORK/noline.xml" --arch X64 | grep 'test-cr-pass')
check "no result line at all reads FAIL" "$out" \
"  test-cr-pass.sh FAIL (ctest: passed; the test printed no result line)"

echo "=== runner: run-one's notices about a test are shown with it ==="
# A pool miss is said by run-one ("  <name>.sh: pool miss (...); running it
# booted") inside the test's own output; the runner printed it live, and a
# test that ran booted for a reason should still say so.
sed -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>  test-cr-pass.sh: pool miss (no guest: miss); running it booted\n  test-cr-pass.sh PASS 0s|' \
    "$FX/stubs.xml" > "$WORK/notice.xml"
out=$(python3 "$R" runner --junit "$WORK/notice.xml" --arch X64 | grep 'test-cr-pass')
check "the notice comes first, then the result line" "$out" \
"  test-cr-pass.sh: pool miss (no guest: miss); running it booted
  test-cr-pass.sh PASS 0s"

# ctest's own backstop (TIMEOUT) kills run-one before it prints a line: say
# what ctest said, not merely that the line is missing.
sed -e 's|<testcase name="integration.test-cr-pass" classname="integration.test-cr-pass" time="0.0111433" status="run">|<testcase name="integration.test-cr-pass" classname="integration.test-cr-pass" time="756" status="fail"><failure message="Timeout"/>|' \
    -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>partial output|' "$FX/stubs.xml" > "$WORK/backstop.xml"
out=$(python3 "$R" runner --junit "$WORK/backstop.xml" --arch X64 | grep 'test-cr-pass')
check "a test ctest killed reads FAIL with ctest's reason" "$out" \
"  test-cr-pass.sh FAIL (ctest: failed (Timeout); the test printed no result line)"
# ...but the backstop SIGTERMs run-one first (TIMEOUT_SIGNAL_NAME, design
# §10.15), and run-one, stopped, prints a line of its own: shown as it is,
# with its excerpt -- ctest's Timeout agrees with a FAIL.
sed -e 's|<testcase name="integration.test-cr-pass" classname="integration.test-cr-pass" time="0.0111433" status="run">|<testcase name="integration.test-cr-pass" classname="integration.test-cr-pass" time="759" status="fail"><failure message="Timeout"/>|' \
    -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>  test-cr-pass.sh FAIL 756s (stopped by SIGTERM from outside -- not its own 240s limit, and not a verdict)\n    --- test-cr-pass.sh output (last 25 lines) ---\n    \| KILLED: test-cr-pass.sh received SIGTERM 5s in\n    --- end test-cr-pass.sh ---|' \
    "$FX/stubs.xml" > "$WORK/stopped.xml"
out=$(python3 "$R" runner --junit "$WORK/stopped.xml" --arch X64 \
    | sed -n '/^  test-cr-pass\.sh /,/--- end test-cr-pass\.sh ---/p')
check "a test the backstop's SIGTERM stopped reads run-one's own line and excerpt" "$out" \
"  test-cr-pass.sh FAIL 756s (stopped by SIGTERM from outside -- not its own 240s limit, and not a verdict)
    --- test-cr-pass.sh output (last 25 lines) ---
    | KILLED: test-cr-pass.sh received SIGTERM 5s in
    --- end test-cr-pass.sh ---"
# A result line must be THIS test's: another test's line in its output (a
# nested run, say) is not its result.
sed -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>  test-other.sh PASS 0s|' "$FX/stubs.xml" > "$WORK/foreign.xml"
out=$(python3 "$R" runner --junit "$WORK/foreign.xml" --arch X64 | grep 'test-cr-pass')
check "another test's result line is not this test's" "$out" \
"  test-cr-pass.sh FAIL (ctest: passed; the test printed no result line)"

echo "=== runner: an UNJUDGED test is counted apart, never as a pass ==="
# run-one.sh prints `<name>.sh UNJUDGED` for a 77 that carried an
# `UNJUDGED: <why>` line (a timing test whose guest the host starved); ctest
# saw the 77 as a skip. It is not a pass, not a skip, not a failure: named,
# counted on its own, and the exit is 3 -- nothing failed, not green.
sed -e 's|<system-out>  test-cr-skip.sh SKIP 0s (SKIP: no widget on this box)|<system-out>  test-cr-skip.sh UNJUDGED 0s (after retry; UNJUDGED: 1 timing bound missed while starved)|' \
    "$FX/stubs.xml" > "$WORK/unjudged.xml"
out=$(python3 "$R" runner --junit "$WORK/unjudged.xml" --arch X64 | grep -E 'test-cr-skip|^integration:|^    unjudged:')
check "the UNJUDGED line is shown, counted apart from passes and skips, and named" "$out" \
"  test-cr-skip.sh UNJUDGED 0s (after retry; UNJUDGED: 1 timing bound missed while starved)
integration: 2 passed, 2 failed, 1 UNJUDGED (X64)
    unjudged: test-cr-skip.sh"
sed -e '/<testcase name="integration.test-cr-fail"/,/<\/testcase>/d' \
    -e '/<testcase name="integration.test-cr-hang"/,/<\/testcase>/d' \
    "$WORK/unjudged.xml" > "$WORK/unjudged-only.xml"
out=$(python3 "$R" runner --junit "$WORK/unjudged-only.xml" --arch X64); rc=$?
check "nothing failed but something UNJUDGED: the headline says so" "$(tail -3 <<<"$out")" \
"integration: 2 passed, 0 failed, 1 UNJUDGED (X64)
    unjudged: test-cr-skip.sh
integration: NOT GREEN: UNJUDGED -- test-cr-skip.sh could not judge its timing bounds (the host starved its guest of CPU). Not a pass; re-run it on a quiet box."
check "...and the runner exits 3" "$rc" "3"
# An UNJUDGED line under a PASSING ctest status is a disagreement, like any.
sed -e 's|<system-out>  test-cr-pass.sh PASS 0s|<system-out>  test-cr-pass.sh UNJUDGED 0s|' \
    "$FX/stubs.xml" > "$WORK/uj-disagree.xml"
out=$(python3 "$R" runner --junit "$WORK/uj-disagree.xml" --arch X64 | grep 'test-cr-pass')
check "status passed + an UNJUDGED line reads FAIL, saying both" "$out" \
"  test-cr-pass.sh FAIL (ctest: passed; the test's own line: test-cr-pass.sh UNJUDGED 0s)"
# ...and under a FAILING one: the test's line says UNJUDGED, ctest says it
# failed -- a disagreement, so FAIL.
sed -e 's|<system-out>  test-cr-fail.sh FAIL 3s (after retry)|<system-out>  test-cr-fail.sh UNJUDGED 3s (after retry)|' \
    "$FX/stubs.xml" > "$WORK/uj-failed.xml"
out=$(python3 "$R" runner --junit "$WORK/uj-failed.xml" --arch X64 | grep 'test-cr-fail')
check "status failed + an UNJUDGED line reads FAIL, saying both" "$out" \
"  test-cr-fail.sh FAIL (ctest: failed; the test's own line: test-cr-fail.sh UNJUDGED 3s (after retry))"
echo "=== verdict: one test's judged result, for the quiet re-run ==="
# PASS only when ctest ran it and its own line says PASS; anything else --
# a skip, UNJUDGED, a failure, a test the report does not hold -- is named.
for v in "test-cr-pass.sh|$FX/stubs.xml|PASS|0" \
         "test-cr-skip.sh|$FX/stubs.xml|SKIP|1" \
         "test-cr-skip.sh|$WORK/unjudged.xml|UNJUDGED|1" \
         "test-cr-fail.sh|$FX/stubs.xml|FAIL|1" \
         "test-cr-gone.sh|$FX/stubs.xml|MISSING|1" \
         "test-cr-pass.sh|$WORK/uj-disagree.xml|FAIL|1"; do
    IFS='|' read -r t f want wrc <<<"$v"
    out=$(python3 "$R" verdict --junit "$f" --test "$t"); rc=$?
    check "verdict $t in ${f##*/} -> $want, exit $wrc" "$out|$rc" "$want|$wrc"
done
out=$(python3 "$R" verdict --junit "$WORK/nonexistent.xml" --test test-cr-pass.sh 2>/dev/null); rc=$?
check "verdict of an unreadable file is exit 2" "$out|$rc" "|2"

echo "=== unjudged: the names a release run must re-run alone ==="
out=$(python3 "$R" unjudged --junit "$WORK/unjudged.xml"); rc=$?
check "one script name per UNJUDGED test" "$out|$rc" "test-cr-skip.sh|0"
out=$(python3 "$R" unjudged --junit "$FX/stubs.xml"); rc=$?
check "none: empty, exit 0 (a SKIP is not UNJUDGED)" "$out|$rc" "|0"
out=$(python3 "$R" unjudged --junit "$WORK/nonexistent.xml" 2>/dev/null); rc=$?
check "an unreadable file is exit 2, not an empty answer" "$out|$rc" "|2"

echo "=== runner: every requested test must be in the results ==="
printf '%s\n' integration.test-cr-pass integration.test-cr-gone > "$WORK/expect"
out=$(python3 "$R" runner --junit "$FX/stubs.xml" --arch X64 --expect "$WORK/expect" \
      | grep -E 'test-cr-gone|^integration:')
check "a requested test ctest never ran reads NOT RUN and counts as failed" "$out" \
"  test-cr-gone.sh NOT RUN (ctest never ran it -- not registered?)
integration: 3 passed, 3 failed, 1 SKIPPED (X64)"

echo "=== verify: the table, from the three runs' JUnit files ==="
# verify-*.xml come from real ctest runs of stand-in tests
# (fixtures/ctest-report/make-verify-fixtures.sh): the one-boot unit run (a
# full verify), the unit binaries and their ratchet (--apps), the gates (one
# labelled artifact), lint, docs, qloop.
V="$WORK/verdict.txt"
echo "qloop-verdict: AxlTestLoop alone on x64 (quiet slot): 173 passed, 0 failed (X64) in 27s; the guest's QEMU waited 5 ms for a CPU" > "$V"
vr() {   # vr ARGS... -> output, then "rc=N"
    python3 "$R" verify --logs /logs/here --known-apps 2 "$@" 2>&1; echo "rc=$?"
}
ALL=(--jobs x64,aa64,lint,gates,imggates,qloop,docs --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64.xml")
out=$(vr "${ALL[@]}" --qloop "$FX/verify-qloop-judged.xml" --qloop-verdict "$V")
check "every job green: the table, the cross-arch line, ALL GREEN" "$out" \
"JOB      RC     TIME DETAIL
x64      0        0s 14824 passed, 0 failed, 12 group(s) SKIPPED (84 assertions) (X64) in 105.504s
aa64     0        0s 14841 passed, 0 failed, 16 group(s) SKIPPED (67 assertions) (AARCH64) in 107.490s
lint     0        0s lint: clean (clang-tidy over the x64-lint preset)
gates    0        0s 2 gate(s) clean
imggates 0        0s 1 image gate(s) clean
qloop    0        0s AxlTestLoop alone on x64 (quiet slot): 173 passed, 0 failed (X64) in 27s; the guest's QEMU waited 5 ms for a CPU
docs     0        0s 0 warnings/errors
cross-arch: both 14908  OK
ALL GREEN
rc=0"
out=$(vr --jobs x64,aa64,gates --x64 "$FX/verify-x64-failed.xml" --aa64 "$FX/verify-aa64.xml")
check "a failure names the failed tests, keeps the logs, says SOMETHING FAILED" "$out" \
"JOB      RC     TIME DETAIL
x64      1        0s FAILED: 14823 passed, 1 failed, 12 group(s) SKIPPED (84 assertions) (X64) in 104.100s
aa64     0        0s 14841 passed, 0 failed, 16 group(s) SKIPPED (67 assertions) (AARCH64) in 107.490s
gates    1        0s FAILED: check-b
cross-arch: MISMATCH x64=14907 aa64=14908
logs: /logs/here
SOMETHING FAILED
rc=1"
out=$(vr --jobs=gates --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64-gate-failed.xml" | grep -E '^gates|^rc=')
check "the gates job reads BOTH trees, and names an aa64 gate as one" "$out" \
"gates    1        0s FAILED: aa64:check-c
rc=1"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64-diverged.xml" | grep -E '^cross-arch|^rc=')
check "a cross-arch divergence of the one-boot totals (passed + skipped) fails" "$out" \
"cross-arch: MISMATCH x64=14908 aa64=14907
rc=1"
out=$(vr --jobs x64 --x64 "$FX/verify-x64-truncated.xml" | grep -E '^x64|^rc=')
check "a one-boot run that passed but left no Results line is a FAIL, not green" "$out" \
"x64      1        0s FAILED: unit.one-boot printed no Results line (its output cut short by --test-output-size?)
rc=1"
out=$(vr --jobs x64,aa64 --apps AxlTestB --x64 "$FX/verify-x64-apps.xml" --aa64 "$FX/verify-aa64-apps.xml")
check "--apps: the subset, compared; the ratchet and the budget NOT applied, and said so" "$out" \
"JOB      RC     TIME DETAIL
x64      0        0s 1 of 1 --apps unit binaries passed (the ratchet and the wall-clock budget do not judge a subset)
aa64     0        0s 1 of 1 --apps unit binaries passed (the ratchet and the wall-clock budget do not judge a subset)
cross-arch: both 5  OK (--apps subset only: AxlTestB -- NOT the full suite)
apps: PARTIAL -- x64/aa64 ran ONLY AxlTestB, not all 2 unit binaries. The ratchet and the wall-clock budget were NOT applied, and .last-pass-count was NOT written.
ALL GREEN (partial run -- NOT RUN: lint gates imggates qloop docs; the unit binaries other than AxlTestB; the ratchet and the wall-clock budget (.last-pass-count NOT written))
rc=0"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64-pooled.xml" --aa64 "$FX/verify-aa64-pooled.xml")
check "a full POOLED run: the ratchet AND its budget verdict, judged or not, in the row" "$out" \
"JOB      RC     TIME DETAIL
x64      0        0s unit ratchet: 10 assertions across 2 binaries (baseline 10; 0 red); unit budget: 61.2 s of pooled guest time across 2 binaries (budget 110 s)
aa64     0        0s unit ratchet: 10 assertions across 2 binaries (baseline 10; 0 red); unit budget: not judged -- a judge-only tree (WRITE 0), as its one-boot run skips the budget too
cross-arch: both 10  OK
ALL GREEN (partial run -- NOT RUN: lint gates imggates qloop docs)
rc=0"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64-pooled-unjudged.xml" --aa64 "$FX/verify-aa64-pooled.xml" | tail -2)
check "...the x64 budget NOT judged: the verdict says what the run no longer proves" "$out" \
"ALL GREEN (partial run -- NOT RUN: lint gates imggates qloop docs; the x64 unit budget (not judged -- 1 of 2 binaries ran booted (its guest time includes its own boot)))
rc=0"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64-pooled-nobudget.xml" --aa64 "$FX/verify-aa64-pooled.xml" | tail -2)
check "...a ratchet that printed NO budget line: unjudged, said -- never silently green" "$out" \
"ALL GREEN (partial run -- NOT RUN: lint gates imggates qloop docs; the x64 unit budget (the ratchet printed no budget line))
rc=0"
out=$(vr --jobs x64,aa64 --apps AxlTestA,AxlTestB --x64 "$FX/verify-x64-apps-whole.xml" --aa64 "$FX/verify-aa64-pooled.xml" | tail -2 | head -1)
check "...--apps naming every binary: the --apps note covers the budget, said ONCE" \
    "$(grep -c 'the x64 unit budget' <<< "$out") $(grep -c 'the ratchet and the wall-clock budget' <<< "$out")" "0 1"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64-pooled-over.xml" --aa64 "$FX/verify-aa64-pooled.xml" | grep -E '^x64|^rc=')
check "...OVER budget: the failed ratchet's row quotes its FAIL line" "$out" \
"x64      1        0s FAILED: unit.ratchet -- FAIL: unit budget: 120.0 s of pooled guest time across 2 binaries is over the 110 s budget
rc=1"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64-pooled-partial.xml" --aa64 "$FX/verify-aa64-pooled.xml" | grep -E '^x64|^rc=|GREEN|FAILED$')
check "...a full pooled run whose ratchet judged no whole run is a FAIL, not '2 of 0 --apps'" "$out" \
"x64      1        0s FAILED: the ratchet did not judge a whole run -- PARTIAL: 1 of 2 unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run
SOMETHING FAILED
rc=1"
out=$(vr --jobs x64,aa64 --apps AxlTestB --x64 "$FX/verify-x64-apps.xml" --aa64 "$FX/verify-aa64-apps-diverged.xml" | grep -E '^cross-arch|^rc=')
check "--apps: a per-binary divergence names the binary" "$out" \
"cross-arch: MISMATCH x64=5 aa64=4 (AxlTestB: x64 5, aa64 4)
rc=1"
out=$(vr --jobs x64 --apps AxlTestB --x64 "$FX/verify-x64-apps-none.xml" | grep -E '^x64|^rc=')
check "--apps: fewer binaries ran than were named is a FAIL, not \"0 passed\" green" "$out" \
"x64      1        0s FAILED: 0 of 1 --apps unit binaries ran (not registered in the tree: AxlTestB)
rc=1"
echo "qloop-verdict: UNJUDGED (probe: 4 gaps) -- no 'LOOP-TIMING: judged' line; the box was not quiet; the guest's QEMU waited 5 ms for a CPU" > "$V"
out=$(vr --jobs qloop --qloop "$FX/verify-qloop-unjudged.xml" --qloop-verdict "$V")
check "a skipped gate.qloop is UNJUDGED -- not green, exit 3 -- with its verdict" "$out" \
"JOB      RC     TIME DETAIL
qloop    UNJ      0s UNJUDGED (probe: 4 gaps) -- no 'LOOP-TIMING: judged' line; the box was not quiet; the guest's QEMU waited 5 ms for a CPU
cross-arch: not compared (both arches were not run)
logs: /logs/here
NOT GREEN: UNJUDGED -- qloop: UNJUDGED (probe: 4 gaps) -- no 'LOOP-TIMING: judged' line; the box was not quiet; the guest's QEMU waited 5 ms for a CPU. Re-run --only=qloop on a quiet box.
rc=3"
out=$(vr --jobs qloop --qloop "$FX/verify-qloop-unjudged.xml" | grep -E '^qloop|^rc=')
check "...but a skip with NO fresh verdict is a FAIL: it did not judge, and nothing says why" "$out" \
"qloop    1        0s FAILED: gate.qloop was skipped and left no verdict
rc=1"
# ctest's OWN status is a cross-check: a fixture the table never shows (the
# temp base's cleanup) can fail with every row green. The runner checks it;
# verify must too. A run whose status was never collected is not a pass.
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64.xml" \
         --ctest-rc x64=8 --ctest-rc aa64=0 | grep -E '^ctest|^logs|FAILED$|GREEN|^rc=')
check "every row green but a ctest exited non-zero: FAILED, the run named" "$out" \
"ctest: the x64 run exited 8 although every row above passed -- a test or fixture outside the table failed (see x64-ctest.txt in the logs)
logs: /logs/here
SOMETHING FAILED
rc=1"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64.xml" \
         --ctest-rc x64=0 --ctest-rc 'aa64=?' | grep -E '^ctest|FAILED$|GREEN|^rc=')
check "a run whose ctest status was never collected is a FAIL" "$out" \
"ctest: the aa64 run left no exit status -- it was not waited for
SOMETHING FAILED
rc=1"
out=$(vr --jobs x64,aa64 --x64 "$FX/verify-x64.xml" --aa64 "$FX/verify-aa64.xml" \
         --ctest-rc x64=0 --ctest-rc aa64=0 | grep -E '^ctest|GREEN|^rc=')
check "control: zero statuses and green rows are ALL GREEN" "$out" \
"ALL GREEN (partial run -- NOT RUN: lint gates imggates qloop docs)
rc=0"
echo "qloop-verdict: FAILED: 1 passed, 1 failed (X64) in 3s; the guest's QEMU waited 6 ms for a CPU" > "$V"
out=$(vr --jobs qloop --qloop "$FX/verify-qloop-failed.xml" --qloop-verdict "$V" | grep -E '^qloop|^rc=|FAILED$')
check "a failed gate.qloop is a FAIL row carrying its verdict" "$out" \
"qloop    1        0s FAILED: 1 passed, 1 failed (X64) in 3s; the guest's QEMU waited 6 ms for a CPU
SOMETHING FAILED
rc=1"
out=$(vr --jobs gates --x64 "$FX/verify-x64.xml" | grep -E '^ALL GREEN')
check "a partial run names what it did NOT run" "$out" \
"ALL GREEN (partial run -- NOT RUN: x64 aa64 lint imggates qloop docs)"

echo "=== refusals ==="
o=$(python3 "$R" verify --jobs make --x64 "$FX/verify-x64.xml" 2>&1); rc=$?
check "an unknown job (make, before 5.0.0's rename) is exit 2, not a green partial run" "$rc $o" \
    "2 ctest-report: unknown job(s) make (known: x64 aa64 lint gates imggates qloop docs)"
python3 "$R" runner --junit "$WORK/no-such.xml" --arch X64 > "$WORK/o" 2>&1; rc=$?
check "an unreadable JUnit file is exit 2, named" "$rc $(grep -c 'cannot read' "$WORK/o")" "2 1"
printf '<testsuite name="(empty)" tests="0"></testsuite>\n' > "$WORK/empty.xml"
python3 "$R" runner --junit "$WORK/empty.xml" --arch X64 > "$WORK/o" 2>&1; rc=$?
check "a run with no integration test in it is exit 2, not a clean 0 passed" \
    "$rc $(grep -c 'no integration test' "$WORK/o")" "2 1"

echo ""
echo "ctest-report: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
