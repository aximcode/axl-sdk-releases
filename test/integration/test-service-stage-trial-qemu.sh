#!/bin/bash
# test-meta: arch=both needs=mtools est=110 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-trial-qemu.sh -- axl_service_start_staged with a TRIAL
# recorded, and the trial itself, row by row (axl-service.h, the resume
# table). Each case stages by putting the candidate at P.new and may cut a
# boot right after a phase is recorded (AXL_SERVICE_STAGE_TEST_CUT):
#
#   staged, --no-staged             -> nothing applied, P.new kept, v1 runs
#   staged                          -> APPLIED, P = v2, .bak = v1
#   trial:1 recorded, P.new gone    -> the record cleared, v1 runs, nothing reported
#   trial:1 recorded, P.new changed -> a new candidate: trial 1 again, APPLIED (not RETRIED)
#   trial:1 recorded, same P.new    -> trial 2: RETRIED
#   a broken candidate              -> ROLLED_BACK, .failed = it, v1 runs
#   quarantined, then rename cut    -> the next boot finishes step 4: ROLLED_BACK
#   P missing, no .bak (first)      -> promoted by rename, no fallback: APPLIED
#
# The try limit and invalid candidates are in test-service-stage-limits-qemu.sh.
#
# Usage: ./test/integration/test-service-stage-trial-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
echo "=== staged upgrade: trials ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
V2="$B/upgrade-svc-dxe-q2.efi"
V5="$B/upgrade-svc-dxe-q5.efi"
BROKEN="$B/upgrade-svc-dxe-broken.efi"
BROKEN2="$B/upgrade-svc-dxe-broken2.efi"

promoted_to() { img_is "$P" "$1" && img_is "$P.bak" "$V1" && ! img_has "$P.new"; }
failed_is()   { img_is "$P.failed" "$1" && ! img_has "$P.new" && img_is "$P" "$V1"; }
staged_kept() { img_is "$P.new" "$V2" && img_is "$P" "$V1"; }
first_ok()    { img_is "$P" "$V2" && ! img_has "$P.bak" && ! img_has "$P.new"; }

# --no-staged: nothing applied
reset_disk
img_put "$V2" "$P.new"
boot nostaged "" --no-staged
check "--no-staged: v1 runs" has nostaged "$(up 1 none)"
check "--no-staged: no verdict line" not has nostaged "upgrade-svc: staged upgrade"
check "--no-staged: P.new is kept for the next start" staged_kept

# staged -> applied
boot plain
check "staged: applied" line plain "$APPLIED"
check "staged: v2 runs" has plain "$(up_v 2)"
check "staged: P = v2, .bak = v1" promoted_to "$V2"

# trial recorded, P.new gone (unstaged)
reset_disk
img_put "$V2" "$P.new"
boot gone1 trial
check "gone1: the boot was cut at the trial" has gone1 "staged upgrade: test cut at trial -- resetting"
img_del "$P.new"
boot gone2
check "trial recorded, P.new gone: v1 runs, the last outcome untouched" has gone2 "$(up 1 applied)"
check "trial recorded, P.new gone: no verdict line" not has gone2 "upgrade-svc: staged upgrade"

# trial recorded, P.new replaced by another candidate
reset_disk
img_put "$V2" "$P.new"
boot other1 trial
img_put "$V5" "$P.new"
boot other2
check "a different candidate: its trial starts again -- applied, not a retry" line other2 "$APPLIED"
check "a different candidate: v5 runs" has other2 "$(up_v 5)"
check "a different candidate: P = v5" promoted_to "$V5"

# trial recorded, same candidate -> retried
reset_disk
img_put "$V2" "$P.new"
boot retry1 trial
boot retry2
check "the same candidate after a dead boot: applied on a retry" line retry2 "$RETRIED"
check "the same candidate after a dead boot: v2 runs" has retry2 "$(up_v 2)"
check "the same candidate after a dead boot: P = v2" promoted_to "$V2"

# a broken candidate whose trial is cut after its quarantine: the next boot
# finishes step 4 without starting it
reset_disk
img_put "$BROKEN2" "$P.new"
boot quar1 quarantined
check "quar1: the trial failed, and the boot was cut after the quarantine" \
    has quar1 "staged upgrade: test cut at quarantined -- resetting"
boot quar2
check "quarantined with a trial recorded: rolled back" line quar2 "$ROLLED"
check "quarantined with a trial recorded: it was not started again" \
    not has quar2 "USVC: v6 setup failing on purpose"
check "quarantined with a trial recorded: it is .failed" failed_is "$BROKEN2"

# a broken candidate
reset_disk
img_put "$BROKEN" "$P.new"
boot broken
check "broken: its setup ran" has broken "USVC: v3 setup failing on purpose"
check "broken: rolled back" line broken "$ROLLED"
check "broken: v1 runs" has broken "$(up 1 rolled-back)"
check "broken: it is .failed, P untouched" failed_is "$BROKEN"

# the same broken image put back by hand: quarantined, never started
reset_disk
img_put "$BROKEN" "$P.new"
boot again
check "a quarantined candidate with nothing recorded: rolled back" line again "$ROLLED"
check "a quarantined candidate with nothing recorded: never started" \
    not has again "USVC: v3 setup failing on purpose"

# first upgrade: P missing, no .bak
reset_disk
img_del "$P"
img_put "$V2" "$P.new"
boot first
check "first upgrade with no P: applied" line first "$APPLIED"
check "first upgrade with no P: said there is no fallback" \
    has first "is missing; promoting with no fallback image"
check "first upgrade with no P: v2 runs from P" has first "$(up_v 2)"
check "first upgrade with no P: P = v2, no .bak" first_ok

stage_finish "staged upgrade trials" 29
