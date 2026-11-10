#!/bin/bash
# test-meta: arch=none needs=python3 est=2 local-only=0
# test-timing-report.sh -- lib/timing-report.py reads a run's log directory
# and prints where each test's wall time went, EXACTLY as
# fixtures/timing-report/expected.txt says.
#
# The fixture logs are written in the TIMING line formats that common-test.sh,
# scripts/run-qemu.sh, lib/run-one.sh and `axlpool run` produce (see the
# "Wall-time accounting" block in scripts/axl-common.sh). They cover: booted
# foreground and background runs, a run with no boot-ready reading (the BOOT
# column becomes a floor), a pooled run with its lease and release, a test
# reached only through run-qemu.sh, run-qemu.sh's own pooled runs
# (mode=pooled: counted pooled, not booted), a nested test's lines inside its caller's
# log, a retry and a pool miss (their earlier attempts are counted, not
# tabled), a skip, a cached test, a host-only test, and three that recorded
# no guest timing -- listed by name, never dropped: the report is a census.
#
# Host-only.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FX="$SCRIPT_DIR/fixtures/timing-report"
set -uo pipefail

PASS=0 FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== timing-report.py over fixture logs ==="
rc=0
out=$(cd "$FX" && python3 "$SCRIPT_DIR/lib/timing-report.py" --tests-dir tests logs 2>&1) || rc=$?
if [[ $rc -eq 0 && "$out" == "$(cat "$FX/expected.txt")" ]]; then
    pass "the report is exactly expected.txt"
else
    fail "the report differs from expected.txt (exit $rc):"
    diff <(printf '%s\n' "$out") "$FX/expected.txt" | head -40 | sed 's/^/      /'
fi

# --top N keeps the table to the N dearest tests; the SUM and the census below
# it still cover every test.
out=$(cd "$FX" && python3 "$SCRIPT_DIR/lib/timing-report.py" --tests-dir tests --top 2 logs 2>&1)
want=$(grep -vE '^ +[0-9.]+ .* test-(miss|nested|retry|pooled|rqpool|direct|vialib|host|skip)\.sh$' "$FX/expected.txt")
if [[ "$out" == "$want" ]]; then
    pass "--top 2: the two dearest rows, the same SUM and census"
else
    fail "--top 2 differs:"
    diff <(printf '%s\n' "$out") <(printf '%s\n' "$want") | head -20 | sed 's/^/      /'
fi

# --sort cpu orders the rows by CPU (a log with none last, by wall): on a
# CPU-bound run that is what a test costs the others.
out=$(cd "$FX" && python3 "$SCRIPT_DIR/lib/timing-report.py" --tests-dir tests --sort cpu logs 2>&1)
got=$(awk '/^ +TOTAL /{t=1; next} t && /SUM over/{exit} t {print $NF}' <<<"$out" | paste -sd' ')
want="test-rq.sh test-boot-fg.sh test-retry.sh test-miss.sh test-nested.sh test-direct.sh test-rqpool.sh test-host.sh test-pooled.sh test-skip.sh test-vialib.sh"
if [[ "$got" == "$want" ]]; then
    pass "--sort cpu: rows by CPU (earlier attempts summed), the one with no reading last"
else
    fail "--sort cpu order: [$got]"
fi

# A directory that is not a log directory is an error, never an empty report.
rc=0
out=$(python3 "$SCRIPT_DIR/lib/timing-report.py" "$FX/no-such-dir" 2>&1) || rc=$?
if [[ $rc -eq 2 && "$out" == "timing-report: $FX/no-such-dir: not a directory" ]]; then
    pass "a missing directory exits 2, saying so"
else
    fail "a missing directory: exit $rc, [$out]"
fi

echo ""
echo "timing-report: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
