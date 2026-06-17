#!/bin/bash
# test-scsi-qemu.sh — AxlScsi against an emulated virtio-scsi disk + CD.
#
# The unit suite (AxlScsi) tests the pure decoders against fixed buffers.
# This boots QEMU with a virtio-scsi HBA carrying a SCSI disk and a SCSI
# CD-ROM and runs tools/scsi.efi to exercise the device-facing path:
# axl_scsi_next -> INQUIRY (identity + serial) and READ CAPACITY, over OVMF's
# ScsiBus / ScsiDisk drivers and EFI_EXT_SCSI_PASS_THRU.
#
# LOG SENSE health (the IE / Temperature log pages) is real-hardware territory
# — QEMU's virtio-scsi does not emulate those pages — so it is NOT asserted
# here; the decoders cover it in the unit suite.
#
# Usage: ./test/integration/test-scsi-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/scsi.efi"
[[ -f "$EFI" ]] || { echo "FAIL: scsi.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
DISK="$WORK/disk.img"
CD="$WORK/cd.img"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

truncate -s 48M "$DISK"
truncate -s 16M "$CD"

# A virtio-scsi HBA with one SCSI disk (carrying a vendor/product/serial so the
# INQUIRY assertions have concrete strings) plus a SCSI CD-ROM (a second
# peripheral device type and a 2048-byte block size).
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    --qemu-arg -device --qemu-arg "virtio-scsi-pci,id=scsi0" \
    --qemu-arg -drive --qemu-arg "file=$DISK,if=none,id=scsidisk,format=raw" \
    --qemu-arg -device \
    --qemu-arg "scsi-hd,drive=scsidisk,bus=scsi0.0,vendor=AXLVND,product=AXLSCSIDISK,serial=SCSI98765" \
    --qemu-arg -drive --qemu-arg "file=$CD,if=none,id=scsicd,format=raw" \
    --qemu-arg -device --qemu-arg "scsi-cd,drive=scsicd,bus=scsi0.0" \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "scsi[0-9]|serial|blocks|health|rev:|EXCEPTION|leak report" \
    || true

fail=0
grep -qE "^scsi0 " "$LOG" || { echo "  MISS: scsi0 device line"; fail=1; }
grep -qF "AXLSCSIDISK" "$LOG" || { echo "  MISS: product AXLSCSIDISK (INQUIRY)"; fail=1; }
grep -qF "SCSI98765" "$LOG" || { echo "  MISS: serial SCSI98765 (VPD 0x80)"; fail=1; }
grep -qE "[0-9]+ blocks x [0-9]+ B" "$LOG" || { echo "  MISS: capacity line (READ CAPACITY)"; fail=1; }
grep -qiF "cdrom" "$LOG" || { echo "  MISS: cdrom device type"; fail=1; }
grep -qF "no SCSI devices found" "$LOG" \
    && { echo "  HIT: tool found no SCSI devices"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: scsi device checks"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi
echo "scsi device test: OK"
exit 0
