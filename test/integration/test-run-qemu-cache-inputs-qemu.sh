#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=0 guest=none
# guest=none: it drives run-qemu.sh's BOOTED path itself (AXL_RUN_QEMU_POOL=0),
#   which is the path under test; a lease would take the pooled one.
# test-run-qemu-cache-inputs-qemu.sh -- a booted run-qemu.sh launch records,
# as the skip cache's inputs, everything it hands the guest from the tree.
#
# The booted path once recorded the image and firmware only, while the pooled
# path recorded --extra files and the --nsh too, so a booted test whose ids
# file or script reaches the guest that way (test-lsusb-qemu.sh's
# share/usb-ids.json5) read FRESH after an edit to it. test-run-qemu-flags.sh
# checks the LIST (the dry run prints it); a dry run exits before the launch,
# so only a real boot shows the launch records that list. One boot: a dummy
# image, an --extra file and an nsh that echoes a marker and resets, ~2 s.
#
# Usage: ./test/integration/test-run-qemu-cache-inputs-qemu.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$PROJECT_DIR" || exit 1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
printf 'x' > "$WORK/dummy.efi"
printf 'v1' > "$WORK/extra.bin"
printf 'echo RQ-CACHE-BOOTED\r\nreset -s\r\n' > "$WORK/custom.nsh"
mkdir "$WORK/cache"

echo "=== a booted launch records its --extra files and --nsh ==="
# A booted, unprofiled launch whatever the runner exported: AXL_TEST_CACHE_MODE
# =pool would name a @pool inputs file, and AXL_TEST_PROFILE would credit this
# boot to a test that does not exist.
env -u AXL_TEST_PROFILE AXL_TEST_CACHE_MODE= \
    AXL_TEST_CACHE="$WORK/cache" AXL_TEST_PROFILE_NAME=test-fixture.sh AXL_TEST_CACHE_ARCH=X64 \
    AXL_RUN_QEMU_POOL=0 timeout 90 "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 60 \
    --extra "$WORK/extra.bin:e.bin" --nsh "$WORK/custom.nsh" \
    --serial-log "$WORK/serial.log" "$WORK/dummy.efi" > "$WORK/out" 2>&1
rc=$?
# Control: the guest really ran the nsh, so the run reached the launch -- a
# run that died before it records nothing, and the checks below would be
# measuring that instead.
if [[ $rc -eq 0 ]] && grep -qx 'RQ-CACHE-BOOTED' <(tr -d '\r' < "$WORK/serial.log" 2>/dev/null); then
    pass "the booted guest ran the custom nsh (the launch happened)"
else
    fail "the boot did not run the nsh (rc=$rc) -- the checks below would be vacuous; tail: $(tail -3 "$WORK/out" | tr '\n' ' ')"
fi
IN="$WORK/cache/test-fixture.sh@X64.inputs"
# ...and the PCI ids file every launch stages beside the image.
for f in "$WORK/dummy.efi" "$WORK/extra.bin" "$WORK/custom.nsh" \
         "$PROJECT_DIR/share/pci-ids.json5"; do
    if grep -qxF "$f" "$IN" 2>/dev/null; then pass "recorded: ${f##*/}"
    else fail "not recorded: $f (inputs: $(tr '\n' ' ' < "$IN" 2>/dev/null))"; fi
done
if grep -qF ':e.bin' "$IN" 2>/dev/null; then
    fail "an --extra was recorded as SRC:DEST, a path that does not exist (a permanent miss)"
else pass "an --extra is recorded by its source path, not SRC:DEST"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]]
