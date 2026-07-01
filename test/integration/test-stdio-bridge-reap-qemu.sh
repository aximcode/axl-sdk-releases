#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# Stdio-bridge dead-instance leak: launchers that skip CRT0 atexit
# (--minimal-runtime, or gBS->Exit) leave their bridge protocol installed.
# Each launcher is a fresh image whose static install handle can't see prior
# images' bridges, so before the fix these dead instances accumulated one per
# invocation in `dh` (the do.efi/doDriver.efi symptom's second, separate leak).
#
# The fix reaps every dead-launcher bridge (a) at the start of each install and
# (b) in axl_shared_driver_unload.
#
# This test runs stdio-bridge-leak.efi TWICE from the shell (each installs a
# bridge then gBS->Exit's, leaking it), then stdio-bridge-reap-test.efi asserts
# the count did not accumulate (<=1) and that a shared_driver_unload reaps the
# residual to 0. RED before the fix (initial=2, after=2); GREEN after.
#
# Usage: ./test/integration/test-stdio-bridge-reap-qemu.sh [--arch X64|AARCH64]

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

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    stdio-bridge-reap-test 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/stdio-bridge-leak.efi"
test_add_efi "$NATIVE_DIR/stdio-bridge-reap-test.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "stdio-bridge-leak.efi"
    echo "stdio-bridge-leak.efi"
    echo "stdio-bridge-reap-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Stdio-bridge Reap Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log

pass=$(grep -c '^PASS:' "$TEST_CLEAN_LOG" || true)
fail=$(grep -c '^FAIL:' "$TEST_CLEAN_LOG" || true)

echo "--- results ---"
grep -E '^(PASS|FAIL|INFO):' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo ""
printf "Results: %d passed, %d failed\n" "$pass" "$fail"

if [[ $fail -eq 0 && $pass -ge 2 ]]; then
    echo "Stdio-bridge reap test: OK"
    exit 0
else
    echo "Stdio-bridge reap test: FAIL"
    exit 1
fi
