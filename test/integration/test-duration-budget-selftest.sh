#!/bin/bash
# test-duration-budget-selftest.sh -- host-only checks (no QEMU) for the suite's
# wall-clock budget (TEST_MAX_DURATION) and hang timeout in common-test.sh.
# selftest-schedule: exclusive -- it samples the LIVE host load and asserts an
# idle box reads x1.00 (scripts/run-selftests.sh runs it alone, first).
#
# THE PROBLEM. The budget exists to notice a slow test. Wall time cannot tell
# that from a busy box: the x64 unit suite is 75 s alone and 115-124 s beside
# verify.sh's other five jobs (host load 19-21 on 12 cores), so a fixed 95 s
# budget failed every full verify.sh, and the 120 s hang timeout cut the run
# short. The budget now scales by the host's measured oversubscription, and the
# hang timeout by TEST_TIMEOUT_SCALE, which verify.sh declares.
#
# WHAT HAS TO HOLD, each with a control so "never fires" cannot pass:
#   1. idle box, run over budget          -> FAILS            (the check can fail)
#   2. loaded box, same run               -> passes, and the
#      line says the budget was scaled                         (the scaling works)
#   3. loaded box, run far over even the
#      scaled budget                      -> FAILS            (a slow test still trips)
#   4. TEST_TIMEOUT_SCALE widens the hang limit; without it the same run is
#      TRUNCATED                                              (and it can still fire)
# The guest is `bash -c 'sleep N'`: its duration is wall-clock by construction,
# so load cannot make it faster or slower and every verdict comes from the
# harness's arithmetic, not from timing luck.
# Excluded from discovery by name (*-selftest.sh); run by run-selftests.sh.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
PROJECT_DIR="$(cd ../.. && pwd)"
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=$((fail + 1)); }

work=$(mktemp -d)
SPIN=()
stop_spin() {
    local p
    for p in ${SPIN[@]+"${SPIN[@]}"}; do kill "$p" 2>/dev/null; done
    for p in ${SPIN[@]+"${SPIN[@]}"}; do wait "$p" 2>/dev/null; done
    SPIN=()
}
cleanup() { stop_spin; rm -rf "$work"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "=== duration budget self-test ==="

# --- the arithmetic, on fixed inputs --------------------------------------
echo "12 6000" > "$work/s1"       # mean 12 runnable on 12 cpus
echo "24 1" > "$work/s2"          # mean 24 on 12 cpus
echo "1000 1" > "$work/s3"        # absurd, capped
echo "30 1" > "$work/s5"          # 2.5x oversubscribed
: > "$work/s4"                    # no samples
out=$(host_contention_factor "$work/s1" 12 3); [[ "$out" == "1.00" ]] \
    && pass "mean 12 runnable on 12 cpus reads 1.00 (not oversubscribed)" \
    || bad "mean 12 on 12 cpus read '$out', expected 1.00"
out=$(host_contention_factor "$work/s2" 12 3); [[ "$out" == "2.00" ]] \
    && pass "mean 24 runnable on 12 cpus reads 2.00" \
    || bad "mean 24 on 12 cpus read '$out', expected 2.00"
out=$(host_contention_factor "$work/s5" 12 3); [[ "$out" == "2.50" ]] \
    && pass "2.5x oversubscribed reads 2.50" \
    || bad "2.5x oversubscribed read '$out', expected 2.50"
out=$(host_contention_factor "$work/s3" 12 3); [[ "$out" == "3.00" ]] \
    && pass "an absurd load is capped at 3.00, not allowed to cancel the budget" \
    || bad "uncapped: read '$out', expected 3.00"
out=$(host_contention_factor "$work/s2" 48 3); [[ "$out" == "1.00" ]] \
    && pass "CONTROL: the same load on 48 cpus reads 1.00 (the factor is relative to cpus)" \
    || bad "CONTROL: 24 runnable on 48 cpus read '$out', expected 1.00"
out=$(host_contention_factor "$work/s4" 12 3 2>&1); rc=$?
[[ $rc -ne 0 && -z "$out" ]] \
    && pass "no samples: fails and prints nothing (never an implied 1.00)" \
    || bad "no samples: rc=$rc out='$out'"
out=$(host_contention_factor "$work/missing" 12 3 2>&1); rc=$?
[[ $rc -ne 0 && -z "$out" ]] \
    && pass "no sample file: fails and prints nothing" \
    || bad "no sample file: rc=$rc out='$out'"

# The default cpu count is the HOST's, matching procs_running's scope: nproc
# shrinks under taskset/cpusets and would inflate the factor toward the cap.
host_cpus=$(getconf _NPROCESSORS_ONLN)
echo "$host_cpus 1" > "$work/s6"
out=$(host_contention_factor "$work/s6"); [[ "$out" == "1.00" ]] \
    && pass "default cpu count is the host's: $host_cpus runnable on $host_cpus cpus reads 1.00" \
    || bad "default cpus: '$out', expected 1.00 for $host_cpus runnable on $host_cpus cpus"
if command -v taskset >/dev/null; then
    echo "$((host_cpus * 2)) 1" > "$work/s7"
    out=$(taskset -c 0 bash -c 'source "$1"; host_contention_factor "$2"' _ \
          "$PROJECT_DIR/scripts/axl-common.sh" "$work/s7")
    [[ "$out" == "2.00" ]] \
        && pass "CONTROL: pinned to one cpu (nproc = 1) the factor is still relative to the host: $out" \
        || bad "pinned to one cpu the factor read '$out', expected 2.00 (nproc leaked in)"
fi

# --- the probe: the REAL test_run_foreground + test_count_results ----------
# The guest is a COPY OF bash named qemu-system-stub, with this test's disk path
# in its argv: _test_qemu_pid finds a guest by exactly that (pgrep on the disk
# path, then a qemu-system* comm), and without it the load sampler has nothing
# to follow. $4=cut leaves Beta without a Results footer, so a hang limit that
# fires has an in-flight binary to name.
cp "$(command -v bash)" "$work/qemu-system-stub"
cat > "$work/probe.sh" <<PROBE
#!/bin/bash
source "$PWD/common-test.sh"
TEST_ARCH=X64
export TEST_SKIP_LEAK_GATE=1
export TEST_BASELINE_FILE="$work/baseline"
export TEST_MAX_DURATION="\$1"
test_setup
[[ -n "\${PROBE_SETUP_SLEEP:-}" ]] && sleep "\$PROBE_SETUP_SLEEP"
_body='printf "=== Alpha Tests ===\nPASS: a\n=== Results: 1 passed ===\n"'
[[ "\${4:-}" == cut ]] && _body='printf "=== Alpha Tests ===\nPASS: a\n=== Results: 1 passed ===\n=== Beta Tests ===\nPASS: b\n"'
TEST_QEMU_CMD=("$work/qemu-system-stub" -c "\$_body; sleep \$2; :" "\$TEST_DISK")
test_run_foreground "\${3:-30}"
test_count_results
PROBE
chmod +x "$work/probe.sh"

NCPU=$(getconf _NPROCESSORS_ONLN)
# Niced to the bottom: they still count in procs_running (the factor rises) but
# do not starve test_setup's image build, CI, or anyone else's work on a shared
# box for the ~20 s this test needs them.
start_spin() {   # oversubscribe the box ~6x (factor caps at 3.00)
    local i
    for ((i = 0; i < NCPU * 6; i++)); do
        nice -n 19 bash -c 'while :; do :; done' &
        SPIN+=("$!")
    done
}
factor_of() { sed -n 's/^Timing:.*host contention x\([0-9.]*\).*/\1/p' <<<"$1"; }
# budget_for <max> <factor> -- the budget the harness derives (same rounding).
budget_for() { awk -v b="$1" -v f="$2" 'BEGIN { printf "%d", b * f + 0.5 }'; }

# 1. a run over budget FAILS. The expected budget is DERIVED from the factor the
#    run measured (a shared box reads 1.25+ easily, and int(2 x 1.25 + .5) is
#    3, not 2), and the guest sleeps long enough to exceed it. Above 2.5 the
#    derived budget outgrows the guest and the control says so.
out=$(bash "$work/probe.sh" 2 6 2>&1); rc=$?
f=$(factor_of "$out")
B=$(budget_for 2 "${f:-1}")
if awk -v f="${f:-1}" 'BEGIN { exit !(f < 2.5) }'; then
    if [[ $rc -ne 0 ]] && grep -qE "FAIL: guest took [0-9]+\.[0-9]{3}s, over the ${B}s budget \(2s x host contention ${f}, declared scale 1\)" <<<"$out"; then
        pass "CONTROL: a 6 s run against a 2 s budget FAILS at the derived budget ${B}s (x$f)"
    else
        bad "CONTROL: 6 s run, derived budget ${B}s (x$f) did not fail as derived (rc=$rc): $out"
    fi
else
    echo "  SKIP: box too loaded (x$f) for the over-budget control"
fi

# 1b. THE IDLE PATH IS NOT STRICTER THAN IT WAS. The old check compared whole
#    seconds (`elapsed_s > MAX`), so a 3.4 s run passes a 3 s budget (the sleep is 3.4 so the run sits mid-way in the 3-4 s window, 0.6 s from either edge); comparing
#    milliseconds would fail it. Judged only at factor 1.00, where the budget is
#    the MAX itself. The message must carry milliseconds, so "took 3s, over the
#    3s budget" can never be printed.
out=$(bash "$work/probe.sh" 3 3.4 2>&1); rc=$?
f=$(factor_of "$out")
if [[ "$f" == "1.00" ]]; then
    if grep -q 'FAIL: guest took' <<<"$out"; then
        bad "a 3.4 s run FAILED a 3 s budget at x1.00 -- stricter than the whole-second check: $out"
    else
        pass "idle path: a 3.4 s run (plus setup) passes a 3 s budget (whole-second comparison, as before)"
    fi
else
    echo "  SKIP: box not idle (x$f) -- the whole-second control needs factor 1.00"
fi

# 1d. A DECLARED SCALE (TEST_BUDGET_SCALE, from verify.sh) covers contention the
#    runnable-task ratio cannot see: under verify.sh the guest ran 1.4-1.5x its
#    idle time with that ratio at 1.00. The same 3.4 s run that FAILS a 2 s
#    budget unscaled must pass at scale 2 (budget 4 s); a 9 s run must still fail
#    at scale 2; the budget is the LARGER of the measured and declared factors,
#    not their product (the failure line shows the budget 4 s at x1.00).
out=$(bash "$work/probe.sh" 2 3.4 2>&1); rc=$?
f0=$(factor_of "$out")
if awk -v f="${f0:-1}" 'BEGIN { exit !(f < 1.5) }'; then
    if [[ $rc -ne 0 ]] && grep -q 'FAIL: guest took' <<<"$out"; then
        pass "CONTROL: a 3.4 s run fails a 2 s budget with no declared scale (x$f0)"
    else
        bad "CONTROL: unscaled 3.4 s run against 2 s did not fail (rc=$rc x$f0): $out"
    fi
    out=$(TEST_BUDGET_SCALE=2 bash "$work/probe.sh" 2 3.4 2>&1); rc=$?
    if [[ $rc -eq 0 ]] && ! grep -q 'FAIL: guest took' <<<"$out" \
        && grep -q 'declared budget scale x2' <<<"$out"; then
        pass "TEST_BUDGET_SCALE=2 lets the same run pass, and the Timing line says why"
    else
        bad "TEST_BUDGET_SCALE=2 did not widen the budget (rc=$rc): $out"
    fi
    out=$(TEST_BUDGET_SCALE=2 bash "$work/probe.sh" 2 9 2>&1); rc=$?
    f=$(factor_of "$out")
    if [[ $rc -ne 0 ]] && grep -qE "over the $(budget_for 2 2)s budget \(2s x host contention ${f}, declared scale 2\)" <<<"$out"; then
        pass "at scale 2 a 9 s run still fails (slow guest is not forgiven), budget = max(factor, scale) x MAX"
    else
        bad "scaled 9 s run did not fail as derived (rc=$rc x$f): $out"
    fi
else
    echo "  SKIP: box not idle (x$f0) -- the declared-scale controls need factor < 1.5"
fi
for bad_scale in 0 0.5 "" junk -1 1.2.3 ".5" 3.01 4 50; do
    out=$(env TEST_BUDGET_SCALE="$bad_scale" bash "$work/probe.sh" 99999 1 2>&1); rc=$?
    if [[ $rc -eq 2 ]] && grep -qF "TEST_BUDGET_SCALE='$bad_scale' is not a decimal between 1 and 3" <<<"$out" \
        && ! grep -qF 'Results:' <<<"$out"; then
        pass "TEST_BUDGET_SCALE='$bad_scale' is refused (rc 2, named, nothing ran)"
    else
        bad "TEST_BUDGET_SCALE='$bad_scale' was not refused (rc=$rc): $out"
    fi
done

# The cap is inclusive: 3 (the measured factor's own cap) is accepted.
out=$(TEST_BUDGET_SCALE=3 bash "$work/probe.sh" 99999 1 2>&1); rc=$?
if [[ $rc -eq 0 ]] && grep -qF 'declared budget scale x3' <<<"$out"; then
    pass "TEST_BUDGET_SCALE=3 (the cap) is accepted"
else
    bad "TEST_BUDGET_SCALE=3 was refused or mis-reported (rc=$rc): $out"
fi

# 1c. THE BUDGET GRADES THE GUEST, NOT SETUP. Setup is the image build, and a
#    cold build (after a merge, a clean tree) took 64 s of a 158 s total while
#    the guest took 93 s -- which failed a 95 s budget that the guest alone
#    met. A 6 s setup beside a 0.5 s guest against a 2 s budget must PASS (the
#    derived budget stays under 6 s below x2.5); the 6 s case above is the
#    control that a slow GUEST still fails. Setup stays on the Timing line.
out=$(PROBE_SETUP_SLEEP=6 bash "$work/probe.sh" 2 0.5 2>&1); rc=$?
f=$(factor_of "$out")
if awk -v f="${f:-1}" 'BEGIN { exit !(f < 2.5) }'; then
    if [[ $rc -eq 0 ]] && ! grep -q 'FAIL: guest took' <<<"$out" \
        && grep -qE 'Timing: 6\.[0-9]{3}s setup \+ 0\.[0-9]{3}s guest' <<<"$out"; then
        pass "a 6 s setup with a 0.5 s guest passes a 2 s budget, and the Timing line still shows the setup"
    else
        bad "slow setup was graded against the guest budget (rc=$rc x$f): $out"
    fi
else
    echo "  SKIP: box too loaded (x$f) for the slow-setup control"
fi

# 2 and 3. oversubscribed: the factor rises, the budget is scaled, and the
#    verdict follows the SCALED number.
start_spin
sleep 1
out=$(bash "$work/probe.sh" 2 3 2>&1); rc=$?
f=$(factor_of "$out")
B=$(budget_for 2 "${f:-1}")
if awk -v f="${f:-1}" 'BEGIN { exit !(f >= 2.0) }'; then
    pass "oversubscribing the box 6x raises the measured factor (x$f)"
else
    bad "6x oversubscription read factor x${f:-?}, expected >= 2.0: $out"
fi
# The run is 3 s of sleep plus setup; judged against the DERIVED budget, so a
# slow image build cannot fail it unless it really exceeds ${B}s.
if [[ $rc -eq 0 ]] && ! grep -q 'FAIL: guest took' <<<"$out"; then
    pass "loaded box: a 3 s run against a 2 s budget passes once the budget scales to ${B}s (x$f)"
else
    bad "loaded box: the scaled budget (${B}s, x$f) did not absorb a 3 s run (rc=$rc): $out"
fi
out=$(bash "$work/probe.sh" 2 9 2>&1); rc=$?
f=$(factor_of "$out")
if [[ $rc -ne 0 ]] && grep -qE 'FAIL: guest took [0-9]+\.[0-9]{3}s, over the [0-9]+s budget \(2s x host contention' <<<"$out"; then
    pass "loaded box: a 9 s run still trips the scaled budget (x$f) -- a slow test is not forgiven"
else
    bad "loaded box: a 9 s run against a 2 s budget (x$f) did not fail (rc=$rc): $out"
fi
# The budget takes the LARGER of the measured and declared factors, not their
# product: with the box oversubscribed (measured x3.00, capped) and a declared
# scale of 2, a 9 s run against a 2 s budget fails at 6 s, where a product (12 s)
# would let it through.
out=$(TEST_BUDGET_SCALE=2 bash "$work/probe.sh" 2 9 2>&1); rc=$?
f=$(factor_of "$out")
if awk -v f="${f:-1}" 'BEGIN { exit !(f > 2.0) }'; then
    if [[ $rc -ne 0 ]] && grep -qE "over the $(budget_for 2 "$f")s budget" <<<"$out"; then
        pass "loaded box + declared scale 2: the budget is max(x$f, 2) x MAX, not the product"
    else
        bad "loaded box + scale 2: a 9 s run was not judged at max() (x$f rc=$rc): $out"
    fi
else
    bad "loaded box did not read above x2 for the max-vs-product control (x$f): $out"
fi
stop_spin

# 4. the hang timeout.
out=$(bash "$work/probe.sh" 99999 3 1 cut 2>&1)
grep -qF "TRUNCATED: the harness's own 1s timeout fired" <<<"$out" \
    && pass "CONTROL: a 3 s run under a 1 s hang limit is TRUNCATED" \
    || bad "CONTROL: the 1 s hang limit did not fire on a 3 s run: $out"
# POSITIVE: the run reached the end and reported the in-flight binary the way an
# un-truncated stall does. Absence of TRUNCATED alone is satisfied by a probe
# that crashed, which is what a malformed scale used to do.
out=$(TEST_TIMEOUT_SCALE=5 bash "$work/probe.sh" 99999 3 1 cut 2>&1)
if grep -qF '*** STALLED' <<<"$out" && grep -qF 'Results:' <<<"$out" \
    && ! grep -qF 'TRUNCATED' <<<"$out"; then
    pass "TEST_TIMEOUT_SCALE=5 widens the 1 s hang limit to 5 s (the guest ends on its own: STALLED, not TRUNCATED)"
else
    bad "TEST_TIMEOUT_SCALE=5 did not widen a 1 s hang limit: $out"
fi
# Bad scales are REFUSED, loudly, before any guest runs. 0 would make
# `timeout 0` (no limit at all); 1.5 is an arithmetic error that aborts the
# script; empty and junk are not numbers.
for bad_scale in 0 1.5 "" junk -2 "3 4"; do
    out=$(env TEST_TIMEOUT_SCALE="$bad_scale" bash "$work/probe.sh" 99999 3 1 cut 2>&1); rc=$?
    if [[ $rc -eq 2 ]] && grep -qF "TEST_TIMEOUT_SCALE='$bad_scale' is not a positive integer" <<<"$out" \
        && ! grep -qF 'Results:' <<<"$out"; then
        pass "TEST_TIMEOUT_SCALE='$bad_scale' is refused (rc 2, named, nothing ran)"
    else
        bad "TEST_TIMEOUT_SCALE='$bad_scale' was not refused (rc=$rc): $out"
    fi
done

# 5. the load sampler runs only when there is a budget to scale. Without
# TEST_MAX_DURATION nothing reads it, and the pool would pay ~50 awk forks a
# second for it.
out=$(bash "$work/probe.sh" "" 1 2>&1)
if grep -qF 'host load not captured' <<<"$out"; then
    pass "no TEST_MAX_DURATION: no load sampler (the Timing line says so)"
else
    bad "a run with no budget still sampled the host: $out"
fi
out=$(bash "$work/probe.sh" 99999 1 2>&1)
if grep -qE 'host contention x[0-9.]+$' <<<"$out"; then
    pass "CONTROL: with a budget the sampler runs and the Timing line has no caveat"
else
    bad "CONTROL: a run with a budget did not sample the host: $out"
fi

# 6. WAITS. test_wait_boot (a wait that covers the guest's boot) is scaled like the
#    foreground limit -- aarch64 +60 s, then TEST_TIMEOUT_SCALE -- because
#    aarch64 TCG stalls at the firmware banner under load and a hard-coded 60 s
#    there is a flake. test_wait_for (a BOUNDED wait on a guest that is already
#    up) is used EXACTLY as given: the bound is often the assertion
#    (test-http-rebind-storm: FREE-DONE within 8 s IS its "returns bounded"
#    check), and the +60 once leaked into it, loosening 8 s to 68 s on aarch64.
scaled() {   # scaled <arch> <seconds> [scale]
    TEST_TIMEOUT_SCALE="${3-1}" bash -c \
        "source '$PWD/common-test.sh'; TEST_ARCH=$1; test_scale_timeout $2; echo \$TEST_SCALED_S"
}
[[ "$(scaled X64 60)" == 60 ]] && pass "x64: a 60 s boot allowance stays 60 s" \
    || bad "x64 60 s wait scaled to '$(scaled X64 60)'"
[[ "$(scaled AARCH64 60)" == 120 ]] && pass "aarch64: a 60 s boot allowance becomes 120 s" \
    || bad "aarch64 60 s wait scaled to '$(scaled AARCH64 60)', expected 120"
[[ "$(scaled AARCH64 60 2)" == 240 ]] && pass "aarch64 under TEST_TIMEOUT_SCALE=2: 240 s" \
    || bad "aarch64 scaled x2 gave '$(scaled AARCH64 60 2)', expected 240"
# The limit each entry point USES, read back from TEST_WAITED_S with the pattern
# already in the log (so nothing actually waits).
waited() {   # waited <fn> <arch> <seconds> [scale]
    TEST_TIMEOUT_SCALE="${4-1}" bash -c \
        "source '$PWD/common-test.sh'; TEST_ARCH=$2; test_setup; echo HERE > \"\$TEST_LOG\"; $1 HERE $3; echo \$TEST_WAITED_S" 2>&1 | tail -1
}
[[ "$(waited test_wait_for AARCH64 8)" == 8 ]] \
    && pass "a bounded 8 s wait stays 8 s on aarch64" \
    || bad "bounded 8 s wait on aarch64 became '$(waited test_wait_for AARCH64 8)'"
[[ "$(waited test_wait_for AARCH64 8 3)" == 8 ]] \
    && pass "a bounded 8 s wait is not widened by TEST_TIMEOUT_SCALE either" \
    || bad "bounded 8 s wait under scale 3 became '$(waited test_wait_for AARCH64 8 3)'"
[[ "$(waited test_wait_boot AARCH64 8)" == 68 ]] \
    && pass "CONTROL: the same 8 s as a BOOT wait gets the aarch64 allowance (68 s)" \
    || bad "boot 8 s wait on aarch64 gave '$(waited test_wait_boot AARCH64 8)', expected 68"
[[ "$(waited test_wait_boot X64 8)" == 8 ]] \
    && pass "a boot wait on x64 is unchanged" \
    || bad "boot 8 s wait on x64 gave '$(waited test_wait_boot X64 8)'"
cat > "$work/wait.sh" <<WAIT
#!/bin/bash
source "$PWD/common-test.sh"
TEST_ARCH=X64
test_setup
: > "\$TEST_LOG"
t0=\$(date +%s%N)
\$1 NEVER-APPEARS 1 && echo found
echo "waited_ms \$(( (\$(date +%s%N) - t0) / 1000000 ))"
WAIT
# In MILLISECONDS: bash's SECONDS truncates time(), so a 1.3 s wait (one
# sleep 1, the grep, the timeout's own report) read "waited 2" whenever it
# crossed a second boundary -- it failed CI under load. What the check
# distinguishes is bounded (~1 s) from scaled x3 (>= 3 s), so the bar is
# [1000, 2500) ms.
out=$(TEST_TIMEOUT_SCALE=3 bash "$work/wait.sh" test_wait_for 2>&1 | grep -a '^waited_ms')
ms=${out#waited_ms }
[[ "$ms" =~ ^[0-9]+$ ]] && (( ms >= 1000 && ms < 2500 )) \
    && pass "CONTROL: a bounded 1 s wait gives up after ~1 s (${ms} ms) even under TEST_TIMEOUT_SCALE=3" \
    || bad "bounded 1 s wait under scale 3: '$out', expected 1000-2499 ms (x3 would be >= 3000)"
out=$(TEST_TIMEOUT_SCALE=3 bash "$work/wait.sh" test_wait_boot 2>&1 | grep -a '^waited_ms')
ms=${out#waited_ms }
[[ "$ms" =~ ^[0-9]+$ ]] && (( ms >= 3000 && ms < 4500 )) \
    && pass "test_wait_boot honours TEST_TIMEOUT_SCALE=3 (waits ~3 s: ${ms} ms)" \
    || bad "scaled 1 s boot wait: '$out', expected 3000-4499 ms"

# 7. A boot wait's failure text names the limit it ACTUALLY used. Hard-coded
#    "within 60s" is wrong the moment the wait is scaled (aarch64 waited 120 s).
#    Every test_wait_boot caller's next lines must use ${TEST_WAITED_S}, not a
#    literal. CONTROL: the scan must find the callers at all (a glob that matched
#    nothing would pass), and it must be able to flag a literal.
# scan_literal_limits <file> -- one "file:line" per boot-wait call whose next 6
# lines name a literal duration (60s, 90 seconds, 2 minutes). The callers may
# spell the call through a variable ($_ready_wait) when only the first of several
# waits covers the boot, so both spellings are scanned.
BOOTCALL='test_wait_boot |\$_ready_wait '
LITERAL='[0-9]+ ?(s\b|sec|second|minute)'
scan_literal_limits() {
    local f="$1" l n
    while IFS= read -r l; do
        n=${l%%:*}
        # The failure block ends at its `fi`: later prose (a comment about a
        # "~2 s teardown") is not the message.
        if sed -n "$((n + 1)),$((n + 6))p" "$f" | sed '/^[[:space:]]*fi\b/,$d' \
            | grep -qE "$LITERAL"; then
            echo "$(basename "$f"):$n"
        fi
    done < <(grep -nE "$BOOTCALL" "$f")
}
nboot=0; lit=""
for f in "$PWD"/test-*.sh "$PWD"/lib/*.sh; do
    [[ "$f" == *selftest* ]] && continue
    nboot=$((nboot + $(grep -cE "$BOOTCALL" "$f")))
    lit="$lit $(scan_literal_limits "$f")"
done
lit=$(echo $lit)
if [[ $nboot -ge 40 && -z "$lit" ]]; then
    pass "all $nboot boot-wait callers name \${TEST_WAITED_S}, not a literal limit"
else
    bad "boot waits ($nboot found) with a hard-coded limit within 6 lines:$lit"
fi
# CONTROL: the REAL scan, over a fixture with the literal four lines after the
# call (a one-line window would miss it), flags it; with the literal at +7
# (outside the window) or absent it stays quiet.
printf 'x\ntest_wait_boot "R" 60\nfail=1\necho a\necho b\necho "FAIL: within 60s"\n' > "$work/lit4.sh"
printf 'x\ntest_wait_boot "R" 60\necho "FAIL: within ${TEST_WAITED_S}s"\n' > "$work/lit0.sh"
[[ "$(scan_literal_limits "$work/lit4.sh")" == "lit4.sh:2" ]] \
    && pass "CONTROL: the real scan flags a literal 4 lines after the wait" \
    || bad "CONTROL: the scan missed a literal at +4: '$(scan_literal_limits "$work/lit4.sh")'"
printf 'x\ntest_wait_boot "R" 60\nfi\n# keeps it open for ~8 s\n' > "$work/litfi.sh"
[[ -z "$(scan_literal_limits "$work/litfi.sh")" ]] \
    && pass "CONTROL: the scan stops at the block's fi (a comment after it is not the message)" \
    || bad "CONTROL: the scan read past the failure block's fi"
[[ -z "$(scan_literal_limits "$work/lit0.sh")" ]] \
    && pass "CONTROL: the real scan is quiet for a message that uses \${TEST_WAITED_S}" \
    || bad "CONTROL: the scan flagged a clean message"

# (verify.sh's declaration of TEST_TIMEOUT_SCALE is asserted against the real
# script in test-gate-scoping-selftest.sh, which owns the stub-tree harness.)

if [[ $fail -eq 0 ]]; then echo "PASS"; exit 0; fi
echo "FAIL: $fail"
exit 1
