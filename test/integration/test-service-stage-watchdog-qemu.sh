#!/bin/bash
# test-meta: arch=both needs=mtools est=110 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: the UEFI watchdog resets a hung
#   trial, across real boots.
# test-service-stage-watchdog-qemu.sh -- a staged candidate that HANGS in its
# entry point (its setup never returns). Nothing returns to the launcher, so
# only the UEFI watchdog the trial arms (30 s, the test launcher's
# watchdog_sec) can reset the box:
#   boot 1: trial 1 hangs; the watchdog resets
#   boot 2: a boot died trying it, tries is 2: trial 2 hangs; the watchdog resets
#   boot 3: two boots died: given up -- ROLLED_BACK, the candidate is .failed
#           and quarantined, v1 runs
#
# Usage: ./test/integration/test-service-stage-watchdog-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
# A boot that hangs ends only on the 30 s watchdog.
STAGE_BOOT_SECS=$((STAGE_BOOT_SECS + 40))
echo "=== staged upgrade: a candidate that hangs ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
HANG="$B/upgrade-svc-dxe-hang.efi"
gave_up() { img_is "$P.failed" "$HANG" && ! img_has "$P.new" && img_is "$P" "$V1"; }

reset_disk
img_put "$HANG" "$P.new"
boot hang1
check "boot 1: the candidate's setup hangs" has hang1 "USVC: v4 setup hanging on purpose"
check "boot 1: the launcher never returned" not has hang1 "BOOT-END"
check "boot 1: the watchdog reset the box (not the harness timeout)" ended_itself hang1
boot hang2
check "boot 2: a boot died trying it; trial 2 of 2" has hang2 "trying again (2 of 2)"
check "boot 2: it hangs again" has hang2 "USVC: v4 setup hanging on purpose"
check "boot 2: the launcher never returned" not has hang2 "BOOT-END"
check "boot 2: the watchdog reset the box (not the harness timeout)" ended_itself hang2
boot hang3
check "boot 3: given up after two dead boots" has hang3 "2 boot(s) died trying"
check "boot 3: rolled back" line hang3 "$ROLLED"
check "boot 3: it was not started a third time" not has hang3 "USVC: v4 setup hanging on purpose"
check "boot 3: v1 runs" has hang3 "$(up 1 rolled-back)"
check "boot 3: the candidate is .failed, P untouched" gave_up

stage_finish "staged upgrade watchdog" 12
