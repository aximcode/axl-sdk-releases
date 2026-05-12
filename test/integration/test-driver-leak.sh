#!/bin/bash
# Driver-load LoadOptions leak regression.
#
# Boots driver-leak-test.efi which exercises:
#   axl_driver_load -> axl_driver_set_load_options -> axl_driver_unload
#
# axl_driver_set_load_options copies the caller's buffer with axl_malloc
# and hands the pointer to the firmware via LoadedImage->LoadOptions.
# The firmware retains the pointer; AXL must free it on unload.
#
# Earlier axl_driver_unload only called gBS->UnloadImage and made no
# attempt to free the load-options copy — leaked one alloc per
# load+set+unload cycle (142 bytes in the axl-webfs reproducer).
#
# Test passes if all 4 PASS lines fire (load, set_load_options, unload,
# no-leak) AND no FAIL lines appear.
#
# Usage: ./test/integration/test-driver-leak.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

# Build both binaries
make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    driver driver-leak-test 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/driver.efi"
test_add_efi "$NATIVE_DIR/driver-leak-test.efi"

# Run the leak test, then shut down. driver.efi never starts (the test
# app deliberately skips axl_driver_start so its DriverEntry doesn't
# install protocols / allocate state that would muddy the leak check).
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "driver-leak-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Driver Load-Options Leak Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

pass=$(grep -c '^PASS:' "$TEST_CLEAN_LOG" || true)
fail=$(grep -c '^FAIL:' "$TEST_CLEAN_LOG" || true)

grep -E '^(PASS|FAIL):' "$TEST_CLEAN_LOG" | while IFS= read -r line; do
    echo "  $line"
done

echo ""
printf "Results: %d passed, %d failed\n" "$pass" "$fail"

# Expect at least 9 PASS lines: 4 from phase_basic, 2 from phase_reset,
# 3 from phase_table_full.
if [[ $fail -eq 0 && $pass -ge 9 ]]; then
    echo "Driver-leak test: OK"
    exit 0
else
    echo "Driver-leak test: FAILED"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
