#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# Shared-driver "transient launcher orphans the resident driver" image leak.
#
# A launcher built on the shared-driver pattern records ParentImageHandle =
# gImageHandle (itself) when it LoadImage's the resident driver. The launcher
# is a short-lived EFI_APPLICATION: the firmware auto-unloads it when it
# returns, so the driver's recorded parent handle is dead by the time a later
# image unloads the driver. Before the fix, gBS->UnloadImage returned
# EFI_SUCCESS and unpublished the protocol correctly, yet never reclaimed the
# driver's image handle + pages — they accumulated in `dh` on every cycle.
#
# driver-parent-leak-test.efi drives the whole scenario in one observing image
# (see its header) and asserts the loaded-image handle count drops by exactly
# one across the unload. RED before the fix (count unchanged, handle survives),
# GREEN after (handle reclaimed).
#
# Usage: ./test/integration/test-driver-parent-leak-qemu.sh [--arch X64|AARCH64]

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
    driver-parent-leak-test 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/driver-parent-leak-test.efi"
# Launcher ONLY — the driver is intentionally NOT staged on disk, so the
# launcher falls back to its embedded blob (a buffer load), which is the path
# that synthesizes a LoadedImageDevicePath and triggers the cross-image leak.
test_add_efi "$NATIVE_DIR/stdio-bridge-fix.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "driver-parent-leak-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Driver Parent-Orphan Image-Leak Test ($TEST_ARCH) ==="

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

# GREEN requires all 7 in-process assertions to pass and none to fail.
if [[ $fail -eq 0 && $pass -ge 7 ]]; then
    echo "Driver parent-orphan image-leak test: OK"
    exit 0
else
    echo "Driver parent-orphan image-leak test: FAIL"
    exit 1
fi
