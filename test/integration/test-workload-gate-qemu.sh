#!/bin/bash
# test-meta: arch=x64 needs= est=29 local-only=0
# test-workload-gate-qemu.sh — run-qemu.sh --workload declares what SHAPE of
# run this is, so the CPU gate stops mismeasuring compute-bound guests.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
#
# The problem: the CPU sampler warns when the guest holds >=CPU_THRESHOLD
# cores for CPU_SUSTAIN seconds past the warm-up. On a 1-vCPU KVM guest that
# tops out ~1.05 cores, a compute-bound binary satisfies that from boot to
# exit, so the only free variable is how LONG the run is — it detects duration
# while reporting a spike. axl-sdk's own unit suite trips it on every x64 run.
#
# --workload compute turns the sustain check off and requires an explicit
# wall-clock budget instead, failing with its own exit code and a message that
# says duration. It must NOT mean "no gate": the duration signal is the one
# thing the sampler could ever legitimately see here, and it is what catches an
# accidental O(n^2) showing up as "the suite got slower".
#
# x64 only: cpu_policy_init carves the warn out on TCG, so AARCH64 (no KVM on
# an x86 host) has no gate to exercise.
#
# Usage: ./test/integration/test-workload-gate-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
export TEST_SKIP_RATCHET=1   # auxiliary; don't touch test-axl.sh's baseline

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# A trivial guest that boots and exits — enough to measure a duration against.
PROBE="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/gfx-avail-probe.efi"
make -C "$PROJECT_DIR" ARCH=x64 gfx-avail-probe > /dev/null 2>&1

# --- host-side argument validation (no QEMU boot) ------------------------

# `--workload compute` with no budget would silently disable the gate. That is
# the one outcome the flag exists to prevent, so it is a usage error.
out=$("$RUN_QEMU" --workload compute "$PROBE" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == "2" ]] && grep -qi "max-duration" <<< "$out"; then
    pass "--workload compute without --max-duration is a usage error"
else
    fail "--workload compute without --max-duration (rc=$rc): $out"
fi

out=$("$RUN_QEMU" --workload bogus --max-duration 60 "$PROBE" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == "2" ]] && grep -qi "workload" <<< "$out"; then
    pass "--workload with an unknown shape is a usage error"
else
    fail "--workload bogus (rc=$rc): $out"
fi

# --- the budget actually fires, and says DURATION not spike --------------

# A budget of 1s cannot be met by any real boot, so this must fail — and the
# distinct exit code is what lets a caller tell it apart from a spike (8) or a
# guest failure (1).
out=$(timeout 180s "$RUN_QEMU" --arch X64 --raw --timeout 60 \
        --workload compute --max-duration 1 "$PROBE" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == "9" ]]; then
    pass "--max-duration breach exits 9 (distinct from spike 8 / failure 1)"
else
    fail "--max-duration breach exit code (rc=$rc)"
fi
if grep -qi "duration budget exceeded" <<< "$out" && ! grep -qi "CPU spike" <<< "$out"; then
    pass "--max-duration breach reports duration, never 'CPU spike'"
else
    fail "--max-duration breach message: $(grep -iE 'duration|spike' <<< "$out" | head -2)"
fi

# --- a met budget passes, and the sustain check is off -------------------

out=$(timeout 180s "$RUN_QEMU" --arch X64 --raw --timeout 60 \
        --workload compute --max-duration 600 "$PROBE" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == "0" ]]; then
    pass "--workload compute within budget exits 0"
else
    fail "--workload compute within budget (rc=$rc)"
fi
if ! grep -qi "CPU spike" <<< "$out"; then
    pass "--workload compute suppresses the CPU-spike warning"
else
    fail "--workload compute still emitted a CPU-spike warning"
fi

# --- the default shape is unchanged --------------------------------------

# No --workload at all must behave exactly as before: the sustain check is
# live and no budget is enforced. A short probe run trips neither.
out=$(timeout 180s "$RUN_QEMU" --arch X64 --raw --timeout 60 "$PROBE" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == "0" ]] && ! grep -qi "duration budget" <<< "$out"; then
    pass "default (idle) shape unchanged: no budget enforced"
else
    fail "default shape changed (rc=$rc)"
fi

echo ""
echo "WORKLOAD-GATE: $PASS passed, $FAIL failed"
[[ "$FAIL" == "0" ]] || exit 1
