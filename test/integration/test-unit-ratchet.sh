#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-unit-ratchet.sh -- the unit RATCHET and INVENTORY when each unit
# binary is its own CTest test (slice 5a step 4, design §10.8).
#
# test-axl.sh runs every unit binary in one boot and judges the total there.
# Under CTest each binary is unit.<App>, writing its count to
# TEST_COUNT_FILE; lib/unit-ratchet.sh, the unit.counts fixture's cleanup,
# sums them against .last-pass-count. It judges only a WHOLE run: a missing
# count means a binary did not run this time (ctest -R one binary), and that
# is reported as not judged (77), never as a pass or a fail. Only a whole,
# green run may raise the baseline, and only when asked (the x64 tree).
#
# The inventory: every built AxlTest*.efi must be one test-axl.sh runs or
# deliberately skips -- the guard test-axl.sh applies when it runs them all,
# which TEST_APPS_ONLY (one binary) bypasses. --check-inventory DIR applies
# it alone, booting nothing.
#
# Host-only, synthetic counts and directories. Boots nothing.
#
# Usage: ./test/integration/test-unit-ratchet.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
R="$SCRIPT_DIR/lib/unit-ratchet.sh"

WORK="$(mktemp -d -t axl-uratchet.XXXXXXXX)"
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

C="$WORK/counts"; B="$WORK/baseline"
counts() {   # counts A=N[:red] B=N ... -> a fresh counts directory, "N green|red" each
    rm -rf "${C:?}"; mkdir -p "$C"
    local kv v st
    for kv in "$@"; do
        v="${kv#*=}"; st=green
        [[ "$v" == *:red ]] && { v="${v%:red}"; st=red; }
        printf '%s %s\n' "$v" "$st" > "$C/${kv%%=*}"
    done
}
run() {   # run WRITE APP... -> "<exit> <first line>"
    local out rc w="$1"; shift
    out=$(TEST_BASELINE_FILE="$B" "$R" "$C" "$w" "$@" 2>&1); rc=$?
    printf '%s %s\n' "$rc" "$(head -1 <<< "$out")"
}

echo "=== the ratchet over per-binary counts ==="
echo 100 > "$B"
counts A=60 B=40
check "a whole run at the baseline: green" "$(run 0 A B | cut -d' ' -f1)" "0"
counts A=60 B=39
check "a whole run below the baseline: FAIL, naming the totals" \
    "$(run 0 A B)" "1 FAIL: expected at least 100 unit assertions but only 99 ran (2 binaries)"
counts A=60 B=45
check "above the baseline, not asked to write: green, the baseline stays" \
    "$(run 0 A B | cut -d' ' -f1) $(cat "$B")" "0 100"
counts A=60 B=45   # the run above consumed them
check "above the baseline, asked to write: the baseline rises" \
    "$(run 1 A B | cut -d' ' -f1) $(cat "$B")" "0 105"
echo 100 > "$B"
counts A=60
check "a binary with no count this run: not judged (77), never a pass" \
    "$(run 1 A B)" "77 PARTIAL: 1 of 2 unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run"
check "...and a partial run never writes the baseline" "$(cat "$B")" "100"
counts A=60 B=junk
check "a count that is not a number: FAIL, naming the binary" \
    "$(run 0 A B)" "1 FAIL: B wrote no count it could read ('junk green')"
echo 100 > "$B"; counts A=60 B=45:red
check "a RED binary's count is still judged (above: green)" "$(run 1 A B | cut -d' ' -f1)" "0"
check "...but a run with a red binary NEVER raises the baseline" "$(cat "$B")" "100"
# AXL_UNIT_NO_WRITE=1 (verify.sh --apps): a WHOLE green run above the
# baseline -- --apps naming every binary is one -- still never writes it, and
# says so: the run claims ".last-pass-count NOT written", so it must not be.
echo 100 > "$B"; counts A=60 B=45
out=$(AXL_UNIT_NO_WRITE=1 TEST_BASELINE_FILE="$B" "$R" "$C" 1 A B 2>&1); rc=$?
check "AXL_UNIT_NO_WRITE: judged, green, NOT written, and said" \
    "$rc $(cat "$B") $(grep -c '^unit ratchet: baseline NOT raised to 105 (AXL_UNIT_NO_WRITE=1: a scoped run never writes it)$' <<< "$out")" "0 100 1"
counts A=60 B=45
out=$(TEST_BASELINE_FILE="$B" "$R" "$C" 1 A B 2>&1)
check "control: the same run without it raises the baseline" "$(cat "$B")" "105"
echo 100 > "$B"
counts A=60 B=45
run 0 A B > /dev/null
check "the counts are CONSUMED once judged (a later partial run cannot reuse them)" \
    "$(ls "$C" | grep -c .)" "0"
counts A=60
check "...so a run that refreshed one count is partial, not whole" \
    "$(run 1 A B)" "77 PARTIAL: 1 of 2 unit binaries left a count this run (the others did not run, or failed before reporting); the ratchet judges only a whole run"
echo garbage > "$B"; counts A=60 B=40
check "an unreadable baseline is a FAILURE, not a pass" \
    "$(run 0 A B)" "1 FAIL: the baseline $B holds no count ('garbage')"
rm -f "$B"; counts A=1 B=2
check "no baseline file yet: green (nothing to hold it to)" "$(run 0 A B | cut -d' ' -f1)" "0"
counts A=1 B=2
check "...and written when asked" "$(run 1 A B | cut -d' ' -f1) $(cat "$B" 2>/dev/null)" "0 3"

echo "=== the wall-clock budget over POOLED per-binary runs ==="
# A pooled unit run writes "<count> <green|red> <guest_ms|-> <pooled|booted>
# <contention>": its binaries' own run time ("-": not measured, a log with no
# timestamps), how it ran, and the host contention factor its run measured.
# When EVERY binary ran pooled and was measured, the guest times are judged
# against the one-boot budget (110 s), as the one boot is: each scaled by its
# own run's max(contention, AXL_UNIT_BUDGET_SCALE), so the figure printed is
# 110 s x the factor those make together. A booted binary's guest time
# includes its own boot, an unmeasured one has none, and the judge-only tree
# (aa64) judges no budget, as its one-boot run does not: each is NOT judged,
# and says so. The old two-field line still reads, as booted.
pooled() {   # pooled A=N:MS[:booted][@C] ... -> counts with guest ms, mode, contention
    rm -rf "${C:?}"; mkdir -p "$C"
    local kv v ms mode c
    for kv in "$@"; do
        v="${kv#*=}"; mode=pooled; c=1.00
        [[ "$v" == *@* ]] && { c="${v#*@}"; v="${v%@*}"; }
        [[ "$v" == *:booted ]] && { v="${v%:booted}"; mode=booted; }
        ms="${v#*:}"; v="${v%%:*}"
        printf '%s green %s %s %s\n' "$v" "$ms" "$mode" "$c" > "$C/${kv%%=*}"
    done
}
bline() { grep -m1 -E '^(FAIL: |unit budget:)' <<< "$1"; }
budget() {   # budget [VAR=VALUE...] -> "<exit> <the budget line, or the first FAIL>", the x64
    # tree (WRITE 1), a scale declared (1) unless overridden; NOSCALE=1: none declared
    local out rc
    if [[ "${NOSCALE:-0}" == 1 ]]; then
        out=$(env -u TEST_MAX_DURATION -u AXL_UNIT_BUDGET_SCALE "$@" TEST_BASELINE_FILE="$B" "$R" "$C" 1 A B 2>&1); rc=$?
    else
        out=$(env -u TEST_MAX_DURATION AXL_UNIT_BUDGET_SCALE=1 "$@" TEST_BASELINE_FILE="$B" "$R" "$C" 1 A B 2>&1); rc=$?
    fi
    printf '%s %s\n' "$rc" "$(bline "$out")"
}
# Both halves pinned with the variable CONTROLLED: CI's release-suite step
# hands its slower runner TEST_MAX_DURATION=190, and an inherited value made
# "the default is 110" read 190 there.
check "the budget is test-axl.sh's own (--budget): 110 by default, a handed one honoured" \
    "$(env -u TEST_MAX_DURATION "$SCRIPT_DIR/test-axl.sh" --budget) $(TEST_MAX_DURATION=190 "$SCRIPT_DIR/test-axl.sh" --budget)" "110 190"
echo 100 > "$B"
pooled A=60:40000 B=40:50000
check "all pooled, under budget: judged, the figure said" "$(budget)" \
    "0 unit budget: 90.0 s of pooled guest time across 2 binaries (budget 110 s)"
pooled A=60:70000 B=40:50000
check "all pooled, OVER budget: a FAIL, the figure said" "$(budget)" \
    "1 FAIL: unit budget: 120.0 s of pooled guest time across 2 binaries is over the 110 s budget"
pooled A=60:70000 B=40:50000
check "...a test-axl.sh budget it is handed is the one it judges (TEST_MAX_DURATION=130)" "$(budget TEST_MAX_DURATION=130)" \
    "0 unit budget: 120.0 s of pooled guest time across 2 binaries (budget 130 s)"
pooled A=60:70000 B=40:50000
check "...the scale verify declares widens it (110 x 1.40 = 154 s)" "$(budget AXL_UNIT_BUDGET_SCALE=1.40)" \
    "0 unit budget: 120.0 s of pooled guest time across 2 binaries (budget 154 s: 110 s x 1.40)"
pooled A=60:60000@2.00 B=40:60000@1.00
check "...and so does the contention each binary's run measured (60/2 + 60/1 = 90 s judged)" "$(budget)" \
    "0 unit budget: 120.0 s of pooled guest time across 2 binaries (budget 146 s: 110 s x 1.33)"
pooled A=60:60000@1.00 B=40:60000@1.00
check "...control: the same times at no contention are over" "$(budget)" \
    "1 FAIL: unit budget: 120.0 s of pooled guest time across 2 binaries is over the 110 s budget"
pooled A=60:60000@2.00 B=40:60000@1.00
check "...the larger of contention and the declared scale, per binary (60/2 + 60/1.40 = 72.9 s judged)" \
    "$(budget AXL_UNIT_BUDGET_SCALE=1.40)" \
    "0 unit budget: 120.0 s of pooled guest time across 2 binaries (budget 182 s: 110 s x 1.65)"
pooled A=60:40000 B=40:900000:booted
check "one booted: NOT judged, and said why" "$(budget)" \
    "0 unit budget: not judged -- 1 of 2 binaries ran booted (its guest time includes its own boot)"
pooled A=60:- B=40:50000
check "one UNMEASURED (no timestamps): NOT judged -- never a 0 s pass" "$(budget)" \
    "0 unit budget: not judged -- 1 of 2 binaries' guest time was not measured (a run log with no timestamps)"
pooled A=60:70000 B=40:50000
out=$(TEST_BASELINE_FILE="$B" "$R" "$C" 0 A B 2>&1); rc=$?
check "the judge-only (aa64) tree judges no budget, as its one-boot run does not" "$rc $(bline "$out")" \
    "0 unit budget: not judged -- a judge-only tree (WRITE 0), as its one-boot run skips the budget too"
counts A=60 B=40
check "the old two-field line still reads, as booted" "$(budget)" \
    "0 unit budget: not judged -- 2 of 2 binaries ran booted (its guest time includes its own boot)"
echo 90 > "$B"; pooled A=60:70000 B=40:50000
check "OVER budget still raises the baseline: every assertion ran and passed" \
    "$(budget | cut -d' ' -f1) $(cat "$B")" "1 100"
pooled A=60:40000 B=40:50000; echo "60 green 40000 leased 1.00" > "$C/A"
check "a mode it does not know: FAIL, naming the binary" "$(budget)" \
    "1 FAIL: A wrote no count it could read ('60 green 40000 leased 1.00')"
pooled A=60:40000@fast B=40:50000
check "a contention that is not a number: FAIL, naming the binary" "$(budget)" \
    "1 FAIL: A wrote no count it could read ('60 green 40000 pooled fast')"
pooled A=60:40000@- B=40:50000
check "a contention its run did not MEASURE (-): NOT judged -- never an idle 1.00" "$(budget)" \
    "0 unit budget: not judged -- 1 of 2 binaries' host contention was not measured"
pooled A=60:70000 B=40:50000
check "no front end declared what the run shared the box with: NOT judged" \
    "$(NOSCALE=1 budget)" \
    "0 unit budget: not judged -- no front end declared what the run shared the box with (AXL_UNIT_BUDGET_SCALE unset)"
for sc in junk 0.5 9; do
    pooled A=60:40000 B=40:50000
    check "a declared scale of '$sc' is refused (2), as the one-boot run refuses it" "$(budget AXL_UNIT_BUDGET_SCALE=$sc)" \
        "2 FAIL: AXL_UNIT_BUDGET_SCALE='$sc' is not a decimal between 1 and 3"
done
pooled A=60:60900 B=40:50000
check "WHOLE SECONDS, as the one-boot run: 110.9 s is not over 110 s" "$(budget)" \
    "0 unit budget: 110.9 s of pooled guest time across 2 binaries (budget 110 s)"
pooled A=60:60960 B=40:50000
check "...and 110.96 s prints 110.9 (truncated, as judged), never '111.0 s' beside a pass" "$(budget)" \
    "0 unit budget: 110.9 s of pooled guest time across 2 binaries (budget 110 s)"
pooled A=60:70000 B=40:50000
check "an EMPTY declared scale is undeclared, as the one-boot wrapper's :- reads it" "$(budget AXL_UNIT_BUDGET_SCALE=)" \
    "0 unit budget: not judged -- no front end declared what the run shared the box with (AXL_UNIT_BUDGET_SCALE unset)"
# 8 + 110213 + 779 ms is 111.000 s exactly; summed as float SECONDS it is
# 110.99999..., which truncates to 110.9 and passes (found by search).
pooled A=1:8 B=1:110213 D=98:779
out=$(env -u TEST_MAX_DURATION AXL_UNIT_BUDGET_SCALE=1 TEST_BASELINE_FILE="$B" "$R" "$C" 1 A B D 2>&1); rc=$?
check "...summed in whole ms: 111.000 s is over, never a float's 110.999" "$rc $(bline "$out")" \
    "1 FAIL: unit budget: 111.0 s of pooled guest time across 3 binaries is over the 110 s budget"
pooled A=60:61000 B=40:50000
check "...and 111.0 s is" "$(budget)" \
    "1 FAIL: unit budget: 111.0 s of pooled guest time across 2 binaries is over the 110 s budget"
echo 100 > "$B"

echo "=== lib/unit-times.awk: each binary's own run time from a timestamped log ==="
# One awk for both readers: the per-binary timing table (common-test.sh) and
# the guest_ms a count file carries (-v total=1: the sum in ms, or "-" when
# nothing was measured or a binary never reached its footer).
UT="$SCRIPT_DIR/lib/unit-times.awk"
esc=$'\e'
printf '%s\n' "[1.000000] ${esc}[1m=== Mem Tests ===${esc}[0m" "[1.100000] PASS: x" \
    "[2.500000] === Results: 3 passed, 0 failed ===" "[2.600000] === Event Loop Tests ===" \
    "[4.850000] ${esc}[32m=== Results: 1 passed, 0 failed ===${esc}[0m" > "$WORK/t1.log"
check "two binaries, ANSI-coloured: each name and time, in order" \
    "$(awk -f "$UT" "$WORK/t1.log" | paste -sd'|')" "$(printf 'Mem\t1.500|Event Loop\t2.250')"
check "...and their sum, in ms" "$(awk -v total=1 -f "$UT" "$WORK/t1.log")" "3750"
printf '%s\n' "[1.000000] === Mem Tests ===" "[2.000000] === Results: 1 passed ===" \
    "[3.000000] === Loop Tests ===" "[3.500000] PASS: y" > "$WORK/t2.log"
check "a header with no footer: that binary is INCOMPLETE (-)" \
    "$(awk -f "$UT" "$WORK/t2.log" | paste -sd'|')" "$(printf 'Mem\t1.000|Loop\t-')"
check "...and the sum is not measured (-), not the part that was" "$(awk -v total=1 -f "$UT" "$WORK/t2.log")" "-"
printf '%s\n' "=== Mem Tests ===" "PASS: x" "=== Results: 1 passed ===" > "$WORK/t3.log"
check "no timestamps: no binaries timed" "$(awk -f "$UT" "$WORK/t3.log" | grep -c .)" "0"
check "...and the sum is not measured (-), never 0" "$(awk -v total=1 -f "$UT" "$WORK/t3.log")" "-"
check "an empty log: not measured (-)" "$(awk -v total=1 -f "$UT" /dev/null)" "-"

echo "=== the per-binary counts, machine-readable, in every outcome ==="
# verify.sh compares the two arches binary by binary, so it can NAME the
# binary that diverged -- under --apps too, where the ratchet is PARTIAL.
ucounts() {   # ucounts WRITE APP... -> the unit-counts line
    TEST_BASELINE_FILE="$B" "$R" "$C" "$@" 2>&1 | grep '^unit-counts:'
}
echo 100 > "$B"; counts A=60 B=40
check "a whole green run prints its counts, in the order named" "$(ucounts 0 A B)" "unit-counts: A=60 B=40"
counts A=60 B=39
check "...and so does a run below the baseline" "$(ucounts 0 A B)" "unit-counts: A=60 B=39"
counts B=40
check "...and a PARTIAL run, for the binaries that reported" "$(ucounts 0 A B)" "unit-counts: B=40"

echo "=== the inventory: every built unit binary is run or deliberately skipped ==="
T="$SCRIPT_DIR/test-axl.sh"
I="$WORK/inv"; mkdir -p "$I"
first=$("$T" --list-apps | head -1); skipped=$("$T" --list-apps-skipped | head -1)
: > "$I/$first.efi"; : > "$I/$skipped.efi"
"$T" --check-inventory "$I" > "$WORK/inv.out" 2>&1; rc=$?
check "listed and skipped binaries only: clean" "$rc" "0"
: > "$I/AxlTestNobodyRunsMe.efi"
"$T" --check-inventory "$I" > "$WORK/inv.out" 2>&1; rc=$?
check "a built binary in neither list: refused, naming it" \
    "$rc $(grep -c 'AxlTestNobodyRunsMe.efi is built but is in neither TEST_APPS nor TEST_APPS_SKIP' "$WORK/inv.out")" "1 1"
"$T" --check-inventory "$WORK/does-not-exist" > "$WORK/inv.out" 2>&1; rc=$?
check "a directory with NO unit binaries is not 'clean' (it saw nothing)" "$rc" "1"

echo "unit-ratchet: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
