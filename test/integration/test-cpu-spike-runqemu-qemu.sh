#!/bin/bash
# test-meta: arch=x64 needs= est=50 local-only=0 builds=cpu-spin-fixture guest=none
# guest=none: run-qemu.sh's own CPU-spike exit code on the guest it boots is
#   the subject.
# test-cpu-spike-runqemu-qemu.sh -- the CPU-spike detector reaches a CALLER as
# run-qemu.sh's exit code. The other half of test-cpu-spike-qemu.sh (split so
# the two run in parallel); the shared setup, fixture and thresholds are
# lib/cpu-spike-common.sh's. See test-cpu-spike-qemu.sh's header for the
# controls and how the thresholds were measured.
set -euo pipefail

source "$(dirname "$0")/lib/cpu-spike-common.sh"

# --- Phase 3: the spike must reach a CALLER as an exit code ----------------
#
# Everything above proves the sampler sees the spin and that common-test.sh
# surfaces it as a WARN. Neither proves the thing a script actually branches
# on: run-qemu.sh exiting non-zero. That was an untested seam --
# test-workload-gate-qemu.sh drives run-qemu.sh directly but only ever with a
# short non-spinning probe, so a regression that silently returned 0 here
# would have passed the whole suite.
#
# Uses run-qemu.sh directly rather than the common-test.sh helpers, because
# the exit code IS the interface under test.
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
spike_rc=0
timeout 200s "$RUN_QEMU" --arch X64 --raw --timeout 70 \
    "$TEST_BUILD_DIR/cpu-spin-fixture.efi" compute "$SPIN_MS" \
    >/dev/null 2>&1 || spike_rc=$?

if [[ "$spike_rc" == "${CPU_SPIKE_EXIT:-8}" ]]; then
    pass "a spinning guest makes run-qemu.sh exit ${CPU_SPIKE_EXIT:-8}"
else
    fail "a spinning guest makes run-qemu.sh exit ${CPU_SPIKE_EXIT:-8}" \
         "got rc=$spike_rc"
fi

# The negative control matters as much: an exit code that fires for every run
# is not a detector. A guest that sleeps must come back clean.
idle_rc=0
timeout 200s "$RUN_QEMU" --arch X64 --raw --timeout 40 \
    "$TEST_BUILD_DIR/cpu-spin-fixture.efi" idle 14000 \
    >/dev/null 2>&1 || idle_rc=$?

if [[ "$idle_rc" == "0" ]]; then
    pass "an idle guest leaves run-qemu.sh at rc=0"
else
    fail "an idle guest leaves run-qemu.sh at rc=0" "got rc=$idle_rc"
fi

echo
printf "cpu-spike detector (run-qemu.sh exit code): %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
[[ $FAIL -eq 0 ]] || exit 1
