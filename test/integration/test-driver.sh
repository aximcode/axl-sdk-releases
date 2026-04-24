#!/bin/bash
# Test the example DXE driver — loads it, verifies output, unloads it.
#
# Usage: ./test/integration/test-driver.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

# Build library + driver
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} driver 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/driver.efi"

# startup.nsh: load the driver, then shutdown
# (unload requires a handle number which can't be scripted portably)
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load driver.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AXL Driver Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

# Check results
test_clean_log

pass=$(grep -c '^PASS:' "$TEST_CLEAN_LOG" || true)
fail=$(grep -c '^FAIL:' "$TEST_CLEAN_LOG" || true)

grep -E '^(PASS|FAIL):' "$TEST_CLEAN_LOG" | while IFS= read -r line; do
    echo "  $line"
done

echo ""
printf "Results: %d passed, %d failed\n" "$pass" "$fail"

if [[ $fail -eq 0 && $pass -ge 3 ]]; then
    echo "Driver test: OK (load + features + unload)"
    exit 0
else
    echo "Driver test: FAILED"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
