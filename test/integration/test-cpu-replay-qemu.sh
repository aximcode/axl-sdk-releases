#!/bin/bash
# test-meta: arch=x64 needs= est=21 local-only=0
# test-cpu-replay-qemu.sh — HF4: end-to-end CPU identity replay.
#
# A fixture cpu.json carrying a distinctive CPU identity, through
# `axl-emulate --cpu-from-fixture` → run-qemu.sh `--cpu` → QEMU's -cpu
# model → the guest's CPUID (x86) / MIDR_EL1 (aarch64), where sysinfo.efi
# reports it. The host arg-parsing for both --cpu knobs is pinned by the
# DRYRUN tests (test-run-qemu-flags.sh, test-axl-emulate.sh); this is the
# live confirmation that the identity reaches the guest.
#
# x86 runs under the integration job's KVM (CPUID overrides honoured by
# KVM via the chosen model). The aarch64 leg needs aa64 tools + an
# aarch64 TCG boot; it runs locally (where `make ARCH=aa64 tools` has
# been done) and SKIPs in the x64-only CI job. Auxiliary; opts out of
# the test-axl.sh ratchet.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
AXL_EMULATE="$PROJECT_DIR/scripts/axl-emulate"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
SYSINFO_X64="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/sysinfo.efi"
SYSINFO_AA64="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs aa64)/tools/sysinfo.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$SYSINFO_X64" ]]; then
    echo "Building x64 tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$SYSINFO_X64" ]] || { echo "FAIL: sysinfo.efi (x64) not found"; exit 1; }

# ----------------------------------------------------------------------
# x86_64: distinctive vendor/family/model/stepping → guest CPUID
# ----------------------------------------------------------------------
FIX_X="$(mktemp -d)"
trap 'rm -rf "$FIX_X" "${FIX_A:-}"' EXIT
cat > "$FIX_X/cpu.json" <<'EOF'
{ "arch": "x86_64", "vendor": "GenuineIntel", "family": 6, "model": 42, "stepping": 7 }
EOF

echo "=== x86: axl-emulate --cpu-from-fixture → guest CPUID ==="
OUT=$(timeout 90s "$AXL_EMULATE" --cpu-from-fixture "$FIX_X" "$SYSINFO_X64" -- cpu 2>&1 || true)
if grep -qE "Model: 42" <<< "$OUT" && grep -qE "Stepping: 7" <<< "$OUT" \
   && grep -qE "Family:[[:space:]]+6" <<< "$OUT" \
   && grep -q "GenuineIntel" <<< "$OUT"; then
    pass "guest CPUID reflects the fixture's family/model/stepping/vendor"
else
    fail "captured CPU identity did not reach the guest" \
         "$(grep -iE 'Vendor:|Family:|Model:' <<< "$OUT" | head)"
fi

# Direct run-qemu.sh --cpu (one less layer): a model QEMU would not pick
# by default proves the override took effect.
OUT2=$(timeout 90s "$RUN_QEMU" \
        --cpu "qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7" \
        "$SYSINFO_X64" cpu 2>&1 || true)
if grep -qE "Model: 42" <<< "$OUT2" && grep -qE "Stepping: 7" <<< "$OUT2"; then
    pass "run-qemu.sh --cpu sets the guest CPUID directly"
else
    fail "run-qemu.sh --cpu did not reach the guest" \
         "$(grep -iE 'Family:|Model:' <<< "$OUT2" | head)"
fi

# ----------------------------------------------------------------------
# aarch64: MIDR replay (local only — needs aa64 tools + TCG boot)
# ----------------------------------------------------------------------
# Gate on the aa64 sysinfo binary (run-qemu.sh does its own QEMU
# discovery). Built locally via `make ARCH=aa64 tools`; absent in the
# x64-only CI job, so this leg skips there.
if [[ -x "$SYSINFO_AA64" ]]; then
    FIX_A="$(mktemp -d)"
    cat > "$FIX_A/cpu.json" <<'EOF'
{ "arch": "aarch64", "midr_el1": "0x410fd0b0" }
EOF
    echo "=== aarch64: axl-emulate --cpu-from-fixture → guest MIDR ==="
    OUTA=$(timeout 150s "$AXL_EMULATE" --cpu-from-fixture --arch AARCH64 \
            "$FIX_A" "$SYSINFO_AA64" -- cpu 2>&1 || true)
    # midr 0x410fd0b0 → Implementer 0x41 (ARM), Part 0xd0b. The default
    # models (cortex-a57 = 0xd07) would NOT report 0xd0b, so this proves
    # the override landed.
    if grep -qE "Part:[[:space:]]+0xd0b" <<< "$OUTA" \
       && grep -q "ARM (0x41)" <<< "$OUTA"; then
        pass "guest MIDR_EL1 reflects the fixture's captured MIDR (part 0xd0b)"
    else
        fail "captured MIDR did not reach the aarch64 guest" \
             "$(grep -iE 'Implementer|Part|MIDR' <<< "$OUTA" | head)"
    fi
else
    echo "SKIP: aarch64 CPU replay (no qemu-system-aarch64 or aa64 sysinfo.efi" \
         "— build with 'make ARCH=aa64 tools'; runs locally, skipped in x64 CI)"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
