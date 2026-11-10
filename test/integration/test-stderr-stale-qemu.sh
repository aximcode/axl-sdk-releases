#!/bin/bash
# test-meta: arch=x64 needs= est=30 local-only=0 builds=stderr-stale guest=none
# guest=none: written for the X64 debugcon (TEST_QEMU_DEBUGCON), which a
#   pooled X64 guest did not have; every one has it now, and this has not
#   been tried pooled yet (it reads load lines from the run's part alone).
# test-stderr-stale-qemu.sh -- a resident image never writes to a Shell
# handle the Shell has freed.
#
# A Shell installs EFI_SHELL_PARAMETERS_PROTOCOL on an image it runs by name,
# and when StartImage returns it uninstalls them and closes a redirect's file
# (ShellProtocol.c, InternalShellExecuteDevicePath; RestoreStdInStdOutStdErr).
# The EDK2 Shell refuses to run a DRIVER by name ("The image is not an
# application"), so stderr-stale.efi does that sequence itself for the
# service's driver image, stderr_tick-dxe.efi, with t.txt as its StdErr: the
# driver stays resident, holding Shell parameters only until StartImage
# returns. Its first line is due 1 s later, and each line is written raw
# too, THROUGH the handle. An image that cached that StdErr
# would write through the freed file: on DEBUG OVMF, whose pool is cleared
# when freed, a fault or an ASSERT -- or, with the pages reused, its lines in
# t.txt. Reading the handles live, it finds no Shell parameters and writes to
# the console.
#
# Asserts: the Shell reaches the end of the script (no fault), the firmware
# logged no ASSERT and no exception, the service wrote its 70 lines, and
# none is in t.txt.
#
# DEBUG OVMF is x86 only, and the cleared-on-free pool is what makes a stale
# write fault rather than land quietly; without it the test SKIPs, and a
# certifying run (release-suite.sh) FAILS it instead.
#
# Usage: ./test/integration/test-stderr-stale-qemu.sh [--arch X64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"

if [[ "$TEST_ARCH" != X64 ]]; then
    echo "SKIP: DEBUG OVMF is x86 only ($TEST_ARCH)"
    exit 77
fi
# find_debug_ovmf (scripts/axl-common.sh) is where every DEBUG-OVMF test
# looks: AXL_OVMF_DEBUG_DIR, else the EDK2 or $HOME build tree. Without one
# this SKIPs -- or FAILS under release-suite.sh (AXL_REQUIRE_DEBUG_OVMF=1).
find_debug_ovmf || debug_ovmf_unavailable
export OVMF_CODE="$DEBUG_OVMF_CODE"
export OVMF_VARS="$DEBUG_OVMF_VARS"

test_setup

_a=x64
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" stderr-stale 2>&1 | tail -2
BUILD_DIR="$(test_build_dir x64)"
test_add_efi "$BUILD_DIR/stderr-stale.efi"
test_add_efi "$BUILD_DIR/stderr_tick-dxe.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "stderr-stale.efi"
    # The service's 70 lines run from 1 s to 8 s after it starts.
    echo "stall 10000000"
    echo "echo STALE_TYPE"
    echo "type t.txt"
    echo "echo STALE_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== stderr: no write to a handle the Shell freed ($TEST_ARCH, DEBUG OVMF) ==="

DEBUGCON="$TEST_TMPDIR/debugcon.log"
export TEST_QEMU_DEBUGCON="$DEBUGCON"
test_build_qemu_cmd
test_add_no_network
test_run_foreground 60
test_clean_log

PASS=0
FAIL=0
check() {
    if [[ "$1" == 1 ]]; then
        echo "  PASS: $2"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $2"; FAIL=$((FAIL + 1))
    fi
}

# The debugcon log must exist and carry the firmware's own output, or its
# silence about ASSERTs says nothing.
seen_dbg=0
[[ -s "$DEBUGCON" ]] && grep -aq "Loading driver" "$DEBUGCON" && seen_dbg=1
check "$seen_dbg" "the firmware's DEBUG log was captured"

done_marker=$(grep -acx 'STALE_DONE' "$TEST_CLEAN_LOG" || true)
check "$(( done_marker >= 1 ))" "the Shell ran the script to its end"

started=$(grep -acx 'stderr-stale: started rc=0, parameters gone, file closed' "$TEST_CLEAN_LOG" || true)
check "$(( started == 1 ))" "the driver started with Shell parameters, which are gone and their file closed"

faults=$( { grep -acE 'ASSERT|!!!! .*Exception' "$DEBUGCON" "$TEST_CLEAN_LOG" || true; } \
    | awk -F: '{ n += $NF } END { print n + 0 }')
check "$(( faults == 0 ))" "no ASSERT and no exception ($faults)"

ticks=$( { grep -aoE 'SR-TICK [0-9]+' "$TEST_CLEAN_LOG" || true; } | wc -l)
check "$(( ticks >= 70 ))" "the service wrote its 70 lines after the command returned ($ticks)"

if test_slice_log "STALE_TYPE" "STALE_DONE" "$TEST_TMPDIR/type.txt"; then
    in_file=$( { grep -aoE 'SR-TICK [0-9]+' "$TEST_TMPDIR/type.txt" || true; } | wc -l)
    check "$(( in_file == 0 ))" "none is in the closed redirect's file ($in_file)"
else
    check 0 "none is in the closed redirect's file (no STALE_TYPE..STALE_DONE window)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
(( FAIL == 0 ))
