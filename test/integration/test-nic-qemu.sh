#!/bin/bash
# test-meta: arch=x64 needs= est=31 local-only=0
# test-nic-qemu.sh — AxlNet driver-selection substrate against a real NIC.
#
# The AxlTestNet unit suite covers the bus-location trim against synthetic
# device paths and the safe-negative argument paths. This boots QEMU with a
# virtio-net NIC (OVMF auto-binds VirtioNetDxe -> SNP) and runs tools/netinfo
# to exercise the live accessors:
#   - axl_net_get_driver_info: per-NIC bound-driver + bus-location columns
#   - axl_net_list_available_drivers: `list-bundle`
#   - axl_net_try_driver: `try`
#
# The iPXE-load-last, OEM-child-handle, and MediaPresent quirks the API
# encapsulates are real-hardware-only and are NOT asserted here.
#
# Usage: ./test/integration/test-nic-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/netinfo.efi"
[[ -f "$EFI" ]] || { echo "FAIL: netinfo.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# --net gives a virtio-net-pci NIC that OVMF binds via its built-in
# VirtioNetDxe; -v makes netinfo print the driver + bus columns.
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 --net \
    "$EFI" -v list 2>&1 | tee "$LOG" \
    | grep -iE "NIC\[|driver=|bus=|IF#|eth|EXCEPTION|leak report" || true

fail=0

# A NIC must enumerate (SNP up via VirtioNetDxe).
grep -qE "NIC\[0\]" "$LOG" || { echo "  MISS: NIC[0] driver line"; fail=1; }

# The headline feature: a bus location anchored on PCI topology, with the
# MAC/network tail trimmed off.
if grep -qE "bus=PciRoot\(0x[0-9a-fA-F]+\)/Pci\(" "$LOG"; then
    : # good
else
    echo "  MISS: bus=PciRoot(..)/Pci(..) location line"; fail=1
fi
# The trim must have removed the MAC node — a bus line carrying MAC( means
# the network tail leaked through.
grep -qE "bus=.*MAC\(" "$LOG" && { echo "  HIT: MAC( not trimmed from bus"; fail=1; }

grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

# `try` on a name that can't be located must report not-found cleanly (no
# load, no crash).
TRYLOG="$WORK/try.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 --net \
    "$EFI" try axl-no-such-nic-driver-xyz.efi 2>&1 | tee "$TRYLOG" \
    | grep -iE "Trying driver|not found|result|EXCEPTION" || true
grep -qiE "not found on the driver search path" "$TRYLOG" \
    || { echo "  MISS: try reports not-found for a bogus driver"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$TRYLOG" \
    && { echo "  HIT: CPU exception in try"; fail=1; }

# `list-bundle` (axl_net_list_available_drivers) must run without crashing;
# the QEMU ESP stages no drivers, so "no drivers staged" is the expected line.
BUNDLELOG="$WORK/bundle.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 --net \
    "$EFI" list-bundle 2>&1 | tee "$BUNDLELOG" \
    | grep -iE "Driver Bundle|drivers staged|bytes|EXCEPTION" || true
grep -qiE "Driver Bundle" "$BUNDLELOG" \
    || { echo "  MISS: list-bundle header"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$BUNDLELOG" \
    && { echo "  HIT: CPU exception in list-bundle"; fail=1; }

if (( fail )); then
    echo "FAIL: NIC driver-selection checks"
    echo "--- list -v log ---"; cat "$LOG"
    exit 1
fi
echo "nic driver-selection test: OK"
exit 0
