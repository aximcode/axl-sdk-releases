#!/bin/bash
# test-meta: arch=x64 needs= est=31 local-only=0
# test-gfx-simd-qemu.sh — validate + benchmark the SIMD-dispatched blur
# across explicit QEMU CPU models (x86).
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# axl_gfx_buffer_blur() dispatches to an AVX2 / SSE4.1 / scalar kernel
# per axl_cpu_simd_tier(); each kernel must be BIT-IDENTICAL to scalar.
# The unit runner's CPU (qemu64 under TCG) reports only the SSE2
# baseline, so it exercises just the scalar path — pinning explicit
# models is the only way to validate the SSE4.1 and AVX2 rungs:
#
#   qemu64  → tier 1 (BASELINE): scalar path (speedup ~1.0).
#   Nehalem → tier 2 (SSE4.1):   pmovzxbd+pmulld kernel.
#   Haswell → tier 3 (AVX2):     2-pixel/iter kernel.
#
# Each run asserts the dispatched blur is byte-for-byte identical to an
# independent in-app scalar reference and prints the measured speedup
# (informational — host-dependent, not gated). x86-only; NEON is
# validated by the AArch64 unit run.
#
# Usage: ./test/integration/test-gfx-simd-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$PROJECT_DIR/out/native-x64/gfx-simd-selftest.efi"
make -C "$PROJECT_DIR" ARCH=x64 gfx-simd-selftest 2>&1 | tail -2

overall_fail=0

# run_model <qemu-cpu> <expect-tier>
run_model() {
    local model="$1" expect_tier="$2"
    local log; log="$(mktemp)"

    echo "=== CPU model: $model (expect tier $expect_tier) ==="
    timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 50 \
        --qemu-arg -cpu --qemu-arg "$model" "$EFI" 2>&1 | tee "$log" \
        | grep -iE "simd tier|timing|speedup|blur radius|PASS:|FAIL:|GFX-SIMD-SELFTEST|mismatch|EXCEPTION" || true

    local fail=0
    grep -qE "simd tier = $expect_tier\b" "$log" || { echo "  MISS: tier $expect_tier"; fail=1; }
    grep -qE "PASS: dispatched blur is bit-exact" "$log" || { echo "  MISS: bit-exact PASS"; fail=1; }
    grep -qE "^GFX-SIMD-SELFTEST: [0-9]+ passed, 0 failed" "$log" || { echo "  MISS: clean verdict"; fail=1; }
    grep -qiE "EXCEPTION|invalid opcode" "$log" && { echo "  HIT: CPU exception"; fail=1; }

    # capture the blit_transform hash for the cross-model equality check
    BLIT_HASH=$(grep -oE "BLIT-HASH: 0x[0-9a-f]+" "$log" | head -1 | awk '{print $2}')
    BLIT_HASHES="$BLIT_HASHES $model=$BLIT_HASH"

    if (( fail )); then echo "FAIL ($model)"; overall_fail=$((overall_fail + 1)); else echo "OK ($model)"; fi
    rm -f "$log"; echo
}

BLIT_HASHES=""
run_model qemu64  1
run_model Nehalem 2
run_model Haswell 3

# blit_transform is scalar (a SIMD combine measured ~4.6x slower and was
# dropped); its output must still be IDENTICAL across CPU models — a
# regression guard that the scalar float math is CPU-independent.
echo "blit_transform hashes:$BLIT_HASHES"
uniq_hashes=$(echo "$BLIT_HASHES" | tr ' ' '\n' | grep -oE '0x[0-9a-f]+' | sort -u | wc -l)
if [[ "$uniq_hashes" == "1" ]]; then
    echo "OK: blit_transform hash identical across CPU models"
else
    echo "FAIL: blit_transform hash differs across CPU models"
    overall_fail=$((overall_fail + 1))
fi
echo

if (( overall_fail > 0 )); then
    echo "$overall_fail CPU model(s) failed"
    exit 1
fi
echo "All GFX-SIMD model checks passed."
exit 0
