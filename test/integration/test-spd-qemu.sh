#!/bin/bash
# test-spd-qemu.sh — exercise AxlSpd's wire path against a custom SPD
# blob attached at QEMU's SMBus 0x50.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# Stock QEMU's smbus-eeprom device has no command-line knob to set
# init_data; this script depends on the locally-patched QEMU at
# $QEMU_DIR built from scripts/qemu-patches/0001-smbus-eeprom-
# add-memdev-link.patch. SmbusHcShim.efi publishes an
# EFI_I2C_MASTER_PROTOCOL on top of QEMU's ICH9 SMBus so AxlSmbus can
# discover a controller in OVMF (which doesn't ship the SMBUS HC
# driver itself).
#
# Verifies that tools/memspd.efi:
#   * decodes the canned DDR4 blob attached at 0x50 (8 GB DDR4-2400 ECC),
#   * resolves the absent vendor code as raw hex (no jedec.json staged),
#   * lists at least one populated slot.
#
# x86-only — ICH9 SMBus and the EEPROM patch are Intel-chipset-specific.
#
# Usage: ./test/integration/test-spd-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Auxiliary; don't clobber test-axl.sh's pass-count baseline.
export TEST_SKIP_RATCHET=1

source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

_native_arch="x64"
make -C "$PROJECT_DIR" ARCH="$_native_arch" tools tests smbus-hc-shim 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
SPD_BLOB="$PROJECT_DIR/test/data/spd-ddr4-micron-8gb.bin"
JEDEC_FILE="$PROJECT_DIR/share/jedec.json"

if [[ ! -f "$SPD_BLOB" ]]; then
    echo "Generating canned SPD blobs..."
    python3 "$PROJECT_DIR/test/data/gen-spd.py"
fi

test_add_efi "$NATIVE_DIR/tools/memspd.efi"
test_add_efi "$NATIVE_DIR/SmbusHcShim.efi"
# Stage the JEDEC sidecar next to memspd.efi so its auto-discovery
# (LoadedImage->FilePath dirname + jedec.json) finds it without a flag.
cp "$JEDEC_FILE" "$TEST_STAGING/jedec.json"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load SmbusHcShim.efi"
    echo "memspd.efi list"
    echo "memspd.efi show 0x50"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd

# Inject the canned DDR4 blob at SMBus address 0x50 via the patched
# smbus-eeprom memdev= property.
test_add_smbus_eeprom "$SPD_BLOB" "0x50"

echo "=== AxlSpd wire-path test ($TEST_ARCH, with patched QEMU + memdev SPD) ==="

test_run_foreground 60

# Custom result check: not pass/fail counts (memspd doesn't emit them);
# scrape the cleaned serial log for the expected decoded values.
test_clean_log
LOG="$TEST_CLEAN_LOG"
echo
echo "--- raw serial log tail (last 60 lines) ---"
tail -60 "$TEST_LOG"
echo
echo "--- memspd output excerpts (cleaned log) ---"
grep -E "^Slot|DDR4|Capacity|Speed|ECC|Module manufacturer|Crucial|Micron" "$LOG" || true
echo "------------------------------"

fail=0
expect_match() {
    local label="$1" pattern="$2"
    if grep -qE "$pattern" "$LOG"; then
        echo "PASS: $label"
    else
        echo "FAIL: $label  (pattern: $pattern)"
        fail=$((fail + 1))
    fi
}

expect_match "memspd list reports DDR4 slot" "Slot 0x50.*DDR4"
expect_match "decoded capacity is 8 GiB"     "Capacity:.*8 GiB"
expect_match "decoded speed is 2400 MT/s"    "Speed:.*2400 MT/s"
expect_match "ECC flag printed as yes"       "ECC:.*yes"

if (( fail > 0 )); then
    echo "$fail wire-path expectation(s) failed"
    exit 1
fi
echo "All wire-path expectations met."
exit 0
