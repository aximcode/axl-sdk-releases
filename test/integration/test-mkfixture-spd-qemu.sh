#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=1
# test-mkfixture-spd-qemu.sh — HF4: mkfixture SPD capture.
#
# Boots mkfixture.efi --spd against a canned DDR4 SPD blob attached at
# QEMU SMBus 0x50, capturing the fixture onto the (writable) FAT boot
# image, then reads it back with mtools and verifies:
#   * spd/0x50.bin was captured and is bit-for-bit the injected EEPROM
#     content (the design's "AxlSpd output matches bit-for-bit" gate);
#   * spd.json decodes the blob as 8 GiB DDR4-2400 ECC.
#
# Same rig as test-spd-qemu.sh: depends on the locally-patched QEMU
# (scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch) for the
# memdev= knob, and SmbusHcShim.efi to publish EFI_I2C_MASTER_PROTOCOL
# in OVMF. x86-only. Auxiliary — opts out of the test-axl.sh ratchet.
# NOT CI-wired (needs the patched QEMU); local pre-release check.
#
# Usage: ./test/integration/test-mkfixture-spd-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1
source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

_native_arch="x64"
make -C "$PROJECT_DIR" ARCH="$_native_arch" tools smbus-hc-shim 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
SPD_BLOB="$PROJECT_DIR/test/data/spd-ddr4-micron-8gb.bin"
JEDEC_FILE="$PROJECT_DIR/share/jedec.json5"

if [[ ! -f "$SPD_BLOB" ]]; then
    echo "Generating canned SPD blobs..."
    python3 "$PROJECT_DIR/test/data/gen-spd.py"
fi

test_add_efi "$NATIVE_DIR/tools/mkfixture.efi"
test_add_efi "$NATIVE_DIR/SmbusHcShim.efi"
# Stage the JEDEC sidecar next to mkfixture.efi so AxlSpd's vendor
# resolution can find it (harmless if unused by the decode path).
cp "$JEDEC_FILE" "$TEST_STAGING/jedec.json5"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load SmbusHcShim.efi"
    echo "mkfixture.efi --spd FS0:\\fix"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd

# Inject the canned DDR4 blob at SMBus 0x50 (patched memdev= EEPROM).
test_add_smbus_eeprom "$SPD_BLOB" "0x50"

echo "=== HF4 mkfixture SPD capture ($TEST_ARCH, patched QEMU + memdev SPD) ==="
test_run_foreground 90

echo
echo "--- raw serial log tail ---"
tail -25 "$TEST_LOG"
echo

PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

# mkfixture wrote the fixture onto the FAT boot image; read it back.
CAP="$TEST_TMPDIR/0x50.bin"
SPDJSON="$TEST_TMPDIR/spd.json"
mcopy -n -i "$TEST_DISK" "::/fix/spd/0x50.bin" "$CAP" 2>/dev/null || true
mcopy -n -i "$TEST_DISK" "::/fix/spd.json"     "$SPDJSON" 2>/dev/null || true

# 1. The raw blob was captured.
if [[ -s "$CAP" ]]; then
    pass "spd/0x50.bin captured ($(stat -c%s "$CAP") bytes)"
else
    fail "spd/0x50.bin not captured" \
         "(mdir: $(mdir -i "$TEST_DISK" "::/fix" 2>&1 | tail -5))"
fi

# 2. Bit-for-bit identical to the injected EEPROM content (the device
#    serves the first N bytes of the blob; compare that prefix).
if [[ -s "$CAP" ]]; then
    sz=$(stat -c%s "$CAP")
    if cmp -s <(head -c "$sz" "$SPD_BLOB") "$CAP"; then
        pass "captured SPD is bit-for-bit the injected EEPROM ($sz B)"
    else
        fail "captured SPD differs from the injected blob prefix"
    fi
fi

# 3. spd.json decodes the canned module: DDR4, 8 GiB, 2400 MT/s, ECC.
if [[ -s "$SPDJSON" ]] && python3 - "$SPDJSON" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["count"] >= 1, f"count {d['count']} < 1"
slot = next((s for s in d["slots"] if s["address"] == "0x50"), None)
assert slot is not None, f"no 0x50 slot; have {[s['address'] for s in d['slots']]}"
assert slot["ddr_generation"] == 4, f"ddr_generation {slot['ddr_generation']} != 4"
assert slot["capacity_bytes"] == 8 * 1024**3, f"capacity {slot['capacity_bytes']}"
assert slot["speed_mts"] == 2400, f"speed {slot['speed_mts']} != 2400"
assert slot["ecc"] is True, "ecc not true"
assert slot["raw_bytes"] >= 256, f"raw_bytes {slot['raw_bytes']} < 256"
PYEOF
then
    pass "spd.json decodes 0x50 as 8 GiB DDR4-2400 ECC"
else
    fail "spd.json missing or wrong decode" "$(cat "$SPDJSON" 2>/dev/null)"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
