# lib/cpu-spike-common.sh -- the shared half of the CPU-spike detector proof
# (test-cpu-spike-qemu.sh: the common-test.sh path; test-cpu-spike-runqemu-qemu.sh:
# run-qemu.sh's exit code). Sourced after `set -euo pipefail`: setup, the
# shared spin fixture's build and SKIP rules, the pass/fail tally and the
# thresholds both halves are measured against.
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

# The spin fixture (test/integration/cpu-spin-fixture.c + its `cpu-spin-fixture`
# goal) is shared with the CPU-burn investigation and is owned by that
# change, not this one. SKIP loudly rather than failing if it is not present:
# a missing fixture means "this proof could not run here", which is a different
# statement from "the detector is broken", and conflating them would make this
# test lie in exactly the way it exists to prevent.
if [[ ! -f "$PROJECT_DIR/test/integration/cpu-spin-fixture.c" ]]; then
    echo "SKIP: cpu-spin-fixture.c not present — the CPU-spike detector proof needs it."
    echo "      (shared fixture; see test/integration/cpu-spin-fixture.c)"
    exit 0
fi

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" \
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
# Must outlast the 15 s warm-up with room to sustain: the spin starts ~6 s
# after launch, so 20 s leaves ~11 s past the warm-up -- 5.5x the 2 s streak
# the detector needs, the margin against vCPU starvation on a loaded host.
SPIN_MS=20000
RUN_SECS=55      # a hang limit only: each phase ends with reset -s

