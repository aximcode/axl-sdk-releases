#!/bin/bash
# test-meta: arch=X64 est=45 local-only=1
# test-kbtune-driver-qemu.sh — hazard-safe lifecycle test for the kbtune-drv
# resident console-conditioning shim.
#
# Loads kbtune-drv (installs the ConIn/ConInEx wrap), round-trips its {get,set}
# config channel, and unloads it (must cleanly unpublish + restore the console).
# Asserts only clean negatives; the pure conditioning logic is unit-tested in
# axl-test-input. Runs kbtune-drv-test.efi in ISOLATION with its own timeout so a
# wedge on load/unload can't starve the main suite (feedback_uefi_firmware_test_hazards).
#
# Usage: ./test/integration/test-kbtune-driver-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

TEST_ARCH="${TEST_ARCH:-X64}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
_arch_lc=$(echo "$TEST_ARCH" | tr 'A-Z' 'a-z'); [[ "$_arch_lc" == "aarch64" ]] && _arch_lc="aa64"
DRV="$(test_build_dir "$_arch_lc")/tools/kbtune-drv.efi"
APP="$(test_build_dir "$_arch_lc")/kbtune-drv-test.efi"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

make -C "$PROJECT_DIR" ARCH="$_arch_lc" kbtune-drv kbtune-drv-test >/dev/null 2>&1
if [[ ! -f "$DRV" || ! -f "$APP" ]]; then
    echo "WARN: kbtune-drv / test app not built; skipping."
    echo "test-kbtune-driver: SKIP"; exit 0
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
# End with `reset -s` so QEMU powers off promptly instead of idling to --timeout
# (feedback_custom_nsh_reset_s).
NSH="$WORK/kbt.nsh"
printf '@echo -off\nkbtune-drv-test.efi\nreset -s\n' > "$NSH"

# Stage the driver as a sibling of the test app (both land at the fsN: root, so
# axl_shared_driver_locate_sibling resolves kbtune-drv.efi).
timeout 90 "$RUN_QEMU" --arch "$TEST_ARCH" --nsh "$NSH" \
    --extra "$DRV:kbtune-drv.efi" \
    --timeout 40 "$APP" >"$WORK/run.log" 2>&1 || true

echo "--- guest output ---"
grep -E "PASS:|FAIL:|Results:" "$WORK/run.log" || { echo "(no test output — boot/stage failure)"; cat "$WORK/run.log" | tail -20; }

# Ratchet on the printed footer: every assertion must pass and none fail.
line=$(grep -E "=== Results:" "$WORK/run.log" | tail -1)
if [[ "$line" =~ ([0-9]+)\ passed,\ ([0-9]+)\ failed ]]; then
    p="${BASH_REMATCH[1]}"; f="${BASH_REMATCH[2]}"
    echo "kbtune-drv lifecycle: $p passed, $f failed"
    [[ "$f" -eq 0 && "$p" -ge 13 ]] && exit 0
fi
echo "kbtune-drv lifecycle: FAILED (no clean Results footer)"
exit 1
