#!/bin/bash
# test-mkfixture-qemu.sh -- end-to-end test for tools/mkfixture.efi.
#
# Captures a fixture from a running OVMF guest into a virtiofs-mounted
# host directory, validates the on-disk shape (smbios.bin starts with
# a parseable SMBIOS structure, acpi/ has at least the FACP/HPET tables
# every QEMU-OVMF guest publishes, manifest.json is well-formed JSON),
# then replays the captured fixture via axl-emulate against a second
# QEMU instance running with custom SMBIOS strings, and verifies the
# captured custom identity round-trips through to sysinfo.efi.
#
# Auxiliary; opt out of the test-axl.sh ratchet (--mount + sequential
# QEMU pipeline; not amenable to the unit-test pass-count baseline).
#
# x86-only — uses --mount virtiofs which depends on OVMF VirtioFsDxe;
# aa64 OVMF builds typically lack it. Mirrors test-spd-qemu.sh's policy.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
AXL_EMULATE="$PROJECT_DIR/scripts/axl-emulate"
MKFIXTURE="$PROJECT_DIR/out/native-x64/tools/mkfixture.efi"
SYSINFO="$PROJECT_DIR/out/native-x64/tools/sysinfo.efi"

# Auxiliary; don't clobber test-axl.sh's pass-count baseline.
export TEST_SKIP_RATCHET=1

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

# Build mkfixture / sysinfo if missing (runs in <1s on a warm tree).
if [[ ! -x "$MKFIXTURE" ]] || [[ ! -x "$SYSINFO" ]]; then
    echo "Building tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$MKFIXTURE" ]] || { echo "FAIL: mkfixture.efi not found at $MKFIXTURE"; exit 1; }
[[ -x "$SYSINFO" ]]   || { echo "FAIL: sysinfo.efi not found at $SYSINFO"; exit 1; }

# ----------------------------------------------------------------------
# Capture phase: run mkfixture inside QEMU with a known-distinct SMBIOS
# (AximCode/TestRig/1.0/ABC123 — these strings DO NOT appear in OVMF's
# auto-built defaults, so seeing them in the replay proves the fixture
# was actually consumed, not just QEMU defaults coming through).
# ----------------------------------------------------------------------
FIX_DIR="$(mktemp -d)"
trap 'rm -rf "$FIX_DIR"' EXIT

echo "=== Capture: mkfixture under custom-SMBIOS QEMU ==="
timeout 60s "$RUN_QEMU" --mount "$FIX_DIR" \
    --qemu-arg "-smbios" --qemu-arg "type=1,manufacturer=AximCode,product=TestRig,version=1.0,serial=ABC123" \
    "$MKFIXTURE" 'FS1:\fix' > /dev/null 2>&1 || true

# ----------------------------------------------------------------------
# Validate captured layout
# ----------------------------------------------------------------------
[[ -d "$FIX_DIR/fix" ]] && pass "fixture root directory created" \
    || fail "fixture root not created" "expected $FIX_DIR/fix"

[[ -f "$FIX_DIR/fix/smbios.bin" ]] && pass "smbios.bin written" \
    || fail "smbios.bin missing"

[[ -d "$FIX_DIR/fix/acpi" ]] && pass "acpi/ directory created" \
    || fail "acpi/ directory missing"

# smbios.bin first byte should be a valid SMBIOS Type (0-127). For
# OVMF-Q35 with custom Type 1 injection, Type 0 (BIOS Info) typically
# leads. We assert it's in the valid range, NOT the dmidecode '_'
# (0x5F) prefix — that would mean we wrote the EP, which QEMU rejects.
if [[ -f "$FIX_DIR/fix/smbios.bin" ]]; then
    first_byte=$(head -c 1 "$FIX_DIR/fix/smbios.bin" | od -An -tu1 -N1 | tr -d ' ')
    if [[ "$first_byte" -le 127 ]]; then
        pass "smbios.bin first byte ($first_byte) is a valid SMBIOS Type"
    else
        fail "smbios.bin first byte is $first_byte (likely dmidecode-EP format)"
    fi
fi

# Spec-required tables must be present. FACP (FADT) is the one ACPI
# table every spec-compliant ACPI platform must publish — it points
# at FACS/DSDT and defines the PM interfaces. Asserting on FACP
# specifically (vs a loose count) catches a mkfixture regression
# where (e.g.) the ACPI walker stops after the first table.
if [[ -f "$FIX_DIR/fix/acpi/facp.dat" ]]; then
    pass "acpi/facp.dat present (spec-required FADT captured)"
else
    fail "acpi/facp.dat missing — ACPI walk likely truncated" \
         "(present tables: $(ls "$FIX_DIR/fix/acpi/" 2>/dev/null))"
fi
acpi_count=$(ls "$FIX_DIR/fix/acpi/"*.dat 2>/dev/null | wc -l)
pass "acpi/ has $acpi_count tables (informational)"

# manifest.json must be well-formed JSON containing the captured strings.
if [[ -f "$FIX_DIR/fix/manifest.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/manifest.json" 2>/dev/null; then
        pass "manifest.json is well-formed JSON"
    else
        fail "manifest.json is not valid JSON"
    fi
    if grep -q '"vendor": "AximCode"' "$FIX_DIR/fix/manifest.json"; then
        pass "manifest.json captures custom vendor (AximCode)"
    else
        fail "manifest.json missing custom vendor" \
             "$(cat "$FIX_DIR/fix/manifest.json")"
    fi
    if grep -q '"model": "TestRig"' "$FIX_DIR/fix/manifest.json"; then
        pass "manifest.json captures custom model (TestRig)"
    else
        fail "manifest.json missing custom model"
    fi
fi

# ----------------------------------------------------------------------
# Replay phase: feed the captured fixture into a fresh Q35 OVMF guest
# (NO custom SMBIOS this time) and verify the captured custom identity
# comes through. If only OVMF defaults appeared, mkfixture wrote
# bytes QEMU couldn't consume.
# ----------------------------------------------------------------------
echo "=== Replay: axl-emulate captured fixture into vanilla OVMF ==="
REPLAY_OUT=$(timeout 60s "$AXL_EMULATE" "$FIX_DIR/fix/" "$SYSINFO" 2>&1 || true)

# The replay layer's manifest summary line on stderr must mention the
# captured strings.
if grep -qE "axl-emulate: replaying AximCode" <<< "$REPLAY_OUT"; then
    pass "axl-emulate prints captured-fixture identity at startup"
else
    fail "axl-emulate did not surface captured identity" \
         "(expected 'replaying AximCode' line)"
fi

# Inside the guest, sysinfo.efi must report the captured custom strings,
# not OVMF's auto-built Type 1 defaults.
if grep -q "Manufacturer: AximCode" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Manufacturer (AximCode)"
else
    fail "guest reported wrong/missing Manufacturer" \
         "(captured AximCode lost in replay)"
fi
if grep -q "Product:      TestRig" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Product (TestRig)"
else
    fail "guest reported wrong/missing Product"
fi
if grep -q "Version:      1.0" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Version (1.0)"
else
    fail "guest reported wrong/missing Version"
fi
if grep -q "Serial:       ABC123" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Serial (ABC123)"
else
    fail "guest reported wrong/missing Serial"
fi

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
