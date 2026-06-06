#!/bin/bash
# Test the canonical Type-B (UEFI Driver Model) example driver: loads it and
# verifies the firmware actually drives the AxlDriverBinding lifecycle —
# Supported -> Start (with the bound interface) -> Stop — against the
# synthetic controller the example publishes.
#
# Usage: ./test/integration/test-binding-driver.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

# Build library + binding-driver example
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} binding-driver 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/binding-driver.efi"

# startup.nsh: load the driver (its entry self-drives connect/disconnect, so
# loading alone walks the whole lifecycle), then shut down.
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load binding-driver.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AXL Binding-Driver (Type-B) Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

# Assert each lifecycle stage fired, in the bound interface's own data so a
# stub that merely logged couldn't pass.
declare -a CHECKS=(
    "binding installed"
    "Supported: a widget controller is present and bindable"
    "Start: bound controller .* model=AXL-Widget-9000 rev=2"
    "Stop: released controller"
)

pass=0
fail=0
for re in "${CHECKS[@]}"; do
    if grep -Eq "$re" "$TEST_CLEAN_LOG"; then
        echo "  PASS: /$re/"
        pass=$((pass + 1))
    else
        echo "  FAIL: /$re/ not found"
        fail=$((fail + 1))
    fi
done

echo ""
printf "Results: %d passed, %d failed\n" "$pass" "$fail"

if [[ $fail -eq 0 ]]; then
    echo "Binding-driver test: OK (Supported -> Start -> Stop)"
    exit 0
else
    echo "Binding-driver test: FAILED"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
