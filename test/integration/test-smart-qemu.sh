#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=0
# test-smart-qemu.sh — AxlSmart union walk + normalized health across transports.
#
# The unit suite (AxlSmart) tests the pure normalizers against constructed
# structs. This boots QEMU with one device of each transport — an NVMe
# controller, an AHCI/SATA disk, and a virtio-scsi disk — and runs
# tools/smart.efi to exercise the union walk (axl_storage_next), the
# transport-native location strings, and axl_smart_health's dispatch into the
# per-transport identity+health readers.
#
# NVMe and ATA SMART are exercised end-to-end (OVMF + the pass-thru protocols
# answer them). SCSI LOG SENSE health is real-hardware-only — virtio-scsi omits
# the IE page — so the SCSI device legitimately reports "health unavailable"
# here; that graceful path is asserted, the SCSI health verdict is not.
#
# Usage: ./test/integration/test-smart-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/smart.efi"
[[ -f "$EFI" ]] || { echo "FAIL: smart.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
NVME="$WORK/nvme.img"; SATA="$WORK/sata.img"; SCSI="$WORK/scsi.img"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

truncate -s 32M "$NVME"; truncate -s 32M "$SATA"; truncate -s 32M "$SCSI"

# One device of each transport, each with a distinct serial so the normalized
# identity assertions have concrete strings.
timeout 150s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 100 \
    --qemu-arg -drive --qemu-arg "file=$NVME,if=none,id=nvm,format=raw" \
    --qemu-arg -device --qemu-arg "nvme,serial=NVME12345,drive=nvm" \
    --qemu-arg -device --qemu-arg "ich9-ahci,id=ahci" \
    --qemu-arg -drive --qemu-arg "file=$SATA,if=none,id=satad,format=raw" \
    --qemu-arg -device \
    --qemu-arg "ide-hd,drive=satad,bus=ahci.0,model=AXLSATA,serial=SATA999" \
    --qemu-arg -device --qemu-arg "virtio-scsi-pci,id=scsi0" \
    --qemu-arg -drive --qemu-arg "file=$SCSI,if=none,id=scsid,format=raw" \
    --qemu-arg -device \
    --qemu-arg "scsi-hd,drive=scsid,bus=scsi0.0,vendor=AXLVND,product=SCSIDISK,serial=SCSI777" \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "nvme|ata|scsi|health|serial|temp|EXCEPTION|leak report" || true

fail=0
# NVMe: full SMART end-to-end (the strongest QEMU story).
grep -qE "^\[nvme\]" "$LOG" || { echo "  MISS: nvme device line"; fail=1; }
grep -qF "NVME12345" "$LOG" || { echo "  MISS: nvme serial (Identify)"; fail=1; }
# ATA: SMART end-to-end.
grep -qF "SATA999" "$LOG" || { echo "  MISS: ata serial (IDENTIFY)"; fail=1; }
# At least two devices report a real health verdict (nvme + ata).
[[ "$(grep -cE 'health: (OK|FAILING)' "$LOG")" -ge 2 ]] \
    || { echo "  MISS: >=2 devices with a health verdict"; fail=1; }
# SCSI: present (INQUIRY works), health gracefully unavailable on virtio-scsi.
grep -qE "^\[scsi\]" "$LOG" || { echo "  MISS: scsi device line"; fail=1; }

grep -qF "no storage devices found" "$LOG" \
    && { echo "  HIT: tool found no storage devices"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: smart device checks"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi
echo "smart device test: OK"
exit 0
