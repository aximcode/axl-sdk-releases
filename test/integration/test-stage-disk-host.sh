#!/bin/bash
# test-meta: arch=none needs=mkfs.vfat,mcopy,mdir,mtype est=5 local-only=0
# test-stage-disk-host.sh -- qemu_stage_disk (scripts/axl-common.sh), which
# builds the FAT image every QEMU test and run-qemu.sh boot from, returns
# non-zero when the image was not made.
#
# Scenarios:
#   control    an empty and a one-file staging dir each return 0 and give a
#              FAT image mdir can list, the file in it
#   dd         a target whose directory does not exist: dd cannot write it,
#              so the call fails (an EMPTY staging dir has nothing for mcopy
#              to fail on, so the dd failure must be the result on its own)
#   mkfs       a label longer than FAT's 11 characters: dd writes the image,
#              mkfs.vfat refuses to format it, and the call fails
#
# Usage: ./test/integration/test-stage-disk-host.sh

export TEST_SKIP_RATCHET=1

for t in mkfs.vfat mcopy mdir mtype; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "SKIP: no $t on the host PATH (needs=$t)"
        exit 77
    fi
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
unset MKIMAGE_DIR   # the mtools path, which CI and the host tools use
# shellcheck source=../../scripts/axl-common.sh
source "$ROOT/scripts/axl-common.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1"
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}

# stage STAGING TARGET [LABEL]: run qemu_stage_disk in a subshell, set RC.
stage() {
    RC=0
    (qemu_stage_disk "$1" "$2" "${3:-TEST}") > "$TMP/out.txt" 2>&1 || RC=$?
}

echo "=== qemu_stage_disk ==="

# --- control ----------------------------------------------------------------
mkdir -p "$TMP/empty" "$TMP/one"
printf 'hello\n' > "$TMP/one/hello.txt"
stage "$TMP/empty" "$TMP/empty.img"
check "control: an empty staging dir returns 0 (rc=$RC)" test "$RC" = 0
lists() { mdir -i "$1" ::/ > /dev/null 2>&1; }
check "control: ... and gives an image mdir can list" lists "$TMP/empty.img"
stage "$TMP/one" "$TMP/one.img"
check "control: a one-file staging dir returns 0 (rc=$RC)" test "$RC" = 0
check "control: ... and the file is in the image" \
    test "$(mtype -i "$TMP/one.img" ::/hello.txt 2>/dev/null)" = hello

# --- dd ---------------------------------------------------------------------
stage "$TMP/empty" "$TMP/no-such-dir/disk.img"
check "dd: a target dd cannot write fails the call (rc=$RC)" test "$RC" != 0

# --- mkfs -------------------------------------------------------------------
stage "$TMP/empty" "$TMP/label.img" LABELTOOLONG
check "mkfs: an image mkfs.vfat refuses to format fails the call (rc=$RC)" test "$RC" != 0

EXPECTED=6
echo ""
printf "qemu_stage_disk: %d passed, %d failed\n" "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -eq $EXPECTED ]] && exit 0 || exit 1
