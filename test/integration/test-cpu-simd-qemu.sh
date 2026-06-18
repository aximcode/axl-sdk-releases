#!/bin/bash
# test-meta: arch=x64 needs= est=31 local-only=0
# test-cpu-simd-qemu.sh — AxlCpu feature detection + AVX-enable path
# across explicit QEMU CPU models (x86).
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite runs under one CPU model, and run-qemu.sh defaults to
# `-cpu host` when KVM is available — so neither deterministically
# exercises the SSE4.1-but-no-AVX rung AND the AVX2 enable+execute path.
# Pinning explicit `-cpu` models (which override the default; QEMU takes
# the last -cpu) makes both rungs reproducible on any host:
#
#   Nehalem → SSE4.2, NO AVX:  tier SSE4.1, enable_avx()=false, no AVX2 op.
#   Haswell → AVX2:            tier AVX2 after enable, real VPADDD runs
#                              (proves CR4.OSXSAVE + XSETBV worked: a bad
#                              enable would #UD on the AVX2 instruction).
#
# x86-only — AVX is an x86 feature; the AArch64 NEON/no-AVX path is
# already covered by the AArch64 unit run.
#
# Usage: ./test/integration/test-cpu-simd-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$PROJECT_DIR/out/native-x64/cpu-simd-selftest.efi"
make -C "$PROJECT_DIR" ARCH=x64 cpu-simd-selftest 2>&1 | tail -2

overall_fail=0

# run_model <qemu-cpu> <expect-avx 0|1> <expect-tier-after>
run_model() {
    local model="$1" expect_avx="$2" expect_tier="$3"
    local log; log="$(mktemp)"

    echo "=== CPU model: $model (expect avx=$expect_avx, tier=$expect_tier) ==="
    timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 40 \
        --qemu-arg -cpu --qemu-arg "$model" "$EFI" 2>&1 | tee "$log" \
        | grep -iE "features:|tier\(|CPU-SIMD-SELFTEST|PASS:|FAIL:|EXCEPTION" || true

    local fail=0
    grep -qE "features:.*\bavx=$expect_avx\b" "$log" || { echo "  MISS: avx=$expect_avx"; fail=1; }
    grep -qE "tier\(after enable\)=$expect_tier\b" "$log" || { echo "  MISS: tier=$expect_tier"; fail=1; }
    grep -qE "^CPU-SIMD-SELFTEST: [0-9]+ passed, 0 failed" "$log" \
        || { echo "  MISS: clean selftest verdict"; fail=1; }
    grep -qiE "EXCEPTION|invalid opcode" "$log" && { echo "  HIT: CPU exception"; fail=1; }
    # Haswell must actually execute the AVX2 instruction.
    if [[ "$expect_avx" == "1" ]]; then
        grep -qE "AVX2 VPADDD executes" "$log" || { echo "  MISS: AVX2 op did not run"; fail=1; }
    fi

    if (( fail )); then
        echo "FAIL ($model)"; overall_fail=$((overall_fail + 1))
    else
        echo "OK ($model)"
    fi
    rm -f "$log"
    echo
}

# Nehalem: SSE4.x, no AVX → tier 2 (AXL_SIMD_SSE41), enable=false.
run_model Nehalem 0 2
# Haswell: AVX2 → tier 3 (AXL_SIMD_AVX2) after enable, AVX2 op runs.
run_model Haswell 1 3
# Icelake-Server: advertises AVX-512 to QEMU. On an AVX-512 host (or under
# TCG) the selftest enables AVX-512 state and runs a real ZMM VPADDD; under
# KVM on a non-AVX-512 host the model is masked down (avx512f=0) and it
# instead validates the "no avx512f -> enable returns false" branch. Either
# way tier stays 3 (AXL kernels top out at AVX2; AVX-512 is consumer-only)
# and enable_avx512 == avx512f. The enable_avx512 LOGIC is covered
# universally by the unit-test invariant; the ZMM execution path reuses the
# same CR4.OSXSAVE+XSETBV mechanism proven by Haswell's AVX2 VPADDD.
run_model Icelake-Server 1 3

if (( overall_fail > 0 )); then
    echo "$overall_fail CPU model(s) failed"
    exit 1
fi
echo "All CPU-SIMD model checks passed."
exit 0
