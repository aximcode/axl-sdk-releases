#!/bin/bash
# test-meta: arch=x64 needs= est=8 local-only=0
# test-ipmi-qemu.sh — run AxlTestIpmi against QEMU's built-in BMC
# simulator to exercise the real KCS transport.
#
# Separate from test-axl.sh because it needs extra QEMU device flags
# (ipmi-bmc-sim + isa-ipmi-kcs) and only makes sense on x86 — the
# ISA KCS interface is x86-only. The test binary soft-skips the
# hardware-specific assertions when no transport is available, so the
# same binary runs harmlessly under test-axl.sh; this script is what
# actually exercises the KCS FSM against a responding BMC.
#
# Regression coverage for code-review finding B2 (KCS empty-body
# commands emitting a spurious trailing zero) — on QEMU's strict
# simulator that bug surfaces as CC=0xC7 from Get Device ID.
#
# SSIF (B1) coverage lives in the sibling script
# test-ipmi-ssif-qemu.sh — it layers an EFI_I2C_MASTER_PROTOCOL shim
# on top of QEMU's ICH9 SMBus controller so AxlSmbus's I2C fallback
# can reach the BMC sim over SMBus.
#
# Usage:
#   ./test/integration/test-ipmi-qemu.sh

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
make -C "$PROJECT_DIR" ARCH="$_native_arch" all tests 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/AxlTestIpmi.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "AxlTestIpmi.efi hw"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlIpmi BMC-simulator tests ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_add_ipmi_bmc_sim_kcs
test_run_foreground 40

# --- src/ipmi/axl-ipmi-kcs.c "IPMI KCS transport ready" stays at debug -------
#
# This is the run that REACHES that line: the BMC sim answers, so kcs_open
# succeeds and its success line executes. AxlTestIpmi's own quiet window now
# asserts on this branch too (3.2.2 demoted the SMBIOS Type 38 discovery lines
# that had forced it to skip), so the two overlap deliberately: this one names
# the specific string, and pairs it with a reach marker, so a regression says
# WHICH line came back rather than only that something did.
#
# Both halves are load-bearing. The absence grep alone would pass vacuously on
# a run where no transport opened, so the reach marker is what makes it a
# measurement rather than a tautology.
test_clean_log
if grep -qa 'real_hw: transport=1' "$TEST_CLEAN_LOG"; then
    test_host_pass "kcs: the run opened a KCS transport (the ready line ran)"
    if grep -qa 'KCS transport ready' "$TEST_CLEAN_LOG"; then
        test_host_fail "kcs: 'transport ready' is a success line, not INFO"
        grep -a 'KCS transport ready' "$TEST_CLEAN_LOG" | sed 's/^/    /'
    else
        test_host_pass "kcs: 'transport ready' is a success line, not INFO"
    fi
else
    test_host_fail "kcs: no KCS transport opened — the BMC sim did not answer"
fi
test_host_summary "ipmi KCS log-level checks ($TEST_ARCH)"

test_count_results
