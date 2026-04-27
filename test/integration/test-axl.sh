#!/bin/bash
# AXL unit tests — boots QEMU and runs all test .efi applications.
#
# Usage: ./test/integration/test-axl.sh [--arch X64|AARCH64] [--log <path>]
#
# --log <path>   Save the raw QEMU serial log (full firmware boot,
#                Shell session, every test's PASS/FAIL lines) to the
#                given path. Equivalent to setting TEST_KEEP_LOG.

[[ -n "$DEBUG" ]] && set -x

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        --log)  TEST_KEEP_LOG="$2"; export TEST_KEEP_LOG; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64] [--log <path>]"; exit 1 ;;
    esac
done

test_setup

# Map test arch to Makefile arch — matches Makefile default PREFIX=out/native-$(ARCH)
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
TEST_APPS=(AxlTestMem AxlTestString AxlTestIO AxlTestLog AxlTestData AxlTestUtil AxlTestLoop AxlTestSmbus AxlTestTask AxlTestNet AxlTestIpmi AxlTestEvent AxlTestRuntime)

for app in "${TEST_APPS[@]}"; do
    test_add_efi "$NATIVE_DIR/$app.efi"
done

# Startup script: init network, then run each test app.
#
# Per-binary timing is extracted host-side from the `=== NAME Tests ===`
# header and the `=== Results: N passed, M failed ===` footer that
# each test emits via axl-test.h. No shell-level markers — an earlier
# revision emitted `echo ____BEGIN/____END` here, but those extra
# UEFI Shell echoes deterministically crashed AxlTestNet on aa64 with
# an ArmCpuDxe data-abort (translation fault) during the socket UDP
# async test's teardown path.
#
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    echo "connect -r"
    echo "stall 1000000"
    echo "ifconfig -s eth0 dhcp"
    echo "stall 3000000"
    echo ""
    for app in "${TEST_APPS[@]}"; do
        echo "$app.efi"
    done
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AXL Integration Tests ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_run_foreground 120
test_count_results
