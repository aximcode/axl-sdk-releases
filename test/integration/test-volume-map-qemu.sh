#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# axl_volume_enumerate must name volumes from the UEFI Shell fsN map, not the
# LocateHandle(SimpleFileSystem) iteration index. The two orders diverge when
# the FS set is anything but trivial — reported as `do fd` printing the wrong
# type/label for a volume after `mkrd` (delldiags). A --mount volume reliably
# makes the LocateHandle order differ from the shell's fsN numbering, so the
# test exercises the divergence without special hardware.
#
# volume-map-test.efi asserts, for every volume the shell has a map entry for,
# that AxlVolume.name == the shell's own fsN alias for that volume's device
# path — before AND after creating a ramdisk (the mkrd scenario). RED before
# the fix (index names mismatch the shell), GREEN after.
#
# Usage: ./test/integration/test-volume-map-qemu.sh [--arch X64|AARCH64]

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
    volume-map-test 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"

# A host directory mounted as an extra filesystem is what makes the
# LocateHandle order diverge from the shell's fsN numbering.
_hostfs="$TEST_TMPDIR/volmap-hostfs"
mkdir -p "$_hostfs"
echo "marker" > "$_hostfs/marker.txt"

_log="$TEST_TMPDIR/volmap-serial.log"

echo "=== Volume fsN-map Test ($TEST_ARCH) ==="

# run-qemu.sh --mount exposes $_hostfs as a second filesystem the shell maps
# as fsN, alongside the boot ESP — the setup that reproduces the divergence.
# The app powers off when done; the verdict is read from the serial log, so a
# non-zero run-qemu exit (timeout fallback) must not abort under `set -e`.
timeout 120 "$PROJECT_DIR/scripts/run-qemu.sh" \
    --arch "$TEST_ARCH" \
    --mount "$_hostfs" \
    --serial-log "$_log" \
    --timeout 45 \
    "$NATIVE_DIR/volume-map-test.efi" 2>&1 | tail -3 || true

pass=$(grep -c '^PASS:' "$_log" 2>/dev/null || true)
fail=$(grep -c '^FAIL:' "$_log" 2>/dev/null || true)

echo "--- results ---"
grep -E '^(PASS|FAIL|INFO):' "$_log" 2>/dev/null | sed 's/^/  /'
echo ""
printf "Results: %d passed, %d failed\n" "$pass" "$fail"

if [[ $fail -eq 0 && $pass -ge 4 ]]; then
    echo "Volume fsN-map test: OK"
    exit 0
else
    echo "Volume fsN-map test: FAIL"
    exit 1
fi
