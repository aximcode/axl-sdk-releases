#!/bin/bash
# test-ipmi.sh — manual AxlIpmi hardware-in-the-loop runbook.
#
# Unlike test-axl.sh (unit tests in QEMU) or test-http.sh (QEMU loopback),
# AxlIpmi's transports (KCS / SSIF / EDKII / Dell) need a real BMC to
# exercise the FSMs and vendor-protocol dispatch. This script doesn't
# run under QEMU; it builds the SDK + the `ipmi` tool, boots a USB
# image on real hardware, and asks the operator to confirm each
# command returns a plausible result.
#
# Usage:
#   ./test/integration/test-ipmi.sh [--arch X64|AARCH64] [--disk /dev/sdX]
#
# Expected fixtures:
#   - A machine with an addressable BMC (any IPMI-compliant; Dell iDRAC,
#     HPE iLO, Supermicro, AMI, etc.).
#   - UEFI firmware that either exposes IPMI_PROTOCOL / EFI_IPMI_TRANSPORT,
#     or has SMBIOS Type 38 describing KCS/SSIF access.
#   - A USB stick or writable FAT32 partition for the runtime image.
#
# Pass criteria (by eyeball):
#   1. `ipmi info`       reports a non-zero Device ID and prints the
#                        auto-detected transport kind.
#   2. `ipmi chassis status`
#                        reports "Power State: on" when the system is
#                        powered up. Bits in misc_state are plausible.
#   3. `ipmi raw 0x06 0x01`
#                        returns CC=00 and >= 11 additional bytes.
#   4. `ipmi sel list`   completes without timeouts (even an empty SEL
#                        returns CC=00 from Get SEL Info).
#   5. `ipmi sdr list`   enumerates >= 1 record without hanging. On
#                        platforms like Dell iDRAC + Nvidia Grace this
#                        verifies the 60ms SSIF inter-command delay.
#   6. `ipmi sensor`     prints rows with sensor-type and entity-ID
#                        strings (validates the format helpers).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

source "$PROJECT_DIR/scripts/axl-common.sh"

ARCH="${ARCH:-X64}"
DISK=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --disk) DISK="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,32p' "$0"; exit 0 ;;
        *) log_error "unknown option: $1"; exit 1 ;;
    esac
done

_native_arch=$(echo "$ARCH" | tr '[:upper:]' '[:lower:]')
case "$_native_arch" in
    x64|aarch64) ;;
    *) log_error "unsupported arch: $ARCH (want X64 or AARCH64)"; exit 1 ;;
esac

#
# Build. If the tool EFI is already there we skip — this script is
# typically invoked in a loop while iterating on the AxlIpmi code.
#
IPMI_EFI="$PROJECT_DIR/out/native-$_native_arch/tools/ipmi.efi"
if [[ ! -f "$IPMI_EFI" ]]; then
    log_info "Building ipmi.efi ($ARCH)..."
    make -C "$PROJECT_DIR" ARCH="$_native_arch" tools 2>&1 | tail -3
fi
log_info "Tool binary: $IPMI_EFI"

if [[ -n "$DISK" ]]; then
    #
    # Optionally drop the tool onto a FAT32 partition so the operator
    # can boot it directly. Disabled by default because it requires
    # root + a correctly identified block device.
    #
    if [[ ! -b "$DISK" ]]; then
        log_error "--disk $DISK is not a block device"
        exit 1
    fi
    log_warning "About to write $IPMI_EFI to $DISK. Hit Ctrl-C within 5 seconds to abort."
    sleep 5

    MNT=$(mktemp -d)
    sudo mount "$DISK" "$MNT"
    sudo cp "$IPMI_EFI" "$MNT/ipmi.efi"
    sudo umount "$MNT"
    rmdir "$MNT"
    log_info "Copied to $DISK as /ipmi.efi"
fi

cat <<EOF

=== AxlIpmi hardware runbook ========================================

Boot $IPMI_EFI on a machine with a local BMC, then at the UEFI Shell:

    Shell> ipmi info
    Shell> ipmi chassis status
    Shell> ipmi raw 0x06 0x01
    Shell> ipmi sel list
    Shell> ipmi sdr list
    Shell> ipmi sensor

Expected per the pass criteria in the script header. If any step
hangs, note the transport reported by 'ipmi info' — KCS hangs often
mean wrong I/O ports; SSIF hangs usually mean the 60ms inter-command
delay is being skipped (check axl-ipmi-ssif.c).

This script deliberately does NOT automate the remote boot — that
depends on your lab fixturing (PXE, BMC-side virtual media, etc.).

=====================================================================
EOF
