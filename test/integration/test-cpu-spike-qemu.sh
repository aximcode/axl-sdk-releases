#!/bin/bash
# test-meta: arch=x64 needs= est=80 local-only=0
# test-cpu-spike-qemu.sh — the CPU-spike detector actually detects.
#
# run-qemu.sh has had a CPU-spike sampler on by default for a long time, but
# our integration suites build their own TEST_QEMU_CMD in common-test.sh and
# never call run-qemu.sh, so they ran with NO sampling at all: a guest that
# busy-waits after boot burned its whole timeout with nothing flagged. The
# sampler now lives in scripts/axl-common.sh and both siblings use it.
#
# A detector that has never been shown to fire is not a detector, so this
# drives the whole chain end to end against a REAL spinning guest, using the
# shared cpu-spin-fixture.efi (test/integration/cpu-spin-fixture.c):
#
#   compute — POSITIVE control. A genuine post-boot busy-wait, ~1.0 host core.
#             Must raise the WARN and set TEST_CPU_SPIKE=1.
#   idle    — NEGATIVE control. The same wall-clock duration spent in
#             axl_msleep, i.e. what a correctly-waiting app looks like.
#             Must stay silent.
#
# Both controls are the same guest, same harness, same duration — only the
# guest's CPU behaviour differs, so nothing else can explain a divergence.
#
# Thresholds here match common-test.sh's shipped values, and are derived from
# measurement rather than tuned until quiet:
#   idle guest, post-firmware   0.00-0.02 cores
#   firmware phase (KVM X64)    ~1.0 core for ~11.2 s -- OVMF POST + the EDK2
#                               Shell startup countdown + startup.nsh stalls
#   one fully pegged vCPU       ~1.00-1.05 cores
# so the warm-up must clear ~12 s, and 0.5 cores sits far above idle yet below
# a peg (a threshold at/above 1.0 can never fire on a 1-vCPU guest).
set -euo pipefail

source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"

# The spin fixture (test/integration/cpu-spin-fixture.c + its `cpu-spin-fixture`
# make target) is shared with the CPU-burn investigation and is owned by that
# change, not this one. SKIP loudly rather than failing if it is not present:
# a missing fixture means "this proof could not run here", which is a different
# statement from "the detector is broken", and conflating them would make this
# test lie in exactly the way it exists to prevent.
if [[ ! -f "$PROJECT_DIR/test/integration/cpu-spin-fixture.c" ]]; then
    echo "SKIP: cpu-spin-fixture.c not present — the CPU-spike detector proof needs it."
    echo "      (shared fixture; see test/integration/cpu-spin-fixture.c)"
    exit 0
fi

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all cpu-spin-fixture 2>&1 | tail -3

if [[ ! -f "$TEST_BUILD_DIR/cpu-spin-fixture.efi" ]]; then
    echo "SKIP: cpu-spin-fixture.efi did not build — cannot prove the detector fires."
    exit 0
fi

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; [[ -n "${2:-}" ]] && echo "    $2"; FAIL=$((FAIL + 1)); }

CPU_WARMUP=15
CPU_SUSTAIN=2
CPU_THRESHOLD=0.5
SPIN_MS=32000    # must outlast the 15 s warm-up with room to sustain
RUN_SECS=55

# run_phase <mode> — boot the fixture in <mode>, leaving the sampler's verdict
# in TEST_CPU_SPIKE, its stderr in PHASE_WARN, its raw numbers in
# PHASE_SUMMARY.
#
# NOT `$( ... )`: a command substitution is a subshell, so the TEST_CPU_SPIKE
# it sets would never reach us. Capture stderr to a file instead.
run_phase() {
    local mode="$1"
    test_add_efi "$TEST_BUILD_DIR/cpu-spin-fixture.efi"
    test_set_startup <<NSH
@echo -off
echo PHASE-$mode-START
cpu-spin-fixture.efi $mode $SPIN_MS
echo PHASE-$mode-DONE
NSH
    test_build_image
    test_build_qemu_cmd
    test_add_no_network
    TEST_CPU_SPIKE=0
    test_run_foreground "$RUN_SECS" 2>"$TEST_TMPDIR/$mode.err" || true
    PHASE_WARN=$(cat "$TEST_TMPDIR/$mode.err" 2>/dev/null || true)
    PHASE_SUMMARY=$(cat "$TEST_TMPDIR/cpu-summary.txt" 2>/dev/null || echo "0 0 0")
    test_clean_log
}

# ---------------------------------------------------------------------------
# POSITIVE control — a real post-boot busy-wait MUST be flagged
# ---------------------------------------------------------------------------
echo "=== compute (positive control: expect a CPU-spike WARN) ==="
run_phase compute
echo "    sampler: $PHASE_SUMMARY   (peak sustain_max mean)"

if grep -q "CPUSPIN: begin compute" "$TEST_CLEAN_LOG" 2>/dev/null; then
    pass "the spin guest booted and entered the busy-wait"
else
    fail "the spin guest booted and entered the busy-wait" \
         "no 'CPUSPIN: begin compute' — otherwise a boot failure could masquerade as a spike"
fi

if [[ "$TEST_CPU_SPIKE" == "1" ]]; then
    pass "a post-boot busy-wait sets TEST_CPU_SPIKE"
else
    fail "a post-boot busy-wait sets TEST_CPU_SPIKE" "sampler said: $PHASE_SUMMARY"
fi

if [[ "$PHASE_WARN" == *"WARN: CPU spike"* ]]; then
    pass "the spike reaches the caller as a WARN line on stderr"
else
    fail "the spike reaches the caller as a WARN line on stderr" "got: $PHASE_WARN"
fi

# ---------------------------------------------------------------------------
# NEGATIVE control — the same guest, waiting properly, MUST NOT be flagged
# ---------------------------------------------------------------------------
echo "=== idle (negative control: expect silence) ==="
run_phase idle
echo "    sampler: $PHASE_SUMMARY   (peak sustain_max mean)"

if grep -q "CPUSPIN: begin idle" "$TEST_CLEAN_LOG" 2>/dev/null; then
    pass "the idle guest booted and entered the wait"
else
    fail "the idle guest booted and entered the wait" "no 'CPUSPIN: begin idle'"
fi

if [[ "$TEST_CPU_SPIKE" == "0" ]]; then
    pass "an event-driven wait is not flagged (firmware boot alone does not trip it)"
else
    fail "an event-driven wait is not flagged" "sampler said: $PHASE_SUMMARY"
fi

if [[ "$PHASE_WARN" != *"WARN: CPU spike"* ]]; then
    pass "no WARN line for a correctly-waiting guest"
else
    fail "no WARN line for a correctly-waiting guest" "got: $PHASE_WARN"
fi

# The negative control must not pass because there was nothing to measure: a
# guest that died inside the warm-up writes "0.00 0.00 0.000" and would look
# clean for the wrong reason. Require a real, non-degenerate sample.
idle_mean=$(awk '{print $3+0}' <<<"$PHASE_SUMMARY")
idle_peak=$(awk '{print $1+0}' <<<"$PHASE_SUMMARY")
if awk -v m="$idle_mean" -v p="$idle_peak" 'BEGIN{exit !(m > 0 || p > 0)}'; then
    pass "the idle guest was actually sampled after the warm-up (mean ${idle_mean} cores)"
else
    fail "the idle guest was actually sampled after the warm-up" \
         "summary was all-zero — the guest did not outlive the warm-up, so the silence proves nothing"
fi

echo
printf "cpu-spike detector: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
[[ $FAIL -eq 0 ]] || exit 1
