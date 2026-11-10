#!/bin/bash
# test-runner-selftest.sh — host-only checks for run-integration.sh using stub
# "tests" (a pass, a fail, a hang). No QEMU. Not a real integration test;
# excluded from discovery by name (*-selftest.sh).
# selftest-schedule: tree -- its stub runs share build/x64-debug's stub owner
# record and temp base, and it plants a fake owner there on purpose.
set -uo pipefail
# Hermetic against the CALLER's run settings: CI's release-suite step exports
# TEST_MAX_DURATION for its unit run, which the runner scrubs (cts_inherited)
# and so would add a ctest-unset line to every exact argv/env check below.
# The scrub itself is checked by handing it explicitly.
unset TEST_MAX_DURATION
cd "$(dirname "$0")"
# shellcheck source=lib/shm-holders.sh
source lib/shm-holders.sh

stub=$(mktemp -d); trap 'rm -rf "$stub"' EXIT
# The runner POOLS BY DEFAULT when the user service's socket exists: this
# host's live pool must not decide what these cases run. Each case that is
# about the default points XDG_RUNTIME_DIR at a socket of its own.
export XDG_RUNTIME_DIR="$stub/xdg-none"; mkdir -p "$XDG_RUNTIME_DIR"
printf '#!/bin/bash\nexit 0\n'   > "$stub/test-pass-qemu.sh"
printf '#!/bin/bash\nexit 1\n'   > "$stub/test-fail-qemu.sh"
printf '#!/bin/bash\nsleep 30\n' > "$stub/test-hang-qemu.sh"
# Exit 77 is the automake convention for "skipped". A test that cannot run --
# an absent external corpus, a missing host tool -- must NOT be counted as a
# pass: 39 of the 176 integration tests can exit 0 having tested nothing, and
# the suite reported them among "170 passed".
printf '#!/bin/bash\necho "SKIP: nothing to test"\nexit 77\n' > "$stub/test-skip-qemu.sh"
# Reports whether the runner enabled caching for it. AXL_TEST_CACHE is the only
# thing a test can observe about that decision, so it is what the assertions
# below read -- rather than inspecting the runner's internals.
printf '#!/bin/bash\necho "CACHEENV=${AXL_TEST_CACHE:-unset}"\nexit 0\n' > "$stub/test-cacheprobe-qemu.sh"
chmod +x "$stub"/*.sh

fail=0
need() { grep -q "$1" <<<"$2" || { echo "  FAIL: expected /$1/ in output"; fail=1; }; }

# --- serial (-j1 explicitly; the no-j default now auto-sizes to nproc-2) ---
out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 2 2>&1); rc=$?
need "test-pass-qemu.sh PASS" "$out"
need "test-fail-qemu.sh FAIL" "$out"
need "test-hang-qemu.sh TIMEOUT" "$out"
# A skip is its own verdict, and must be neither PASS nor FAIL.
need "test-skip-qemu.sh SKIP" "$out"
grep -qE 'test-skip-qemu\.sh (PASS|FAIL)' <<<"$out" \
    && { echo "  FAIL: an exit-77 test was scored PASS/FAIL, not SKIP"; fail=1; }
# ...and the totals must say so, or a suite that skipped half of itself still
# reads as a clean run.
need "skipped" "$out"
[[ $rc -ne 0 ]] || { echo "  FAIL: runner exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: serial verdicts + non-zero exit"

# --- caching is ON BY DEFAULT, and OFF for the runs that must be authoritative
#
# The cache skips a test whose inputs are byte-identical to its last green run
# (lib/test-cache.sh). It is the default because an opt-in flag does not get
# typed -- measured the hard way: it was built and then not used once in the
# session that built it. What must NOT be cached is the run that certifies a
# tree: --no-cache for a pre-push/release gate, and --ci, because CI is the
# backstop and a backstop that skips is not one.
# A test's stdout goes to its per-test log, not the runner's, so the probe is
# read from there. The runner prints the log directory on its last line.
# Only the probe runs: the cache decision is per-run (--only leaves CACHE_DIR
# alone), and the whole stub set cost each call ~12 s of retry back-off and
# hang timeouts that this case never reads. The banner run below is narrowed
# the same way: the cache banner keys on CACHE_DIR and the cached count, never
# on --only. Together they were 62 s of the file.
cacheenv() {   # $1.. = runner args; echoes the probe's observation
    local out logdir
    out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh "$@" --only=test-cacheprobe-qemu.sh 2>&1)
    logdir=$(grep -oE '^logs: .*' <<<"$out" | tail -1 | cut -d' ' -f2)
    [[ -n "$logdir" ]] && grep -h -oE 'CACHEENV=[^ ]*' \
        "$logdir/test-cacheprobe-qemu.sh.log" 2>/dev/null | tail -1
}
[[ "$(cacheenv -j1 --timeout 2)" == CACHEENV=/* ]] \
    || { echo "  FAIL: caching is not on by default"; fail=1; }
[[ "$(cacheenv -j1 --timeout 2 --no-cache)" == "CACHEENV=unset" ]] \
    || { echo "  FAIL: --no-cache did not disable the cache"; fail=1; }
# An INHERITED cache (the caller's shell, or a test of an outer ctest run,
# whose environment carries one) must not turn it back on.
[[ "$(AXL_TEST_CACHE=/tmp/inherited-cache cacheenv -j1 --timeout 2 --no-cache)" == "CACHEENV=unset" ]] \
    || { echo "  FAIL: an inherited AXL_TEST_CACHE re-enabled the cache under --no-cache"; fail=1; }
[[ "$(cacheenv -j1 --timeout 2 --ci)" == "CACHEENV=unset" ]] \
    || { echo "  FAIL: --ci did not disable the cache — CI would skip tests"; fail=1; }
outc=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 2 --only=test-cacheprobe-qemu.sh 2>&1)
# The PARTIAL banner must fire only when something was actually skipped, or it
# is printed on every run and stops carrying information. (This looked for
# "PARTIAL -- --cache", a string the runner never printed: it could not fail.
# Its control, that the banner DOES fire on a cached run, is in the fixture
# block below.)
grep -q '^integration: PARTIAL -- cache:' <<<"$outc" \
    && { echo "  FAIL: PARTIAL printed with 0 cached tests"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: cache on by default; off for --no-cache and --ci"

# --- parallel (-j4): same verdicts as serial, still non-zero exit ---
outp=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j4 --timeout 2 2>&1); rcp=$?
need "test-pass-qemu.sh PASS" "$outp"
need "test-fail-qemu.sh FAIL" "$outp"
need "test-hang-qemu.sh TIMEOUT" "$outp"
need "test-skip-qemu.sh SKIP" "$outp"
[[ $rcp -ne 0 ]] || { echo "  FAIL: -j4 exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: -j4 verdicts + non-zero exit"

# --- retry-once: a test that fails on attempt 1 but passes on attempt 2 is
#     reported PASS (the parallel pool's transient-contention mitigation). ---
rstub=$(mktemp -d)
printf '#!/bin/bash\nm=%s/flaky.marker\n[[ -e "$m" ]] && exit 0\ntouch "$m"; exit 1\n' "$rstub" \
    > "$rstub/test-flaky-qemu.sh"
chmod +x "$rstub"/*.sh
rout=$(RUN_INTEGRATION_DIR="$rstub" ./run-integration.sh -j1 --timeout 5 2>&1); rrc=$?
rm -rf "$rstub"
need "test-flaky-qemu.sh PASS" "$rout"
need "(retry" "$rout"
[[ $rrc -eq 0 ]] || { echo "  FAIL: retry-recovered flaky test should exit 0"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: transient failure retried -> PASS, exit 0"

# --- UNJUDGED: a timing test whose guest the host starved (exit 77 with an
#     `UNJUDGED: <why>` line) is neither PASS nor SKIP nor FAIL. It is retried
#     like a failure -- an unjudged run is no verdict, so the retry's is -- and
#     reads UNJUDGED only when no attempt judged. The runner names it, counts
#     it apart, and exits 3: not red, never green. ---
ustub=$(mktemp -d)
printf '#!/bin/bash\necho "UNJUDGED: 1 timing bound missed while starved"\nexit 77\n' \
    > "$ustub/test-unjudged-qemu.sh"
# Unjudged on attempt 1, a judged PASS on the retry: the retry's verdict.
printf '#!/bin/bash\nm=%s/uj.marker\n[[ -e "$m" ]] && exit 0\ntouch "$m"; echo "UNJUDGED: starved"; exit 77\n' "$ustub" \
    > "$ustub/test-ujpass-qemu.sh"
chmod +x "$ustub"/*.sh
uout=$(RUN_INTEGRATION_DIR="$ustub" ./run-integration.sh -j1 --timeout 5 2>&1); urc=$?
uerr=0
grep -qxE '  test-unjudged-qemu\.sh UNJUDGED [0-9]+s \(after retry; UNJUDGED: 1 timing bound missed while starved\)' <<<"$uout" \
    || { echo "  FAIL: an unjudged test (both attempts) did not read UNJUDGED with its reason"; uerr=1; }
grep -qxE '  test-ujpass-qemu\.sh PASS [0-9]+s \(retry; attempt 1 UNJUDGED -- see test-ujpass-qemu\.sh\.attempt1\.log\)' <<<"$uout" \
    || { echo "  FAIL: unjudged then a judged pass did not read PASS (retry; attempt 1 UNJUDGED ...)"; uerr=1; }
grep -qx 'integration: 1 passed, 0 failed, 1 UNJUDGED (X64)' <<<"$uout" \
    || { echo "  FAIL: the headline does not count the unjudged test apart from the passes"; uerr=1; }
grep -qx '    unjudged: test-unjudged-qemu.sh' <<<"$uout" \
    || { echo "  FAIL: the unjudged test is not named under the headline"; uerr=1; }
[[ $urc -eq 3 ]] || { echo "  FAIL: a run with an UNJUDGED test and no failure exited $urc, want 3"; uerr=1; }
# Unjudged, then a judged FAIL: the FAIL stands.
printf '#!/bin/bash\nm=%s/uf.marker\n[[ -e "$m" ]] && exit 1\ntouch "$m"; echo "UNJUDGED: starved"; exit 77\n' "$ustub" \
    > "$ustub/test-ujfail-qemu.sh"
chmod +x "$ustub/test-ujfail-qemu.sh"
uout=$(RUN_INTEGRATION_DIR="$ustub" ./run-integration.sh -j1 --timeout 5 --only=test-ujfail-qemu.sh 2>&1); urc=$?
grep -qxE '  test-ujfail-qemu\.sh FAIL [0-9]+s \(after retry\)' <<<"$uout" \
    || { echo "  FAIL: unjudged then a judged FAIL did not read FAIL"; uerr=1; }
[[ $urc -eq 1 ]] || { echo "  FAIL: unjudged-then-FAIL exited $urc, want 1"; uerr=1; }
# A judged FAIL, then UNJUDGED on the retry: the FAIL is not laundered.
printf '#!/bin/bash\nm=%s/fu.marker\n[[ -e "$m" ]] && { echo "UNJUDGED: starved"; exit 77; }\ntouch "$m"; exit 1\n' "$ustub" \
    > "$ustub/test-failuj-qemu.sh"
chmod +x "$ustub/test-failuj-qemu.sh"
uout=$(RUN_INTEGRATION_DIR="$ustub" ./run-integration.sh -j1 --timeout 5 --only=test-failuj-qemu.sh 2>&1); urc=$?
grep -qxE '  test-failuj-qemu\.sh FAIL [0-9]+s \(after retry\)' <<<"$uout" \
    || { echo "  FAIL: a judged FAIL then an UNJUDGED retry did not read FAIL"; uerr=1; }
[[ $urc -eq 1 ]] || { echo "  FAIL: FAIL-then-unjudged exited $urc, want 1"; uerr=1; }
# UNJUDGED, then a plain SKIP on the retry: neither judged -- UNJUDGED.
printf '#!/bin/bash\nm=%s/us.marker\n[[ -e "$m" ]] && { echo "SKIP: gone"; exit 77; }\ntouch "$m"; echo "UNJUDGED: starved"; exit 77\n' "$ustub" \
    > "$ustub/test-ujskip-qemu.sh"
chmod +x "$ustub/test-ujskip-qemu.sh"
uout=$(RUN_INTEGRATION_DIR="$ustub" ./run-integration.sh -j1 --timeout 5 --only=test-ujskip-qemu.sh 2>&1); urc=$?
grep -qxE '  test-ujskip-qemu\.sh UNJUDGED [0-9]+s \(after retry, which skipped; UNJUDGED: starved\)' <<<"$uout" \
    || { echo "  FAIL: unjudged then a skipped retry did not read UNJUDGED"; uerr=1; }
[[ $urc -eq 3 ]] || { echo "  FAIL: unjudged-then-skip exited $urc, want 3"; uerr=1; }
# Without --retry (a direct run-one.sh call), an UNJUDGED run is reported as
# it stands: 77, and the UNJUDGED line.
ulog=$(mktemp -d)
uout=$(AXL_ONE_LOGDIR="$ulog" ./lib/run-one.sh "$ustub/test-unjudged-qemu.sh" 2>&1); urc=$?
rm -rf "$ulog"
grep -qxE '  test-unjudged-qemu\.sh UNJUDGED [0-9]+s \(UNJUDGED: 1 timing bound missed while starved\)' <<<"$uout" \
    || { echo "  FAIL: run-one.sh without --retry did not read UNJUDGED"; uerr=1; }
[[ $urc -eq 77 ]] || { echo "  FAIL: run-one.sh without --retry exited $urc for UNJUDGED, want 77"; uerr=1; }
# A plain 77 (no UNJUDGED line) is still a SKIP, not retried.
printf '#!/bin/bash\necho "SKIP: no widget"\nexit 77\n' > "$ustub/test-plainskip-qemu.sh"
chmod +x "$ustub/test-plainskip-qemu.sh"
uout=$(RUN_INTEGRATION_DIR="$ustub" ./run-integration.sh -j1 --timeout 5 --only=test-plainskip-qemu.sh 2>&1); urc=$?
grep -qxE '  test-plainskip-qemu\.sh SKIP [0-9]+s \(SKIP: no widget\)' <<<"$uout" \
    || { echo "  FAIL: a plain exit-77 skip no longer reads SKIP"; uerr=1; }
[[ $urc -eq 0 ]] || { echo "  FAIL: a plain skip exited $urc, want 0"; uerr=1; }
rm -rf "$ustub"
if (( uerr )); then fail=1; printf '%s\n' "$uout" | tail -20 | sed 's/^/    | /'
else echo "  PASS: UNJUDGED -- retried, named, counted apart, exit 3; a judged retry decides"; fi

# --- concurrent workers get distinct host-port bases ---
# The runner no longer assigns TEST_PORT_BASE (a formula only keeps ONE
# invocation self-consistent — see run_one). It must leave the variable
# unset so each test claims its own base through common-test.sh, and those
# claims must not collide across workers running at the same time. The stubs
# source common-test.sh for real and hold their claim while the others run,
# so the overlap is genuine rather than sequential reuse of one port.
bstub=$(mktemp -d); bout=$(mktemp -d)
for n in a b c; do
    printf '#!/bin/bash\nsource "%s/common-test.sh"\necho "$TEST_PORT_BASE" > "%s/$$.base"\nsleep 2\nexit 0\n' \
        "$PWD" "$bout" > "$bstub/test-base-$n-qemu.sh"
done
chmod +x "$bstub"/*.sh
RUN_INTEGRATION_DIR="$bstub" ./run-integration.sh -j4 --timeout 20 >/dev/null 2>&1
nbases=$(cat "$bout"/*.base 2>/dev/null | wc -l)
ndistinct=$(cat "$bout"/*.base 2>/dev/null | sort -u | wc -l)
inrange=$(awk '$1 >= 18000 && $1 <= 19999' "$bout"/*.base 2>/dev/null | wc -l)
rm -rf "$bstub" "$bout"
if [[ "$nbases" -eq 3 && "$ndistinct" -eq 3 ]]; then
    echo "  PASS: 3 concurrent workers claimed 3 distinct port bases"
else
    echo "  FAIL: expected 3 distinct port bases, got $ndistinct of $nbases"; fail=1
fi
if [[ "$inrange" -eq 3 ]]; then
    echo "  PASS: runner left TEST_PORT_BASE to the allocator (all in 18000-19999)"
else
    echo "  FAIL: $inrange/3 bases came from the allocator range"; fail=1
fi

# --- the fixture pre-build happens BEFORE the cache decision -----------------
#
# THE DEFECT THIS PINS. The cache key hashes the STAGED ARTIFACTS a test
# recorded on its last green run (lib/test-cache.sh), and ~74 fixture targets
# are built by the tests themselves, inside their own run. run_one decides the
# cache first. So on the first cached invocation after a library change the
# fixture .efi still hashes as its previous self, every input matches, the
# test reports CACHED -- and because it never ran, it never rebuilt the
# fixture either, so the next invocation caches again. Measured 2026-09-26: a
# sabotage of src/net/axl-ssh-server.c went UNDETECTED through
# `--only=test-ssh-auth-qemu.sh` with the cache on.
#
# The same missing pre-build is also a build RACE: five ssh tests each built
# ssh-server-selftest under the pool and four of five failed
# attempt 1 with "ssh-server-selftest.so is empty" -- one worker's objcopy
# reading the .so another worker's ld was writing.
#
# So the assertion is not "the union is computed right", it is "the build ran
# even though the test did not".
echo "-- the fixture pre-build --"
fxstub=$(mktemp -d); fxproj=$(mktemp -d); fxlog=$(mktemp -d)
export FXLOG="$fxlog/order.txt"
# A CMake project of four goals, each logging its arch and the one variable
# a builds= entry may pass through (SSHD_RTK_OUT, which axl-build.sh hands
# the build in the environment), in the presets axl-build.sh configures.
cat > "$fxproj/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.29)
project(fx NONE)
foreach(t fixture-a fixture-b fixture-c report-x)
    add_custom_target(${t} COMMAND sh ${CMAKE_SOURCE_DIR}/log.sh ${t} ${AXL_ARCH} VERBATIM)
endforeach()
CM
cat > "$fxproj/log.sh" <<'LG'
echo "build:$1 ARCH=$2${SSHD_RTK_OUT:+ SSHD_RTK_OUT=$SSHD_RTK_OUT}" >> "$FXLOG"
LG
cat > "$fxproj/CMakePresets.json" <<'PR'
{"version": 6, "configurePresets": [
  {"name": "x64-debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/x64-debug",
   "cacheVariables": {"AXL_ARCH": "x64"}},
  {"name": "aa64-debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/aa64-debug",
   "cacheVariables": {"AXL_ARCH": "aa64"}}]}
PR
# What the prep reads before writing its pre-built manifest.
mkdir -p "$fxproj/scripts"
printf 'NON_ARTIFACT_GOALS = report-x\n' > "$fxproj/scripts/axl-lists.conf"
# The runner builds through the project's scripts/axl-build.sh, so the fixture
# project carries the REAL one, not a stand-in -- what is asserted below is
# what the runner actually runs.
# With the pinned-CMake helper it sources and the pin beside it
# (scripts/axl-cmake-pin.sh).
cp "$PWD/../../scripts/axl-build.sh" "$PWD/../../scripts/axl-cmake-pin.sh" \
   "$PWD/../../scripts/ci-cmake.sh" "$fxproj/scripts/"
# The stub records a cache input of its own (a file that does not change), so
# a second invocation with nothing touched is a genuine cache HIT -- which is
# the state the defect hides in.
cat > "$fxstub/test-fxa-qemu.sh" <<FXA
#!/bin/bash
# test-meta: arch=both est=1 builds=fixture-a
source "$PWD/lib/test-cache.sh"
cache_record_input "\$(basename "\$0")" "$fxproj/CMakeLists.txt"
echo "test:fxa" >> "\$FXLOG"
exit 0
FXA
# No builds= at all: the control. If the runner ran a build for this one too,
# the assertions above would pass for a reason that has nothing to do with
# the declaration.
cat > "$fxstub/test-fxnone-qemu.sh" <<FXN
#!/bin/bash
# test-meta: arch=both est=1
echo "test:fxnone" >> "\$FXLOG"
exit 0
FXN
chmod +x "$fxstub"/*.sh
fxcache=$(mktemp -d)
fxrun() {   # $@ = extra runner args
    RUN_TEST_CACHE_DIR="$fxcache" RUN_PROJECT_DIR="$fxproj" \
        RUN_INTEGRATION_DIR="$fxstub" ./run-integration.sh -j1 --timeout 30 "$@" 2>&1
}
: > "$FXLOG"
fx1=$(fxrun)
grep -q '^build:fixture-a ARCH=x64$' "$FXLOG" \
    || { echo "  FAIL: the declared fixture was not built at all: $(cat "$FXLOG")"; fail=1; }
# ORDER: the build must precede every test, since the cache decision for each
# test is made inside the same call that runs it.
[[ "$(grep -n 'build:fixture-a' "$FXLOG" | head -1 | cut -d: -f1)" == "1" ]] \
    || { echo "  FAIL: the fixture build did not run before the tests: $(cat "$FXLOG")"; fail=1; }
grep -q 'test-fxa-qemu.sh PASS' <<<"$fx1" \
    || { echo "  FAIL: the fixture stub did not run green on the first pass: $fx1"; fail=1; }
# THE ONE THAT MATTERS: run again, nothing changed, the test is CACHED -- and
# the fixture build must STILL have run.
: > "$FXLOG"
fx2=$(fxrun)
grep -q 'test-fxa-qemu.sh CACHED' <<<"$fx2" \
    || { echo "  FAIL: control -- the second run did not hit the cache, so the next check proves nothing: $fx2"; fail=1; }
grep -q '^integration: PARTIAL -- cache: 1 test(s) SKIPPED as unchanged' <<<"$fx2" \
    || { echo "  FAIL: a run with a CACHED test did not say it is PARTIAL: $fx2"; fail=1; }
grep -q '^build:fixture-a ARCH=x64$' "$FXLOG" \
    || { echo "  FAIL: a CACHED test skipped its fixture build -- the key will hash the stale artifact again"; fail=1; }
[[ "$(grep -c '^build:' "$FXLOG")" == "1" ]] \
    || { echo "  FAIL: the run built more than the one declared fixture: $(cat "$FXLOG")"; fail=1; }
# A VAR=VALUE entry reaches the build, and an ARCH= entry overrides the run's arch
# (test-fwtool-host.sh runs on both arches and builds its host tool at x64).
cat > "$fxstub/test-fxb-qemu.sh" <<'FXB'
#!/bin/bash
# test-meta: arch=both est=1 builds=SSHD_RTK_OUT=/fx-out,fixture-b
exit 0
FXB
cat > "$fxstub/test-fxc-qemu.sh" <<'FXC'
#!/bin/bash
# test-meta: arch=both est=1 builds=ARCH=x64,fixture-c
exit 0
FXC
chmod +x "$fxstub"/*.sh
: > "$FXLOG"
fxrun --arch AARCH64 >/dev/null 2>&1
grep -q '^build:fixture-b ARCH=aa64 SSHD_RTK_OUT=/fx-out$' "$FXLOG" \
    || { echo "  FAIL: a VAR=VALUE entry did not reach the build: $(cat "$FXLOG")"; fail=1; }
grep -q '^build:fixture-c ARCH=x64$' "$FXLOG" \
    || { echo "  FAIL: an ARCH= entry did not override the run's arch: $(cat "$FXLOG")"; fail=1; }
grep -q '^build:fixture-a ARCH=aa64$' "$FXLOG" \
    || { echo "  FAIL: an undecorated fixture did not build at the run's arch: $(cat "$FXLOG")"; fail=1; }
# THE PRE-BUILT MANIFEST (scripts/axl-build.sh skips a test's identical
# build by it): the runner writes one line per fixture build, its
# fingerprint computed from THAT build's arguments, and leaves out a goal the
# project declares non-artifact (report-x) -- whose call the test still gets
# run, while its call for the pre-built fixture is skipped.
mfstub=$(mktemp -d); mflogs=$(mktemp -d)
cat > "$mfstub/test-mf-qemu.sh" <<MF
#!/bin/bash
# test-meta: arch=both est=1 builds=fixture-a,report-x
"$fxproj/scripts/axl-build.sh" fixture-a
"$fxproj/scripts/axl-build.sh" report-x
exit 0
MF
chmod +x "$mfstub"/*.sh
: > "$FXLOG"
mfout=$(RUN_PROJECT_DIR="$fxproj" RUN_INTEGRATION_DIR="$mfstub" \
        ./run-integration.sh -j1 --timeout 30 --no-cache --logdir "$mflogs" 2>&1)
want_fp=$("$fxproj/scripts/axl-build.sh" --env-fingerprint ARCH=x64 fixture-a report-x)
[[ "$(cat "$mflogs/_prebuilt" 2>/dev/null)" == "x64||fixture-a|$want_fp" ]] \
    && echo "  PASS: the manifest: one line per fixture build, its own fingerprint, no non-artifact goal" \
    || { echo "  FAIL: the pre-built manifest is not [x64||fixture-a|<fp>]: [$(cat "$mflogs/_prebuilt" 2>/dev/null)] $mfout"; fail=1; }
[[ "$(grep -c '^axl-build.sh: pre-built by the runner: ARCH=x64 fixture-a$' "$mflogs/test-mf-qemu.sh.log") $(grep -c '^build:report-x' "$FXLOG")" == "1 2" ]] \
    && echo "  PASS: the test's own call: the pre-built fixture skipped, the report goal run (pre-build + test)" \
    || { echo "  FAIL: skip/run of the test's own calls: log=[$(cat "$mflogs/test-mf-qemu.sh.log")] fx=[$(cat "$FXLOG")]"; fail=1; }
# An answer that could not be had is not "none": a project whose list of
# non-artifact goals cannot be read stops the run.
nproj=$(mktemp -d); mkdir -p "$nproj/scripts"; cp "$fxproj"/scripts/{axl-build,axl-cmake-pin,ci-cmake}.sh "$nproj/scripts/"
nout=$(RUN_PROJECT_DIR="$nproj" RUN_INTEGRATION_DIR="$mfstub" \
       ./run-integration.sh -j1 --timeout 30 --no-cache --logdir "$(mktemp -d)" 2>&1); nrc=$?
# Under CTest the prep is a fixture: its failure fails the run (1) and the
# report quotes its output under "integration.prep FAILED".
[[ "$nrc $(grep -c "cannot read the project's non-artifact goals" <<<"$nout") $(grep -c '^integration: integration.prep FAILED' <<<"$nout")" == "1 1 1" ]] \
    && echo "  PASS: a project whose non-artifact goals cannot be read stops the run, the prep named" \
    || { echo "  FAIL: an unanswerable non-artifact query did not stop the run: rc=$nrc $nout"; fail=1; }
rm -rf "$mfstub" "$mflogs" "$nproj"

# A FIXTURE THAT WILL NOT BUILD STOPS THE RUN. Letting it through means every
# test that needs it fails one at a time, minutes apart, for a reason none of
# them names.
cat > "$fxstub/test-fxd-qemu.sh" <<'FXD'
#!/bin/bash
# test-meta: arch=both est=1 builds=fixture-missing
exit 0
FXD
chmod +x "$fxstub"/*.sh
fxbad=$(fxrun); fxbadrc=$?
[[ $fxbadrc -ne 0 ]] && grep -q 'fixture pre-build FAILED' <<<"$fxbad" \
    || { echo "  FAIL: an unbuildable declared fixture did not stop the run (rc=$fxbadrc): $fxbad"; fail=1; }
unset FXLOG
rm -rf "$fxstub" "$fxproj" "$fxlog" "$fxcache"
[[ $fail -eq 0 ]] && echo "  PASS: declared fixtures are built once, before the cache decision, at the right arch"

# --- --shard i/K partitions the set: union == full, no overlap ---
a=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 0/2 --list | sort)
b=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 1/2 --list | sort)
allset=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --list | sort)
union=$(printf '%s\n%s\n' "$a" "$b" | grep -v '^$' | sort -u)
overlap=$(comm -12 <(printf '%s\n' "$a" | grep -v '^$') <(printf '%s\n' "$b" | grep -v '^$'))
if [[ "$union" == "$allset" ]]; then echo "  PASS: shards cover the full set"; else echo "  FAIL: shard union != full set"; fail=1; fi
if [[ -z "$overlap" ]]; then echo "  PASS: shards do not overlap"; else echo "  FAIL: shard overlap: $overlap"; fail=1; fi

# --- a signalled runner must reap the guest pool BEFORE removing the tree ---
# The runner exports one shared AXL_QEMU_TMPDIR and every worker launches a
# guest underneath it. If the signal trap rm -rf's that tree without first
# killing the guests, each survivor keeps its disk open as a DELETED inode:
# the directory is gone, so every directory-based check says "clean", while
# tmpfs cannot reclaim the pages. 365 MB accumulated that way before anyone
# noticed. du cannot see it, and a check that the directory went away cannot
# either. This was a `df -k /dev/shm` delta, which sees it but is GLOBAL: any
# other run writing /dev/shm moved it, so this file could not share the box.
# It now lists the processes still holding a file under the run's base
# (lib/shm-holders.sh) -- the pages are held by exactly those -- with a
# positive control: while the stand-ins are up, the scan must see them.
sig_check() {
    local signal="$1" sstub sout base held rpid rc=0
    sstub=$(mktemp -d); sout=$(mktemp -d)
    # Two stand-ins, because the runner reaps by two different mechanisms and
    # a test that only covered one would let the other rot:
    #
    #   A: an ordinary descendant of the worker -- caught by the PID walk.
    #   B: outside the worker's session and orphaned -- reparented to the
    #      nearest subreaper (run-one.sh, lib/subreaper.py) or init -- and
    #      named qemu-system-*, so it passes the reaper's comm filter. The
    #      session kill cannot reach it, so only reap_pool can (re-verified
    #      by sabotage once run-one.sh became a subreaper). This is the shape
    #      the real orphans had.
    #
    # B is launched as `( setsid ... & )`, not a bare `setsid ... &`. setsid
    # changes the SESSION, not the parent: on its own the guest stays a child
    # of the stub and the PID walk reaches it anyway, so the assertion would
    # pass without reap_pool existing at all. The throwaway subshell exits
    # immediately, which is what actually reparents it away. Sabotage
    # caught this -- the first version passed with reap_pool deleted.
    #
    # `tail -f` is the body of B: it holds the disk open AND carries the path
    # in argv, which are the two properties the reaper depends on. A shell
    # script could not be used -- /proc/PID/comm would read `bash`.
    cp "$(command -v tail)" "$sstub/qemu-system-selftest"
    cat > "$sstub/test-guest-qemu.sh" <<STUB
#!/bin/bash
echo "\$AXL_QEMU_TMPDIR" > "$sout/base"
d="\$AXL_QEMU_TMPDIR/axl-qemu.\$\$"
mkdir -p "\$d"
dd if=/dev/zero of="\$d/a.img" bs=1M count=32 status=none
dd if=/dev/zero of="\$d/b.img" bs=1M count=32 status=none
bash -c 'exec 9< "\$1/a.img"; sleep 300' _ "\$d" &
( setsid "$sstub/qemu-system-selftest" -f "\$d/b.img" >/dev/null 2>&1 & )
echo ready > "$sout/ready.\$\$"
sleep 300
STUB
    chmod +x "$sstub"/*.sh "$sstub/qemu-system-selftest"

    RUN_INTEGRATION_DIR="$sstub" ./run-integration.sh -j1 --timeout 120 >/dev/null 2>&1 &
    rpid=$!
    # Wait for the stand-in guest to be holding its disk open.
    for _ in $(seq 1 100); do
        compgen -G "$sout/ready.*" >/dev/null 2>&1 && break
        sleep 0.2
    done
    base=$(cat "$sout/base" 2>/dev/null)
    # The control: both stand-ins hold a disk under the base (a.img through
    # bash's fd 9, b.img through tail). A scan that cannot see them would make
    # "nothing held" below mean nothing. DISTINCT disks, not lines: a bash
    # that forks `sleep` holds a.img twice, which must not stand in for b.img.
    # Polled (5 s): `ready` follows the LAUNCH of both, not their open().
    local seen=0 _h
    if [[ "$base" == /* ]]; then
        for _ in $(seq 1 25); do
            _h=$(shm_holders "$base")
            seen=$(grep -oE ' fd .*/[ab]\.img' <<<"$_h" | grep -oE '[ab]\.img' | sort -u | wc -l)
            [[ "$seen" -ge 2 ]] && break
            sleep 0.2
        done
    fi

    # Signal only the runner -- that is the `timeout` case, and the one that
    # produced the observed orphans. (A terminal Ctrl-C hits the whole process
    # group, so the workers die of their own accord and the bug is masked.)
    kill -"$signal" "$rpid" 2>/dev/null
    # Bounded wait, then escalate the way `timeout --kill-after` does. The
    # handler exits on its own now, so this normally completes in one pass;
    # it exists so a regression that leaves the runner spinning fails the
    # assertion instead of hanging the suite.
    local exited=0
    for _ in $(seq 1 40); do
        kill -0 "$rpid" 2>/dev/null || { exited=1; break; }
        sleep 0.25
    done
    if [[ "$exited" -eq 0 ]]; then
        kill -9 "$rpid" 2>/dev/null
    fi
    wait "$rpid" 2>/dev/null
    sleep 1

    # 1. No survivor may still reference the run's tree.
    held=$(pgrep -af -- "$base" 2>/dev/null | grep -cv 'run-integration' || true)
    # 2. The tree itself must be gone.
    local tree_gone=1; [[ -n "$base" && -e "$base" ]] && tree_gone=0
    # 3. tmpfs must actually be reclaimed -- the deleted-inode check. This is
    #    the one a directory-existence assertion cannot make: no process may
    #    still hold a file of the tree, deleted or not.
    local holders=""
    [[ "$base" == /* ]] && holders=$(shm_holders "$base")

    if [[ "$held" -ne 0 ]]; then
        echo "  FAIL: SIG$signal left $held process(es) holding $base"
        pgrep -af -- "$base" 2>/dev/null | grep -v 'run-integration' | sed 's/^/         /'
        rc=1
    fi
    [[ "$tree_gone" -eq 1 ]] || { echo "  FAIL: SIG$signal left the tree $base behind"; rc=1; }
    if [[ "$seen" -lt 2 ]]; then
        echo "  FAIL: SIG$signal: the holder scan saw $seen of the 2 stand-in disks under" \
             "'${base:-<no base>}' while they were held -- its \"nothing held\" means nothing"
        rc=1
    fi
    if [[ -n "$holders" ]]; then
        echo "  FAIL: SIG$signal left files of $base held open (tmpfs not reclaimed):"
        sed 's/^/         /' <<<"$holders"
        rc=1
    fi
    # Never leave our own strays behind, pass or fail. Two handles are needed:
    # the guest stand-ins carry the /dev/shm base, the stub test scripts carry
    # the stub dir.
    [[ -n "$base" ]] && { pkill -f -- "$base" 2>/dev/null; sleep 0.5; rm -rf "$base"; }
    pkill -f -- "$sstub" 2>/dev/null
    sleep 0.3
    rm -rf "$sstub" "$sout"
    if [[ "$rc" -eq 0 ]]; then
        echo "  PASS: SIG$signal reaps the pool, removes the tree, reclaims tmpfs"
    else
        fail=1
    fi
}
# --- --only: an explicit subset, and a LOUD refusal for a name that is not there
#
# WHY EXPLICIT AND NOT COMPUTED. §12.5 of AXL-CI-Release-Speed-Design.md is the
# argument: `8af4e530` touched src/log/ and the Makefile, any relevance map
# picks "the logging tests", and the real blast radius was every image in the
# tree failing at link. So the runner gains the ability to run a SUBSET and no
# ability whatever to decide which -- the caller names them, and the caller
# that matters (ci-plan.sh) is itself fixture-tested and sabotage-verified.
echo "-- --only --"
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu.sh --list 2>&1)
[[ "$(echo "$o" | grep -c .)" == "1" ]] && grep -q 'test-pass-qemu.sh' <<<"$o"     || { echo "  FAIL: --only=<name> did not select exactly that test: $o"; fail=1; }

# The `.sh` is optional, because a caller naming tests is a human or a script
# quoting a test-meta name, and both spellings are what people write.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu --list 2>&1)
grep -q 'test-pass-qemu.sh' <<<"$o"     || { echo "  FAIL: --only without .sh did not match: $o"; fail=1; }

o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu.sh,test-skip-qemu.sh --list 2>&1)
[[ "$(echo "$o" | grep -c .)" == "2" ]]     || { echo "  FAIL: --only with two names did not select two: $o"; fail=1; }

# THE ONE THAT MATTERS. A typo must not select nothing and report success --
# "the tool ran and found nothing" and "you asked for something that is not
# there" are the same empty set and opposite facts, and a CI caller passing a
# renamed test would silently verify NOTHING while going green.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-nope-qemu.sh --list 2>&1); rc=$?
[[ $rc -ne 0 ]] && grep -qi 'test-nope-qemu' <<<"$o"     || { echo "  FAIL: --only with an unknown name did not fail loudly (rc=$rc): $o"; fail=1; }

# A filtered run is PARTIAL and must say so, like --shard and --ci do.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 5 --only=test-pass-qemu.sh 2>&1)
grep -q 'PARTIAL' <<<"$o"     || { echo "  FAIL: an --only run did not announce itself as PARTIAL: $o"; fail=1; }

# ...and NO run-integration.sh run writes the release stamp any more: only
# scripts/release-suite.sh does (AXL-CI-Release-Speed-Design.md §14.4 R-A).
# What a complete, green, uncached run does instead is SAY that it does not
# certify a release -- and a filtered run, which never could, must not say
# anything that reads like a complete run.
# THE CONTROL COMES FIRST, and it has to be a stub set that can actually go
# green: $stub holds a failing test and a hanging one, so the message could
# never print on it and "the --only run did not print it" would pass for a
# reason that has nothing to do with --only.
stampdir=$(mktemp -d)
gstub=$(mktemp -d)
printf '#!/bin/bash\nexit 0\n' > "$gstub/test-green-a-qemu.sh"
printf '#!/bin/bash\nexit 0\n' > "$gstub/test-green-b-qemu.sh"
chmod +x "$gstub"/*.sh
o=$(AXL_STAMP_FILE="$stampdir/control" RUN_INTEGRATION_DIR="$gstub" ./run-integration.sh -j1 --timeout 5 --no-cache 2>&1)
grep -qxF 'integration: this run does not certify a release -- scripts/release-suite.sh does' <<<"$o" \
    || { echo "  FAIL: control -- a complete green uncached run did not say it certifies nothing, so the next check proves nothing"; fail=1; }
[[ ! -f "$stampdir/control" ]] || { echo "  FAIL: a complete run-integration.sh run wrote the release stamp -- only release-suite.sh may"; fail=1; }
o=$(AXL_STAMP_FILE="$stampdir/only" RUN_INTEGRATION_DIR="$gstub" ./run-integration.sh -j1 --timeout 5 --no-cache --only=test-green-a-qemu.sh 2>&1)
[[ ! -f "$stampdir/only" ]] || { echo "  FAIL: an --only run wrote the release stamp"; fail=1; }
! grep -qF 'does not certify a release' <<<"$o" \
    || { echo "  FAIL: an --only run spoke as a complete run"; fail=1; }
rm -rf "$stampdir" "$gstub"

# --- --ci applies CI's per-test limit, and refuses what it cannot hold ----
#
# CI's limit is the x64-ci test preset's AXL_IT_TIMEOUT: CI runs that preset,
# gate.check-test-est grades against it, and --ci applies it -- read back
# from the preset, not copied (its copy, CI_TIMEOUT, went with the engine).
# Read back here too, never written down.
echo "-- --ci limit --"
lstub=$(mktemp -d); llogs=$(mktemp -d)
printf '#!/bin/bash\n# test-meta: est=1\necho "LIMIT=${AXL_TEST_TIMEOUT:-unset}"\nexit 0\n' \
    > "$lstub/test-limitprobe-qemu.sh"
chmod +x "$lstub"/*.sh
cilim=$(jq -r '.testPresets[] | select(.name == "x64-ci") | .environment.AXL_IT_TIMEOUT' ../../CMakePresets.json)
[[ "$cilim" =~ ^[0-9]+$ ]] \
    || { echo "  FAIL: the x64-ci preset holds no AXL_IT_TIMEOUT to read (got '$cilim')"; fail=1; cilim=0; }
# And the runner no longer keeps a copy of it.
[[ "$(grep -c '^CI_TIMEOUT=' ./run-integration.sh)" == 0 ]] \
    || { echo "  FAIL: run-integration.sh still keeps its own CI_TIMEOUT copy"; fail=1; }
# The limit each test is RUN under, as the test sees it (AXL_TEST_TIMEOUT).
limit_seen() {   # $1 = a tag for the log dir, then runner args
    local d="$llogs/$1"; shift
    RUN_INTEGRATION_DIR="$lstub" ./run-integration.sh -j1 --logdir "$d" "$@" >/dev/null 2>&1
    grep -h -oE 'LIMIT=[^ ]*' "$d/test-limitprobe-qemu.sh.log" 2>/dev/null | tail -1
}
[[ "$(limit_seen ci --ci)" == "LIMIT=$cilim" ]] \
    || { echo "  FAIL: a --ci run did not apply the preset's ${cilim}s limit"; fail=1; }
# An explicit --timeout wins over --ci's default, in either order.
[[ "$(limit_seen ci-t --ci --timeout 7)" == "LIMIT=7" ]] \
    || { echo "  FAIL: --ci --timeout 7 did not run the test under 7 s"; fail=1; }
[[ "$(limit_seen t-ci --timeout 7 --ci)" == "LIMIT=7" ]] \
    || { echo "  FAIL: --timeout 7 --ci did not run the test under 7 s"; fail=1; }
# ...and --ci is what applies it: a plain run must not see the same limit.
# The plain run must also have RUN the probe: an empty reading is not a
# different limit, it is no reading at all.
lloc=$(limit_seen local)
[[ "$lloc" =~ ^LIMIT=[0-9]+$ ]] \
    || { echo "  FAIL: a plain run did not run the probe under any limit (read '$lloc')"; fail=1; }
[[ "$lloc" != "LIMIT=$cilim" ]] \
    || { echo "  FAIL: --ci and a plain run use the same ${cilim}s limit -- --ci is not applying CI's limit"; fail=1; }
# A --ci RUN under CI's limit refuses to start on an est it cannot hold, by
# name, BEFORE running anything: the probe must not have run.
printf '#!/bin/bash\n# test-meta: est=%s\nexit 0\n' "$(( cilim + 1 ))" > "$lstub/test-over-qemu.sh"
o=$(RUN_INTEGRATION_DIR="$lstub" ./run-integration.sh --ci -j1 --logdir "$llogs/refuse" 2>&1); rc=$?
[[ $rc -eq 2 ]] && grep -qF "run-integration: --ci REFUSES to start" <<<"$o" \
    && grep -qF "test-over-qemu.sh: est=$(( cilim + 1 ))s > ${cilim}s" <<<"$o" \
    || { echo "  FAIL: a --ci run under CI's limit did not refuse an over-limit est by name (rc=$rc): $o"; fail=1; }
[[ ! -e "$llogs/refuse/test-limitprobe-qemu.sh.log" ]] \
    || { echo "  FAIL: a --ci run that refused still ran a test"; fail=1; }
# A hand-picked --timeout is advice, not CI's limit: it warns and runs.
[[ "$(limit_seen ci-own --ci --timeout "$cilim")" == "LIMIT=$cilim" ]] \
    || { echo "  FAIL: --ci with its own --timeout refused instead of warning"; fail=1; }
rm -f "$lstub/test-over-qemu.sh"
# The gate itself moved; the old flag says where, and runs nothing.
o=$(RUN_INTEGRATION_DIR="$lstub" ./run-integration.sh --ci --check-est 2>&1); rc=$?
[[ $rc -eq 2 ]] && grep -qxF "run-integration.sh: --check-est moved -- run scripts/check-test-est.py (gate.check-test-est), which grades ctest --preset x64-ci." <<<"$o" \
    || { echo "  FAIL: --check-est was not refused with a pointer to scripts/check-test-est.py (rc=$rc): $o"; fail=1; }
rm -rf "$lstub" "$llogs"
[[ $fail -eq 0 ]] && echo "  PASS: --ci applies the preset's limit and refuses what it cannot hold; --check-est points at scripts/check-test-est.py"

# --- a KILLED test says so, and still runs its cleanup ------------------------
#
# common-test.sh's INT/TERM handler must do two things. It must report the kill
# instead of letting the script resume into whatever its interrupted wait
# concludes: CI once read "the event loop never reached its deadline -- it is
# wedged" for a test the runner had killed. And it must still run the test's
# EXIT trap. Its first version re-raised the signal and trusted bash to run
# the EXIT trap on the way out -- which bash does NOT do for a signal trapped
# before any EXIT trap existed: the kill skipped every cleanup, leaking work
# dirs and orphaning background children. So the probe below holds a
# background child and a cleanup that reaps it, exactly as a test holding
# QEMU does, and both are asserted -- the message and the cleanup.
echo "-- a killed test --"
kstub=$(mktemp -d); kout=$(mktemp -d)
cat > "$kstub/test-killprobe-qemu.sh" <<KPROBE
#!/bin/bash
source "$PWD/common-test.sh"
sleep 300 &
child=\$!
echo "\$child" > "$kout/child.\$\$"
trap 'kill '"\$child"' 2>/dev/null; echo "\$\$" >> "$kout/cleanup"' EXIT
echo ready > "$kout/ready.\$\$"
# Short sleeps: a signal sent to this shell alone is handled only once the
# command in its foreground returns.
for _ in \$(seq 1 300); do sleep 0.2; done
KPROBE
chmod +x "$kstub"/*.sh
# 1. Killed by the runner at its limit. The runner retries a TIMEOUT once, so
#    two attempts, and each must run its cleanup exactly once.
RUN_INTEGRATION_DIR="$kstub" ./run-integration.sh -j1 --timeout 2 --no-cache \
    --logdir "$kout/logs" >/dev/null 2>&1
krx='^KILLED: test-killprobe-qemu\.sh was killed by the runner at its 2s time limit \(SIGTERM, handled [0-9]+s in\)\.$'
for kl in test-killprobe-qemu.sh.log test-killprobe-qemu.sh.attempt1.log; do
    grep -qE "$krx" "$kout/logs/$kl" 2>/dev/null \
        || { echo "  FAIL: $kl has no 'KILLED: ... killed by the runner at its 2s time limit' line"; fail=1; }
done
kn=$(cat "$kout/cleanup" 2>/dev/null | wc -l)
kd=$(sort -u "$kout/cleanup" 2>/dev/null | wc -l)
[[ "$kn" -eq 2 && "$kd" -eq 2 ]] \
    || { echo "  FAIL: a runner kill did not run each attempt's EXIT trap exactly once ($kn run(s), $kd attempt(s))"; fail=1; }
# 2. A TERM from outside the runner -- a hand `kill` of a test. The shell alone
#    is signalled, so nothing but the EXIT trap can reap its child.
rm -f "$kout"/cleanup "$kout"/ready.* "$kout"/child.*
env -u AXL_TEST_TIMEOUT bash "$kstub/test-killprobe-qemu.sh" >"$kout/direct.log" 2>&1 &
kpid=$!
for _ in $(seq 1 100); do [[ -e "$kout/ready.$kpid" ]] && break; sleep 0.1; done
kill -TERM "$kpid" 2>/dev/null
wait "$kpid"; krc=$?
kchild=$(cat "$kout/child.$kpid" 2>/dev/null)
[[ -e "$kout/ready.$kpid" ]] \
    || { echo "  FAIL: the direct-TERM probe never got ready, so the checks below prove nothing"; fail=1; }
[[ "$krc" -eq 143 ]] \
    || { echo "  FAIL: a direct TERM did not end the test by SIGTERM (status $krc, want 143)"; fail=1; }
[[ "$(cat "$kout/cleanup" 2>/dev/null)" == "$kpid" ]] \
    || { echo "  FAIL: a direct TERM did not run the test's EXIT trap exactly once"; fail=1; }
grep -qE '^KILLED: test-killprobe-qemu\.sh received SIGTERM [0-9]+s in -- stopped from outside \(no runner time limit is known to this run\)\.$' "$kout/direct.log" \
    || { echo "  FAIL: a direct TERM did not print the no-known-limit KILLED line"; fail=1; }
if [[ -n "$kchild" ]] && kill -0 "$kchild" 2>/dev/null; then
    echo "  FAIL: a direct TERM orphaned the test's background child ($kchild)"; fail=1
fi
# Never leave a stray behind, pass or fail.
for kc in "$kout"/child.*; do [[ -e "$kc" ]] && kill "$(cat "$kc")" 2>/dev/null; done
rm -rf "$kstub" "$kout"
[[ $fail -eq 0 ]] && echo "  PASS: a killed test prints KILLED with its limit, runs its EXIT trap once, reaps its child"

# --- the stall detector must not accuse a binary the HARNESS cut short -------
#
# THE DEFECT THIS PINS. test_count_results decided "stalled" purely from log
# shape: a `=== X Tests ===` header with no `=== Results:` footer. test-axl.sh
# runs every unit binary in ONE QEMU under ONE `timeout`, so when the
# harness's own limit fires the log ends mid-binary and whichever binary was
# in flight was named as having hung, with a confident causal diagnosis
# ("likely an infinite loop or a firmware call that wedged"). It accused
# AxlTestSsh four times under box load, against a binary that runs in 1.1 s
# of a 120 s limit.
#
# The harness HAD the distinguishing fact and threw it away: line 702 was
# `wait "$_wrapper" 2>/dev/null || true`, and `timeout` exits 124 when it
# kills. Both messages are pinned here, exactly, because a detector whose
# wording drifts back is the same defect again.
#
# No QEMU: TEST_QEMU_CMD is an array the harness execs, so a `bash -c` that
# prints the two log shapes drives test_run_foreground and test_count_results
# for real, with the real status capture in between.
echo "-- a truncated run vs a real stall --"
sdir=$(mktemp -d)
cat > "$sdir/probe.sh" <<PROBE
#!/bin/bash
source "$PWD/common-test.sh"
TEST_ARCH=X64
export TEST_SKIP_LEAK_GATE=1
_BODY='printf "=== Alpha Tests ===\nPASS: alpha one\n=== Results: 1 passed ===\n=== Beta Tests ===\nPASS: beta one\n"'
test_setup
case "\$1" in
    truncated) TEST_QEMU_CMD=(bash -c "\$_BODY; sleep 30"); _lim=2 ;;
    stalled)   TEST_QEMU_CMD=(bash -c "\$_BODY");           _lim=20 ;;
    signalled) TEST_QEMU_CMD=(bash -c "\$_BODY"'; kill -9 \$\$'); _lim=20 ;;
esac
export TEST_SKIP_RATCHET="\${2:-1}"
export TEST_BASELINE_FILE="\${3:-}"
export TEST_MAX_DURATION=99999
test_run_foreground "\$_lim"
test_count_results
PROBE
chmod +x "$sdir/probe.sh"

tout=$(bash "$sdir/probe.sh" truncated 2>&1)
grep -qF '*** TRUNCATED: the harness'"'"'s own 2s timeout fired, so the log stops' <<<"$tout" \
    || { echo "  FAIL: a harness timeout did not report itself as a truncation: $tout"; fail=1; }
grep -qF 'did NOT necessarily hang' <<<"$tout" \
    || { echo "  FAIL: the truncation message did not clear the in-flight binary: $tout"; fail=1; }
grep -qE '^      - Beta$' <<<"$tout" \
    || { echo "  FAIL: the truncation message did not name the in-flight binary: $tout"; fail=1; }
grep -qF '*** STALLED' <<<"$tout" \
    && { echo "  FAIL: a harness timeout still reported STALLED: $tout"; fail=1; }

sout=$(bash "$sdir/probe.sh" stalled 2>&1)
grep -qF '*** STALLED: the following test binary started but never' <<<"$sout" \
    || { echo "  FAIL: a real stall stopped reporting STALLED: $sout"; fail=1; }
grep -qF 'the guest then ended on its own' <<<"$sout" \
    || { echo "  FAIL: a real stall lost its diagnosis: $sout"; fail=1; }
grep -qF 'faulted or reset rather' <<<"$sout" \
    || { echo "  FAIL: the stall message does not name what it CAN have been: $sout"; fail=1; }
# ...and it must NOT say "infinite loop": that branch is reached only when
# the guest ended on its own, which is exactly what a loop cannot do.
grep -qF 'infinite loop' <<<"$sout" \
    && { echo "  FAIL: the stall message still blames an infinite loop: $sout"; fail=1; }
grep -qE '^      - Beta$' <<<"$sout" \
    || { echo "  FAIL: the stall message did not name the binary: $sout"; fail=1; }
grep -qF 'TRUNCATED' <<<"$sout" \
    && { echo "  FAIL: a run that was NOT cut short reported a truncation: $sout"; fail=1; }
# THE CONTROL: the two runs differ ONLY in whether the harness's timeout
# fired. Both logs carry the same "Beta with no Results" shape, so if the
# verdict did not move, the branch is reading something else.
grep -qE '^  Alpha +[0-9]+\.[0-9]+s$' <<<"$sout" \
    || { echo "  FAIL: control -- the stalled probe did not produce the normal per-binary timing: $sout"; fail=1; }
# A SIGNAL IS NOT THE HARNESS TIMEOUT, and saying so would be the same
# defect mirrored. The harness passes NO -k (checked), so its own kill is
# always 124 and 128+N means something else ended the guest -- on a loaded
# box, the OOM killer. Claiming "the harness's own Ns timeout fired" for
# that is a confident wrong cause, so it gets its own branch.
#
# (The `-k` folklore on both sides of this was wrong twice. Measured:
# `timeout -k 1 1 sleep 10` -> 124 because SIGTERM sufficed, but
# `timeout -k 1 1 bash -c '"'"'trap "" TERM; sleep 10'"'"'` -> 137. Neither "137
# under -k" nor "124 whether or not -k" is true in general; both were
# committed before the measurement discriminated.)
kout=$(bash "$sdir/probe.sh" signalled 2>&1)
grep -qF '*** CUT SHORT: the guest was killed by signal 9' <<<"$kout" \
    || { echo "  FAIL: an external kill did not report itself as a signal: $kout"; fail=1; }
grep -qF 'the harness limit had NOT been reached' <<<"$kout" \
    || { echo "  FAIL: the signal message did not clear the harness limit: $kout"; fail=1; }
grep -qF 'TRUNCATED' <<<"$kout" \
    && { echo "  FAIL: an external kill was blamed on the harness timeout: $kout"; fail=1; }
grep -qF '*** STALLED' <<<"$kout" \
    && { echo "  FAIL: an external kill was reported as a hang: $kout"; fail=1; }
# The per-binary timing line must stay NEUTRAL in every one of the three:
# it used to say STALLED, which is the diagnosis the verdict block owns.
for _v in "$tout" "$sout" "$kout"; do
    grep -qE '^  Beta +INCOMPLETE \(no Results line seen\)$' <<<"$_v" \
        || { echo "  FAIL: the per-binary line is not neutral: $(grep -E '^  Beta' <<<"$_v")"; fail=1; }
done
# AND THE RATCHET LINE THAT FOLLOWS IT. A truncated run reports a short
# count; the line naming the cause had no assertion at all until now, so
# nothing had shown it could fire.
rdir=$(mktemp -d); echo 9999 > "$rdir/base"
rout=$(bash "$sdir/probe.sh" truncated 0 "$rdir/base" 2>&1)
grep -qF 'expected at least 9999 tests but only' <<<"$rout" \
    || { echo "  FAIL: control -- the truncated run did not trip the ratchet: $rout"; fail=1; }
grep -qE '^      Cut short by the harness.s own 2s timeout while running: Beta$' <<<"$rout" \
    || { echo "  FAIL: the ratchet did not name the truncation as the cause: $rout"; fail=1; }
grep -qF 'Culprit (no Results footer)' <<<"$rout" \
    && { echo "  FAIL: the ratchet still blamed the in-flight binary: $rout"; fail=1; }
rm -rf "$rdir" "$sdir"
[[ $fail -eq 0 ]] && echo "  PASS: a harness timeout reads as TRUNCATED, an external kill as a signal, a run that ended on its own still reads as STALLED"

# --- a breached wall-clock budget must not swallow the ratchet ---------------
#
# The budget check `return 1`d BEFORE the ratchet, so a slow run neither
# checked the count nor recorded it. Four load-failed verify.sh runs left the
# committed baseline four assertions short, and a genuine count regression on
# a slow box would have been reported as "took too long" and nothing else.
# The leak verdict beside it already folds into the final exit instead of
# returning early; this is the same fix applied to its sibling.
echo "-- a breached budget still runs the ratchet --"
bdir=$(mktemp -d)
cat > "$bdir/probe.sh" <<BPROBE
#!/bin/bash
source "$PWD/common-test.sh"
TEST_ARCH=X64
export TEST_SKIP_LEAK_GATE=1
export TEST_BASELINE_FILE="\$1"
export TEST_MAX_DURATION="\$2"
test_setup
TEST_QEMU_CMD=(bash -c 'printf "=== Alpha Tests ===\nPASS: a\nPASS: b\nPASS: c\n=== Results: 3 passed ===\n"')
test_run_foreground 20
test_count_results
BPROBE
chmod +x "$bdir/probe.sh"
# CONTROL FIRST: within budget, the baseline is written.
echo 1 > "$bdir/base"
bok=$(bash "$bdir/probe.sh" "$bdir/base" 600 2>&1); bokrc=$?
[[ $bokrc -eq 0 && "$(cat "$bdir/base")" == "3" ]] \
    || { echo "  FAIL: control -- an in-budget run did not record its count (rc=$bokrc, baseline $(cat "$bdir/base")): $bok"; fail=1; }
# Over budget: the run FAILS, and the count is still recorded.
echo 1 > "$bdir/base"
bbad=$(bash "$bdir/probe.sh" "$bdir/base" 0 2>&1); bbadrc=$?
[[ $bbadrc -ne 0 ]] && grep -q 'over the 0s budget' <<<"$bbad" \
    || { echo "  FAIL: a breached budget did not fail the run (rc=$bbadrc): $bbad"; fail=1; }
[[ "$(cat "$bdir/base")" == "3" ]] \
    || { echo "  FAIL: a breached budget swallowed the ratchet baseline write (baseline $(cat "$bdir/base"))"; fail=1; }
# ...and a count DROP under a breached budget is still reported as a drop.
echo 99 > "$bdir/base"
bdrop=$(bash "$bdir/probe.sh" "$bdir/base" 0 2>&1); bdroprc=$?
[[ $bdroprc -ne 0 ]] && grep -q 'expected at least 99 tests but only 3 ran' <<<"$bdrop" \
    || { echo "  FAIL: a count drop under a breached budget was not reported (rc=$bdroprc): $bdrop"; fail=1; }
[[ "$(cat "$bdir/base")" == "99" ]] \
    || { echo "  FAIL: a run that DROPPED its count still lowered the baseline"; fail=1; }
rm -rf "$bdir"
[[ $fail -eq 0 ]] && echo "  PASS: the ratchet is checked and recorded even when the wall-clock budget is breached"

# SIGTERM only, and that is not a gap. SIGTERM is the delivery that actually
# produced the orphans (a `timeout` wrapper expiring, which signals just the
# runner), and run-integration.sh routes both signals through one _signal_exit
# body, so this covers the SIGINT path's logic too.
#
# SIGINT genuinely cannot be driven from here: bash starts a background job of
# a non-interactive shell with SIGINT ignored (/proc/PID/status SigIgn bit 2),
# and a signal ignored on entry cannot be trapped -- so `kill -INT` at the
# runner is swallowed and the assertion would be measuring the harness, not
# the runner. A terminal Ctrl-C reaches the whole foreground process group
# instead, where the trap does fire.
sig_check TERM
# A dropped terminal (SSH) is a hangup: ctest runs in its own session and
# would otherwise run on, guests and all, holding the tree's owner record.
sig_check HUP
# No HUP/PIPE cases: bash runs the EXIT trap for untrapped fatal signals too,
# so they exercise the same body as TERM and cannot fail independently of it.
# Verified by sabotage -- deleting per-signal HUP/PIPE handlers changed nothing.

# --- --pool: guest= tests on a pooled guest ----------------------------------
#
# A stub `axlpool` (in a fake project root's test/pool) stands in for the
# service: FAKE_POOL picks how it behaves. The stub test reports whether it
# ran pooled (AXL_POOL_AGENT set) or booted, and exits FAKE_TEST_RC.
pproj=$(mktemp -d); ptests=$(mktemp -d); pcache=$(mktemp -d)
trap 'rm -rf "$stub" "$pproj" "$ptests" "$pcache"' EXIT
mkdir -p "$pproj/test/pool/axlpool"
: > "$pproj/test/pool/axlpool/__init__.py"
mkdir -p "$pproj/scripts"; printf 'NON_ARTIFACT_GOALS = none-here\n' > "$pproj/scripts/axl-lists.conf"
cat > "$pproj/test/pool/axlpool/__main__.py" <<'PYEOF'
"""Stub axlpool for test-runner-selftest.sh: FAKE_POOL = ok | down | noarch | miss."""
import os
import subprocess
import sys

mode = os.environ.get("FAKE_POOL", "ok")
if sys.argv[1] == "status":
    if mode == "down":
        print("axlpool: no pool service on stub.sock: refused", file=sys.stderr)
        sys.exit(1)
    print(f"axlpool: service stub on stub.sock, agents: {'AARCH64' if mode == 'noarch' else 'AARCH64 X64'}")
    sys.exit(0)
if sys.argv[1] == "code":
    # The code identity a pooled cache key holds (no identity, no key).
    print("c0de")
    sys.exit(0)
if sys.argv[1] == "run":
    if mode == "miss":
        print("axlpool run: no guest: miss (no-slot): stub", file=sys.stderr)
        print("AXLPOOL-MISS: no guest: miss (no-slot): stub", file=sys.stderr)
        sys.exit(77)
    child = sys.argv[sys.argv.index("--") + 1:]
    sys.exit(subprocess.call(child, env=dict(os.environ, AXL_POOL_AGENT="127.0.0.1:1")))
sys.exit(2)
PYEOF
cat > "$ptests/test-pooled-qemu.sh" <<SHEOF
#!/bin/bash
# test-meta: arch=x64 est=1 guest=standard
# A real test records what it ran on; one that records nothing is never cached.
source "$(pwd)/lib/test-cache.sh"
cache_record_input "\$(basename "\$0")" "\$0"
SHEOF
cat >> "$ptests/test-pooled-qemu.sh" <<'SHEOF'
[[ -n "${AXL_POOL_AGENT:-}" ]] && echo "MODE=pooled" || echo "MODE=booted"
[[ "${FAKE_TEST_RC:-0}" == 77 ]] && echo "SKIP: the test's own skip"
# A pool failure the pooled test's HARNESS met (common-test.sh _test_pool_miss).
[[ "${FAKE_TEST_RC:-0}" == harness-miss && -n "${AXL_POOL_AGENT:-}" ]] \
    && { echo "AXLPOOL-MISS: test-pooled-qemu.sh: the agent could not run it" >&2; exit 77; }
[[ "${FAKE_TEST_RC:-0}" == harness-miss ]] && exit 0
exit "${FAKE_TEST_RC:-0}"
SHEOF
chmod +x "$ptests"/*.sh
prun() {   # FAKE_POOL=... prun ARGS -> runner output + rc=N; the test's own log as LOG:
    local o r
    o=$(RUN_PROJECT_DIR="$pproj" RUN_INTEGRATION_DIR="$ptests" RUN_TEST_CACHE_DIR="$pcache" \
        ./run-integration.sh -j1 --timeout 20 --no-build --logdir "$pcache/logs" "$@" 2>&1); r=$?
    printf '%s\nrc=%s\nLOG:%s\n' "$o" "$r" "$(grep -m1 '^MODE=' "$pcache/logs/test-pooled-qemu.sh.log" 2>/dev/null)"
}
po=$(FAKE_POOL=down prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'run-integration: --pool, but axlpool: no pool service' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po")" == "1 rc=2" ]] \
    && echo "  PASS: --pool with no service answering refuses to start (2)" \
    || { echo "  FAIL: --pool with no service: $po"; fail=1; }
po=$(FAKE_POOL=noarch prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'the service has no agent for X64' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po")" == "1 rc=2" ]] \
    && echo "  PASS: --pool with no agent for the run's arch refuses to start (2)" \
    || { echo "  FAIL: --pool with no agent for the arch: $po"; fail=1; }
po=$(FAKE_POOL=ok prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'test-pooled-qemu.sh PASS' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 LOG:MODE=pooled" ]] \
    && echo "  PASS: --pool runs a guest= test pooled" \
    || { echo "  FAIL: --pool did not run the guest= test pooled: $po"; fail=1; }
po=$(FAKE_POOL=miss prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'test-pooled-qemu.sh: pool miss (no guest: miss' <<<"$po") $(grep -c 'test-pooled-qemu.sh PASS' <<<"$po") $(grep -o 'LOG:.*' <<<"$po") $(ls "$pcache/logs" | grep -c 'pool-miss.log')" == "1 1 LOG:MODE=booted 1" ]] \
    && echo "  PASS: a pool miss reruns the test booted, says why, keeps the pool's log" \
    || { echo "  FAIL: a pool miss was not rerun booted: $po"; fail=1; }
po=$(FAKE_POOL=ok FAKE_TEST_RC=77 prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'pool miss' <<<"$po") $(grep -c 'test-pooled-qemu.sh SKIP' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "0 1 LOG:MODE=pooled" ]] \
    && echo "  PASS: the test's OWN 77 under the pool is its skip, not a pool miss (no booted rerun)" \
    || { echo "  FAIL: a test's own skip was taken for a pool miss: $po"; fail=1; }
po=$(FAKE_POOL=ok FAKE_TEST_RC=harness-miss prun --pool=stub.sock --no-cache)
[[ "$(grep -c 'test-pooled-qemu.sh: pool miss (test-pooled-qemu.sh: the agent could not run it' <<<"$po") $(grep -c 'test-pooled-qemu.sh PASS' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 1 LOG:MODE=booted" ]] \
    && echo "  PASS: a pool failure INSIDE the test (its harness's AXLPOOL-MISS:) reruns it booted, not a quiet SKIP" \
    || { echo "  FAIL: a pool failure inside the test was not rerun booted: $po"; fail=1; }
# The cache: a green pooled run is no reason to skip the booted run, nor back.
rm -rf "$pcache"/*.key "$pcache"/*.inputs
FAKE_POOL=ok prun --pool=stub.sock > /dev/null
po=$(FAKE_POOL=ok prun --pool=stub.sock)
pb=$(prun)
[[ "$(grep -c 'test-pooled-qemu.sh CACHED' <<<"$po") $(grep -c 'test-pooled-qemu.sh CACHED' <<<"$pb") $(grep -o 'LOG:.*' <<<"$pb")" == "1 0 LOG:MODE=booted" ]] \
    && echo "  PASS: pooled and booted runs are cached apart (a pooled green does not skip a booted run)" \
    || { echo "  FAIL: one cache key across pooled and booted: pooled=[$po] booted=[$pb]"; fail=1; }

# THE DEFAULT IS THE POOL (Mike, 2026-11-06: every QEMU test that can run on
# a pooled guest should). No flag: the user service's socket, if it exists
# and the service serves this arch; otherwise booted -- and either way the
# run SAYS which, and why. --no-pool boots; --ci pools like any run (Mike,
# 2026-11-07: the runners on this host use the pool); --pool still refuses a
# pool it cannot have.
# A LISTENING socket for the stub service to answer behind (the runner asks
# the SERVICE -- lib/pool-check.sh -- not merely whether the socket accepts).
xdg_live="$pcache/xdg-live"; mkdir -p "$xdg_live/axlpool"
python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(8); time.sleep(900)' \
    "$xdg_live/axlpool/pool.sock" & xdg_live_pid=$!
# An interrupted self-test must not leave the listener, nor a borrowed owner record.
bowner="$(cd ../.. && pwd -P)/build/x64-debug/integration-logs-stub/.run-owner"
trap 'kill "$xdg_live_pid" ${bpid:-} 2>/dev/null; if [[ -f "$pcache/owner.bak" ]]; then mv -f "$pcache/owner.bak" "$bowner"; elif [[ -n "${bpid:-}" ]]; then rm -f "$bowner"; fi; rm -rf "$stub" "$pproj" "$ptests" "$pcache"' EXIT
for _i in $(seq 1 50); do [[ -S "$xdg_live/axlpool/pool.sock" ]] && break; sleep 0.05; done
pdef() { XDG_RUNTIME_DIR="$1" prun --no-cache "${@:2}"; }   # pdef XDG ARGS
po=$(pdef "$XDG_RUNTIME_DIR")
[[ "$(grep -c "^run-integration: pool: none (no service at $XDG_RUNTIME_DIR/axlpool/pool.sock) -- guest= tests boot\$" <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 rc=0 LOG:MODE=booted" ]] \
    && echo "  PASS: default, no service socket: booted, and said" \
    || { echo "  FAIL: default with no service socket: $po"; fail=1; }
po=$(FAKE_POOL=ok pdef "$xdg_live")
[[ "$(grep -c '^run-integration: pool (default): axlpool: service stub on stub.sock, agents: AARCH64 X64$' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 LOG:MODE=pooled" ]] \
    && echo "  PASS: default, a serving service: the guest= test runs pooled, and said" \
    || { echo "  FAIL: default with a serving service: $po"; fail=1; }
po=$(FAKE_POOL=down pdef "$xdg_live")
[[ "$(grep -c '^run-integration: pool: unavailable (axlpool: no pool service on stub.sock: refused) -- guest= tests boot$' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 rc=0 LOG:MODE=booted" ]] \
    && echo "  PASS: default, a service that does not answer: booted (not refused), and said why" \
    || { echo "  FAIL: default with a dead service: $po"; fail=1; }
po=$(FAKE_POOL=ok pdef "$xdg_live" --no-pool)
[[ "$(grep -c '^run-integration: pool: off (--no-pool) -- guest= tests boot$' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 LOG:MODE=booted" ]] \
    && echo "  PASS: --no-pool boots beside a serving service" \
    || { echo "  FAIL: --no-pool: $po"; fail=1; }
po=$(FAKE_POOL=ok pdef "$xdg_live" --ci)
[[ "$(grep -c '^run-integration: pool (default): axlpool: service stub on stub.sock, agents: AARCH64 X64$' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 LOG:MODE=pooled" ]] \
    && echo "  PASS: --ci pools beside a serving service, as CI on this host does" \
    || { echo "  FAIL: --ci: $po"; fail=1; }
po=$(FAKE_POOL=noarch pdef "$xdg_live")
[[ "$(grep -c '^run-integration: pool: unavailable (the service has no agent for X64 (axlpool: service stub on stub.sock, agents: AARCH64)) -- guest= tests boot$' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po") $(grep -o 'LOG:.*' <<<"$po")" == "1 rc=0 LOG:MODE=booted" ]] \
    && echo "  PASS: default, a service with no agent for the arch: booted, and said why" \
    || { echo "  FAIL: default with no agent for the arch: $po"; fail=1; }
# A run that only DEFAULTED to the pool hands ctest AXL_IT_POOL=auto, not a
# socket: the registration re-decides at run time, and its pool check stays in
# AUTO mode -- a service that dies mid-run lets the tests boot instead of
# failing every one. --print-ctest prints exactly that.
pc=$(XDG_RUNTIME_DIR="$xdg_live" RUN_PROJECT_DIR="$pproj" RUN_INTEGRATION_DIR="$ptests" ./run-integration.sh --print-ctest 2>&1)
[[ "$(grep -cx 'ctest-env: AXL_IT_POOL=auto' <<<"$pc")" == 1 ]] \
    && echo "  PASS: a defaulted run hands ctest AXL_IT_POOL=auto (--print-ctest)" \
    || { echo "  FAIL: --print-ctest of a defaulted run: $(grep 'AXL_IT_POOL' <<<"$pc")"; fail=1; }
pc=$(XDG_RUNTIME_DIR="$xdg_live" RUN_PROJECT_DIR="$pproj" RUN_INTEGRATION_DIR="$ptests" ./run-integration.sh --print-ctest --pool=/x/asked.sock 2>&1)
[[ "$(grep -cx 'ctest-env: AXL_IT_POOL=/x/asked.sock' <<<"$pc")" == 1 ]] \
    && echo "  PASS: an ASKED run hands ctest its socket (refuse mode)" \
    || { echo "  FAIL: --print-ctest of --pool: $(grep 'AXL_IT_POOL' <<<"$pc")"; fail=1; }
pc=$(XDG_RUNTIME_DIR="$xdg_live" RUN_PROJECT_DIR="$pproj" RUN_INTEGRATION_DIR="$ptests" ./run-integration.sh --print-ctest --ci --pool=/x/asked.sock 2>&1)
[[ "$(grep -cx 'ctest-env: AXL_IT_POOL=/x/asked.sock' <<<"$pc") $(grep -c 'contradict' <<<"$pc")" == "1 0" ]] \
    && echo "  PASS: --ci with --pool is an ASKED pooled CI run (its socket handed to ctest)" \
    || { echo "  FAIL: --ci with --pool: $pc"; fail=1; }
po=$(prun --no-pool --pool=stub.sock --no-cache)
[[ "$(grep -c 'run-integration: --pool and --no-pool' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po")" == "1 rc=2" ]] \
    && echo "  PASS: --pool with --no-pool is refused" \
    || { echo "  FAIL: --pool with --no-pool: $po"; fail=1; }
# A refusal comes out ALONE: the pool is asked nothing, and said nothing,
# before a run refuses. Another live run owns the tree -> refused, no line.
sleep 300 & bpid=$!
mkdir -p "$(dirname "$bowner")"; [[ -f "$bowner" ]] && cp "$bowner" "$pcache/owner.bak"
echo "$bpid" > "$bowner"
po=$(FAKE_POOL=ok pdef "$xdg_live")
kill "$bpid" 2>/dev/null; wait "$bpid" 2>/dev/null
if [[ -f "$pcache/owner.bak" ]]; then mv -f "$pcache/owner.bak" "$bowner"; else rm -f "$bowner"; fi
bpid=""
[[ "$(grep -c 'another ctest run' <<<"$po") $(grep -c '^run-integration: pool' <<<"$po") $(grep -o 'rc=[0-9]*' <<<"$po")" == "1 0 rc=2" ]] \
    && echo "  PASS: a refused run (tree busy) asks and says nothing about the pool" \
    || { echo "  FAIL: a refusal printed after a pool line: $po"; fail=1; }
kill "$xdg_live_pid" 2>/dev/null; wait "$xdg_live_pid" 2>/dev/null

# --no-build REACHES THE PREPARATION. It is the runner's flag but the build
# step is lib/suite-prep.sh, a child process: an unexported RUN_NO_BUILD left
# release-suite.sh's `--no-build` run rebuilding and restaging the tree its
# caller had just built, AND listing the shared targets in the manifest as
# pre-built -- so axl-build.sh would skip a test's build on the strength of a
# build that was never made. The stub mode above cannot see it (stubs skip the
# build anyway), so this is a REAL-tree run of one host-only test.
nb_logs=$(mktemp -d)
nb=$(env -u RUN_INTEGRATION_DIR -u RUN_PROJECT_DIR -u RUN_TEST_CACHE_DIR -u RUN_NO_BUILD ./run-integration.sh --no-build --no-cache --logdir "$nb_logs" --only=test-axl-build-backend 2>&1)
nb_rc=$?
# The prep is a CTest fixture now: what it says is in its JUnit output.
[[ $nb_rc -eq 0 && -s "$nb_logs/ctest-junit.xml" && "$(grep -c 'pre-building once' "$nb_logs/ctest-junit.xml")" == 0 \
   && "$(grep -c '^x64||' "$nb_logs/_prebuilt" 2>/dev/null)" == 0 ]] \
    && echo "  PASS: --no-build skips the shared build and lists nothing as pre-built" \
    || { echo "  FAIL: --no-build still built or listed the shared targets (rc=$nb_rc): $(head -3 <<<"$nb") / manifest: $(cat "$nb_logs/_prebuilt" 2>/dev/null)"; fail=1; }
# Control: the same run WITHOUT --no-build does build and list them, so the
# silence above is the flag's doing.
nb2_logs=$(mktemp -d)
nb2=$(env -u RUN_INTEGRATION_DIR -u RUN_PROJECT_DIR -u RUN_TEST_CACHE_DIR -u RUN_NO_BUILD ./run-integration.sh --no-cache --logdir "$nb2_logs" --only=test-axl-build-backend 2>&1)
[[ "$(grep -c 'pre-building once' "$nb2_logs/ctest-junit.xml" 2>/dev/null)" == 1 && "$(grep -c '^x64||' "$nb2_logs/_prebuilt" 2>/dev/null)" == 1 ]] \
    && echo "  PASS: control: without --no-build the shared build runs and is listed" \
    || { echo "  FAIL: control: the shared build did not run/list without --no-build: $(head -3 <<<"$nb2")"; fail=1; }
rm -rf "${nb_logs:?}" "${nb2_logs:?}"

# --- THE TRANSLATION: what each flag becomes, in ctest's argv and environment
#
# The runner is a front end over CTest (slice 5a step 6). --print-ctest prints
# the ONE argv and environment the run would use, and runs nothing, so each
# flag's translation is pinned exactly -- and then CTest itself is asked, with
# that same argv, what it would run: a translation that reads right but
# selects the wrong thing fails there.
echo "-- the ctest translation --"
before=$fail
tstub=$(mktemp -d); tlogs=$(mktemp -d)
printf '#!/bin/bash\n# test-meta: arch=both est=5\nexit 0\n' > "$tstub/test-tra-qemu.sh"
printf '#!/bin/bash\n# test-meta: arch=both est=1\nexit 0\n' > "$tstub/test-trb-qemu.sh"
chmod +x "$tstub"/*.sh
TREE_X64="$(cd ../.. && pwd)/build/x64-debug"
pc() { RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh --print-ctest "$@" 2>&1; }
ncpu=$(nproc)
o=$(pc -j3 --no-cache --logdir "$tlogs")
[[ "$o" == "ctest-env: AXL_IT_LOGDIR=$tlogs
ctest-env: AXL_IT_TIMEOUT=900
ctest-env: AXL_IT_CACHE=0
ctest-env: AXL_IT_DIR=$tstub
ctest-prep-tests: $tstub/test-tra-qemu.sh
ctest-prep-tests: $tstub/test-trb-qemu.sh
ctest-test: integration.test-tra-qemu
ctest-test: integration.test-trb-qemu
ctest: ctest --test-dir $TREE_X64 --tests-from-file $tlogs/_selected.txt -j 3 --no-tests=error --output-junit $tlogs/ctest-junit.xml --test-output-size-passed 65536 --test-output-size-failed 65536" ]] \
    || { echo "  FAIL: -j3 --no-cache --logdir did not translate exactly:"; sed 's/^/      |/' <<<"$o"; fail=1; }
o=$(pc --timeout 7 --no-build)
grep -qx 'ctest-env: AXL_IT_TIMEOUT=7' <<<"$o" && grep -qx 'ctest-env: RUN_NO_BUILD=1' <<<"$o" \
    && ! grep -q 'AXL_IT_CACHE=' <<<"$o" \
    || { echo "  FAIL: --timeout 7 --no-build (cache on) did not translate: $o"; fail=1; }
# No -j is a CPU budget of nproc+4 that each test draws its cpu= from, and
# NO load-average gate (AXL-CI-Release-Speed-Design.md §15).
o=$(pc --no-cache --logdir "$tlogs")
[[ "$(grep '^ctest: ' <<<"$o")" == "ctest: ctest --test-dir $TREE_X64 --tests-from-file $tlogs/_selected.txt -j $((ncpu + 4)) --no-tests=error --output-junit $tlogs/ctest-junit.xml --test-output-size-passed 65536 --test-output-size-failed 65536" ]] \
    || { echo "  FAIL: no -j did not become -j nproc+4 with no --test-load: $(grep '^ctest: ' <<<"$o")"; fail=1; }
o=$(pc --ci --only=test-trb-qemu)
grep -qx "ctest-env: AXL_IT_TIMEOUT=$cilim" <<<"$o" && grep -qx 'ctest-env: AXL_IT_CACHE=0' <<<"$o" \
    && [[ "$(grep -c '^ctest-test: ' <<<"$o")" == 1 ]] \
    || { echo "  FAIL: --ci --only did not take the preset's limit, the cache off and one test: $o"; fail=1; }
# CTest's own answer for that argv: the selection, plus the fixtures it adds.
o=$(pc --no-cache --logdir "$tlogs")
sed -n 's/^ctest-test: //p' <<<"$o" > "$tlogs/_selected.txt"
mapfile -t _envs < <(sed -n 's/^ctest-env: //p' <<<"$o")
sel=$(env "${_envs[@]}" ctest --test-dir "$TREE_X64" --tests-from-file "$tlogs/_selected.txt" \
          --show-only=json-v1 --output-junit /dev/null 2>/dev/null | jq -r '.tests[].name' | sort | paste -sd' ')
[[ "$sel" == "integration.prep integration.test-tra-qemu integration.test-trb-qemu integration.tmp-cleanup integration.tmp-setup" ]] \
    || { echo "  FAIL: ctest would not run exactly the selection and its fixtures: $sel"; fail=1; }
# What the runner does not set, it does not pass on: a caller's AXL_IT_* /
# AXL_PREP_* / AXL_ONE_* / AXL_TEST_CACHE would otherwise override a flag
# (--no-cache, no --pool) or this run's prep, and a TEST_MAX_DURATION would
# loosen the unit budget. Handed explicitly: CI's release-suite step sets
# TEST_MAX_DURATION itself, and an inherited one changed the exact list.
o=$(AXL_TEST_CACHE=/x AXL_IT_POOL=/y AXL_PREP_SKIP_LOCAL=1 AXL_ONE_POOL_SOCK=/z TEST_MAX_DURATION=99 pc --no-cache)
[[ "$(grep '^ctest-unset: ' <<<"$o" | sort | paste -sd' ')" == "ctest-unset: AXL_IT_POOL ctest-unset: AXL_ONE_POOL_SOCK ctest-unset: AXL_PREP_SKIP_LOCAL ctest-unset: AXL_TEST_CACHE ctest-unset: TEST_MAX_DURATION" ]] \
    || { echo "  FAIL: inherited run settings were not cleared: $o"; fail=1; }
[[ $fail -eq $before ]] && echo "  PASS: each flag's translation is exact, ctest selects exactly that set, and nothing inherited rides along"

echo "-- refusals and the tree --"
before=$fail
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh --ci --arch AARCH64 2>&1); rc=$?
[[ $rc -eq 2 ]] && grep -qF 'run-integration: --ci is CI'"'"'s x64 run' <<<"$o" \
    || { echo "  FAIL: --ci --arch AARCH64 was not refused (rc=$rc): $o"; fail=1; }
# ONE RUN PER TREE, refused by name before ctest starts -- not 200 tests Not
# Run. A live process stands in for the other run's ctest.
owner="$TREE_X64/integration-logs-stub/.run-owner"
sleep 60 & other=$!
printf '%s\n' "$other" > "$owner"
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1); rc=$?
[[ $rc -eq 2 ]] && grep -qF "run-integration: another ctest run (pid $other) is using $TREE_X64" <<<"$o" \
    && ! grep -q '^ctest:' <<<"$o" \
    || { echo "  FAIL: a second run on the tree was not refused before ctest (rc=$rc): $o"; fail=1; }
kill "$other" 2>/dev/null; wait "$other" 2>/dev/null; rm -f "$owner"
# CONTROL: with the other run gone, the same run goes ahead.
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1); rc=$?
[[ $rc -eq 0 ]] && grep -q '^integration: 2 passed, 0 failed (X64)$' <<<"$o" \
    || { echo "  FAIL: control: the run did not go ahead once the other run was gone (rc=$rc): $o"; fail=1; }
# A STALE registration (its template newer than the configured copy, as after
# a pull) is reconfigured, not run as yesterday's.
touch -d '2000-01-01' "$TREE_X64/axl-integration-ctest.cmake"
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1); rc=$?
[[ $rc -eq 0 ]] && grep -qF "run-integration: configuring $TREE_X64" <<<"$o" \
    && [[ "$TREE_X64/axl-integration-ctest.cmake" -nt "../../cmake/AxlIntegrationTests.cmake.in" ]] \
    || { echo "  FAIL: a stale registration was not reconfigured (rc=$rc): $o"; fail=1; }
# ...and a CURRENT one is not: the reconfigure must not become every run's.
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1)
grep -q 'configuring' <<<"$o" && { echo "  FAIL: a current registration was reconfigured: $o"; fail=1; }
# An EMPTY selection runs nothing -- not the whole tree, which is what ctest
# might make of an empty --tests-from-file -- and says 0 passed.
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache --shard 4/5 2>&1); rc=$?
[[ $rc -eq 0 ]] && grep -q '^integration: 0 passed, 0 failed (X64)$' <<<"$o" && ! grep -q '^ctest:' <<<"$o" \
    || { echo "  FAIL: an empty shard was not a clean 0-test run without ctest (rc=$rc): $o"; fail=1; }
# A RELATIVE --logdir is the caller's directory, as it always was.
rel="rel-logdir.$$"
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache --logdir "$rel" 2>&1); rc=$?
[[ $rc -eq 0 && -f "$rel/ctest-junit.xml" ]] && grep -qx "logs: $PWD/$rel" <<<"$o" \
    || { echo "  FAIL: a relative --logdir did not work (rc=$rc): $o"; fail=1; }
rm -rf "${rel:?}"
# AARCH64 SELECTS WHAT ITS TREE REGISTERS: an arch=none (host-only) test runs
# once, in the x64 tree, so an aa64 run neither lists it nor reports it NOT
# RUN -- and --only naming it is refused, by name.
printf '#!/bin/bash\n# test-meta: arch=none est=1\nexit 0\n' > "$tstub/test-trhost.sh"
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh --arch AARCH64 --list 2>&1 | xargs -n1 basename | sort | paste -sd' ')
[[ "$o" == "test-tra-qemu.sh test-trb-qemu.sh" ]] \
    || { echo "  FAIL: an aa64 --list did not leave out the arch=none test: $o"; fail=1; }
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh --arch AARCH64 --only=test-trhost --list 2>&1); rc=$?
[[ $rc -eq 2 ]] && grep -qF 'run-integration: --only names test(s) that discovery did not find' <<<"$o" \
    || { echo "  FAIL: an aa64 --only of an arch=none test was not refused (rc=$rc): $o"; fail=1; }
o=$(RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh --list 2>&1 | grep -c 'test-trhost.sh$')
[[ "$o" == 1 ]] || { echo "  FAIL: control: x64 did not list the arch=none test ($o)"; fail=1; }
rm -f "$tstub/test-trhost.sh"
# THE REAP IS THIS RUN'S ONLY WHEN THE TREE'S RUN WAS: a run that never owned
# it (its setup refused -- here, a caller base that does not exist) must not
# reap guests at the base's path, which belong to whoever does own it. A
# qemu-system stand-in (a copied tail) carries the path in its argv.
qb="/tmp/axl-race.$$"; rstand=$(mktemp -d)
cp "$(command -v tail)" "$rstand/qemu-system-racestub"
"$rstand/qemu-system-racestub" -F "$qb/x" >/dev/null 2>&1 &
guest=$!
o=$(AXL_QEMU_TMPDIR="$qb" RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1); rc=$?
[[ $rc -eq 1 ]] && grep -q '^integration: integration.tmp-setup FAILED' <<<"$o" && kill -0 "$guest" 2>/dev/null \
    || { echo "  FAIL: a run that never owned the tree reaped at its base, or did not say why (rc=$rc): $o"; fail=1; }
# CONTROL: the same path, a run whose setup DOES run -- its guest is reaped.
mkdir -p "$qb"
o=$(AXL_QEMU_TMPDIR="$qb" RUN_INTEGRATION_DIR="$tstub" ./run-integration.sh -j1 --no-cache 2>&1); rc=$?
sleep 0.5
[[ $rc -eq 0 ]] && ! kill -0 "$guest" 2>/dev/null \
    || { echo "  FAIL: control: a run that owned the tree did not reap the guest at its base (rc=$rc): $o"; fail=1; }
kill "$guest" 2>/dev/null; wait "$guest" 2>/dev/null
rmdir "$qb" 2>/dev/null; rm -rf "${rstand:?}"
rm -rf "${tstub:?}" "${tlogs:?}"
[[ $fail -eq $before ]] && echo "  PASS: refusals; a stale registration is reconfigured and a current one not; an empty selection runs nothing; a relative --logdir works"

[[ $fail -eq 0 ]] && echo "runner selftest: OK" || { echo "runner selftest: FAILED"; exit 1; }
