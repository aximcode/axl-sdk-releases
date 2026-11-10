#!/bin/bash
# test-meta: arch=none needs=cmake,ctest,jq est=80 local-only=0 builds=build.ninja
# test-ctest-integration.sh -- the integration scripts are CTest tests
# (slice 5a step 2, docs/AXL-Build-System-Design.md §10.8).
#
# Each debug build tree registers, at CTEST time (TEST_INCLUDE_FILES runs
# lib/discover.sh --emit-ctest, the way gtest_discover_tests works), one
# test per integration script its arch runs, named integration.<script>,
# run through lib/run-one.sh, with:
#   LABELS            integration, arch:<x64|aa64|none>, needs:<tool>...,
#                     local-only, guest:<class>
#   COST              est=, for a test the tree's CTestCostData.txt does not
#                     name (ctest starts the costliest first, by its measured
#                     average when it has one -- and ignores that average for
#                     any test with a COST)
#   PROCESSORS        cpu= (default 1), the slots of -j it holds
#   TIMEOUT           AXL_IT_TIMEOUT from the environment, else 900
#   SKIP_RETURN_CODE  77
#   FIXTURES_REQUIRED integration.prep -- the suite's build-once prep
#                     step, as a FIXTURES_SETUP test
# arch=none (host-only) tests register in the x64 tree only, so a run of both
# trees runs each once.
#
# Reads the real build/x64-debug and build/aa64-debug trees with `ctest -N`
# (nothing runs), then RUNS a stub directory (AXL_IT_DIR) -- a pass, a skip
# and a fail -- through ctest, which proves discovery happens at ctest time
# (no reconfigure) and that run-one.sh's verdicts reach CTest. Boots nothing.
#
# Usage: ./test/integration/test-ctest-integration.sh

set -uo pipefail
# Hermetic against the runner it runs on: CI (set by GitHub Actions) makes
# a registration with no pool service SAY so, which the developer-host cases
# below pin as silent; the one case that wants it sets CI itself.
unset CI

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
export TEST_SKIP_RATCHET=1
# shellcheck source=lib/discover.sh
source "$SCRIPT_DIR/lib/discover.sh"

# The registration's knobs, unset: a caller's (CI exports AXL_IT_TIMEOUT)
# would change what this test reads.
unset AXL_IT_DIR AXL_SHELL_LAUNCHER
# Run under a test preset (CI: ctest --preset x64-ci), this inherits the
# preset's environment; clear every key any preset sets (lib/preset-env.sh).
# shellcheck source=lib/preset-env.sh
source "$SCRIPT_DIR/lib/preset-env.sh"
preset_env_unset "$PROJECT_DIR" || exit 1

WORK="$(mktemp -d -t axl-ctest-it.XXXXXXXX)"
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

# The trees must be configured from THIS source: the registration is a
# configure-time TEST_INCLUDE_FILES entry. Brought up to date, not rebuilt:
# the `build.ninja` goal configures a tree that has none and re-runs CMake
# only when an input of the configure changed -- and through axl-build.sh,
# under its per-tree build lock. These are the SHARED trees the rest of the
# suite is building in at this moment: an unconditional `cmake --preset`
# here re-ran CMake under a dozen concurrent builds, and once made every
# wrapped image stale mid-run (CI, 2026-11-09; test-cmake-reconfigure-noop.sh,
# test-axl-build-lock.sh). The tests below read the registration and run
# stubs, never an image. `build.ninja` is the tree's own regeneration edge.
for a in x64 aa64; do
    if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$a" build.ninja >"$WORK/cfg-$a.log" 2>&1; then
        echo "FAIL: could not configure build/$a-debug"; tail -5 "$WORK/cfg-$a.log"; exit 1
    fi
done

# listed TREE [ENV...] -> the integration.* test names ctest registers, sorted.
listed() {
    local tree="$1"; shift
    env "$@" ctest --test-dir "$PROJECT_DIR/$tree" -N -L '^integration$' 2>/dev/null \
        | sed -n 's/^ *Test *#[0-9]*: \(integration\.test-[^ ]*\)$/\1/p' | sort
}
# discovered ARCH -> what discover.sh selects, as CTest names.
discovered() {
    local f b
    while IFS= read -r f; do
        b=$(basename "$f" .sh)
        if [[ "$1" == AARCH64 && "$(test_meta_field "$f" arch)" == none ]]; then continue; fi
        echo "integration.$b"
    done < <(discover_tests --arch "$1" --include-local-only) | sort
}

echo "=== registration: one CTest test per script the arch runs ==="
x_list=$(listed build/x64-debug); a_list=$(listed build/aa64-debug)
x_want=$(discovered X64); a_want=$(discovered AARCH64)
check "x64 tree: the registered tests are exactly discovery's ($(grep -c . <<< "$x_want"))" \
    "$(diff <(echo "$x_list") <(echo "$x_want") >/dev/null && echo same || echo differ)" "same"
check "aa64 tree: the registered tests are exactly discovery's, minus arch=none ($(grep -c . <<< "$a_want"))" \
    "$(diff <(echo "$a_list") <(echo "$a_want") >/dev/null && echo same || echo differ)" "same"
check "more than 150 registered on x64 (a parser that matched nothing is not 'same')" \
    "$(( $(grep -c . <<< "$x_list") > 150 ))" "1"
check "an arch=none test (test-axl-build-backend) is in the x64 tree" \
    "$(grep -cx 'integration.test-axl-build-backend' <<< "$x_list")" "1"
check "...and not in the aa64 tree" \
    "$(grep -cx 'integration.test-axl-build-backend' <<< "$a_list")" "0"
check "the build-once step is registered (integration.prep)" \
    "$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N -R '^integration\.prep$' 2>/dev/null | grep -c 'integration.prep')" "1"

echo "=== a test's properties come from its test-meta header ==="
# test-tar-qemu.sh: arch=both, est known, no needs; test-sshd-flags-qemu has needs.
props() {   # props TREE NAME -> "KEY=VALUE" lines for that test (lists ;-joined)
    ctest --test-dir "$PROJECT_DIR/$1" --show-only=json-v1 -R "^$2\$" 2>/dev/null \
        | jq -r --arg n "$2" '.tests[] | select(.name == $n)
            | (.properties[] | "\(.name)=\(.value | if type == "array" then join(";") else tostring end)"),
              "COMMAND=\(.command | join(" "))"'
}
sorted() { tr ';' '\n' <<< "$1" | sort | paste -sd';' -; }   # a label set, order-free
# The tree's own cost data decides whether a test HAS a COST, and this tree
# may or may not have run before: AXL_IT_COST_DATA points the registration at
# a file this test controls, and an absent one is "never measured".
tar=$(AXL_IT_COST_DATA="$WORK/no-cost-data" props build/x64-debug integration.test-tar-qemu)
est=$(test_meta_field "$SCRIPT_DIR/test-tar-qemu.sh" est)
check "LABELS: integration and its arch" "$(sed -n 's/^LABELS=//p' <<< "$tar")" \
    "$(sorted "integration;arch:$(test_meta_field "$SCRIPT_DIR/test-tar-qemu.sh" arch)")"
check "COST is its est= when the cost data has never measured it" \
    "$(sed -n 's/^COST=//p' <<< "$tar" | cut -d. -f1)" "$est"
# Measured, it has NO COST property, so ctest orders it by the measurement.
printf 'integration.prep 3 50\nintegration.test-tar-qemu 4 3.5\n---\nintegration.test-tar-qemu\n' > "$WORK/cost-data"
measured=$(AXL_IT_COST_DATA="$WORK/cost-data" props build/x64-debug integration.test-tar-qemu)
check "...and none once the cost data has measured it (ctest's average orders it)" \
    "$(grep -c '^COST=' <<< "$measured")" "0"
# Control: the same listing DID describe the test, so the missing COST is a
# verdict and not an empty answer.
check "...(control: that listing still carries the test's LABELS)" \
    "$(grep -c '^LABELS=' <<< "$measured")" "1"
# ctest abandons the whole file at the first line with fewer than three
# fields, so the registration must not claim a measurement past one.
printf 'integration.prep 3 50\ngarbage\nintegration.test-tar-qemu 4 3.5\n---\n' > "$WORK/cost-data-bad"
check "...but est= again past a line ctest would stop reading at" \
    "$(AXL_IT_COST_DATA="$WORK/cost-data-bad" props build/x64-debug integration.test-tar-qemu | sed -n 's/^COST=//p' | cut -d. -f1)" "$est"
# ctest's listing omits a PROCESSORS of 1, its own default: absent reads as 1.
_p=$(sed -n 's/^PROCESSORS=//p' <<< "$tar")
check "PROCESSORS is its cpu= (default 1)" "${_p:-1}" "1"
# A test that declares one: the registration carries the measured budget.
cpu_t=$(grep -l '^# test-meta:.* cpu=[2-9]' "$SCRIPT_DIR"/test-*.sh | head -1)
check "a cpu= test exists to check (a grep that matched nothing is not a pass)" \
    "$([[ -n "$cpu_t" ]] && echo yes)" "yes"
check "...and its PROCESSORS is that cpu=" \
    "$(props build/x64-debug "integration.$(basename "$cpu_t" .sh)" | sed -n 's/^PROCESSORS=//p')" \
    "$(test_meta_field "$cpu_t" cpu)"
# ctest's TIMEOUT is a BACKSTOP above everything run-one.sh can legitimately
# take -- a pooled attempt, its booted rerun after a pool miss, and the retry,
# each under the test's own limit, plus the 3 s drain -- so that limit always
# fires first and run-one.sh / run-qemu.sh get to report and clean up. It was
# limit + 30 while ctest's --repeat did the retrying; with the retry inside
# run-one, that killed a slow or hung test partway through its retry.
check "TIMEOUT defaults to 3 x 900 + 36" "$(sed -n 's/^TIMEOUT=//p' <<< "$tar" | cut -d. -f1)" "2736"
check "...and the test's own limit is 900" \
    "$(sed -n 's/^ENVIRONMENT=//p' <<< "$tar" | tr ';' '\n' | grep -c '^AXL_TEST_TIMEOUT=900$')" "1"
check "it runs with a per-tree temp base (AXL_QEMU_TMPDIR)" \
    "$(sed -n 's/^ENVIRONMENT=//p' <<< "$tar" | tr ';' '\n' | grep -c '^AXL_QEMU_TMPDIR=/.*/axl-ct-')" "1"
check "SKIP_RETURN_CODE is 77" "$(sed -n 's/^SKIP_RETURN_CODE=//p' <<< "$tar")" "77"
check "it requires the build-once and temp-base fixtures" \
    "$(sed -n 's/^FIXTURES_REQUIRED=//p' <<< "$tar")" "integration.prep;integration.tmp"
# The runner's tests run from the source root; ctest's default cwd is the
# build tree's test/integration, where a test's `cmake --preset` found no
# presets (test-cmake-ccache.sh failed exactly so on the first ctest run).
check "it runs from the source root, as under the runner" \
    "$(sed -n 's/^WORKING_DIRECTORY=//p' <<< "$tar")" "$PROJECT_DIR"
check "it runs through lib/run-one.sh" \
    "$(sed -n 's/^COMMAND=//p' <<< "$tar" | grep -c 'lib/run-one.sh .*test-tar-qemu.sh$')" "1"
check "AXL_IT_TIMEOUT sets TIMEOUT at ctest time (no reconfigure)" \
    "$(AXL_IT_TIMEOUT=240 props build/x64-debug integration.test-tar-qemu | sed -n 's/^TIMEOUT=//p' | cut -d. -f1)" "756"
# A test with several needs= tools, and a guest= test: their labels.
needs_t=""; guest_t=""
for f in "$SCRIPT_DIR"/test-*.sh; do
    discover_is_test_name "$(basename "$f")" || continue
    [[ -z "$needs_t" && "$(test_meta_field "$f" needs)" == *,* \
       && "$(test_meta_field "$f" arch)" != aa64 ]] && needs_t=$f
    [[ -z "$guest_t" && -n "$(test_meta_pool_class "$f")" \
       && "$(test_meta_field "$f" arch)" != aa64 ]] && guest_t=$f
done
want_labels() {   # want_labels SCRIPT -> its expected label set, sorted
    local f="$1" l n
    l="integration;arch:$(test_meta_field "$f" arch)"
    [[ "$(test_meta_field "$f" local-only)" == 1 ]] && l+=";local-only"
    for n in $(tr ',' ' ' <<< "$(test_meta_field "$f" needs)"); do l+=";needs:$n"; done
    [[ -n "$(test_meta_pool_class "$f")" ]] && l+=";guest:$(test_meta_pool_class "$f")"
    sorted "$l"
}
for f in "$needs_t" "$guest_t"; do
    if [[ -z "$f" ]]; then
        check "a needs=a,b test and a guest= test exist to check labels on" "missing" "both"
        continue
    fi
    b=$(basename "$f" .sh)
    check "LABELS from its header: needs:<tool> each, local-only, guest: ($b)" \
        "$(props build/x64-debug "integration.$b" | sed -n 's/^LABELS=//p')" "$(want_labels "$f")"
done

echo "=== discovery at ctest time: a stub directory runs through run-one.sh ==="
S="$WORK/stubs"; mkdir -p "$S/lib"
cp "$SCRIPT_DIR"/lib/discover.sh "$SCRIPT_DIR"/lib/test-cache.sh "$S/lib/"
mk() {   # mk NAME EXIT
    printf '#!/bin/bash\n# test-meta: arch=both needs= est=1 local-only=0\necho "stub %s"\nexit %s\n' "$1" "$2" \
        > "$S/test-$1.sh"
    chmod +x "$S/test-$1.sh"
}
mk ctstub-pass 0; mk ctstub-skip 77; mk ctstub-fail 1
rm -f "$PROJECT_DIR/build/x64-debug/integration-logs-stub/_prebuild.log"
# A stub run must never touch a REAL run's manifest: the registration's own
# test runs INSIDE real runs, and it once emptied theirs.
real_manifest="$PROJECT_DIR/build/x64-debug/integration-logs/_prebuilt"
before=$(sha256sum "$real_manifest" 2>/dev/null; stat -c %Y "$real_manifest" 2>/dev/null)
check "with AXL_IT_DIR, ctest lists exactly the 3 stubs (plus prep and the temp-base pair)" \
    "$(AXL_IT_DIR="$S" ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N 2>/dev/null \
        | grep -cE 'Test +#[0-9]+: integration\.test-')" "3"
out=$(AXL_IT_DIR="$S" AXL_IT_CACHE=0 ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
        -R '^integration\.(prep|test-ctstub-)' --no-tests=error 2>&1); rc=$?
# RESULT lines only ("N/M Test #K: name ... Passed"), not the "Start" lines.
results=$(grep -E '^ *[0-9]+/[0-9]+ Test +#[0-9]+: ' <<< "$out")
status() { grep -E "integration\.$1 " <<< "$results" | grep -oE 'Passed|Skipped|\*\*\*Failed' | head -1; }
check "the stub directory's tests are registered with no reconfigure" \
    "$(grep -cE 'integration\.test-ctstub-' <<< "$results")" "3"
check "a passing test: Passed" "$(status test-ctstub-pass)" "Passed"
check "exit 77: Skipped (SKIP_RETURN_CODE)" "$(status test-ctstub-skip)" "Skipped"
check "a failing test: Failed" "$(status test-ctstub-fail)" "***Failed"
check "the prep fixture ran first and passed" "$(status prep)" "Passed"
check "ctest exits non-zero for the failure" "$(( rc != 0 ))" "1"
real_tmp=$(props build/x64-debug integration.test-tar-qemu | sed -n 's/^ENVIRONMENT=//p' | tr ';' '\n' | sed -n 's/^AXL_QEMU_TMPDIR=//p')
stub_tmp=$(AXL_IT_DIR="$S" props build/x64-debug integration.test-ctstub-pass | sed -n 's/^ENVIRONMENT=//p' | tr ';' '\n' | sed -n 's/^AXL_QEMU_TMPDIR=//p')
check "a stub run's temp base is not the real run's (its cleanup removed it once)" \
    "$([[ -n "$real_tmp" && -n "$stub_tmp" && "$real_tmp" != "$stub_tmp" ]] && echo apart || echo "same:$real_tmp")" "apart"
check "the stub run left the real run's manifest alone" \
    "$(sha256sum "$real_manifest" 2>/dev/null; stat -c %Y "$real_manifest" 2>/dev/null)" "$before"
check "the stub prep built nothing (its own log directory has no pre-build log)" \
    "$([[ -e "$PROJECT_DIR/build/x64-debug/integration-logs-stub/_prebuild.log" ]] && echo built || echo none)" "none"

# RETRIES LIVE IN run-one.sh (--retry), never in ctest's --repeat, which
# repeats EVERY test: the unit ratchet's retry found its counts consumed and
# erased a failed judgement as PARTIAL, and a judged qloop FAIL could be
# retried into a pass (decided 2026-11-06, design §10.8). One ctest run of a
# failing stub: two attempts, the first one's log kept.
SL="$PROJECT_DIR/build/x64-debug/integration-logs-stub"
rm -f "$SL/test-ctstub-fail.sh.attempt1.log"
rout=$(AXL_IT_DIR="$S" AXL_IT_CACHE=0 ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
    -R '^integration\.test-ctstub-fail$' --output-on-failure 2>&1)
check "a failing integration test is retried once, inside run-one (one ctest run)" \
    "$(grep -c '^stub ctstub-fail$' "$SL/test-ctstub-fail.sh.attempt1.log" 2>/dev/null || echo missing)" "1"
check "...and reported FAIL after the retry" \
    "$(grep -cE 'test-ctstub-fail\.sh FAIL [0-9]+s \(after retry\)' <<< "$rout")" "1"
check "every integration test is registered through run-one.sh --retry" \
    "$(props build/x64-debug integration.test-tar-qemu | sed -n 's/^COMMAND=//p' | grep -c 'run-one\.sh --retry .*/test-tar-qemu\.sh$')" "1"
check "...and no test preset repeats tests (units, the ratchet, gates and qloop are never retried)" \
    "$(jq '[.testPresets[] | select(.execution.repeat)] | length' "$PROJECT_DIR/CMakePresets.json")" "0"
# A later run's own attempt cannot be mistaken for this one's: run-one clears
# a test's attempt-1 log as it starts, and writes one only on a retry.
printf 'stale\n' > "$SL/test-ctstub-pass.sh.attempt1.log"
printf 'stale\n' > "$SL/test-ctstub-pass.sh.pool-miss.log"
AXL_IT_DIR="$S" AXL_IT_CACHE=0 ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
    -R '^integration\.test-ctstub-pass$' >/dev/null 2>&1
check "a run that passes first time leaves no earlier run's attempt-1 log" \
    "$([[ -e "$SL/test-ctstub-pass.sh.attempt1.log" ]] && echo stale || echo cleared)" "cleared"
check "...nor an earlier run's pool-miss log" \
    "$([[ -e "$SL/test-ctstub-pass.sh.pool-miss.log" ]] && echo stale || echo cleared)" "cleared"

echo "=== ctest's TIMEOUT backstop: SIGTERM, a grace, then the KILL ==="
# ctest's TIMEOUT used to KILL the test's process tree outright, so no
# cleanup ran: podman containers (conmon is not in the tree) and pool leases
# outlived it. TIMEOUT_SIGNAL_NAME SIGTERM sends a TERM first, to the test's
# OWN process only, and KILLs the tree TIMEOUT_SIGNAL_GRACE_PERIOD later.
# Only where that process ACTS on it: run-one.sh and pool-or-boot.sh pass it
# on (lib/fwd-signal.sh), test-axl.sh handles it itself (common-test.sh).
# Every other test -- the gates, the prep, the fixtures -- keeps the KILL:
# its process is a script that a TERM kills on the spot, which would orphan
# its children (ninja, clang-tidy) beyond the KILL's reach.
GRACE=30
TERM_TESTS='^(integration\.test-|unit\.Axl|unit\.one-boot$)'
term_props() {   # term_props TREE [ENV...] -> "<TERM tests> <lacking SIGTERM> <lacking the grace> <others> <others WITH it>"
    local tree="$1"; shift
    env "$@" ctest --test-dir "$PROJECT_DIR/$tree" --show-only=json-v1 2>/dev/null \
        | jq -r --argjson g "$GRACE" --arg re "$TERM_TESTS" '
            [.tests[] | {n: .name} + ((.properties // []) | from_entries)] as $all
            | [$all[] | select(.n | test($re))] as $t
            | [$all[] | select(.n | test($re) | not)] as $o
            | "\($t | length) \([$t[] | select(.TIMEOUT_SIGNAL_NAME != "SIGTERM")] | length) \([$t[] | select(.TIMEOUT_SIGNAL_GRACE_PERIOD != $g)] | length) \($o | length) \([$o[] | select(.TIMEOUT_SIGNAL_NAME != null)] | length)"'
}
for tr in x64-debug aa64-debug; do
    read -r tn tsig tgr on osig <<< "$(term_props "build/$tr")"
    check "$tr: every integration test and unit run gets SIGTERM and a ${GRACE}s grace (${tn:-0} of them)" \
        "$(( ${tn:-0} > 100 )) ${tsig:-?} ${tgr:-?}" "1 0 0"
    check "$tr: ...and no other test does (${on:-0} keep the KILL)" "$(( ${on:-0} > 10 )) ${osig:-?}" "1 0"
done
check "...and so does a stub directory's test (the backstop case below runs one)" \
    "$(term_props build/x64-debug AXL_IT_DIR="$S" | cut -d' ' -f1-3)" "3 0 0"
check "AXL_IT_BACKSTOP replaces the computed backstop (the seam the case below uses)" \
    "$(AXL_IT_BACKSTOP=4 props build/x64-debug integration.test-tar-qemu | sed -n 's/^TIMEOUT=//p' | cut -d. -f1)" "4"

# The backstop firing on a test that is still running: a stub whose TERM
# handler is its cleanup -- it ends a guest stand-in it started in a SESSION
# of its own (a group kill cannot reach it, as none reaches a pooled guest or
# a container) and hands back a "lease" file -- under a tiny backstop and a
# test limit far above it. The handler records WHEN it ran: the test's own
# limit sends a TERM too (30 s in), and that one must not pass for ctest's.
S2="$WORK/stubs-hang"; mkdir -p "$S2/lib"
cp "$SCRIPT_DIR"/lib/discover.sh "$SCRIPT_DIR"/lib/test-cache.sh "$S2/lib/"
GUEST="$WORK/ctstub-guest"; cp "$(command -v sleep)" "$GUEST"
LEASE="$WORK/ctstub-lease"; MARK="$WORK/ctstub-mark"
cat > "$S2/test-ctstub-hang.sh" <<SH
#!/bin/bash
# test-meta: arch=both needs= est=1 local-only=0
echo held > "$LEASE"
setsid "$GUEST" 300 &
guest=\$!
# ...and one it has lost track of: orphaned at once, so reparented to
# run-one.sh (its subreaper), as a background guest's owner is when the TERM
# lands mid-launch. Only run-one can end it.
( setsid "$GUEST" 301 & )
trap 'echo "TERM handled \$SECONDS s in" > "$MARK"; kill "\$guest"; rm -f "$LEASE"; exit 143' TERM
"$GUEST" 300
SH
chmod +x "$S2/test-ctstub-hang.sh"
rm -f "$MARK" "$LEASE"
t0=$SECONDS
hout=$(AXL_IT_DIR="$S2" AXL_IT_CACHE=0 AXL_IT_TIMEOUT=30 AXL_IT_BACKSTOP=3 \
    ctest --test-dir "$PROJECT_DIR/build/x64-debug" -R '^integration\.test-ctstub-hang$' \
    -FS integration.prep --output-on-failure 2>&1)
htook=$(( SECONDS - t0 ))
sleep 1
check "the backstop timed the test out (ctest: Timeout)" \
    "$(grep -cE 'Test +#[0-9]+: integration\.test-ctstub-hang \.+\*\*\*Timeout' <<< "$hout")" "1"
check "...its SIGTERM reached the test, whose cleanup ran, at the backstop (under 10 s in, not the 30 s limit)" \
    "$(sed -E 's/ [0-9] s in$/ at the backstop/' "$MARK" 2>/dev/null)" "TERM handled at the backstop"
check "...handing back its lease" "$([[ -e "$LEASE" ]] && echo held || echo released)" "released"
check "...and ending the guest it started in another session, and the one it lost (no orphan)" \
    "$(pgrep -fc -- "^$GUEST " 2>/dev/null || true)" "0"
check "...promptly: ctest did not wait out the ${GRACE}s grace (${htook}s)" "$(( htook < 3 + GRACE / 2 ))" "1"
check "...and run-one says the test was stopped, not how it did" \
    "$(grep -cxE '  test-ctstub-hang\.sh FAIL [0-9]+s \(stopped by SIGTERM from outside -- not its own 30s limit, and not a verdict\)' <<< "$hout")" "1"
# An orphan the cases above failed to end must not outlive this test.
for p in $(pgrep -f -- "^$GUEST " 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done

echo "=== the prep builds only what the run can run ==="
# CTest hands the prep EVERY registered test (it cannot hand a fixture the
# selection), so under x64-ci -- local-only tests excluded -- the prep built
# their fixtures too, and one of those breaking would fail every CI test
# over a test CI never runs. The preset says so (AXL_PREP_SKIP_LOCAL=1) and
# the prep drops them. Driven through suite-prep.sh's own stub seams: a stub
# tree whose axl-build.sh records what it is asked to build.
P="$WORK/prep"; PT="$WORK/prep-tree"; mkdir -p "$P" "$PT/scripts"
printf '#!/bin/bash\n# test-meta: arch=x64 needs= est=1 local-only=1 builds=lo-only-fixture\nexit 0\n' > "$P/test-lo.sh"
printf '#!/bin/bash\n# test-meta: arch=x64 needs= est=1 local-only=0 builds=ci-fixture\nexit 0\n' > "$P/test-ci.sh"
printf '#!/bin/bash\n[[ "$1" == --env-fingerprint ]] && { echo fp; exit 0; }\necho "$*" >> "%s/built"\n' "$WORK" > "$PT/scripts/axl-build.sh"
chmod +x "$P"/*.sh "$PT/scripts/axl-build.sh"
printf 'NON_ARTIFACT_GOALS = check-x\n' > "$PT/scripts/axl-lists.conf"
prep_built() {   # [ENV...] -> what the prep asked to build
    rm -f "$WORK/built"
    env RUN_INTEGRATION_DIR="$P" RUN_PROJECT_DIR="$PT" AXL_PREBUILT_MANIFEST="$WORK/prep-manifest" \
        AXL_PREP_LOGDIR="$WORK/prep-logs" "$@" bash "$SCRIPT_DIR/lib/suite-prep.sh" "$P/test-lo.sh" "$P/test-ci.sh" \
        > /dev/null 2>&1
    sort "$WORK/built" 2>/dev/null | paste -sd'|' -
}
check "control: the prep builds every test's fixtures" "$(prep_built)" "ARCH=x64 ci-fixture lo-only-fixture"
check "AXL_PREP_SKIP_LOCAL=1: the local-only test's fixture is not built" "$(prep_built AXL_PREP_SKIP_LOCAL=1)" "ARCH=x64 ci-fixture"
check "x64-ci sets AXL_PREP_SKIP_LOCAL=1" \
    "$(jq -r '.testPresets[] | select(.name == "x64-ci") | .environment.AXL_PREP_SKIP_LOCAL' "$PROJECT_DIR/CMakePresets.json")" "1"

echo "=== what a front end hands the run (step 6, decision 1) ==="
# AXL_IT_LOGDIR: the run's logs, tallies and prep manifest go to a directory
# of the front end's choosing -- a fresh one per run, as the runner's were --
# instead of the tree's shared integration-logs, where every earlier run's
# logs sit beside this one's. The registration cache stays in the tree.
RL="$WORK/runlogs"; mkdir -p "$RL"
AXL_IT_DIR="$S" AXL_IT_LOGDIR="$RL" AXL_IT_CACHE=0 ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
    -R '^integration\.test-ctstub-pass$' >/dev/null 2>&1
check "AXL_IT_LOGDIR: the test's log is written there" \
    "$(grep -c '^stub ctstub-pass$' "$RL/test-ctstub-pass.sh.log" 2>/dev/null || echo missing)" "1"
check "...and so is the prep's manifest" "$([[ -f "$RL/_prebuilt" ]] && echo there || echo missing)" "there"
check "...while the registration cache stays in the tree" \
    "$([[ -f "$PROJECT_DIR/build/x64-debug/integration-logs-stub/axl-integration-tests.cmake" ]] && echo tree || echo moved)" "tree"
# AXL_PREP_TESTS: the scripts this run selected, so the prep builds their
# fixtures only. Set and EMPTY means "no integration test was selected" (a
# unit-only run): the shared targets, no fixture, no restage.
check "AXL_PREP_TESTS: only the selected test's fixture is built" \
    "$(prep_built AXL_PREP_TESTS="$P/test-ci.sh")" "ARCH=x64 ci-fixture"
check "...and set but empty builds no fixture at all" "$(prep_built AXL_PREP_TESTS=)" ""
# AXL_IT_QEMU_TMPDIR: a caller's temp base is used as-is, and NEVER removed
# -- the runner's rule for an AXL_QEMU_TMPDIR it was handed. So the temp-base
# fixture pair does not own it.
QB="$WORK/qbase"; mkdir -p "$QB"
check "AXL_IT_QEMU_TMPDIR: tests get it as their AXL_QEMU_TMPDIR" \
    "$(AXL_IT_QEMU_TMPDIR="$QB" props build/x64-debug integration.test-tar-qemu | sed -n 's/^ENVIRONMENT=//p' | tr ';' '\n' | grep -cx "AXL_QEMU_TMPDIR=$QB")" "1"
AXL_IT_DIR="$S" AXL_IT_QEMU_TMPDIR="$QB" AXL_IT_LOGDIR="$RL" AXL_IT_CACHE=0 \
    ctest --test-dir "$PROJECT_DIR/build/x64-debug" -R '^integration\.test-ctstub-pass$' >/dev/null 2>&1
check "...and a run leaves it in place, with no owner file written into it" \
    "$([[ -d "$QB" ]] && echo kept || echo removed) $(ls -A "$QB" | wc -l)" "kept 0"

# AXL_IT_CACHE_DIR: a private skip cache (the runner self-test's stub runs
# must not write the real one), as RUN_TEST_CACHE_DIR was for the runner.
check "AXL_IT_CACHE_DIR: tests get it as their AXL_TEST_CACHE" \
    "$(AXL_IT_CACHE_DIR="$WORK/cache" props build/x64-debug integration.test-tar-qemu | sed -n 's/^ENVIRONMENT=//p' | tr ';' '\n' | grep -cx "AXL_TEST_CACHE=$WORK/cache")" "1"

# The artifact gates carry their own label: verify.sh's imggates job selects
# by it. Exactly these -- the gates that read the tree's images and archives
# (a mislabelled one would run before, or without, what it reads).
check "the artifact gates, and only they, carry the artifact label (x64 tree)" \
    "$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N -L '^artifact$' 2>/dev/null | sed -nE 's/.*Test +#[0-9]+: gate\.(check-.*)$/\1/p' | sort | paste -sd' ')" \
    "check-bss-clear check-ctors check-libc-overlap check-log-linkage check-no-avx check-nx-compat check-pe-stripped check-reloc-coverage check-size-gate-paths"
# aa64 reads its own images: everything above but the x86-only AVX scan and
# the size gate (x64's derivation). pe-stripped joined once pe-set-debug
# stopped leaving objcopy's string table for `.rela.dyn` behind.
check "the artifact gates, and only they, carry the artifact label (aa64 tree)" \
    "$(ctest --test-dir "$PROJECT_DIR/build/aa64-debug" -N -L '^artifact$' 2>/dev/null | sed -nE 's/.*Test +#[0-9]+: gate\.(check-.*)$/\1/p' | sort | paste -sd' ')" \
    "check-bss-clear check-ctors check-libc-overlap check-log-linkage check-nx-compat check-pe-stripped check-reloc-coverage"

echo "=== the one-boot unit run (verify.sh's x64/aa64 jobs) ==="
# Every binary in ONE guest per arch, as test-axl.sh ran them for verify.sh:
# per-binary runs made verify 357 s against 162 s (49 boots per arch, TCG on
# aa64), so verify selects this (Mike, 2026-11-06). Labelled unit-boot, never
# unit -- the suite presets run the per-binary tests and leave this out.
ob=$(props build/x64-debug unit.one-boot)
check "x64: unit.one-boot runs test-axl.sh's one boot, labelled unit-boot" \
    "$(sed -n 's/^COMMAND=//p' <<< "$ob" | grep -c 'test-axl\.sh --arch X64$') $(sed -n 's/^LABELS=//p' <<< "$ob")" "1 arch:x64;unit-boot"
envof() { sed -n 's/^ENVIRONMENT=//p' <<< "$1" | tr ';' '\n' | grep -cx "$2" | tr -d ' '; }
# One variable at a time: a count over an alternation passes when one goes
# missing and another appears (TEST_SKIP_RATCHET in for TEST_TIMEOUT_SCALE).
check "x64 writes the ratchet: no TEST_SKIP_RATCHET" "$(envof "$ob" 'TEST_SKIP_RATCHET=.*')" "0"
check "...and declares no scale of its own (the front end does, below)" \
    "$(envof "$ob" 'TEST_TIMEOUT_SCALE=.*') $(envof "$ob" 'TEST_BUDGET_SCALE=.*')" "0 0"
oba=$(props build/aa64-debug unit.one-boot)
check "aa64: unit.one-boot never writes the ratchet (TEST_SKIP_RATCHET=1, which also skips its judge and budget)" \
    "$(envof "$oba" 'TEST_SKIP_RATCHET=1')" "1"
# THE SCALES, run for real: the registered command with a stub in place of
# test-axl.sh, which prints what it was handed. Strict (1) unless the front
# end says it is sharing the box -- a lone `ctest -R unit.one-boot` judges the
# real budget, as a lone `verify.sh --only=x64` did.
obstub="$WORK/ob-stub.sh"
printf '#!/bin/bash\necho "T=${TEST_TIMEOUT_SCALE-unset} B=${TEST_BUDGET_SCALE-unset} ARGS=$*"\n' > "$obstub"
chmod +x "$obstub"
mapfile -t obcmd < <(ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 -R '^unit.one-boot$' 2>/dev/null \
    | jq -r '.tests[] | select(.name == "unit.one-boot") | .command[]' \
    | sed "s#^.*/test/integration/test-axl\.sh\$#$obstub#")
check "the stub stands in for test-axl.sh (control)" "$(printf '%s\n' "${obcmd[@]}" | grep -cx "$obstub")" "1"
check "unit.one-boot alone is strict: both scales 1" \
    "$(env -u AXL_UNIT_TIMEOUT_SCALE -u AXL_UNIT_BUDGET_SCALE "${obcmd[@]}")" "T=1 B=1 ARGS=--arch X64"
check "...and takes the front end's AXL_UNIT_TIMEOUT_SCALE / AXL_UNIT_BUDGET_SCALE" \
    "$(AXL_UNIT_TIMEOUT_SCALE=2 AXL_UNIT_BUDGET_SCALE=1.40 "${obcmd[@]}")" "T=2 B=1.40 ARGS=--arch X64"
check "...and so do the plain tree presets: one tree run never runs the unit suite twice" \
    "$(for pr in x64-debug aa64-debug; do (cd "$PROJECT_DIR" && ctest --preset "$pr" --show-only=json-v1 2>/dev/null) | jq -r '.tests[].name' | grep -cx 'unit.one-boot'; done | paste -sd' ')" "0 0"
check "the suite presets leave it out (they run the per-binary tests)" \
    "$(for pr in x64-suite aa64-suite; do (cd "$PROJECT_DIR" && ctest --preset "$pr" --show-only=json-v1 2>/dev/null) | jq -r '.tests[].name' | grep -cx 'unit.one-boot'; done | paste -sd' ')" "0 0"

echo "=== two stub registrations never share one cache at once ==="
# Every AXL_IT_DIR run registers into the same integration-logs-stub cache,
# so two tests that drive stub directories, run side by side under ctest -j,
# rewrite each other's registration between its write and its include.
lock_of() { props build/x64-debug "integration.$1" | sed -n 's/^RESOURCE_LOCK=//p' | tr ';' '\n' | grep -cx it-stub; }
check "test-ctest-integration takes the it-stub lock" "$(lock_of test-ctest-integration)" "1"
check "test-check-test-est takes it too" "$(lock_of test-check-test-est)" "1"
check "control: a test that drives no stub directory does not" "$(lock_of test-tar-qemu)" "0"

echo "=== the cached registration follows the scripts ==="
# discover.sh's output is cached per log directory (it costs seconds); a new
# script, and an edited header, must each show up on the next ctest.
stubn() { AXL_IT_DIR="$S" ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N 2>/dev/null \
            | grep -cE 'Test +#[0-9]+: integration\.test-'; }
mk ctstub-new 0
check "a new test script is registered on the next ctest" "$(stubn)" "4"
stubcost() {
    AXL_IT_DIR="$S" ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 2>/dev/null \
        | jq -r '.tests[] | select(.name == "integration.test-ctstub-new") | .properties[] | select(.name == "COST") | .value'
}
sleep 1   # mtime resolution: the edit must be NEWER than the cached output
sed -i.bak 's/ est=1 / est=42 /' "$S/test-ctstub-new.sh" && rm -f "$S/test-ctstub-new.sh.bak"
check "an edited header (est=) is re-read on the next ctest" "$(stubcost | cut -d. -f1)" "42"

echo "=== pooled runs: the guests are a CTest resource ==="
# The tree binds a resource spec (CTEST_RESOURCE_SPEC_FILE), rewritten from
# `axlpool resource-spec` at ctest time, so a PLAIN ctest -- no
# --resource-spec-file -- still bounds the pool's guests (design §10.7).
spec="$PROJECT_DIR/build/x64-debug/axl-resource-spec.json"
ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N >/dev/null 2>&1
check "the tree's resource spec names the standard x64 guests" \
    "$(jq -r '.local[0] | has("guest_standard_x64")' "$spec" 2>/dev/null)" "true"
gb=$(basename "$guest_t" .sh); gclass=$(test_meta_pool_class "$guest_t")
rg() { jq -r --arg n "$2" '.tests[] | select(.name == $n) | .properties[]
        | select(.name == "RESOURCE_GROUPS") | .value | tostring' <<< "$1"; }
plain=$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 -R "^integration\.$gb\$" 2>/dev/null)
check "booted (no AXL_IT_POOL): a guest= test takes no guest resource" "$(rg "$plain" "integration.$gb")" ""
pooled=$(AXL_IT_POOL=/nonexistent/pool.sock ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
            --show-only=json-v1 -R "^integration\.$gb\$" 2>/dev/null)
check "pooled (AXL_IT_POOL): it takes one slot of guest_${gclass}_x64" \
    "$(rg "$pooled" "integration.$gb" | grep -c "guest_${gclass}_x64")" "1"
penv=$(jq -r --arg n "integration.$gb" '.tests[] | select(.name == $n) | .properties[]
        | select(.name == "ENVIRONMENT") | .value[]' <<< "$pooled")
check "...is told the socket (AXL_ONE_POOL_SOCK)" \
    "$(grep -cx 'AXL_ONE_POOL_SOCK=/nonexistent/pool.sock' <<< "$penv")" "1"
pfix=$(jq -r --arg n "integration.$gb" '.tests[] | select(.name == $n) | .properties[]
        | select(.name == "FIXTURES_REQUIRED") | .value | join(";")' <<< "$pooled")
check "...and requires the pool check (integration.pool)" "$(grep -c 'integration.pool' <<< "$pfix")" "1"
check "a test with no guest= takes no guest resource even pooled" \
    "$(AXL_IT_POOL=/nonexistent/pool.sock props build/x64-debug integration.test-tar-qemu | grep -c '^RESOURCE_GROUPS=')" "0"

# AXL_IT_POOL=auto -- THE DEFAULT for the plain and suite presets and the
# runner: every QEMU test that can run on a pooled guest does, when the user
# service is there (Mike, 2026-11-06: the pool exists to cut boots and wall
# time). The service's socket decides the registration: absent, nothing is
# pooled; present, guest= tests are, and the pool check runs in AUTO mode --
# an unhealthy service is SAID, and the tests boot, rather than failing them
# all as an explicit AXL_IT_POOL=1 / --pool rightly does.
# A socket that ACCEPTS is still not a service: the installed unit is
# socket-activated, so systemd keeps it listening while the service itself has
# failed. auto pools only when the service ANSWERS (axlpool status) and serves
# the arch -- the stub-service cases further down; a bare listener, a stale
# socket file and no socket all boot.
xr_none="$WORK/xdg-none"; mkdir -p "$xr_none"
xr_live="$WORK/xdg-live"; mkdir -p "$xr_live/axlpool"
xr_stale="$WORK/xdg-stale"; mkdir -p "$xr_stale/axlpool"
python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(8); time.sleep(600)' \
    "$xr_live/axlpool/pool.sock" & live_pid=$!
python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])' "$xr_stale/axlpool/pool.sock"
for _i in $(seq 1 50); do [[ -S "$xr_live/axlpool/pool.sock" ]] && break; sleep 0.05; done
auto_ls() {   # auto_ls XDG -> the show-only json for the guest= test and the pool check
    XDG_RUNTIME_DIR="$1" AXL_IT_POOL=auto ctest --test-dir "$PROJECT_DIR/build/x64-debug" \
        --show-only=json-v1 -R "^integration\.($gb|pool)\$" 2>/dev/null
}
a0=$(auto_ls "$xr_none"); a1=$(auto_ls "$xr_live"); as=$(auto_ls "$xr_stale")
check "auto, no service socket: the guest= test boots (no guest resource), no pool check" \
    "$(rg "$a0" "integration.$gb")|$(jq -r '[.tests[].name] | index("integration.pool") // "none"' <<< "$a0")" "|none"
check "auto, a STALE socket (nothing listening): boots (no slot), and a check SAYS why" \
    "$(rg "$as" "integration.$gb")|$(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$as" | grep -c "echo .pool: unavailable")" "|1"
says() { jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$1" | grep -c "echo .pool: unavailable"; }
check "auto, a socket that ACCEPTS but no pool service answers: boots (no slot), and a check SAYS why" \
    "$(rg "$a1" "integration.$gb")|$(says "$a1")" "|1"
# A caller's own AXL_IT_POOL outranks a preset's default: a preset's
# `environment` overrides the caller's, so the default is a SECOND variable
# (AXL_IT_POOL_DEFAULT) that the registration reads only when AXL_IT_POOL is
# empty -- an explicit socket stays explicit, in refuse mode.
ex=$( (cd "$PROJECT_DIR" && AXL_IT_POOL=/x/explicit.sock ctest --preset x64-suite --show-only=json-v1 -R "^integration\.($gb|pool)\$" 2>/dev/null) )
check "an explicit AXL_IT_POOL survives a preset: its socket, in refuse mode" \
    "$(jq -r --arg n "integration.$gb" '.tests[] | select(.name == $n) | .properties[] | select(.name == "ENVIRONMENT") | .value[]' <<< "$ex" | grep -cx 'AXL_ONE_POOL_SOCK=/x/explicit.sock') $(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$ex" | grep -c ' auto$')" "1 0"
# pool-check.sh itself: AUTO says why and lets the tests boot (exit 0); asked
# for explicitly, the same dead socket is a refusal (exit 2) -- the control.
pc() { "$SCRIPT_DIR/lib/pool-check.sh" "$xr_live/axlpool/pool.sock" X64 "$PROJECT_DIR" "$@" 2>&1; echo "rc=$?"; }
check "pool-check AUTO on a dead service: exit 0, and says the guest= tests boot" \
    "$(pc "ctest AXL_IT_POOL=auto" auto | sed -n '$p;/^pool: /p' | sed 's/ (.*)//' | paste -sd'|')" \
    "pool: unavailable -- guest= tests boot|rc=0"
check "control: asked for explicitly, the same dead service is refused (exit 2)" \
    "$(pc "run-integration: --pool" | tail -1)" "rc=2"
# The presets: every run pools by default -- CI's and a release's too (Mike,
# 2026-11-07: the runners on this host use the pool like everything else).
# A runner with no service (a hosted one) boots, said; AXL_IT_POOL=0 opts out.
pe() { jq -r --arg n "$1" '.testPresets[] | select(.name == $n) | .environment.AXL_IT_POOL // "unset"' "$PROJECT_DIR/CMakePresets.json"; }
pd() { jq -r --arg n "$1" '.testPresets[] | select(.name == $n) | .environment.AXL_IT_POOL_DEFAULT // "unset"' "$PROJECT_DIR/CMakePresets.json"; }
check "the plain and suite presets pool by default (base's AXL_IT_POOL_DEFAULT=auto, inherited; no AXL_IT_POOL of their own)" \
    "$(pd base) $(pe base) $(for pr in x64-debug aa64-debug x64-suite aa64-suite; do jq -r --arg n "$pr" '.testPresets[] | select(.name == $n) | .environment.AXL_IT_POOL_DEFAULT // .environment.AXL_IT_POOL // "inherited"' "$PROJECT_DIR/CMakePresets.json"; done | sort -u)" \
    "auto unset inherited"
check "x64-ci and x64-integration pool by default too (AXL_IT_POOL_DEFAULT=auto; no AXL_IT_POOL, so a caller's opt-out stands)" \
    "$(pe x64-ci)|$(pd x64-ci)|$(pe x64-integration)|$(pd x64-integration)" "unset|auto|unset|auto"
check "...and with a LIVE service socket each registers the pool check" \
    "$(for pr in x64-ci x64-integration; do (cd "$PROJECT_DIR" && XDG_RUNTIME_DIR="$xr_live" ctest --preset "$pr" --show-only=json-v1 2>/dev/null) \
        | jq '[.tests[] | select(.name == "integration.pool")] | length'; done | paste -sd' ')" "1 1"
check "...each with its OWN tree's agent build (AXL_IT_POOL_AGENT=tree): CI exercises the commit's agent" \
    "$(jq -r '.testPresets[] | select(.name == "x64-ci" or .name == "x64-integration") | .environment.AXL_IT_POOL_AGENT // "unset"' "$PROJECT_DIR/CMakePresets.json" | sort -u)" "tree"
xr_none=$(mktemp -d)
hj=$( (cd "$PROJECT_DIR" && XDG_RUNTIME_DIR="$xr_none" CI=true ctest --preset x64-ci --show-only=json-v1 2>/dev/null) )
check "a hosted runner (CI set, no service socket): nothing pooled, and a passing check SAYS so" \
    "$(jq '[.tests[] | select([.properties[]? | select(.name == "RESOURCE_GROUPS")] | length > 0)] | length' <<< "$hj") $(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$hj" | grep -cE "echo 'pool: none \(no service at $xr_none/axlpool/pool\.sock\) -- guest= tests boot'$")" \
    "0 1"
rmdir "$xr_none"
check "...which AXL_IT_POOL=0 turns off (booted, said by a passing check)" \
    "$( (cd "$PROJECT_DIR" && XDG_RUNTIME_DIR="$xr_live" AXL_IT_POOL=0 ctest --preset x64-ci --show-only=json-v1 2>/dev/null) \
        | jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' | grep -c 'pool: off (AXL_IT_POOL=0)')" "1"
check "control: x64-suite beside the same live socket does" \
    "$( (cd "$PROJECT_DIR" && XDG_RUNTIME_DIR="$xr_live" ctest --preset x64-suite --show-only=json-v1 2>/dev/null) | jq '[.tests[] | select(.name == "integration.pool")] | length')" "1"
kill "$live_pid" 2>/dev/null; wait "$live_pid" 2>/dev/null

# A PLAIN ctest enforces the bound: a guest class with no resource in the
# spec is not run ("Insufficient resources"), and a real class runs -- its
# pool misses (no service at that socket) and run-one.sh boots it instead.
# -FS skips the pool check, which would (rightly) refuse the dead socket.
P="$WORK/pstubs"; mkdir -p "$P/lib"
cp "$SCRIPT_DIR"/lib/discover.sh "$SCRIPT_DIR"/lib/test-cache.sh "$P/lib/"
mkg() {   # mkg NAME CLASS
    printf '#!/bin/bash\n# test-meta: arch=both needs= est=1 local-only=0 guest=%s\necho "stub %s"\nexit 0\n' "$2" "$1" \
        > "$P/test-$1.sh"
    chmod +x "$P/test-$1.sh"
}
mkg ctpool-real "$gclass"; mkg ctpool-ghost ctghostclass
pout=$(AXL_IT_DIR="$P" AXL_IT_CACHE=0 AXL_IT_POOL=/nonexistent/pool.sock \
       ctest --test-dir "$PROJECT_DIR/build/x64-debug" -R '^integration\.test-ctpool-' -FS integration.pool 2>&1)
presults=$(grep -E '^ *[0-9]+/[0-9]+ Test +#[0-9]+: ' <<< "$pout")
check "plain ctest: a guest class with no resource in the spec is Not Run" \
    "$(grep -E 'integration\.test-ctpool-ghost ' <<< "$presults" | grep -c 'Not Run')" "1"
check "...saying the resources are insufficient" "$(grep -c 'Insufficient resources for test integration.test-ctpool-ghost' <<< "$pout")" "1"
check "a real class gets its slot, misses the dead pool, and passes booted" \
    "$(grep -E 'integration\.test-ctpool-real ' <<< "$presults" | grep -c 'Passed')" "1"
# THE AUTO DECISION ASKS THE SERVICE, through a stub project whose axlpool
# answers as told (FAKE_POOL): healthy -> pooled, its check in AUTO mode;
# down, or no agent for the arch -> booted (no slot, no pool check).
sp="$WORK/stubproj"; mkdir -p "$sp/test/pool/axlpool"
: > "$sp/test/pool/axlpool/__init__.py"
printf '%s\n' \
    '"""Stub axlpool status for test-ctest-integration.sh: FAKE_POOL = ok | down | noarch."""' \
    'import os' \
    'import sys' \
    'mode = os.environ.get("FAKE_POOL", "ok")' \
    'if sys.argv[1] == "status":' \
    '    if mode == "down":' \
    '        print("axlpool: no pool service: refused", file=sys.stderr)' \
    '        sys.exit(1)' \
    '    agents = "AARCH64" if mode == "noarch" else "AARCH64 X64"' \
    '    print(f"axlpool: service stub, agents: {agents}")' \
    '    sys.exit(0)' \
    'if sys.argv[1] == "code":' \
    '    if os.environ.get("FAKE_CODE"):' \
    '        print(os.environ["FAKE_CODE"])' \
    '        sys.exit(0)' \
    '    sys.exit(3)' \
    'sys.exit(2)' > "$sp/test/pool/axlpool/__main__.py"
sa() {   # sa FAKE_POOL [VAR=VALUE...] -> listing of the stub guest test and the pool check
    local m="$1"; shift
    env FAKE_POOL="$m" XDG_RUNTIME_DIR="$xr_live" AXL_IT_DIR="$P" RUN_PROJECT_DIR="$sp" AXL_IT_CACHE=0 "$@" \
        ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 -R '^integration\.(test-ctpool-real|pool)$' 2>/dev/null
}
rm -f "$xr_live/axlpool/pool.sock"   # the earlier listener's, now stale
python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(8); time.sleep(600)' \
    "$xr_live/axlpool/pool.sock" & live_pid=$!
for _i in $(seq 1 50); do [[ -S "$xr_live/axlpool/pool.sock" ]] && break; sleep 0.05; done
h=$(sa ok AXL_IT_POOL=auto)
check "auto, the service answers for the arch: pooled -- a slot, and its pool check in AUTO mode" \
    "$(rg "$h" integration.test-ctpool-real | grep -c "guest_${gclass}_x64") $(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$h" | grep -c ' auto$')" "1 1"
# The service's CODE identity, asked once at registration: every pooled
# guest= test is told it (its skip-cache key holds it; its lease is held to
# it), and told it EMPTY when the service gives none -- set, so a stray
# AXL_POOL_CODE_ID in the caller's environment cannot stand in for it.
cenv() { jq -r '.tests[] | select(.name == "integration.test-ctpool-real") | .properties[]
          | select(.name == "ENVIRONMENT") | .value[]' <<< "$1" | grep '^AXL_POOL_CODE_ID=' | paste -sd'|'; }
check "a pooled guest= test is told the service's code identity (AXL_POOL_CODE_ID, asked once)" \
    "$(cenv "$(sa ok AXL_IT_POOL=auto FAKE_CODE=c0de42)")" "AXL_POOL_CODE_ID=c0de42"
check "...EMPTY when the service reports none, whatever the caller's environment says" \
    "$(cenv "$(sa ok AXL_IT_POOL=auto AXL_POOL_CODE_ID=feed)")" "AXL_POOL_CODE_ID="
check "...and EMPTY when its answer is not an identity (only hex is)" \
    "$(cenv "$(sa ok AXL_IT_POOL=auto FAKE_CODE='not an id')")" "AXL_POOL_CODE_ID="
for m in down noarch; do
    b=$(sa "$m" AXL_IT_POOL=auto)
    check "auto, the service $m: booted -- no slot -- and a check that the guest= test requires SAYS why" \
        "$(rg "$b" integration.test-ctpool-real)|$(says "$b")|$(jq -r '.tests[] | select(.name == "integration.test-ctpool-real") | .properties[] | select(.name == "FIXTURES_REQUIRED") | .value | join(";")' <<< "$b" | grep -c 'integration.pool')" "|1|1"
done
d=$(sa ok AXL_IT_POOL_DEFAULT=auto)
check "AXL_IT_POOL_DEFAULT=auto with no AXL_IT_POOL is auto" "$(rg "$d" integration.test-ctpool-real | grep -c "guest_${gclass}_x64")" "1"
d=$(sa ok AXL_IT_POOL_DEFAULT=auto AXL_IT_POOL=)
check "control: an EMPTY AXL_IT_POOL defers to the default too (presets blank it to say nothing)" \
    "$(rg "$d" integration.test-ctpool-real | grep -c "guest_${gclass}_x64")" "1"
kill "$live_pid" 2>/dev/null; wait "$live_pid" 2>/dev/null
check "...and the pool's miss was recorded beside its log" \
    "$(ls "$PROJECT_DIR/build/x64-debug/integration-logs-stub/" 2>/dev/null | grep -c '^test-ctpool-real.sh.pool-miss.log$')" "1"

echo "=== the unit binaries: one CTest test each ==="
apps=$("$SCRIPT_DIR/test-axl.sh" --list-apps | sort)
for t in x64 aa64; do
    got=$(ctest --test-dir "$PROJECT_DIR/build/$t-debug" -N -L '^unit$' 2>/dev/null \
            | sed -n 's/^ *Test *#[0-9]*: unit\.\(AxlTest[^ ]*\)$/\1/p' | sort)
    check "$t tree: one unit.<App> per binary test-axl.sh runs ($(grep -c . <<< "$apps"))" \
        "$(diff <(echo "$got") <(echo "$apps") >/dev/null && echo same || echo differ)" "same"
done
um=$(props build/x64-debug unit.AxlTestMem)
uenv=$(sed -n 's/^ENVIRONMENT=//p' <<< "$um" | tr ';' '\n')
check "unit.AxlTestMem runs test-axl.sh for that one binary" \
    "$(sed -n 's/^COMMAND=//p' <<< "$um" | grep -c 'test-axl.sh --arch X64$') $(grep -cx 'TEST_APPS_ONLY=AxlTestMem' <<< "$uenv")" "1 1"
check "...with the whole-run ratchet off and its count written for the sum" \
    "$(grep -cx 'TEST_SKIP_RATCHET=1' <<< "$uenv") $(grep -c '^TEST_COUNT_FILE=.*/unit-counts/AxlTestMem$' <<< "$uenv")" "1 1"
# POOLED UNIT RUNS (docs/AXL-Test-Guest-Pool-Design.md §6.7): every unit.<App>
# runs through lib/pool-or-boot.sh, which boots it when no pool is given and,
# pooled, leases a `unit` guest -- a miss boots it instead. So a pooled tree's
# unit tests take a slot of guest_unit_<arch> and are told the socket.
check "unit.AxlTestMem runs test-axl.sh through lib/pool-or-boot.sh, class unit" \
    "$(sed -n 's/^COMMAND=//p' <<< "$um" | grep -c 'pool-or-boot.sh --class unit --arch X64 .* -- bash .*/test-axl.sh --arch X64$')" "1"
check "...and unpooled takes no guest slot and has no pool socket" \
    "$(grep -c '^RESOURCE_GROUPS=' <<< "$um") $(grep -c '^AXL_ONE_POOL_SOCK=' <<< "$uenv")" "0 0"
upj=$(AXL_IT_POOL=/x/explicit.sock ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 \
          -R '^unit\.AxlTestMem$' 2>/dev/null)
check "pooled: unit.AxlTestMem takes one slot of guest_unit_x64" \
    "$(rg "$upj" unit.AxlTestMem | grep -c 'guest_unit_x64')" "1"
check "...is told the socket, and requires the pool check" \
    "$(jq -r '.tests[] | select(.name == "unit.AxlTestMem") | .properties[] | select(.name == "ENVIRONMENT") | .value[]' <<< "$upj" | grep -cx 'AXL_ONE_POOL_SOCK=/x/explicit.sock') $(jq -r '.tests[] | select(.name == "unit.AxlTestMem") | .properties[] | select(.name == "FIXTURES_REQUIRED") | .value | join(";")' <<< "$upj" | grep -c 'integration.pool')" "1 1"
tmo() { jq -r --arg n "$2" '.tests[] | select(.name == $n) | .properties[] | select(.name == "TIMEOUT") | .value | floor' <<< "$1"; }
check "...with room for a pool miss to run it booted after the attempt (600 s, booted 300 s)" \
    "$(tmo "$upj" unit.AxlTestMem) $(tmo "$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 -R '^unit\.AxlTestMem$' 2>/dev/null)" unit.AxlTestMem)" "600 300"
check "...with verify.sh's concurrent hang-timeout scale (it runs beside everything)" \
    "$(grep -cx 'TEST_TIMEOUT_SCALE=2' <<< "$uenv")" "1"
check "...booting as the one-boot run does: no forced launcher, no skip cache" \
    "$(grep -c '^AXL_SHELL_LAUNCHER=\|^AXL_TEST_CACHE=' <<< "$uenv")" "0"
check "...requiring the build, the temp base and the counts fixture" \
    "$(sed -n 's/^FIXTURES_REQUIRED=//p' <<< "$um")" "integration.prep;integration.tmp;unit.counts"
ur=$(props build/x64-debug unit.ratchet)
check "unit.ratchet is the counts fixture's CLEANUP, and the x64 tree may raise the baseline" \
    "$(sed -n 's/^FIXTURES_CLEANUP=//p' <<< "$ur") $(sed -n 's/^COMMAND=//p' <<< "$ur" | grep -c 'unit-ratchet.sh .*/unit-counts 1 AxlTest')" "unit.counts 1"
check "...where the aa64 tree only judges (one baseline, x64's)" \
    "$(props build/aa64-debug unit.ratchet | sed -n 's/^COMMAND=//p' | grep -c 'unit-ratchet.sh .*/unit-counts 0 AxlTest')" "1"
check "...and a partial run is its SKIP (77), not a pass" \
    "$(sed -n 's/^SKIP_RETURN_CODE=//p' <<< "$ur")" "77"
check "unit.counts-reset is the counts fixture's setup" \
    "$(props build/x64-debug unit.counts-reset | sed -n 's/^FIXTURES_SETUP=//p')" "unit.counts"
check "unit.inventory checks the tree's built binaries (test-axl.sh --check-inventory)" \
    "$(props build/x64-debug unit.inventory | sed -n 's/^COMMAND=//p' | grep -c 'test-axl.sh --check-inventory .*/build/x64-debug$')" "1"
check "a stub run (AXL_IT_DIR) registers no unit tests" \
    "$(AXL_IT_DIR="$S" ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N -L '^unit$' 2>/dev/null | grep -c 'unit\.')" "0"

echo "=== verify.sh's jobs: the gates as CTest tests ==="
# Each gate is gate.check-<name>, run directly against its tree (slice 5c;
# the Makefile's two gates of itself went with it in slice 6). Read off each
# tree's registration.
gjson() { ctest --test-dir "$PROJECT_DIR/build/$1-debug" --show-only=json-v1 2>/dev/null; }
gx=$(gjson x64); ga=$(gjson aa64)
ng=$(jq '[.tests[] | select(.name | startswith("gate.check-"))] | length' <<< "$gx")
check "the x64 tree registers the gates (control: >= 30, got $ng)" "$(( ng >= 30 ))" "1"
# Every gate niced and ordered after the prep (DEPENDS: ordering only). Without
# the order ctest starts the gates at t=0, the load passes --test-load, and the
# 1.5 s prep the one-boot unit run needs waited ~60 s (verify's x64 tree: 195 s).
nd=$(jq '[.tests[] | select(.name | startswith("gate.check-"))
          | select([.properties[]? | select(.name == "DEPENDS") | .value[]] | index("integration.prep"))
          | select((.command[0] | test("(^|/)nice$")) and .command[1] == "-n" and .command[2] == "15")] | length' <<< "$gx")
check "every gate is niced and ordered after integration.prep" "$nd" "$ng"
# An ARTIFACT gate reads what the prep built, so it REQUIRES the prep.
na=$(jq '[.tests[] | select(.name | startswith("gate.check-"))
          | select([.properties[]? | select(.name == "LABELS") | .value[]] | index("artifact"))] | length' <<< "$gx")
nr=$(jq '[.tests[] | select(.name | startswith("gate.check-"))
          | select([.properties[]? | select(.name == "LABELS") | .value[]] | index("artifact"))
          | select([.properties[]? | select(.name == "FIXTURES_REQUIRED") | .value[]] | index("integration.prep"))] | length' <<< "$gx")
check "every artifact gate requires the prep ($na of them)" "$nr $(( na >= 5 ))" "$na 1"
# No gate runs make: the Makefile, and its two gates of itself, went in
# 5.0.0. A command WORD -- make or gmake, by any path, or as the first word
# of a `bash -c` script -- not a substring: `cmake` and paths hold "make".
runs_make='.tests[] | select(.name | startswith("gate."))
           | .command as $c | ($c | index(["-c"])) as $i
           | select(any($c[]?; test("(^|/)g?make$"))
                    or ($i != null and ($c[$i + 1] // "" | test("^(.*[;&|( ])?g?make( |$)"))))
           | .name'
check "control: the make detector sees /usr/bin/make and a bash -c make" \
    "$(jq -r "$runs_make" <<< '{"tests":[{"name":"gate.a","command":["/usr/bin/make","x"]},
        {"name":"gate.b","command":["bash","-c","cd x && make y"]},
        {"name":"gate.c","command":["cmake","--build","x"]},
        {"name":"gate.d","command":["sh","x","then make it"]}]}' | paste -sd' ')" "gate.a gate.b"
check "no gate runs make" "$(jq -r "$runs_make" <<< "$gx" | sort | paste -sd' ')" ""
# The arch's own gates register in the aa64 tree too, read from THAT tree.
check "the aa64 tree registers its own cxx-entry, ctors and libc-overlap gates" \
    "$(jq -r '.tests[] | select(.name | test("^gate.check-(cxx-entry|ctors|libc-overlap)$"))
              | .command | join(" ")' <<< "$ga" | grep -c "/build/aa64-debug")" "3"
# The make-tree lock went with make: its libaxl.a recipe began `rm -f`, so
# two makes on one prefix raced. Builds in a tree are serialised by
# scripts/axl-build.sh's per-tree flock now (test-axl-build-lock.sh).
check "no test holds the retired make-tree lock" \
    "$(jq -r '.tests[] | select([.properties[]? | select(.name == "RESOURCE_LOCK") | .value[]] | index("make-tree")) | .name' <<< "$gx")" ""
pp=$(props build/x64-debug integration.prep)
check "...nor the prep (whose properties were read: it has LABELS)" \
    "$(grep -c '^LABELS=' <<< "$pp") $(grep -c '^RESOURCE_LOCK=' <<< "$pp")" "1 0"
# NICED, as verify.sh ran them beside the guests: unniced, the box ran at
# load 19-21 and a guest hit its own hang timeout (verify.sh's NICE comment).
check "gate.lint runs scripts/lint.sh, niced" \
    "$(props build/x64-debug gate.lint | sed -n 's/^COMMAND=//p' | grep -c 'nice -n 15 .*scripts/lint.sh$')" "1"
check "gate.docs runs scripts/build-docs.sh, niced" \
    "$(props build/x64-debug gate.docs | sed -n 's/^COMMAND=//p' | grep -c 'nice -n 15 .*scripts/build-docs.sh$')" "1"
gq=$(props build/x64-debug gate.qloop)
check "gate.qloop is scripts/qloop-verdict.sh into the tree's qloop-logs, RUN_SERIAL, its UNJUDGED (3) a SKIP" \
    "$(sed -n 's/^COMMAND=//p' <<< "$gq" | grep -c "scripts/qloop-verdict.sh --dir $PROJECT_DIR/build/x64-debug/qloop-logs\$") $(sed -n 's/^RUN_SERIAL=//p' <<< "$gq") $(sed -n 's/^SKIP_RETURN_CODE=//p' <<< "$gq")" "1 true 3"
# The quiet slot on a leased `unit` guest (pool design §6.7 step 5): pooled,
# gate.qloop takes a unit slot, is told the socket, and requires the pool
# check, as unit.<App> does; unpooled it has neither.
check "...and unpooled takes no guest slot and has no pool socket" \
    "$(grep -c '^RESOURCE_GROUPS=' <<< "$gq") $(sed -n 's/^ENVIRONMENT=//p' <<< "$gq" | tr ';' '\n' | grep -c '^AXL_ONE_POOL_SOCK=')" "0 0"
gqp=$(AXL_IT_POOL=/x/explicit.sock ctest --test-dir "$PROJECT_DIR/build/x64-debug" --show-only=json-v1 \
          -R '^gate\.qloop$' 2>/dev/null)
check "pooled: gate.qloop takes one slot of guest_unit_x64, is told the socket, requires the pool check" \
    "$(rg "$gqp" gate.qloop | grep -c 'guest_unit_x64') $(jq -r '.tests[] | select(.name == "gate.qloop") | .properties[] | select(.name == "ENVIRONMENT") | .value[]' <<< "$gqp" | grep -cx 'AXL_ONE_POOL_SOCK=/x/explicit.sock') $(jq -r '.tests[] | select(.name == "gate.qloop") | .properties[] | select(.name == "FIXTURES_REQUIRED") | .value | join(";")' <<< "$gqp" | grep -c 'integration.pool')" "1 1 1"
check "verify's gates are the x64 tree's only (aa64 registers none of them)" \
    "$(ctest --test-dir "$PROJECT_DIR/build/aa64-debug" -N -L '^gate$' 2>/dev/null | grep -cE 'gate\.(make\.|lint$|docs$|qloop$)')" "0"

echo "=== the test presets ==="
# A PRIVATE runtime dir: the presets pool by default, and these listings
# must not ask (and socket-activate) this host's real service.
pjson() { (cd "$PROJECT_DIR" && XDG_RUNTIME_DIR="$WORK/xdg-presets" ctest --preset "$1" --show-only=json-v1 2>/dev/null); }
mkdir -p "$WORK/xdg-presets"
allx=$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N 2>/dev/null | grep -c 'Test *#')
sx=$(pjson x64-suite)
check "x64-suite selects the whole x64 tree but the one-boot unit run" \
    "$(jq '[.tests[] | select(.name != "integration.pool")] | length' <<< "$sx") $(jq '[.tests[] | select(.name == "unit.one-boot")] | length' <<< "$sx")" \
    "$((allx - 1)) 0"
check "...and, with no service socket, no pool check (nothing pooled, nothing said)" \
    "$(jq '[.tests[] | select(.name == "integration.pool")] | length' <<< "$sx")" "0"
# THE OPT-OUT under a preset: its AXL_IT_POOL_DEFAULT cannot be unset by the
# caller (a preset's environment wins), so AXL_IT_POOL=0 / off means booted,
# said by a check that passes -- not a socket path in refuse mode.
for off in 0 off; do
    o=$( (cd "$PROJECT_DIR" && AXL_IT_POOL="$off" XDG_RUNTIME_DIR="$WORK/xdg-presets" ctest --preset x64-suite --show-only=json-v1 -R "^integration\.($gb|pool)\$" 2>/dev/null) )
    check "AXL_IT_POOL=$off under a preset: booted (no slot), and a passing check says so" \
        "$(rg "$o" "integration.$gb")|$(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$o" | grep -c "echo .pool: off (AXL_IT_POOL=$off)")" "|1"
done
cix=$(pjson x64-ci)
# Counted by NAME: ctest adds the fixtures a selection requires (prep, the
# temp-base pair), and those are not the selection.
itests() { jq -r '.tests[].name' <<< "$1" | grep -c '^integration\.test-'; }
others() { jq -r '.tests[].name' <<< "$1" | grep -cE '^(unit|gate|build)\.'; }
alli=$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N 2>/dev/null | grep -c 'Test *#[0-9]*: integration\.test-')
lo=$(ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N -L '^local-only$' 2>/dev/null | grep -c 'Test *#[0-9]*: integration\.test-')
check "x64-ci runs the integration tests but the local-only ones ($alli - $lo)" "$(itests "$cix")" "$((alli - lo))"
check "...and nothing else: CI's unit run and lint are their own steps and jobs" "$(others "$cix")" "0"
check "...with the prep and temp-base fixtures it needs" \
    "$(jq -r '.tests[].name' <<< "$cix" | grep -cxE 'integration\.(prep|tmp-setup|tmp-cleanup)')" "3"
ix=$(pjson x64-integration)
check "x64-integration runs every integration test, local-only included ($alli)" "$(itests "$ix")" "$alli"
check "...and nothing else" "$(others "$ix")" "0"
check "x64-integration is a suite preset on the x64-debug tree" \
    "$(jq -r '.testPresets[] | select(.name == "x64-integration") | "\(.inherits) \(.configurePreset)"' "$PROJECT_DIR/CMakePresets.json")" "suite-base x64-debug"
check "x64-ci runs each test under CI's 240 s limit (ctest's backstop 3 x 240 + 36)" \
    "$(jq -r '.tests[] | select(.name == "integration.test-tar-qemu") | .properties[] | select(.name == "TIMEOUT") | .value' <<< "$cix" | cut -d. -f1)" "756"
check "control: x64-suite keeps the skip cache on" \
    "$(jq -r '.tests[] | select(.name == "integration.test-tar-qemu") | .properties[] | select(.name == "ENVIRONMENT") | .value[]' <<< "$sx" | grep -c '^AXL_TEST_CACHE=')" "1"
check "x64-ci turns the skip cache off" \
    "$(jq -r '.tests[] | select(.name == "integration.test-tar-qemu") | .properties[] | select(.name == "ENVIRONMENT") | .value[]' <<< "$cix" | grep -c '^AXL_TEST_CACHE=')" "0"

echo "=== a registration that refuses never takes the tree's other tests with it ==="
# The spec file is the tree's (CTEST_RESOURCE_SPEC_FILE): if it is missing,
# ctest runs NO test at all. So it is written before any early exit of the
# registration -- here a bad AXL_IT_TIMEOUT on a tree whose spec is gone.
rm -f "$spec"
bt=$(AXL_IT_TIMEOUT=abc ctest --test-dir "$PROJECT_DIR/build/x64-debug" -R '^integration\.bad-timeout$' 2>&1)
check "a bad AXL_IT_TIMEOUT registers a failing integration.bad-timeout" \
    "$(grep -E 'Test +#[0-9]+: integration\.bad-timeout ' <<< "$bt" | grep -c 'Failed')" "1"
check "...and the spec is still written, so ctest runs the tree at all" \
    "$([[ -s "$spec" ]] && echo written || echo missing) $(grep -c 'Could not read/parse resource spec' <<< "$bt")" "written 0"
# ...and stays registered: listed, not run (it reads a built object, and
# this test builds none).
check "...and the tree's other tests stay registered beside the refusal (one listing)" \
    "$(AXL_IT_TIMEOUT=abc ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N -R '^(gate\.hello-minimal-no-ctors|integration\.bad-timeout)$' 2>&1 | grep -cE 'Test +#[0-9]+: (gate\.hello-minimal-no-ctors|integration\.bad-timeout)$')" "2"

echo "=== one run per tree: the run's owner record ==="
# lib/ctest-tmpbase.sh records its parent (the ctest) as the RUN's owner, in
# a file keyed on the TREE (the registration's .run-owner), not in the temp
# base: the refusal protects the shared build tree and registration, which a
# caller's own temp base (AXL_IT_QEMU_TMPDIR) does nothing to separate. Here
# the parent is this shell, so a foreign owner is simulated with a live
# `sleep` and a dead one with a pid that has exited.
TB="$SCRIPT_DIR/lib/ctest-tmpbase.sh"
B="$WORK/tmpbase"; OWN="$WORK/run-owner"
sleep 60 & live=$!
mkdir -p "$B" && echo "$live" > "$OWN"
bash "$TB" setup "$B" "$OWN" > "$WORK/tb.out" 2>&1; rc=$?
check "setup refuses while another live run owns the tree" "$rc $(grep -c 'another ctest run' "$WORK/tb.out")" "1 1"
check "...and leaves that run's record and base alone" "$(cat "$OWN") $([[ -d "$B" ]] && echo kept)" "$live kept"
bash "$TB" setup "$WORK/elsewhere" "$OWN" --foreign > "$WORK/tb.out" 2>&1; rc=$?
check "...refuses a caller-based run on the same tree too (the base is not what it guards)" \
    "$rc $(grep -c 'another ctest run' "$WORK/tb.out")" "1 1"
bash "$TB" cleanup "$B" "$OWN" > /dev/null 2>&1
check "cleanup leaves a base another run owns" "$([[ -d "$B" ]] && echo kept || echo removed)" "kept"
kill "$live" 2>/dev/null; wait "$live" 2>/dev/null
bash "$TB" setup "$B" "$OWN" > "$WORK/tb.out" 2>&1; rc=$?
check "setup takes over from an owner that is gone (an interrupted run)" "$rc $(cat "$OWN")" "0 $$"
bash "$TB" cleanup "$B" "$OWN" > /dev/null 2>&1
check "cleanup removes the base and the record its own run owns" \
    "$([[ -d "$B" ]] && echo kept || echo removed) $([[ -e "$OWN" ]] && echo kept || echo removed)" "removed removed"
# --foreign: a caller's base -- never created, emptied or removed.
mkdir -p "$WORK/callers"; : > "$WORK/callers/precious"
bash "$TB" setup "$WORK/callers" "$OWN" --foreign > /dev/null 2>&1; rc=$?
check "--foreign setup records the owner and leaves the caller's directory as it was" \
    "$rc $(cat "$OWN") $(ls "$WORK/callers")" "0 $$ precious"
bash "$TB" cleanup "$WORK/callers" "$OWN" --foreign > /dev/null 2>&1
check "--foreign cleanup removes only the record" \
    "$(ls "$WORK/callers") $([[ -e "$OWN" ]] && echo kept || echo removed)" "precious removed"
bash "$TB" setup "$WORK/no-such" "$OWN" --foreign > "$WORK/tb.out" 2>&1; rc=$?
check "--foreign refuses a caller base that does not exist" "$rc $(grep -c 'no directory' "$WORK/tb.out")" "1 1"

echo "=== a front end's settings never reach a nested run ==="
# A front end sets AXL_IT_* / AXL_PREP_* in ctest's environment, and every
# test inherits them -- including a test that starts a NESTED stub ctest,
# whose prep would then empty the outer run's tallies and manifest. Such a
# test clears them (lib/preset-env.sh), whatever their names.
lk=$(env AXL_IT_LOGDIR=/outer AXL_IT_NEWKNOB=1 AXL_PREP_TESTS=/x KEEP_ME=1 bash -c '
    source "$1/lib/preset-env.sh"; preset_env_unset "$2" || exit 9
    env | grep -E "^(AXL_IT_|AXL_PREP_|KEEP_ME=)" | sort' _ "$SCRIPT_DIR" "$PROJECT_DIR")
check "preset_env_unset clears every AXL_IT_* and AXL_PREP_*, named or not, and nothing else" "$lk" "KEEP_ME=1"
# A relative path would be created against ctest's cwd and read against the
# tests' (the source root): two places. Refused, as a bad AXL_IT_TIMEOUT is.
check "a relative AXL_IT_LOGDIR registers a failing integration.bad-env" \
    "$(AXL_IT_LOGDIR=rel/logs ctest --test-dir "$PROJECT_DIR/build/x64-debug" -N 2>/dev/null | grep -cE 'Test +#[0-9]+: integration\.bad-env$')" "1"

echo "ctest-integration: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
