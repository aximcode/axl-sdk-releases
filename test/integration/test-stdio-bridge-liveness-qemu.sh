#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# Handle-reuse false-alive regression: a stdio-bridge instance that fools the
# pre-v2.7.1 LoadedImage-proto liveness match (alive image + matching proto)
# must be rejected by the per-dispatch token gate and reaped. The fixture
# installs such a decoy, sets a distinct AxlDispatchToken current, triggers the
# reap via axl_shared_driver_unload, and asserts the bridge handle lost the
# protocol. RED before the fix (proto-match keeps it); GREEN after.
#
# Usage: ./test/integration/test-stdio-bridge-liveness-qemu.sh [--arch X64|AARCH64]

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
    stdio-bridge-liveness-test 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/stdio-bridge-liveness-test.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "stdio-bridge-liveness-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Stdio-bridge Liveness Test ($TEST_ARCH) ==="

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

if [[ $fail -eq 0 && $pass -ge 1 ]]; then
    echo "Stdio-bridge liveness test: OK"
    exit 0
else
    echo "Stdio-bridge liveness test: FAIL"
    exit 1
fi
