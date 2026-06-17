#!/bin/bash
# test-ata-qemu.sh — AxlAta against an emulated AHCI/SATA disk.
#
# The unit suite (AxlAta) tests the pure decoders against fixed buffers.
# This boots QEMU with an AHCI controller + a SATA disk and runs
# tools/ata.efi to exercise the device-facing path: axl_ata_next ->
# IDENTIFY DEVICE and SMART, over OVMF's AtaAtapiPassThru driver and the
# EFI_ATA_PASS_THRU_PROTOCOL.
#
# Usage: ./test/integration/test-ata-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/ata.efi"
[[ -f "$EFI" ]] || { echo "FAIL: ata.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
IMG="$WORK/sata.img"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

truncate -s 64M "$IMG"

# An AHCI controller with one SATA disk carrying a model + serial so the
# IDENTIFY assertions have a concrete string to match.
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    --qemu-arg -device --qemu-arg "ich9-ahci,id=ahci" \
    --qemu-arg -drive --qemu-arg "file=$IMG,if=none,id=satadisk,format=raw" \
    --qemu-arg -device \
    --qemu-arg "ide-hd,drive=satadisk,bus=ahci.0,model=AXLSATA,serial=ATA12345" \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "ata[0-9]|serial|health|blocks|self-test|EXCEPTION|leak report" \
    || true

fail=0
grep -qE "^ata0 " "$LOG" || { echo "  MISS: ata0 device line"; fail=1; }
grep -qF "ATA12345" "$LOG" || { echo "  MISS: serial ATA12345 (IDENTIFY)"; fail=1; }
grep -qF "AXLSATA" "$LOG" || { echo "  MISS: model AXLSATA (IDENTIFY)"; fail=1; }
grep -qE "[0-9]+ blocks x [0-9]+ B" "$LOG" || { echo "  MISS: capacity line"; fail=1; }
grep -qF "no ATA/SATA devices found" "$LOG" \
    && { echo "  HIT: tool found no ATA devices"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

# SMART is best-effort under QEMU's IDE emulation; if the device reports
# SMART support, a health verdict must have been produced.
if grep -qF "SMART: supported" "$LOG"; then
    grep -qE "health: (OK|FAILING)" "$LOG" \
        || { echo "  MISS: SMART supported but no health verdict"; fail=1; }
fi

if (( fail )); then
    echo "FAIL: ata device checks"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi
echo "ata device test: OK"
exit 0
