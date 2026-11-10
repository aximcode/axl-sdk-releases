#!/bin/bash
# test-meta: arch=both needs= est=10 local-only=0 builds=driver,driver-identity-test guest=standard
# Embedded/buffer-loaded driver image identity.
#
# A buffer load (gBS->LoadImage with DevicePath=NULL) leaves the loaded
# image's LoadedImage->FilePath and gEfiLoadedImageDevicePathProtocol
# interface NULL. The x64 UEFI shell tolerates this; the aarch64 shell
# faults with a Synchronous Exception while rendering the handle's device
# path under `dh -p` / `dh -v`. AXL now synthesizes a
# MemoryMapped(...)/FilePath device path after the load.
#
# driver-identity-test.efi buffer-loads driver.efi, tags the image handle
# with a private MARKER protocol, and asserts in-process that the loaded
# image's device path + FilePath are non-NULL and render to text. The
# startup.nsh then runs `dh -p <marker>` / `dh -v -p <marker>` against that
# exact handle; this script asserts no Synchronous Exception and that the
# rendered device path appears. Its Shell commands are
# test-driver-identity-qemu.nsh; under `axlpool run` they run on a pooled
# guest instead of a boot.
#
# RED (before the fix): aa64 raises a Synchronous Exception and the
# in-process device-path assertions FAIL. GREEN (after the fix): no
# exception, all PASS, device path shown.
#
# Usage: ./test/integration/test-driver-identity-qemu.sh [--arch X64|AARCH64]

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

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" \
    driver driver-identity-test 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"
test_add_efi "$NATIVE_DIR/driver.efi"
test_add_efi "$NATIVE_DIR/driver-identity-test.efi"

# Run the test app (loads driver.efi as a buffer, tags the image handle),
# then inspect that handle from the shell (test-driver-identity-qemu.nsh).
# `dh -v` over the NULL device path is what faults on aa64 before the fix.
test_use_nsh

test_build_image

echo "=== Driver Image-Identity Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 40

test_clean_log

pass=$(grep -c '^PASS:' "$TEST_CLEAN_LOG" || true)
fail=$(grep -c '^FAIL:' "$TEST_CLEAN_LOG" || true)
exc=$(grep -ciE 'Synchronous Exception|Exception .*Type' "$TEST_CLEAN_LOG" || true)
devpath=$(grep -c '^DEVPATH: ' "$TEST_CLEAN_LOG" || true)
dhdone=$(grep -c '^DH_DONE' "$TEST_CLEAN_LOG" || true)

echo "--- in-process results ---"
grep -E '^(PASS|FAIL|DEVPATH|MARKER):' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo "--- shell dh output (around marker) ---"
sed -n '/DH_PLAIN_BEGIN/,/DH_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

echo ""
printf "Results: %d passed, %d failed; exceptions=%d devpath=%d dh_done=%d\n" \
    "$pass" "$fail" "$exc" "$devpath" "$dhdone"

# GREEN requires: all in-process checks pass (4 real assertions: marker
# install, device-path non-NULL, renders to text, FilePath non-NULL), no
# Synchronous Exception, the device path was rendered in-process, and the
# shell reached DH_DONE (i.e. `dh -v` did not crash the shell mid-script).
if [[ $fail -eq 0 && $pass -ge 4 && $exc -eq 0 && $devpath -ge 1 && $dhdone -ge 1 ]]; then
    echo "Driver image-identity test: OK"
    exit 0
else
    echo "Driver image-identity test: FAIL"
    exit 1
fi
