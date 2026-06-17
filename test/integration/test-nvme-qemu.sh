#!/bin/bash
# test-nvme-qemu.sh — AxlNvme against an emulated NVMe controller.
#
# The unit suite (AxlNvme) tests the pure decoders against fixed buffers
# with no device. This boots QEMU with `-device nvme` + a backing drive
# and runs tools/nvme.efi to exercise the device-facing path end to end:
# axl_nvme_next -> Identify Controller, the SMART/Health log, and the
# namespace walk + Identify Namespace, over OVMF's NvmExpressDxe and the
# EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL.
#
# Usage: ./test/integration/test-nvme-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/nvme.efi"
[[ -f "$EFI" ]] || { echo "FAIL: nvme.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
IMG="$WORK/nvme0.img"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# A backing namespace for the emulated controller (contents irrelevant —
# the tool only issues admin Identify / Get Log Page, never reads media).
truncate -s 64M "$IMG"

# Attach an NVMe controller with one namespace. Each QEMU token is one
# --qemu-arg (run-qemu does not word-split).
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    --qemu-arg -drive --qemu-arg "file=$IMG,if=none,id=nvm0,format=raw" \
    --qemu-arg -device --qemu-arg "nvme,drive=nvm0,serial=AXLNVME0001" \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "nvme|health|serial|namespace|ns [0-9]|EXCEPTION|leak report" || true

fail=0
# The controller must enumerate and Identify must succeed (model line).
grep -qE "^nvme0:" "$LOG" || { echo "  MISS: nvme0 controller line"; fail=1; }
# Our serial reached the tool (proves Identify Controller decoded).
grep -qF "AXLNVME0001" "$LOG" || { echo "  MISS: serial AXLNVME0001"; fail=1; }
# SMART/Health log read and decoded.
grep -qE "health: (OK|FAILING)" "$LOG" || { echo "  MISS: SMART health line"; fail=1; }
# Namespace walk + Identify Namespace produced a capacity line.
grep -qE "^  ns 1: [0-9]+ blocks" "$LOG" || { echo "  MISS: namespace 1 line"; fail=1; }
# Hygiene.
grep -qF "no NVMe controllers found" "$LOG" \
    && { echo "  HIT: controller not found by the tool"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: nvme device checks"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi
echo "nvme device test: OK"
exit 0
