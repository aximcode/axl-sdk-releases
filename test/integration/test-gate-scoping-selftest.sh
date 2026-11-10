#!/bin/bash
# test-gate-scoping-selftest.sh — the scoping knobs on the gate runners:
# verify.sh --apps and run-integration.sh --for -- and verify.sh's refusal of
# --lint-files, now that the lint is incremental (section 2).
# selftest-schedule: tree -- it READS build/x64-debug's ctest registration
# (ctest --test-dir, verify.sh --print-ctest), which test-runner-selftest.sh's
# stub runs can reconfigure.
#
# WHY A SELF-TEST AND NOT AN INTEGRATION TEST. These are the GATE RUNNERS. A
# defect here does not fail a build, it makes every other gate lie -- a scoped
# run that silently widened, or silently narrowed, or wrote the ratchet
# baseline from a subset, reports the same green as a full one. Excluded from
# discovery by name (*-selftest.sh) and run by scripts/run-selftests.sh, for
# the reason that script's header gives.
#
# WHAT EACH KNOB HAS TO PROVE, and both halves are asserted for all three:
#   1. it NARROWS what actually runs, and
#   2. it ANNOUNCES the narrowing, so a scoped log cannot be mistaken for a
#      full one.
#
# HERMETIC WHERE IT CAN BE. verify.sh resolves its tree from its own path
# ($SCRIPT_DIR/..), so a copy of it under a stub tree runs the REAL script
# against stub jobs -- no QEMU, no build, and the stub records exactly what
# environment it was handed. That is how the ratchet assertions are made: the
# thing to prove is "verify.sh passes TEST_SKIP_RATCHET=1", which is a fact
# about verify.sh, not about QEMU.
# NOT SAFE TO RUN BESIDE test-runner-selftest.sh: it reads build/x64-debug's ctest
# registration (above), which that file's stub runs reconfigure.
# scripts/run-selftests.sh runs it once, in its tree lane
# (never beside itself or the runner self-test), and CI runs that after the
# suite. lint.sh itself is test-lint-gate-selftest.sh's, which owns build/x64-lint.
set -uo pipefail
# Hermetic against the runner: CI (set by GitHub Actions) makes a pool
# registration with no service SAY so, which verify then quotes -- the
# no-service cases below pin the developer host's wording.
unset CI
cd "$(dirname "$0")" || exit 1
REPO="$(cd ../.. && pwd)"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$REPO/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

# COUNTS, not a flag. Each block below reports its verdict by comparing $fail
# against its own entry value, and with a flag that saturates at 1 every block
# after the first failure printed PASS regardless -- "passes for the wrong
# reason", in the harness whose job is to catch exactly that.
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

echo "=== gate scoping self-test ==="
echo ""

# ---------------------------------------------------------------------------
# 1. verify.sh --apps=<csv>
# ---------------------------------------------------------------------------
# A stub tree: a real copy of verify.sh, a stub test-axl.sh that records its
# environment and argv instead of booting anything. --only=x64,aa64 needs
# nothing else (MAKE_CHECKS and the artifact gates are computed only when
# their job is selected).
# Cleanup on EXIT (which still runs when INT or TERM kills the script); the
# signals only EXIT -- a handler that cleans up and returns RESUMES the script
# (scripts/run-selftests.sh rejects one).
vstub=$(mktemp -d); trap 'rm -rf "$vstub"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$vstub/scripts" "$vstub/test/integration/lib" "$vstub/test/pool/axlpool"
cp "$REPO/scripts/verify.sh" "$REPO/scripts/qloop-verdict.sh" "$REPO/scripts/axl-common.sh" \
   "$REPO/scripts/axl-cmake-pin.sh" "$REPO/scripts/ci-cmake.sh" "$vstub/scripts/"
cp "$REPO/test/integration/lib/pool-or-boot.sh" "$REPO/test/integration/lib/fwd-signal.sh" \
   "$vstub/test/integration/lib/"
# A stub `axlpool` for the quiet slot's pooled runs (1k3): `settle` answers as
# AXL_STUB_SETTLE says (ok | busy), `run` leases (AXL_POOL_AGENT, a QEMU pid)
# or misses (AXL_STUB_POOL=miss before the command runs; =harness-miss after
# it ran and printed, as a miss the harness meets on the guest); each call is
# recorded, in order.
: > "$vstub/test/pool/axlpool/__init__.py"
cat > "$vstub/test/pool/axlpool/__main__.py" <<'PY'
"""Stub axlpool for test-gate-scoping-selftest.sh's quiet-slot cases."""
import os
import subprocess
import sys
with open(os.environ["AXL_STUB_CALLS"], "a", encoding="utf-8") as f:
    f.write(f"axlpool {sys.argv[1]}\n")
if sys.argv[1] == "settle":
    if os.environ.get("AXL_STUB_SETTLE") == "traceback":
        print("Traceback (most recent call last):\n  File x\nboom: the last line", file=sys.stderr)
        sys.exit(1)
    if os.environ.get("AXL_STUB_SETTLE", "ok") == "busy":
        print("axlpool settle: not settled after 60 s: g1 resetting")
        sys.exit(1)
    print("axlpool settle: settled after 12 ms")
    sys.exit(0)
if sys.argv[1] == "run":
    if os.environ.get("AXL_STUB_POOL") == "miss":
        print("AXLPOOL-MISS: no guest: miss (no-slot): stub", file=sys.stderr)
        sys.exit(77)
    child = sys.argv[sys.argv.index("--") + 1:]
    rc = subprocess.call(child, env=dict(os.environ, AXL_POOL_AGENT="127.0.0.1:1",
                                         AXL_POOL_QEMU_PID="4242"))
    if os.environ.get("AXL_STUB_POOL") == "harness-miss":
        print("AXLPOOL-MISS: the agent could not run it: stub", flush=True)
        sys.exit(77)
    sys.exit(rc)
sys.exit(2)
PY
cat > "$vstub/test/integration/test-axl.sh" <<'STUB'
#!/bin/bash
# Stub test-axl.sh: records what it was handed, then reports a plausible
# Results line so verify.sh's table and cross-arch check have something real
# to parse.
arch=X64
[[ "${1:-}" == "--arch" ]] && arch="$2"
serial=""
for ((i = 1; i <= $#; i++)); do
    [[ "${!i}" == "--log" ]] && { j=$((i + 1)); serial="${!j}"; }
done
if [[ "${1:-}" == "--list-apps-skipped" ]]; then
    printf '%s\n' AxlTestOmitted
    exit 0
fi
if [[ "${1:-}" == "--list-apps" ]]; then
    printf '%s\n' AxlTestAlpha AxlTestBeta AxlTestGamma AxlTestDelta \
                  AxlTestEpsilon AxlTestZeta AxlTestEta AxlTestTheta \
                  AxlTestIota AxlTestKappa AxlTestLambda
    exit 0
fi
{
    echo "ARCH=$arch APPS=${TEST_APPS_ONLY:-<unset>} RATCHET=${TEST_SKIP_RATCHET:-<unset>} BSCALE=${TEST_BUDGET_SCALE:-<unset>} SCALE=${TEST_TIMEOUT_SCALE:-<unset>}${AXL_POOL_AGENT:+ POOLED}"
} >> "$AXL_STUB_CALLS"
n=7
[[ -n "${TEST_APPS_ONLY:-}" ]] && n=2
# The quiet slot. A judged AxlTestLoop writes the fixed marker to its serial
# log; an unjudged one writes none and declares a counted skip -- in
# axl-test-loop.c's wording (AXL_STUB_UNJUDGED=N, a probe count) or in any
# other (AXL_STUB_UNJ_TEXT), which must make no difference.
if [[ "${TEST_APPS_ONLY:-}" == AxlTestLoop ]]; then
    if [[ -n "${AXL_STUB_UNJUDGED:-}" ]]; then
        echo "  SKIP[14]: loop: driver idle budget timing: host too noisy to judge" \
             "(probe: $AXL_STUB_UNJUDGED gaps over 1000 us) -- every attempt voided"
        echo "Results: $n passed, 0 failed, 1 group(s) SKIPPED (14 assertions) ($arch) in 3s"
        exit 0
    fi
    if [[ -n "${AXL_STUB_UNJ_TEXT:-}" ]]; then
        echo "  SKIP[14]: $AXL_STUB_UNJ_TEXT"
        echo "Results: $n passed, 0 failed, 1 group(s) SKIPPED (14 assertions) ($arch) in 3s"
        exit 0
    fi
    # A bound missed in the only judged attempt: the guest's "unconfirmed"
    # skip and one fixed line per bound (AXL_STUB_UNCONFIRMED=name[|name...]).
    if [[ -n "${AXL_STUB_UNCONFIRMED:-}" ]]; then
        if [[ -n "$serial" ]]; then
            : > "$serial"
            IFS='|' read -r -a _ums <<<"$AXL_STUB_UNCONFIRMED"
            for _um in "${_ums[@]}"; do
                printf '[30.2] LOOP-TIMING: unconfirmed miss: %s\r\n' "$_um" >> "$serial"
            done
        fi
        echo "  SKIP[14]: loop: driver idle budget timing: a bound missed in the only judged" \
             "attempt was not reproduced (probe: 0 gaps over 1000 us) -- unconfirmed"
        echo "Results: $n passed, 0 failed, 1 group(s) SKIPPED (14 assertions) ($arch) in 3s"
        exit 0
    fi
    # A pooled attempt that a miss then discards (AXL_STUB_POOL=harness-miss):
    # AXL_STUB_POOLED_STARVE is its own starvation figure, and
    # AXL_STUB_BOOTED_NOSERIAL=1 makes the booted rerun write no serial log --
    # so neither may stand as the rerun's evidence.
    if [[ -n "${AXL_POOL_AGENT:-}" && -n "${AXL_STUB_POOLED_STARVE:-}" ]]; then
        AXL_STUB_STARVE="$AXL_STUB_POOLED_STARVE"
    fi
    [[ -z "${AXL_POOL_AGENT:-}" && -n "${AXL_STUB_BOOTED_NOSERIAL:-}" ]] && serial=""
    [[ -n "$serial" ]] && echo "LOOP-TIMING: judged" > "$serial"
    # The witness: what the harness's starvation sampler reports
    # (AXL_STUB_STARVE=N ms; unset, no line -- not measured), and a judged
    # FAIL that is a missed timing bound (AXL_STUB_FAIL=timing: the guest
    # names it on its "judged attempt N missed:" lines) or anything else
    # (AXL_STUB_FAIL=other). AXL_STUB_EXTRA is one more harness line (a leak
    # verdict, say), and AXL_STUB_NFAILED the Results line's failed count.
    # The job log's real shape: build output, then the harness header, then
    # the guest's lines. AXL_STUB_PRE is a build-output line before the
    # header; AXL_STUB_PASS a PASS line in the guest's region.
    [[ -n "${AXL_STUB_PRE:-}" ]] && echo "$AXL_STUB_PRE"
    echo "=== AXL Integration Tests ($arch) ==="
    [[ -n "${AXL_STUB_STARVE:-}" ]] \
        && echo "vCPU starvation: $AXL_STUB_STARVE ms run delay, 0 ms host steal (all CPUs)"
    [[ -n "${AXL_STUB_PASS:-}" ]] && echo "  PASS: $AXL_STUB_PASS"
    if [[ -n "${AXL_STUB_FAIL:-}" ]]; then
        # A functional check the guest names as "failed (not timing)".
        if [[ "$AXL_STUB_FAIL" == functional && -n "$serial" ]]; then
            printf '[20.1]   driver idle budget: judged attempt 1 failed (not timing): %s\r\n' \
                "loop: driver budget mid-tick: attach at 20 ms" >> "$serial"
            echo "  FAIL: loop: driver budget mid-tick: attach at 20 ms"
        elif [[ "$AXL_STUB_FAIL" == timing && -n "$serial" ]]; then
            printf '[20.1]   driver idle budget: judged attempt 1 missed: %s\r\n' \
                "loop: a renderer waking on 10 ms driver ticks keeps its frame rate" >> "$serial"
            # The real serial shape: a ts prefix, a cursor move, CRLF.
            printf '[28.3] \033[50;01H  driver idle budget: judged attempt 2 missed: %s\r\n' \
                "loop: a renderer waking on 10 ms driver ticks keeps its frame rate" >> "$serial"
            echo "  FAIL: loop: a renderer waking on 10 ms driver ticks keeps its frame rate"
        else
            echo "  FAIL: loop: set_driver_idle_budget_ms(NULL) is AXL_INVALID"
        fi
        [[ -n "${AXL_STUB_EXTRA:-}" ]] && echo "$AXL_STUB_EXTRA"
        # The harness's skipped-groups notice re-lists every skip reason,
        # prefix stripped and indented (AXL_STUB_SKIPREASON).
        if [[ -n "${AXL_STUB_SKIPREASON:-}" ]]; then
            echo "  SKIP[5]: $AXL_STUB_SKIPREASON"
            echo "*** 1 test GROUP(S) SKIPPED (5 assertions declared):"
            echo "     $AXL_STUB_SKIPREASON"
        fi
        echo "Results: $((n - 1)) passed, ${AXL_STUB_NFAILED:-1} failed ($arch) in 3s"
        exit "${AXL_STUB_RC:-1}"   # the harness's own status (AXL_STUB_RC)
    fi
fi
echo "Results: $n passed, 0 failed ($arch) in 3s"
exit 0
STUB
chmod +x "$vstub/test/integration/test-axl.sh"

# NOT a command substitution: $(...) is a subshell, so an rc captured inside
# one never reaches the caller. Output goes to a file and is read back.
vrun() {   # vrun <args...>; sets VRC, leaves output in $vstub/out.txt
    : > "$vstub/calls.txt"
    AXL_STUB_CALLS="$vstub/calls.txt" "$vstub/scripts/verify.sh" "$@" \
        > "$vstub/out.txt" 2>&1
    VRC=$?
}
vout() { cat "$vstub/out.txt"; }

before=$fail
# --- 1a. an unknown binary name is REFUSED, loudly, and nothing runs.
# The same rule run-integration.sh --only follows: "the tool matched nothing"
# and "you asked for something that is not here" are the same empty set and
# opposite facts.
vrun --only=x64,aa64 --apps=AxlTestNoSuchThing; out=$(vout)
[[ "$VRC" -eq 2 ]] || bad "--apps with an unknown name exited $VRC, expected 2"
need 'AxlTestNoSuchThing' "$out" "the refusal does not name the bad input"
need 'unknown unit binary' "$out" "the refusal does not say what was wrong"
need 'AxlTestAlpha' "$out" "the refusal does not list the known binaries"
[[ -s "$vstub/calls.txt" ]] && bad "a refused --apps still ran an arch job"
# A binary the suite deliberately EXCLUDES is known, not unknown. Saying
# "unknown unit binary" about AxlTestCpuIdle names the wrong cause and hides
# the reason recorded beside it in TEST_APPS_SKIP.
vrun --only=x64,aa64 --apps=AxlTestOmitted
[[ "$VRC" -eq 2 ]] || bad "--apps naming a deliberately-skipped binary exited $VRC, expected 2"
needi 'deliberately|excluded, not unknown' "$(vout)" \
      "a deliberately-skipped binary was reported as unknown"
[[ $fail -eq $before ]] && pass "verify.sh --apps refuses an unknown binary, names an excluded one, and runs nothing"

# --- 1a2. A name the SUITE knows but the TREE does not register is refused
# too. --apps is checked against test-axl.sh --list-apps (the source), the
# selection against the tree's registration (made at configure time, and a
# new binary in TEST_APPS does not trigger one): the selection was then
# unit.inventory alone and the run read "0 unit binaries passed", ALL GREEN.
# The stub's --list-apps names binaries no real tree registers; its build/
# is the real trees, read through --print-ctest (lists, runs nothing).
before=$fail
mkdir -p "$vstub/test/integration/lib"
cp "$REPO/test/integration/lib/ctest-session.sh" "$vstub/test/integration/lib/"
ln -sfn "$REPO/build" "$vstub/build"
vrun --print-ctest --only=x64 --apps=AxlTestAlpha
[[ "$VRC" -eq 2 ]] || bad "--apps naming a binary the tree does not register exited $VRC, expected 2"
need 'AxlTestAlpha' "$(vout)" "the unregistered-binary refusal does not name it"
needi 'not registered' "$(vout)" "the unregistered-binary refusal does not say why"
nonope '^x64-ctest:' "$(vout)" "a run was printed for an unregistered --apps binary"
[[ $fail -eq $before ]] && pass "verify.sh --apps refuses a binary the tree does not register, naming it"

# --- 1a3. A refusal or a --print-ctest run leaves no log directory behind:
# 1630 /tmp/axl-verify.* were counted on 2026-11-06. A fresh TMPDIR, then
# each of the paths that exit before any run starts.
before=$fail
vt="$vstub/vtmp"; mkdir -p "$vt"
TMPDIR="$vt" "$REPO/scripts/verify.sh" --only=nosuchjob > /dev/null 2>&1
TMPDIR="$vt" "$REPO/scripts/verify.sh" --print-ctest --only=x64 > /dev/null 2>&1
TMPDIR="$vt" "$REPO/scripts/verify.sh" --only=x64 --apps=AxlTestNoSuchThing > /dev/null 2>&1
vrun_tmp() { TMPDIR="$vt" AXL_STUB_CALLS=/dev/null "$vstub/scripts/verify.sh" "$@" > /dev/null 2>&1; }
vrun_tmp --print-ctest --only=x64 --apps=AxlTestAlpha
[[ -z "$(ls -A "$vt")" ]] || bad "a refusal or --print-ctest left $(ls "$vt" | paste -sd' ') in TMPDIR"
# control: the directory is made under TMPDIR at all (else this proves nothing)
mkdir -p "$vt/probe" && TMPDIR="$vt/probe" bash -c 'mktemp -d -t axl-verify.XXXXXX' > /dev/null
[[ -n "$(ls -A "$vt/probe")" ]] || bad "control: mktemp -t did not use TMPDIR, so the leak check sees nothing"
[[ $fail -eq $before ]] && pass "a refusal or --print-ctest leaves no /tmp/axl-verify.* behind"

# --- 1b-1e. WHAT EACH JOB SELECTS, on the REAL trees (verify.sh is a front
# end over CTest since slice 5a step 6). --print-ctest prints each run's
# environment, tests and argv, and runs nothing. What these blocks pinned
# before through a stub test-axl.sh moved with the engine: the table and the
# rendering of every row are test-ctest-report.sh's (fixtures from real ctest
# runs); WHAT the qloop verdict decides is qloop-verdict.sh's, pinned in 1k
# and 1k2 below; the environment the registration gives the jobs (nice) is
# pinned by test-ctest-integration.sh, the unit-run scales verify declares
# just below.
before=$fail
V="$REPO/scripts/verify.sh"
pv=$("$V" --print-ctest --only=x64,aa64 --apps=AxlTestString,AxlTestMem 2>&1)
check_sel() {   # check_sel RUN EXPECTED-SORTED-NAMES WHAT
    local got; got=$(sed -n "s/^$1-ctest-test: //p" <<<"$pv" | sort | paste -sd' ')
    [[ "$got" == "$2" ]] || bad "$3 (got: $got)"
}
check_sel x64 "unit.AxlTestMem unit.AxlTestString unit.inventory" "--apps: x64 runs exactly the named binaries and the inventory"
check_sel aa64 "unit.AxlTestMem unit.AxlTestString unit.inventory" "--apps: aa64 runs exactly the named binaries and the inventory"
nonope '^qloop-ctest' "$pv" "--apps with --only=x64,aa64 started a qloop run"
# CTest's own answer for the --apps argv: the ratchet and its reset come in
# as FIXTURES -- the ratchet's PARTIAL line carries the per-binary counts the
# cross-arch check compares, without being asked for.
sed -n 's/^x64-ctest-test: //p' <<<"$pv" > "$vstub/sel.txt"
fx=$(ctest --test-dir "$REPO/build/x64-debug" --tests-from-file "$vstub/sel.txt" \
         --show-only=json-v1 --output-junit /dev/null 2>/dev/null \
     | jq -r '.tests[].name' | grep -cxE 'unit\.(ratchet|counts-reset)')
[[ "$fx" == 2 ]] || bad "ctest does not bring in the ratchet and its reset as fixtures ($fx)"
# A FULL unit job is the one-boot run, alone: every binary in one guest with
# its ratchet and budget. Per-binary tests there would be 49 boots per arch.
pv=$(XDG_RUNTIME_DIR="$vstub/xdg-none" "$V" --print-ctest --only=x64,aa64 2>&1)
check_sel x64 "unit.one-boot" "no --apps: x64 runs the one-boot unit run, and only it"
check_sel aa64 "unit.one-boot" "no --apps: aa64 runs the one-boot unit run, and only it"
# POOLED (design §6.7): when the registration pools the units, a full unit
# job is the per-binary unit.<App> tests -- each on a leased `unit` guest,
# spread over idle guests by ctest -- plus the inventory; no boot. THE
# REGISTRATION DECIDES: verify hands each arch run AXL_IT_POOL
# (AXL_VERIFY_POOL, default auto) and reads back whether the listing gave the
# units a guest slot, saying why not from its integration.pool line. A socket
# with no service behind it is NOT a pool -- verify used to decide from the
# socket's existence and said "pooled" while every binary booted.
units_line() { grep -m1 "^verify: $1 units:" <<<"$2"; }
[[ "$(units_line x64 "$pv")" == "verify: x64 units: one boot (no pool service (AXL_IT_POOL=auto))" ]] \
    || bad "no pool service: the one boot is not said, with why ($(units_line x64 "$pv"))"
[[ "$(grep -cx "x64-ctest-env: AXL_IT_POOL=auto" <<<"$pv")" == 1 ]] \
    || bad "verify did not hand the x64 run AXL_IT_POOL=auto (the registration's to decide)"
mkdir -p "$vstub/xdg-none" "$vstub/xdg-pool/axlpool"
python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])' "$vstub/xdg-pool/axlpool/pool.sock"
pf=$(XDG_RUNTIME_DIR="$vstub/xdg-pool" "$V" --print-ctest --only=x64,aa64 2>&1)
check_sel_in() {   # check_sel_in TEXT RUN EXPECTED WHAT
    local got; got=$(sed -n "s/^$2-ctest-test: //p" <<<"$1" | sort | paste -sd' ')
    [[ "$got" == "$3" ]] || bad "$4 (got: $got)"
}
check_sel_in "$pf" x64 "unit.one-boot" "a socket with NO service: x64 boots once, not 'pooled'"
check_sel_in "$pf" aa64 "unit.one-boot" "a socket with NO service: aa64 boots once, not 'pooled'"
[[ "$(units_line x64 "$pf")" == "verify: x64 units: one boot (pool: unavailable (axlpool: no pool service on $vstub/xdg-pool/axlpool/pool.sock: [Errno 111] Connection refused) -- guest= tests boot)" ]] \
    || bad "a socket with no service: the registration's reason is not said ($(units_line x64 "$pf"))"
# An explicit socket pools without asking (as AXL_IT_POOL=<socket> does for
# the runner); its integration.pool check would refuse at run time.
pp=$(AXL_VERIFY_POOL="$vstub/explicit.sock" XDG_RUNTIME_DIR="$vstub/xdg-none" "$V" --print-ctest --only=x64,aa64 2>&1)
nbin=$("$REPO/test/integration/test-axl.sh" --list-apps | grep -c .)
for r in x64 aa64; do
    [[ "$(grep -c "^$r-ctest-test: unit\.AxlTest" <<<"$pp") $(grep -cx "$r-ctest-test: unit.inventory" <<<"$pp") $(grep -cx "$r-ctest-test: unit.one-boot" <<<"$pp")" == "$nbin 1 0" ]] \
        || bad "pooled: $r does not run the $nbin per-binary unit tests + inventory instead of the one boot"
    [[ "$(grep -cx "$r-ctest-env: AXL_IT_POOL=$vstub/explicit.sock" <<<"$pp")" == 1 ]] \
        || bad "pooled: $r's run is not handed AXL_VERIFY_POOL's socket as AXL_IT_POOL"
    [[ "$(units_line "$r" "$pp")" == "verify: $r units: pooled, one leased guest per binary (AXL_IT_POOL=$vstub/explicit.sock)" ]] \
        || bad "pooled: $r does not say so ($(units_line "$r" "$pp"))"
done
po=$(AXL_VERIFY_POOL=0 "$V" --print-ctest --only=x64 2>&1)
check_sel_in "$po" x64 "unit.one-boot" "AXL_VERIFY_POOL=0: the units boot, pool or not"
[[ "$(units_line x64 "$po")" == "verify: x64 units: one boot (pool: off (AXL_IT_POOL=0) -- guest= tests boot)" ]] \
    || bad "AXL_VERIFY_POOL=0: not said ($(units_line x64 "$po"))"
# WHAT A CALLER DECLARES (release-suite.sh's unit step, owner decision 3):
# the pooled guests run the tree's agent, and CI's unit budget -- each by its
# own AXL_VERIFY_ name, and nothing of either when undeclared (an inherited
# TEST_MAX_DURATION is scrubbed: it would loosen the budget unannounced).
pg=$(TEST_MAX_DURATION=5 AXL_VERIFY_POOL_AGENT=tree AXL_VERIFY_UNIT_BUDGET=190 "$V" --print-ctest --only=x64 2>&1)
[[ "$(grep -cx 'x64-ctest-env: AXL_IT_POOL_AGENT=tree' <<<"$pg") $(grep -cx 'x64-ctest-env: TEST_MAX_DURATION=190' <<<"$pg") $(grep -cx 'x64-ctest-env: TEST_MAX_DURATION=5' <<<"$pg")" == "1 1 0" ]] \
    || bad "AXL_VERIFY_POOL_AGENT / AXL_VERIFY_UNIT_BUDGET did not reach the x64 run as AXL_IT_POOL_AGENT / TEST_MAX_DURATION (or the inherited one did)"
[[ "$(grep -cx 'verify: unit budget declared by the caller: 190 s (AXL_VERIFY_UNIT_BUDGET)' <<<"$pg")" == 1 ]] \
    || bad "a declared unit budget is not announced"
pb=$(AXL_VERIFY_UNIT_BUDGET=190s "$V" --print-ctest --only=x64 2>&1); rc=$?
[[ "$rc $pb" == "2 verify.sh: AXL_VERIFY_UNIT_BUDGET='190s' -- whole seconds expected" ]] \
    || bad "a malformed AXL_VERIFY_UNIT_BUDGET was not refused by name (rc=$rc: $pb)"
pn=$(TEST_MAX_DURATION=5 "$V" --print-ctest --only=x64 2>&1)
nonope '^x64-ctest-env: (AXL_IT_POOL_AGENT|TEST_MAX_DURATION)=' "$pn" "undeclared, a pool agent or a unit budget reached the x64 run"
[[ "$(grep -c -- ' --output-on-failure ' <<<"$(grep '^x64-ctest:' <<<"$pn")")" == 1 ]] \
    || bad "the x64 run does not keep its failing tests' output (--output-on-failure)"
pa=$(AXL_VERIFY_POOL="$vstub/explicit.sock" "$V" --print-ctest --only=x64 --apps=AxlTestMem 2>&1)
nonope '^x64-ctest-env: AXL_IT_POOL' "$pa" "--apps handed its run AXL_IT_POOL (a few binaries stay booted)"
# THE SCALES verify declares for the one-boot run (unit.one-boot is strict
# without them), by the rules the old verify had: the hang timeout x2 when
# more than one of x64/aa64/lint/gates/docs runs; the budget x1.40 only when
# lint, gates or docs run beside the guests (the pressure it was measured
# under). A lone arch, or two guests alone, keep the 110 s budget strict.
scales() {   # scales ONLY -> "RUN:T=..,B=.." per arch run
    local out r; out=$("$V" --print-ctest "--only=$1" 2>&1)
    for r in x64 aa64; do
        grep -q "^$r-ctest:" <<<"$out" || continue
        printf '%s:T=%s,B=%s ' "$r" \
            "$(sed -n "s/^$r-ctest-env: AXL_UNIT_TIMEOUT_SCALE=//p" <<<"$out")" \
            "$(sed -n "s/^$r-ctest-env: AXL_UNIT_BUDGET_SCALE=//p" <<<"$out")"
    done
}
# The budget scale is ALWAYS declared (1 when strict): the pooled units'
# ratchet judges a budget only when a front end says what the run shared.
[[ "$(scales x64)" == "x64:T=,B=1 " ]] || bad "a lone x64 run is not strict ($(scales x64))"
[[ "$(scales x64,aa64)" == "x64:T=2,B=1 aa64:T=2,B=1 " ]] \
    || bad "two guests alone: timeout x2, budget strict ($(scales x64,aa64))"
[[ "$(scales x64,aa64,lint,gates,docs)" == "x64:T=2,B=1.40 aa64:T=2,B=1.40 " ]] \
    || bad "beside lint/gates/docs: timeout x2 and budget x1.40 ($(scales x64,aa64,lint,gates,docs))"
# imggates used to run AFTER the guests; now it builds beside them -- and
# its gates run in BOTH trees (the arch ones register in each).
[[ "$(scales x64,imggates)" == "x64:T=2,B=1.40 aa64:T=2,B=1.40 " ]] \
    || bad "x64 beside imggates: both scales, both trees ($(scales x64,imggates))"
# --apps is scoped: its ratchet must never write the baseline.
nw() { "$V" --print-ctest "$@" 2>&1 | grep -c '^x64-ctest-env: AXL_UNIT_NO_WRITE=1$'; }
[[ "$(nw --only=x64 --apps=AxlTestString) $(nw --only=x64)" == "1 0" ]] \
    || bad "AXL_UNIT_NO_WRITE=1 is not set exactly under --apps"
# the gates job's gates run in both trees, so both runs are listed (gates only).
[[ "$(scales aa64,gates)" == "x64:T=2,B=1.40 aa64:T=2,B=1.40 " ]] \
    || bad "aa64 beside gates: both scales ($(scales aa64,gates))"
# THE GATES, as each tree REGISTERS them (gate.check-*; label artifact for
# imggates's): the selection must be exactly the registration, per tree.
gates_of() {   # gates_of TREE artifact|source -> sorted names
    ctest --test-dir "$REPO/build/$1-debug" --show-only=json-v1 2>/dev/null \
        | jq -r --arg k "$2" '.tests[] | select(.name | startswith("gate.check-"))
              | ([.properties[]? | select(.name == "LABELS") | .value[]] | index("artifact")) as $a
              | select(($k == "artifact") == ($a != null)) | .name' | sort
}
art=$(gates_of x64 artifact | paste -sd' ')
lintg=$(gates_of x64 source)
pv=$("$V" --print-ctest --only=gates,imggates,lint,docs 2>&1)
want_x64=$(printf '%s\n' "$lintg" $art gate.lint gate.docs | sort -u | paste -sd' ')
check_sel x64 "$want_x64" "the gate jobs select the x64 tree's gates (gates + imggates), lint and docs"
check_sel aa64 "$( (gates_of aa64 source; gates_of aa64 artifact) | sort | paste -sd' ')" \
    "the gate jobs select the aa64 tree's own gates, and nothing else there"
# Controls: there are gates to select, and the aa64 tree has its own.
[[ $(wc -w <<<"$want_x64") -ge 30 && -n "$(gates_of aa64 artifact)" ]] \
    || bad "the trees register too few gates to make the selection checks mean anything"
[[ "$(comm -12 <(tr ' ' '\n' <<<"$art" | sort) <(sed -n 's/^x64-ctest-test: //p' <<<"$pv" | sort) | paste -sd' ')" == "$art" ]] \
    || bad "imggates does not run every artifact gate"
pv=$("$V" --print-ctest --only=gates 2>&1)
[[ -z "$(comm -12 <(tr ' ' '\n' <<<"$art" | sort) <(sed -n 's/^x64-ctest-test: //p' <<<"$pv" | sort))" ]] \
    || bad "gates alone ran an artifact gate (those are imggates's)"
nonope '^aa64-ctest-test: unit\.' "$pv" "a gates-only run selected aa64 unit tests"
# --apps beside gates: the aa64 tree runs for its GATES only, so its missing
# unit tests must not read as "--apps names X, not registered" (review M2).
pv=$("$V" --print-ctest --only=x64,gates --apps=AxlTestMem 2>&1); rc=$?
[[ $rc -eq 0 ]] || bad "--only=x64,gates --apps refused (rc=$rc): $(tail -1 <<<"$pv")"
check_sel x64 "$( (printf '%s\n' unit.AxlTestMem unit.inventory; printf '%s\n' "$lintg") | sort | paste -sd' ')" \
    "--only=x64,gates --apps: x64 runs the named binary, the inventory and the source gates"
nonope '^aa64-ctest-test: unit\.' "$pv" "--only=x64,gates --apps selected aa64 unit tests"
# The two jobs had make's names until the Makefile went: an old name is told
# the new one, not "unknown job".
for _old in make:gates makeimg:imggates; do
    o=$("$V" --print-ctest "--only=${_old%%:*}" 2>&1); rc=$?
    [[ "$rc $o" == "2 verify.sh: the '${_old%%:*}' job is '${_old#*:}' since 5.0.0 (the Makefile is gone)" ]] \
        || bad "--only=${_old%%:*} was not told its new name (rc=$rc: $o)"
done
pv=$("$V" --print-ctest --only=qloop 2>&1)
check_sel qloop "gate.qloop" "qloop runs gate.qloop, alone"
need '^qloop-ctest: ctest .* -j 1 ' "$pv" "qloop does not run at -j 1"
nonope 'test-load' "$(grep '^qloop-ctest:' <<<"$pv")" "qloop is throttled by --test-load (it runs alone)"
# ...and leases its guest as the units do: verify's pool mode, or an opt-out.
need '^qloop-ctest-env: AXL_IT_POOL=auto$' "$pv" "qloop is not handed verify's pool mode (AXL_IT_POOL=auto)"
need '^qloop-ctest-env: AXL_IT_POOL=0$' "$(AXL_VERIFY_POOL=0 "$V" --print-ctest --only=qloop 2>&1)" \
    "AXL_VERIFY_POOL=0 does not reach the qloop run"
[[ $fail -eq $before ]] && pass "verify.sh: each job selects exactly its tests on its tree; --apps narrows both arches; qloop runs alone"

# --- 1k. the verdict is ONE script, scripts/qloop-verdict.sh, which verify.sh's
# qloop job and CTest's gate.qloop both run. Its exit status IS the verdict
# (0 judged, 3 UNJUDGED -- gate.qloop's SKIP_RETURN_CODE -- anything else a
# FAIL) and its LAST line states it. A harness that itself exits 3 is a FAIL,
# never mistaken for the skip: under CTest that would read as a pass.
before=$fail
# Its own temp directory (no --dir) goes under a fresh TMPDIR inside the stub
# tree, so what it removes or keeps is asserted directly -- and nothing it
# keeps outlives this test.
qrun() {   # qrun [args...]; sets QRC, output in $vstub/qout.txt
    : > "$vstub/calls.txt"
    rm -rf "${vstub:?}/qtmp"; mkdir -p "$vstub/qtmp"
    TMPDIR="$vstub/qtmp" AXL_STUB_CALLS="$vstub/calls.txt" \
        AXL_ONE_POOL_SOCK="${QSOCK:-}" \
        "$vstub/scripts/qloop-verdict.sh" "$@" > "$vstub/qout.txt" 2>&1
    QRC=$?
}
qkept() { find "$vstub/qtmp" -mindepth 1 -maxdepth 1 -name 'axl-qloop.*' | wc -l; }
qlast() { tail -1 "$vstub/qout.txt"; }
qrun
[[ "$QRC" -eq 0 ]] || bad "CONTROL: a judged qloop-verdict.sh exited $QRC, expected 0"
[[ "$(qlast)" == "qloop-verdict: AxlTestLoop alone on x64 (quiet slot): 2 passed, 0 failed (X64) in 3s; booted (no pool); vCPU starvation not measured" ]] \
    || bad "a judged verdict line read '$(qlast)'"
need 'ARCH=X64 APPS=AxlTestLoop RATCHET=1' "$(cat "$vstub/calls.txt")" "qloop-verdict.sh did not run AxlTestLoop alone on x64"
need '^Results: 2 passed, 0 failed \(X64\) in 3s$' "$(cat "$vstub/qout.txt")" "the harness output is not passed through (CTest shows it on failure)"
[[ "$(qkept)" -eq 0 ]] || bad "a judged run left its temp directory behind"
AXL_STUB_UNJUDGED=7 qrun
[[ "$QRC" -eq 3 ]] || bad "an unjudged qloop-verdict.sh exited $QRC, expected 3"
[[ "$(qlast)" == "qloop-verdict: UNJUDGED (probe: 7 gaps) -- no 'LOOP-TIMING: judged' line; the box was not quiet; booted (no pool); vCPU starvation not measured" ]] \
    || bad "an unjudged verdict line read '$(qlast)'"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=6 qrun
[[ "$QRC" -eq 1 ]] || bad "a quiet timing FAIL exited $QRC, expected 1"
[[ "$(qlast)" == "qloop-verdict: FAILED: 1 passed, 1 failed (X64) in 3s; booted (no pool); the guest's QEMU waited 6 ms for a CPU" ]] \
    || bad "a FAIL verdict line read '$(qlast)'"
[[ "$(qkept)" -eq 1 ]] || bad "a failed run did not keep its temp directory"
need "^qloop-verdict: logs kept in $vstub/qtmp/axl-qloop\\." "$(cat "$vstub/qout.txt")" "a failed run did not name the directory it kept"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 qrun
[[ "$QRC" -eq 3 ]] || bad "a timing FAIL under load exited $QRC, expected 3"
AXL_STUB_FAIL=other AXL_STUB_RC=3 qrun
[[ "$QRC" -eq 1 ]] || bad "a harness exiting 3 made qloop-verdict.sh exit $QRC, expected 1 (3 is the SKIP)"
# A relative --dir is the CALLER's: run from a directory that is not the
# tree's root, which is where the script itself works.
mkdir -p "$vstub/qdir" "$vstub/qtmp/elsewhere"
(cd "$vstub/qtmp/elsewhere" && TMPDIR="$vstub/qtmp" AXL_STUB_CALLS="$vstub/calls.txt" \
    "$vstub/scripts/qloop-verdict.sh" --dir ../../qdir > "$vstub/qout.txt" 2>&1); QRC=$?
[[ "$QRC" -eq 0 && -f "$vstub/qdir/qloop.serial" ]] \
    || bad "--dir DIR (relative, the caller's) did not receive the serial log (exit $QRC)"
# The verdict is written beside the run's files: under ctest a SKIPPED
# gate.qloop's output reaches only LastTest.log, which every later ctest
# invocation on the tree rewrites -- a listing included.
[[ "$(cat "$vstub/qdir/qloop-verdict.txt" 2>/dev/null)" == "$(tail -1 "$vstub/qout.txt")" ]] \
    || bad "--dir DIR did not receive the verdict line as qloop-verdict.txt"
# A --dir is REUSED (gate.qloop's is the tree's qloop-logs): what a previous
# run left there is not this run's evidence. A run that writes no serial log
# of its own must not be judged by a stale marker.
printf 'LOOP-TIMING: judged\r\n' > "$vstub/qdir/qloop.serial"
AXL_STUB_UNJ_TEXT='loop: the timing cases were not run' qrun --dir "$vstub/qdir"
[[ "$QRC" -eq 3 ]] || bad "a stale judged marker in a reused --dir made a run that judged nothing exit $QRC, expected 3"
qrun --dir "$vstub/qdir" extra
[[ "$QRC" -eq 2 ]] || bad "an extra argument was ignored (exit $QRC), expected a usage refusal (2)"
[[ $fail -eq $before ]] && pass "qloop-verdict.sh: its status is the verdict (0/3/other), its last line states it, a harness 3 is a FAIL"

# --- 1k2. WHAT the verdict decides (the cases verify.sh's own engine was
# pinned on, now asked of qloop-verdict.sh directly -- the report helper only
# echoes its last line, so this is where the classification is tested).
# A FAIL made only of missed TIMING bounds while the guest's QEMU waited past
# the limit is UNJUDGED (3): load, not a verdict. Positive test: everything
# that failed must be a named timing miss. Only the guest's own FAIL lines
# count. A bound missed in the only judged attempt is UNJUDGED and NAMED.
before=$fail
qv() {   # qv WANT-STATUS LAST-LINE-REGEX WHAT  (after a qrun)
    [[ "$QRC" -eq "$1" ]] || bad "$3: exited $QRC, expected $1"
    need "$2" "$(qlast)" "$3: verdict line"
}
AXL_STUB_UNJ_TEXT='loop: the timing cases were not run on this box' qrun
qv 3 "^qloop-verdict: UNJUDGED -- no 'LOOP-TIMING: judged' line" \
   "a REWORDED skip (the marker, not the wording, decides)"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 qrun
qv 3 "^qloop-verdict: UNJUDGED -- timing bounds missed while the guest.s QEMU waited 7014 ms for a CPU \(> 50 ms\): load, not a verdict; booted \(no pool\)$" \
   "a timing FAIL under load"
AXL_STUB_FAIL=other AXL_STUB_STARVE=7014 qrun
qv 1 '^qloop-verdict: FAILED: ' "a non-timing FAIL under load"
AXL_STUB_FAIL=timing qrun
qv 1 '^qloop-verdict: FAILED: .*; booted \(no pool\); vCPU starvation not measured$' "an unmeasured timing FAIL"
AXL_STUB_STARVE=7014 qrun
qv 0 "; the guest.s QEMU waited 7014 ms for a CPU$" "CONTROL: a judged PASS under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_EXTRA='*** FAIL: memory leaked at teardown. These blocks were still live' qrun
qv 1 '^qloop-verdict: FAILED: ' "a timing FAIL plus a leak under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_EXTRA='*** STALLED: the following test binary started but never finished' qrun
qv 1 '^qloop-verdict: FAILED: ' "a timing FAIL plus a STALLED verdict under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_EXTRA='  heap check: FAIL in pool 3 (guard bytes overwritten)' qrun
qv 1 '^qloop-verdict: FAILED: ' "a timing FAIL plus another FAIL-bearing line under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 AXL_STUB_NFAILED=2 qrun
qv 1 '^qloop-verdict: FAILED: ' "a Results count the named misses do not explain"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_EXTRA='*** 1 test GROUP(S) SKIPPED (5 assertions declared):' qrun
qv 3 '^qloop-verdict: UNJUDGED' "CONTROL: a skipped-groups notice beside a timing FAIL"
AXL_STUB_FAIL=functional AXL_STUB_STARVE=7014 qrun
qv 1 '^qloop-verdict: FAILED: ' "a functional FAIL under load (load cannot miss a functional check)"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_PASS='loop: a FAILED attach is reported, not retried' qrun
qv 3 '^qloop-verdict: UNJUDGED' "CONTROL: a PASS named with FAIL beside a timing FAIL under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 \
    AXL_STUB_SKIPREASON='loop: retry after a FAILED probe is not built here' qrun
qv 3 '^qloop-verdict: UNJUDGED' "CONTROL: a re-listed skip reason saying FAIL beside a timing FAIL under load"
AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 AXL_STUB_PRE='cc: note: FAIL_ON_WARN is unset' qrun
qv 3 '^qloop-verdict: UNJUDGED' "CONTROL: build output saying FAIL before the header"
AXL_STUB_UNCONFIRMED='loop: a renderer waking on 10 ms driver ticks keeps its frame rate' qrun
qv 3 '^qloop-verdict: UNJUDGED \(unconfirmed miss: loop: a renderer waking on 10 ms driver ticks keeps its frame rate\)' \
   "an unconfirmed miss names its bound"
AXL_STUB_UNCONFIRMED='loop: a renderer waking on 10 ms driver ticks keeps its frame rate|loop: the idle extension is capped at AXL_LOOP_DRIVER_BUDGET_MAX_MS' qrun
qv 3 '^qloop-verdict: UNJUDGED \(unconfirmed misses: loop: a renderer waking on 10 ms driver ticks keeps its frame rate, loop: the idle extension is capped at AXL_LOOP_DRIVER_BUDGET_MAX_MS\)' \
   "two unconfirmed misses are both named"
[[ $fail -eq $before ]] && pass "qloop-verdict.sh: timing FAIL under load is UNJUDGED; quiet, non-timing, functional, leak/stall and uncounted FAILs stay FAIL; FAIL-worded PASS/skip/build lines do not count; an unconfirmed miss names its bounds"

# --- 1k3. WHERE the quiet slot runs: on a leased `unit` guest when a pool
# serves (AXL_ONE_POOL_SOCK, as gate.qloop's registration hands it), after
# asking the pool to settle -- a released guest's reset is the load the slot
# exists to avoid. A pool still busy is named, not waited on forever; a pool
# miss runs it booted, said. The verdict says which.
before=$fail
QSOCK=/stub.sock qrun
qv 0 "^qloop-verdict: AxlTestLoop alone on x64 \(quiet slot\): 2 passed, 0 failed \(X64\) in 3s; on a leased guest; vCPU starvation not measured$" \
   "a pooled judged run"
[[ "$(paste -sd, "$vstub/calls.txt")" == "axlpool settle,axlpool run,ARCH=X64 APPS=AxlTestLoop RATCHET=1 BSCALE=<unset> SCALE=<unset> POOLED" ]] \
    || bad "a pooled qloop did not settle, THEN lease, THEN run AxlTestLoop on the guest ($(paste -sd, "$vstub/calls.txt"))"
QSOCK=/stub.sock AXL_STUB_STARVE=6 qrun
qv 0 "; on a leased guest; the guest.s QEMU waited 6 ms for a CPU$" "a pooled run carries the witness"
QSOCK=/stub.sock AXL_STUB_SETTLE=busy qrun
qv 0 "; on a leased guest, the pool not settled \(not settled after 60 s: g1 resetting\); vCPU starvation not measured$" \
   "a pool still busy after the settle bound is named"
QSOCK=/stub.sock AXL_STUB_POOL=miss qrun
qv 0 "; booted after a pool miss \(no guest: miss \(no-slot\): stub\); vCPU starvation not measured$" \
   "a pool miss runs it booted, said"
[[ "$(grep -c '^ARCH=X64 APPS=AxlTestLoop' "$vstub/calls.txt")" == 1 ]] \
    || bad "a pool miss did not run AxlTestLoop booted exactly once"
QSOCK=/stub.sock AXL_STUB_FAIL=timing AXL_STUB_STARVE=7014 qrun
qv 3 "load, not a verdict; on a leased guest$" "a pooled timing FAIL under load keeps where it ran"
# A miss the harness met AFTER the pooled attempt printed: only the booted
# rerun is the verdict. The attempt's starvation figure sampled ANOTHER QEMU
# and must not excuse the rerun's timing FAIL as load; its serial log's
# judged marker must not stand for a rerun that wrote none.
QSOCK=/stub.sock AXL_STUB_POOL=harness-miss AXL_STUB_FAIL=timing AXL_STUB_POOLED_STARVE=7014 qrun
qv 1 "^qloop-verdict: FAILED: 1 passed, 1 failed \(X64\) in 3s; booted after a pool miss \(the agent could not run it: stub\); vCPU starvation not measured$" \
   "after a harness miss, the discarded attempt's witness does not excuse the rerun's FAIL"
QSOCK=/stub.sock AXL_STUB_POOL=harness-miss AXL_STUB_BOOTED_NOSERIAL=1 qrun
qv 3 "^qloop-verdict: UNJUDGED -- no 'LOOP-TIMING: judged' line in the serial log; booted after a pool miss" \
   "after a harness miss, the discarded attempt's judged marker does not stand"
# A settle that says more than one line (a traceback) cannot split the verdict.
QSOCK=/stub.sock AXL_STUB_SETTLE=traceback qrun
qv 0 "^qloop-verdict: AxlTestLoop alone on x64 \(quiet slot\): 2 passed, 0 failed \(X64\) in 3s; on a leased guest, the pool not settled \(boom: the last line\); vCPU starvation not measured$" \
   "a multi-line settle failure is named by its last line, the verdict one line"
[[ $fail -eq $before ]] && pass "qloop-verdict.sh: pooled, it settles then leases; a busy pool and a miss are named; the verdict says where it ran"

# --- 1f. --apps with no arch job selected is a typo, not a no-op.
before=$fail
vrun --only=docs --apps=AxlTestBeta; out=$(vout)
[[ "$VRC" -eq 2 ]] || bad "--apps with neither arch job exited $VRC, expected 2"
need 'x64|aa64' "$out" "the refusal does not say which jobs --apps affects"
[[ $fail -eq $before ]] && pass "verify.sh --apps refuses when no arch job is selected"

# --- 1h. an EMPTY value is a typo, not "flag absent".
# `--apps=` and `--apps="$VAR"` with VAR unset are the same string, and the
# second is how it happens. Falling back to a full run is the safe DIRECTION
# but it is still the flag silently not applying -- and the same shape in
# --for silently widens to the whole suite, which is not safe at all.
before=$fail
vrun --only=x64,aa64 --apps=
[[ "$VRC" -eq 2 ]] || bad "an empty --apps= exited $VRC, expected 2"
[[ -s "$vstub/calls.txt" ]] && bad "an empty --apps= still ran the arch jobs"
[[ $fail -eq $before ]] && pass "verify.sh refuses an empty --apps="

# --- 1k. the REAL test-axl.sh publishes the list --apps validates against,
# and does it without building anything (verify.sh calls this before it
# launches the jobs; a make here would race the builds it is about to start).
before=$fail
real=$( cd "$REPO" && timeout 60 ./test/integration/test-axl.sh --list-apps 2>&1 )
rc=$?
[[ $rc -eq 0 ]] || bad "test-axl.sh --list-apps exited $rc"
grep -qx 'AxlTestSsh' <<<"$real" || bad "--list-apps does not name AxlTestSsh"
[[ "$(grep -c . <<<"$real")" -ge 20 ]] || bad "--list-apps returned a suspiciously short list"
# What a build prints NOW: Ninja's "[n/m] " status and axl-build.sh's own
# lines (the Makefile's "AXL library built" went with it in 5.0.0, and a
# pattern of only that would be satisfied by every build). Control: the
# pattern must see a real Ninja status line before its silence counts.
built_re='^\[[0-9]+/[0-9]+\] |^ninja: |axl-build\.sh: |Building C|Linking C'
grep -qE -- "$built_re" <<<'[3/10] Building C object src/CMakeFiles/axl.dir/mem/axl-mem.c.obj' \
    || bad "the build-output pattern does not match a Ninja status line (control)"
nonope "$built_re" "$real" "--list-apps ran a build"
[[ $fail -eq $before ]] && pass "test-axl.sh --list-apps is the single source of truth, and builds nothing"


# ---------------------------------------------------------------------------
# 2. verify.sh --lint-files -- refused
# ---------------------------------------------------------------------------
# lint.sh's own refusal, its coverage guard and its default run are
# test-lint-gate-selftest.sh's: they touch only build/x64-lint, so they run
# beside this file instead of lengthening its lane.
# --- 2e. verify.sh refuses --lint-files (the lint job is incremental, so a
# file list would only narrow what it proves). That lint.sh runs BARE is the
# gate.lint registration's now, pinned by test-ctest-integration.sh.
before=$fail
vrun --only=lint --lint-files=a.c
[[ "$VRC" -eq 2 ]] || bad "--lint-files exited $VRC, expected 2 (refused)"
needi 'incremental' "$(vout)" "the --lint-files refusal does not say why"
[[ $fail -eq $before ]] && pass "verify.sh refuses --lint-files, and says why"

# ---------------------------------------------------------------------------
# 3. run-integration.sh --for=<paths>  (and which-gates.sh --porcelain)
# ---------------------------------------------------------------------------
# --for hands the "which test covers this file?" question to which-gates.sh
# rather than answering it again here. These assertions are about the HAND-OFF:
# that the answer is taken machine-readably (not regexed out of prose), that
# what is NOT runnable is dropped with a reason rather than silently, and that
# an empty answer FAILS instead of widening to 204 tests.
WG="$REPO/scripts/which-gates.sh"

# --- 3a. porcelain is machine-readable, and answers about explicit PATHS
# (there is no diff to read: --for names files, not a commit range).
before=$fail
po=$("$WG" --porcelain --files src/net/axl-ssh-server.c 2>&1); rc=$?
[[ $rc -eq 0 ]] || bad "which-gates --porcelain exited $rc"
need '^jobs=x64,aa64,lint,gates,imggates$' "$po" "porcelain did not name the verify.sh jobs"
need '^cover=test/integration/test-ssh-hostile-qemu\.sh$' "$po" \
     "porcelain did not name the covering test"
# Machine-readable means EVERY line parses. A stray prose line is how a
# consumer ends up regexing the output after all.
while read -r l; do
    [[ -z "$l" ]] && continue
    grep -qE '^(jobs|cover|vague|missing|note)=' <<<"$l" \
        || bad "porcelain emitted a non key=value line: $l"
done <<<"$po"
[[ $fail -eq $before ]] && pass "which-gates --porcelain --files is machine-readable"

# --- 3a1. a CMakeLists.txt in ANY directory is the build, not only the root
# one: the arm once matched `CMakeLists.txt` literally, so a directory's own
# file routed to no job at all.
before=$fail
for f in sdk/CMakeLists.txt CMakeLists.txt cmake/AxlImages.cmake; do
    po=$("$WG" --porcelain --files "$f" 2>&1)
    # imggates too since slice 5c: a build edit can add a link input to the
    # size-capped image (check-size-gate-paths, an artifact gate).
    need '^jobs=lint,gates,imggates$' "$po" "$f was not routed to the lint+gates+imggates jobs"
done
# ...but NOT a CMakeLists.txt that is not this tree's build: vendored
# code's own. (The consumer's shipped example project is example CODE, which
# routes to every arch job as sdk/examples/* does.)
po=$("$WG" --porcelain --files deps/mbedtls/CMakeLists.txt 2>&1)
nonope '^jobs=.*lint' "$po" "deps/mbedtls/CMakeLists.txt is not the tree's build, yet routed to lint"
# A gate's OWN implementation routes to the gates: the scripts the CTest
# registration runs (read off it), and the compile-like helper.
for f in scripts/check-cxx-entry.sh scripts/check-examples.sh scripts/size-gate-inputs.sh; do
    po=$("$WG" --porcelain --files "$f" 2>&1)
    need '^jobs=gates,imggates$' "$po" "$f (a gate's implementation) was not routed to gates+imggates"
done
need '^jobs=lint,gates,imggates$' "$("$WG" --porcelain --files cmake/AxlIntegrationTests.cmake.in 2>&1)" \
    "the gate registration was not routed to lint+gates+imggates"
# ...and ONLY a gate's: the registration also runs gate.docs, gate.lint and
# gate.qloop, whose scripts keep their own jobs (review: an unscoped lookup
# sent build-docs.sh to every job, and qloop-verdict.sh to gates+imggates).
need '^jobs=docs$' "$("$WG" --porcelain --files scripts/build-docs.sh 2>&1)" \
    "build-docs.sh left the docs job alone"
nonope '^jobs=.*imggates' "$("$WG" --porcelain --files scripts/qloop-verdict.sh 2>&1)" \
    "qloop-verdict.sh (not a check gate) was routed to imggates"
nonope '^jobs=.*imggates' "$("$WG" --porcelain --files scripts/lint.sh 2>&1)" \
    "lint.sh (not a check gate) was routed to imggates"
need '^jobs=gates,imggates$' "$("$WG" --porcelain --files test/integration/lib/discover.sh 2>&1)" \
    "discover.sh (gate.check-test-meta's implementation) was not routed to the gates"
[[ $fail -eq $before ]] && pass "which-gates routes every CMakeLists.txt and names the image comparison"

# --- 3a2. `--files` with no paths must not silently answer a DIFFERENT
# question. It fell through to the git-diff branch and reported on the
# working tree -- which is the defect this file's own comment records for
# `$#`, reintroduced one flag over.
before=$fail
po2=$("$WG" --porcelain --files 2>&1); rc=$?
[[ $rc -ne 0 ]] || bad "which-gates --files with no paths exited 0 (it read the git diff instead)"
needi 'no path|empty' "$po2" "the refusal does not say what was wrong"
[[ $fail -eq $before ]] && pass "which-gates --files with no paths is refused, not answered from the diff"

# --- 3b. a path that does not exist is NOT the same fact as a path nothing
# covers, and they must not print the same thing.
before=$fail
po=$("$WG" --porcelain --files src/net/no-such-file.c 2>&1)
need '^missing=src/net/no-such-file\.c$' "$po" \
     "porcelain did not distinguish a path that does not exist"
[[ $fail -eq $before ]] && pass "which-gates --porcelain reports a path it cannot see"

# --- 3c. the PROSE mode is unchanged -- it is what a human runs, and CI's
# plan step and CLAUDE.md both point at it.
before=$fail
# NOT `"$WG" HEAD~1`, which is what this used to be: CI checks out with
# actions/checkout's default fetch-depth of 1, so HEAD~1 DOES NOT EXIST
# there. This assertion passed on every developer clone and failed on CI
# the first time it ran, which is the "passes for the wrong reason locally"
# shape -- and the script's own answer was worse than the test's: it
# swallowed git's fatal and printed "no changes — nothing to gate", exit 0.
# `--files` names the input outright and needs no history at all.
pr=$("$WG" --files src/net/axl-ssh-server.c 2>&1)
need '^which-gates — [0-9]+ file' "$pr" "the prose mode changed"
need 'RUN:' "$pr" "the prose mode lost its RUN: section"
nonope '^cover=' "$pr" "the prose mode leaked porcelain lines"
[[ $fail -eq $before ]] && pass "which-gates prose mode is unchanged"

# --- 3c2. the prose list of covering tests holds ONLY runnable tests. A
# helper that merely names a changed file (release-fixture.sh) used to be
# listed beside the tests, and pasting the list into `run-integration.sh
# --only=` made the runner refuse all of it. Asserted against the runner's own
# --list, not a name: every line of the runnable list must be something
# --list prints. CONTROLS, so "the filter dropped everything" cannot pass:
# a real covering test (test-toolchain-prefix.sh) must still be listed, and the helper
# must still be reported -- under the "cannot run" comment, not lost.
before=$fail
pr=$("$WG" --files scripts/install-toolchain.sh 2>&1)
# Up to the DO NOT: section, whose own ./test/... line is advice, not a result.
listed=$(sed '/^DO NOT/q' <<<"$pr" | grep -E '^    \./test/integration/' | sed 's|^    \./||')
runlist=$("$REPO/test/integration/run-integration.sh" --list 2>/dev/null \
          | sed "s|^$REPO/||")
need 'test/integration/test-toolchain-prefix\.sh' "$listed" \
     "CONTROL: a real covering test vanished from the runnable list"
nonope 'release-fixture' "$listed" "a helper is listed as a covering TEST"
need '#   \./test/integration/release-fixture\.sh' "$pr" \
     "the helper is no longer reported at all (it must be named as not runnable)"
while IFS= read -r l; do
    [[ -n "$l" ]] || continue
    grep -qxF -- "$l" <<<"$runlist" \
        || bad "runnable list names '$l', which run-integration.sh --list does not"
done <<<"$listed"
[[ $fail -eq $before ]] && pass "which-gates lists only discoverable tests as covering, and names helpers apart"

# --- 3c3. the name predicate is shared and agrees with discovery, and the
# prose mode does not run discovery when nothing is covered. The predicate is
# checked against discover_tests itself: every discovered file must satisfy it,
# and the names discovery is documented to drop must not (CONTROLS, so a
# predicate that says yes to everything cannot pass).
before=$fail
# shellcheck source=lib/discover.sh
source "$REPO/test/integration/lib/discover.sh"
while IFS= read -r f; do
    discover_is_test_name "$f" || bad "discovery lists $f but discover_is_test_name refuses it"
done < <(discover_tests --arch X64 --include-local-only)
for n in test-axl.sh test-all.sh test-foo-selftest.sh release-fixture.sh lib/discover.sh fs-read-common.c \
         test/integration/lib/test-cache.sh test/integration/lib/pack-lib.sh; do
    discover_is_test_name "$n" && bad "CONTROL: discover_is_test_name accepted $n"
done
discover_is_test_name test/integration/test-http.sh || bad "discover_is_test_name refused test-http.sh"
t0=$SECONDS
"$WG" --files README.md > /dev/null 2>&1
[[ $((SECONDS - t0)) -le 2 ]] || bad "which-gates on a file nothing covers took $((SECONDS - t0)) s (discovery ran)"
pr=$("$WG" --files scripts/install-toolchain.sh 2>&1)
need "test-axl\\.sh is verify\\.sh's" "$pr" "the not-runnable explanation is not the runner's wording"
[[ $fail -eq $before ]] && pass "discover_is_test_name is shared, agrees with discovery, and which-gates stays fast"

# --- 3c4. SIZE GATES: a change that can grow a size-capped image names the
# test that caps it, though that test names none of the files. A loop change
# grew sshd.efi by 512 B past test-efi-pack-release-qemu.sh's limit and the
# path grep could not see it -- twice. Routing is a literal match on the
# test's declared size-gate-paths=. Paths ASSEMBLED, never literal, so this
# file does not become their covering test by naming them (see 3f).
#
# CONTROLS in both directions: a design doc and an unlinked integration
# script must NOT pull in the 112 s release test, so an answer of "the size
# gate covers everything" cannot pass. tools/sshd-svc.c, not tools/sshd.c:
# the release test already names "sshd.c", so the path grep alone would
# answer for that one and prove nothing about the size gate.
before=$fail
SG=test/integration/test-efi-pack-release-qemu.sh
wg_routes() {   # wg_routes <path>: does which-gates name the size gate for it?
    # Captured, not piped: the answer is in WG_OUT so a miss can show it.
    WG_OUT=$("$WG" --porcelain --files "$1" 2>&1)
    WG_RC=$?
    grep -qx "cover=$SG" <<<"$WG_OUT"
}
for p in "src/loop/axl-$(echo loop).c" "tools/sshd-$(echo svc).c" \
         "scripts/elf_x86_64_$(echo efi).lds" "include/axl/axl-$(echo loop).h" \
         "deps/$(echo mbed)tls" "./src/loop/axl-$(echo loop).c" \
         "$REPO/src/loop/axl-$(echo loop).c"; do
    # deps/mbedtls is the bare gitlink path `git diff --name-only` reports for
    # a submodule bump -- mbedTLS is the largest thing sshd links.
    # A miss prints which-gates' own answer and status: this failed ONCE in a
    # full run-selftests.sh pass (2026-11-09, the x86_64 linker script) and
    # passed alone twice, with nothing on record to say what it answered.
    wg_routes "$p" || bad "a change to $p (linked into sshd.efi) does not name the size gate (which-gates rc=$WG_RC: $(tr '\n' ' ' <<<"$WG_OUT" | cut -c1-400))"
done
# src/loop/README.md is under the src/ prefix and excluded by the test's
# size-gate-exclude=*.md: prose cannot move the image. (axl-loop.c, beside it,
# still routes -- asserted above -- so this is the exclusion, not the prefix.)
for p in "docs/AXL-$(echo Design).md" "test/integration/test-cpu-$(echo idle).sh" \
         "src/loop/READ$(echo ME).md"; do
    wg_routes "$p" && bad "$p is not linked into sshd.efi, yet it names the size gate"
done
# ...and NOTHING in the caller's environment moves the answer: an aa64 shell,
# a stray PREFIX, a parent make's MAKEFLAGS.
plain=$("$WG" --porcelain --files "src/crt0/axl-crt0-gcc-x86_$(echo 64).S" 2>&1)
envd=$(ARCH=aa64 PREFIX=/tmp/nowhere MAKEFLAGS=ARCH=aa64 \
       "$WG" --porcelain --files "src/crt0/axl-crt0-gcc-x86_$(echo 64).S" 2>&1)
need "^cover=$SG\$" "$plain" "the x64 crt0 does not name the size gate"
[[ "$plain" == "$envd" ]] || bad "which-gates answered differently under ARCH=aa64/PREFIX/MAKEFLAGS"
pr=$("$WG" --files "src/loop/axl-$(echo loop).c" 2>&1)
need "SIZE GATE" "$pr" "the prose mode does not say why the size gate is listed"
need "$SG <- src/loop/axl-loop\\.c" "$pr" "the prose mode does not name the matched file"
[[ $fail -eq $before ]] && pass "which-gates routes what can grow a size-gated image to its gate, and only that"

# --- 3c5. the DRIFT CHECK behind that declaration (gate.check-size-gate-paths):
# the CMake build's own record of what links into sshd (ninja -t inputs and
# -t deps over the x64 DEBUG tree) must lie inside the declared prefixes. The
# caller's ARCH, BUILD, RTK_USB_UNDI and PREFIX are set here on purpose and
# must change nothing: the tree is the x64 DEBUG one, named, never inherited.
before=$fail
sgl=$(ARCH=aa64 BUILD=RELEASE RTK_USB_UNDI=/nowhere/nic.efi PREFIX=/tmp/nowhere MAKEFLAGS=ARCH=aa64 \
      "$REPO/scripts/size-gate-inputs.sh" "$REPO/$SG" 2>&1); rc=$?
[[ $rc -eq 0 ]] || bad "size-gate-inputs.sh exited $rc: $(head -5 <<<"$sgl")"
# The x64 image, whatever the caller's shell says.
need '^scripts/elf_x86_64_efi\.lds$' "$sgl" "the derivation lost the x64 linker script"
nonope '^scripts/elf_aarch64_efi\.lds$' "$sgl" "the derivation answered for aa64 (the caller's ARCH leaked in)"
need '^scripts/efi-localize\.ver$' "$sgl" "the derivation lost the version script in AXL_LDFLAGS_EFI"
# A header no -I directory holds, reachable only as src/gfx's
# "../../deps/stb/stb_truetype.h": a scan that stopped resolving "../"
# includes, or normalised them away, loses it.
need '^deps/stb/stb_truetype\.h$' "$sgl" "a header reached only through a \"../\" include is missing"
nonope '^/|(^|/)\.\.?/' "$sgl" "size-gate-inputs.sh printed a path no change can match"
ck=$("$REPO/scripts/size-gate-inputs.sh" --check 2>&1); rc=$?
[[ $rc -eq 0 ]] || bad "check-size-gate-paths is not clean on the tree: $ck"
# CONTROLS: the gate must be able to FAIL, once per way a declaration can
# stop routing what sshd links. Each is a copy of the release test's header
# with ONE edit; the edit is asserted to have happened, so a sed that matched
# nothing fails here instead of passing for the wrong reason.
sgfix=$(mktemp -d)
sgfix_check() {   # sgfix_check <sed expr> -> $ckf, $rc
    awk '/^[[:space:]]*($|#)/ { print; next } { exit }' "$REPO/$SG" \
        | sed "$1" > "$sgfix/test-sgfix-qemu.sh"
    cmp -s <(awk '/^[[:space:]]*($|#)/ { print; next } { exit }' "$REPO/$SG") \
        "$sgfix/test-sgfix-qemu.sh" && bad "CONTROL: the header edit '$1' changed nothing"
    ckf=$("$REPO/scripts/size-gate-inputs.sh" --check "$sgfix/test-sgfix-qemu.sh" 2>&1); rc=$?
}
# 1. a prefix dropped: deps/ gone, so sshd's mbedTLS files are uncovered.
sgfix_check '/size-gate-paths=/s|deps/,||'
[[ $rc -ne 0 ]] || bad "CONTROL: the drift check passed a size-gate-paths= with deps/ removed"
need '^    deps/mbedtls/library/[a-z_0-9]+\.c$' "$ckf" "the drift check did not name the uncovered file"
# 2. deps/ narrowed to one prefix per library (and deps/CMakeLists.txt, the
# CMake build's own definition of them). Every FILE is still covered --
# but a submodule bump arrives as the bare deps/mbedtls, which deps/mbedtls/
# does not match, so routing for it silently ends. Only the submodule rule
# sees that.
sgfix_check '/size-gate-paths=/s|deps/,|deps/CMakeLists.txt,deps/mbedtls/,deps/lzma/,deps/libvterm/,deps/ed25519-ref10/,deps/freetype/,deps/sdefl/,deps/stb/,|'
[[ $rc -ne 0 ]] || bad "CONTROL: the drift check passed deps/ narrowed past the submodule path"
need 'submodule\(s\) whose own path it does not route' "$ckf" "the drift check did not report the unrouted submodule"
need '^    deps/mbedtls$' "$ckf" "the drift check did not name the submodule"
nonope 'does not cover,' "$ckf" "CONTROL: the narrowed prefixes left a FILE uncovered, so this proves nothing about submodules"
# 3. an exclusion that hides a linked file: *.c excludes sshd's own sources.
sgfix_check 's|size-gate-exclude=\*\.md|size-gate-exclude=*.md,*.c|'
[[ $rc -ne 0 ]] || bad "CONTROL: the drift check passed a size-gate-exclude= that hides linked files"
need "size-gate-exclude= matches file\\(s\\) it LINKS" "$ckf" "the drift check did not report the hiding exclusion"
need '^    src/loop/axl-loop\.c$' "$ckf" "the drift check did not name the excluded linked file"
# 4. the submodule rule checks the RIGHT list: the index's gitlinks, which
# hold deps/mbedtls (the path a bump arrives as) whether or not .gitmodules
# agrees. An empty or wrong list would leave control 2 above the only thing
# standing between a narrowed prefix and a silent clean.
need '^check-size-gate-paths: submodules \(index gitlinks\): .*deps/mbedtls' "$ck" \
     "the drift check's submodule list does not hold deps/mbedtls"
rm -rf "$sgfix"
[[ $fail -eq $before ]] && pass "check-size-gate-paths derives sshd's inputs env-independently and fails on an uncovered file, an unrouted submodule and a hiding exclusion"

# --- 3d. and the case that reached CI: a base ref this repository does not
# have must be REFUSED, not answered with "nothing to gate". A bogus name
# exercises the same code path as a shallow clone's missing HEAD~1 and needs
# no shallow fixture to drive.
before=$fail
unres=$("$WG" no-such-ref-xyz 2>&1); rc=$?
[[ $rc -eq 2 ]] || bad "an unresolvable base ref did not exit 2 (got $rc)"
needi 'cannot resolve' "$unres" "the refusal does not name the unresolvable ref"
nonope 'nothing to gate' "$unres" \
     "an unresolvable ref still reported 'nothing to gate' -- the gate answering for a diff it could not read"
[[ $fail -eq $before ]] && pass "which-gates refuses a base ref it cannot resolve"

# --- 3d. --for derives, PRINTS what it chose, and narrows.
before=$fail
fo=$(cd "$REPO" && ./test/integration/run-integration.sh \
        --for=src/net/axl-ssh-server.c --list 2>&1); rc=$?
[[ $rc -eq 0 ]] || bad "--for --list exited $rc"
need 'test-ssh-hostile-qemu\.sh' "$fo" "--for did not derive the covering test"
need 'which-gates' "$fo" "--for did not say where the selection came from"
# NARROWED: the full discovery is 200+ tests. If --for ever widened to the
# suite this count is the assertion that notices.
# Count only the SELECTED list -- --list prints it as absolute paths. The
# DROPPED list names covering files by repo-relative path, and a dropped
# *-selftest.sh matches test-*.sh too; counting it read 4 selected as 6.
nsel=$(grep -c '^/.*/test/integration/test-[^/]*\.sh$' <<<"$fo")
[[ "$nsel" -ge 1 && "$nsel" -le 5 ]] \
    || bad "--for selected $nsel test(s) -- it widened instead of narrowing"
[[ $fail -eq $before ]] && pass "--for derives a narrow set and names its source"

# --- 3e. covering files that are NOT runnable tests are dropped WITH A
# REASON. which-gates legitimately names lib/*.sh helpers and *-selftest.sh
# files as covering; run-integration cannot run either. Dropping them quietly
# is the same defect as widening quietly, one direction over.
before=$fail
# Which SSH helper library names axl-ssh-server.c moves as code moves (the cap
# regex moved from ssh-hostile-lib.sh to ssh-guest-lib.sh); any lib/ helper
# reported as dropped proves the point.
need 'test/integration/lib/[a-z0-9-]+\.sh' "$fo" "--for did not report the covering file it dropped"
needi 'dropped|not a discovered test|discovery' "$fo" \
     "--for dropped a covering file without saying why"
[[ $fail -eq $before ]] && pass "--for names the covering files it could not run, and why"

# --- 3f. resolving to NOTHING fails; it does NOT fall back to the suite.
# Silently widening is the failure this repo keeps meeting.
#
# The fixture is CHOSEN, not hardcoded: "a file no test names" is a property
# of the tree today, and a literal here would turn the day someone writes a
# covering test for it into a mysterious self-test failure. If every
# candidate acquires coverage, that is reported as an untestable assertion
# rather than passing by default.
before=$fail
# ASSEMBLED FROM PIECES, never written out whole. which-gates finds a
# covering test by grepping test/ for the path and for its basename -- so a
# literal candidate path in this file would make THIS file its covering test,
# and the search for an uncovered path could never succeed. It did not, the
# first time.
uncovered=""
for cand in "src/data/axl-$(echo list).c" "src/data/axl-$(echo queue).c" \
            "src/acpi/axl-$(echo acpi).c" "docs/AXL-$(echo Coding)-Style.md"; do
    [[ -e "$REPO/$cand" ]] || continue
    if [[ -z "$("$WG" --porcelain --files "$cand" | sed -n 's/^cover=//p')" ]]; then
        uncovered="$cand"; break
    fi
done
if [[ -z "$uncovered" ]]; then
    bad "cannot test the empty resolution: every candidate path now has a covering test"
else
    fo2=$(cd "$REPO" && ./test/integration/run-integration.sh \
            --for="$uncovered" --list 2>&1); rc=$?
    [[ $rc -ne 0 ]] || bad "--for that resolves to nothing exited 0"
    needi 'no covering test' "$fo2" "--for's empty resolution does not say so"
    nonope 'test-cpu-spike-qemu\.sh' "$fo2" "--for widened to the whole suite on an empty resolution"
fi
[[ $fail -eq $before ]] && pass "--for fails on an empty resolution instead of widening ($uncovered)"

# --- 3g. ...and "covering files exist but none of them is runnable" is a
# DIFFERENT fact from "nothing covers this", so it gets a different message.
#
# The fixture is a script THIS file tests, covered by no runnable test (only
# *-selftest.sh files, which lib/discover.sh excludes by name, and lib/
# helpers). It is CHOSEN, not fixed: scripts/verify.sh was the fixed one
# until test-release-suite.sh rightly began naming it (the unit step runs
# verify), and the assertion then failed on a fact that had changed.
before=$fail
selfonly=""
for cand in scripts/verify.sh scripts/which-gates.sh scripts/run-selftests.sh; do
    # Captured first: which-gates that cannot run gives no covers either,
    # and "no runnable test names it" must not be read off a failure.
    if ! wgo=$("$WG" --porcelain --files "$cand" 2>&1); then
        bad "which-gates could not answer for $cand: $wgo"; break
    fi
    runnable=$(sed -n 's/^cover=//p' <<<"$wgo" | grep -E '/test-[^/]*\.sh$' | grep -cvE -- '-selftest\.sh$')
    if [[ "$runnable" == 0 ]]; then
        selfonly="$cand"; break
    fi
done
[[ -n "$selfonly" ]] || bad "cannot test 3g: every candidate now has a runnable covering test"
fo3=$(cd "$REPO" && ./test/integration/run-integration.sh \
        --for="${selfonly:-scripts/verify.sh}" --list 2>&1); rc=$?
[[ $rc -ne 0 ]] || bad "--for on a path with no RUNNABLE covering test exited 0"
needi 'none is a runnable test' "$fo3" \
     "--for did not distinguish 'nothing covers it' from 'nothing runnable covers it'"
need 'selftest\.sh' "$fo3" "--for did not name the covering file it found"
need 'run-selftests\.sh' "$fo3" "--for did not say where a self-test IS run"
[[ $fail -eq $before ]] && pass "--for distinguishes 'no coverage' from 'no RUNNABLE coverage'"

# --- 3g2. a path too COMMON to attribute is its own fact, and saying
# "no file under test/ names any of those paths" about it is FALSE.
#
# which-gates has four outcomes, not three: cover=, vague=, missing= and
# none. `scripts/run-qemu.sh` is named by 78 test files and is reported
# vague=; --for consumed only three of the four and told the caller nothing
# covers it. These are the HIGHEST-coverage files in the tree, so it was
# wrong exactly where being wrong costs most.
before=$fail
vaguepath=""
for cand in "scripts/run-$(echo qemu).sh" "scripts/$(echo install).sh" "Make$(echo file)"; do
    [[ -e "$REPO/$cand" ]] || continue
    if [[ -n "$("$WG" --porcelain --files "$cand" | sed -n 's/^vague=//p')" ]]; then
        vaguepath="$cand"; break
    fi
done
if [[ -z "$vaguepath" ]]; then
    bad "cannot test the vague path: no candidate is reported vague= any more"
else
    fo8=$(cd "$REPO" && ./test/integration/run-integration.sh \
            --for="$vaguepath" --list 2>&1); rc=$?
    [[ $rc -ne 0 ]] || bad "--for on a name too common to attribute exited 0"
    nonope 'No file under test/ names' "$fo8" \
           "--for said nothing names a path that $vaguepath's own lookup attributes to dozens of tests"
    needi 'too common|cannot attribute|vague' "$fo8" \
          "--for did not say the name was too common to attribute"
    # MIXED: one attributable path and one vague one. The vague one must not
    # vanish -- "2 path(s) -> 1 covering test(s)" reads as both accounted for.
    fo9=$(cd "$REPO" && ./test/integration/run-integration.sh \
            --for="src/net/axl-ssh-server.c,$vaguepath" --list 2>&1)
    need "$(basename "$vaguepath")" "$fo9" \
         "--for silently dropped the unattributable path from a mixed selection"
    # ...and says explicitly that the run is narrower than what was asked
    # for. Asserted on its own: the warning block above already names the
    # path, so a grep for the path alone cannot tell whether this line is
    # there -- which a sabotage of exactly that line demonstrated.
    needi 'NOT covered by this run|narrower than' "$fo9" \
          "--for did not say the selection is narrower than the paths named"
fi
[[ $fail -eq $before ]] && pass "--for reports a name too common to attribute, and never claims nothing names it"

# --- 3h. a path that does not exist fails loudly, naming the path.
before=$fail
fo4=$(cd "$REPO" && ./test/integration/run-integration.sh \
        --for=src/net/no-such-file.c --list 2>&1); rc=$?
[[ $rc -ne 0 ]] || bad "--for on a nonexistent path exited 0"
need 'no-such-file\.c' "$fo4" "--for did not name the path it could not see"
[[ $fail -eq $before ]] && pass "--for on a path that does not exist fails loudly"

# --- 3i. --for and --only together are ambiguous, not additive.
before=$fail
(cd "$REPO" && ./test/integration/run-integration.sh \
        --for=src/net/axl-ssh-server.c --only=test-ssh-auth-qemu.sh --list >/dev/null 2>&1); rc=$?
[[ $rc -ne 0 ]] || bad "--for with --only exited 0"
[[ $fail -eq $before ]] && pass "--for refuses to combine with --only"

# --- 3k. an EMPTY selection value must not silently become the whole suite.
# This is the dangerous direction: `--for=` and `--only=` both resolved to
# "flag absent" and selected all 204 tests, so a scoping run driven by an
# unset shell variable ran the full suite believing it had scoped. --only had
# the defect before --for existed.
before=$fail
for flag in --for= --only=; do
    fo7=$(cd "$REPO" && ./test/integration/run-integration.sh "$flag" --list 2>&1); rc=$?
    [[ $rc -ne 0 ]] || bad "an empty $flag exited 0"
    n=$(grep -c 'test/integration/test-.*\.sh$' <<<"$fo7")
    [[ "$n" -eq 0 ]] || bad "an empty $flag selected $n test(s) -- it widened to the suite"
    needi 'empty|no value' "$fo7" "the refusal for an empty $flag does not say what was wrong"
done
[[ $fail -eq $before ]] && pass "an empty --for= / --only= is refused, not read as 'run everything'"

# --- 3j. a --for run is PARTIAL and never speaks as a complete run.
#
# Driven through the stub seam so it costs no QEMU, with AXL_WHICH_GATES
# standing in for the resolver (the stub tests exist nowhere the real one
# would look). No run-integration.sh run writes the release stamp any more --
# only scripts/release-suite.sh does (design §14.4 R-A) -- so what a COMPLETE
# green uncached run does instead is say it does not certify a release. THE
# CONTROL IS THE POINT: the same stub set, unfiltered, DOES say it -- without
# it, its absence from the --for run would prove nothing at all.
before=$fail
rstub=$(mktemp -d)
trap 'rm -rf "$vstub" "$rstub"' EXIT
printf '#!/bin/bash\nexit 0\n' > "$rstub/test-pass-qemu.sh"
printf '#!/bin/bash\nexit 0\n' > "$rstub/test-other-qemu.sh"
cat > "$rstub/wg-stub.sh" <<STUBWG
#!/bin/bash
echo "jobs=x64"
echo "cover=$rstub/test-pass-qemu.sh"
STUBWG
chmod +x "$rstub"/*.sh
stamp="$rstub/stamp.txt"

# CONTROL: a full, unfiltered, uncached stub run says it certifies nothing,
# and writes no stamp. --no-pool on both runs: the runner pools by default,
# and these stubs must not depend on this host's live service.
fo5=$( cd "$REPO/test/integration" \
       && RUN_INTEGRATION_DIR="$rstub" AXL_STAMP_FILE="$stamp" \
          ./run-integration.sh -j1 --timeout 10 --no-cache --no-pool 2>&1 )
need 'this run does not certify a release -- scripts/release-suite\.sh does' "$fo5" \
    "CONTROL FAILED: an unfiltered stub run did not say it certifies nothing, so the --for assertion below proves nothing"
[[ -f "$stamp" ]] && bad "a complete run-integration.sh run WROTE the release stamp -- only release-suite.sh may"
rm -f "$stamp"

fo6=$( cd "$REPO/test/integration" \
       && RUN_INTEGRATION_DIR="$rstub" AXL_STAMP_FILE="$stamp" \
          AXL_WHICH_GATES="$rstub/wg-stub.sh" \
          ./run-integration.sh -j1 --timeout 10 --no-cache --no-pool \
          --for=whatever.c 2>&1 )
[[ -f "$stamp" ]] && bad "a --for run WROTE the release stamp"
need 'PARTIAL -- --for' "$fo6" "a --for run did not announce itself PARTIAL"
need 'test-pass-qemu\.sh' "$fo6" "the --for run did not run the derived test"
nonope 'test-other-qemu\.sh (PASS|FAIL)' "$fo6" "--for ran a test outside the derived set"
nonope 'does not certify a release' "$fo6" "a --for run spoke as a complete run"
[[ $fail -eq $before ]] && pass "--for is PARTIAL, runs only the derived set, and writes no release stamp"

echo ""
echo "gate-scoping self-test: $([[ $fail -eq 0 ]] && echo PASS || echo FAIL)"
exit $fail
