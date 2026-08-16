#!/bin/bash
# test-meta: arch=x64 needs= est=9 local-only=1
# test-i2c-qemu.sh — exercise tools/i2c.efi against a canned SMBus
# EEPROM (SPD blob at 0x50) under QEMU's ICH9 SMBus + SmbusHcShim.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# Same fixture as test-spd-qemu.sh: relies on the patched QEMU at
# $QEMU_DIR (scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch)
# plus SmbusHcShim.efi to publish an EFI_I2C_MASTER_PROTOCOL OVMF
# doesn't ship.
#
# Verifies that tools/i2c.efi:
#   * `list` reports at least one controller (the I2C master from
#     SmbusHcShim).
#   * `probe 0` finds the slave at 0x50.
#   * `get 0 0x50 0x02` returns 0x0c (DDR4 memory-type byte, JEDEC
#     SPD layout offset 2).
#   * `dump 0 0x50` produces a non-empty hex dump.
#   * `set` without --force refuses (safety check).
#
# x86-only — ICH9 SMBus and the EEPROM patch are Intel-chipset-specific.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

_native_arch="x64"
make -C "$PROJECT_DIR" ARCH="$_native_arch" tools tests smbus-hc-shim 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
SPD_BLOB="$PROJECT_DIR/test/data/spd-ddr4-micron-8gb.bin"

if [[ ! -f "$SPD_BLOB" ]]; then
    echo "Generating canned SPD blobs..."
    python3 "$PROJECT_DIR/test/data/gen-spd.py"
fi

test_add_efi "$NATIVE_DIR/tools/i2c.efi"
test_add_efi "$NATIVE_DIR/SmbusHcShim.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load SmbusHcShim.efi"
    echo "echo === LIST ==="
    echo "i2c.efi list"
    echo "echo === PROBE ==="
    # --read uses byte-data read at register 0; QEMU's smbus-eeprom
    # device does not implement SMBus Receive Byte, so AUTO mode
    # would NACK every address in 0x50..0x5F. Force --read to
    # exercise byte-data path which the fixture supports.
    echo "i2c.efi probe --read 0"
    echo "echo === GET ONE ==="
    echo "i2c.efi get 0 0x50 0x02"
    echo "echo === GET RANGE ==="
    echo "i2c.efi get 0 0x50 0x00 16"
    echo "echo === DUMP ==="
    echo "i2c.efi dump 0 0x50"
    echo "echo === SET REFUSED ==="
    echo "i2c.efi set 0 0x50 0x00 0xAA"
    echo "echo === DONE ==="
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd

test_add_smbus_eeprom "$SPD_BLOB" "0x50"

echo "=== i2c-tool wire-path test ($TEST_ARCH, ICH9 SMBus + SPD@0x50) ==="

test_run_foreground 60

test_clean_log
LOG="$TEST_CLEAN_LOG"
echo
echo "--- i2c output excerpts (cleaned log) ---"
sed -n '/=== LIST ===/,/=== DONE ===/p' "$LOG" || true
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

expect_match "list reports a controller"        "^i2c-0[[:space:]]"
expect_match "probe finds slave at 0x50"        "^50:.* 50"
expect_match "get reports 0x0C at offset 2"     "^0x0C$"
expect_match "get range hex-dumps 16 bytes"     "^ +0000: ([0-9a-f]{2} +){8}"
expect_match "dump prints offset 0x80 line"     "^ +0080:"
expect_match "set without --force refuses"      "Refusing to write without --force"

if (( fail > 0 )); then
    echo "$fail i2c-tool expectation(s) failed"
    exit 1
fi
echo "All i2c-tool expectations met."
exit 0
