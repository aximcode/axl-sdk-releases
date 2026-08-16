#!/bin/bash
# test-meta: arch=x64 needs= est=9 local-only=0
# test-ipmi-ssif-qemu.sh — exercise AxlIpmi's SSIF transport against
# QEMU's BMC simulator via the SMBus IPMI device.
#
# Separate from test-axl.sh (auxiliary, single-binary, no ratchet) and
# parallel to test-ipmi-qemu.sh (same shape, different transport).
#
# Regression coverage for code-review finding B1 (I2C Master fallback
# in AxlSmbus: missing byte-count prefix on writes, missing count
# strip on reads). Unit tests in AxlTestSmbus already cover the
# framing against a capturing mock; this script adds end-to-end
# coverage: Shell loads SmbusHcShim.efi which publishes
# EFI_I2C_MASTER_PROTOCOL on top of QEMU's ICH9 SMBus controller,
# then AxlTestIpmi runs its hardware path and issues real SSIF
# commands to the simulated BMC.
#
# x86-only: QEMU q35 ICH9 SMBus + Intel-chipset-specific shim. The
# script hard-skips on AARCH64.
#
# Usage:
#   ./test/integration/test-ipmi-ssif-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Auxiliary runner; don't clobber test-axl.sh's baseline with our
# single-binary pass count.
export TEST_SKIP_RATCHET=1

source "$SCRIPT_DIR/common-test.sh"

TEST_ARCH="X64"
test_setup

PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_DIR="$(dirname "$PROJECT_DIR")"

_native_arch="x64"
make -C "$PROJECT_DIR" ARCH="$_native_arch" all tests smbus-hc-shim 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/AxlTestIpmi.efi"
test_add_efi "$NATIVE_DIR/SmbusHcShim.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load SmbusHcShim.efi"
    echo "AxlTestIpmi.efi hw"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlIpmi SSIF BMC-simulator tests ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_add_ipmi_bmc_sim_ssif
test_run_foreground 40

test_count_results
