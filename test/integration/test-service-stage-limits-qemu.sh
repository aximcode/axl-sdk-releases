#!/bin/bash
# test-meta: arch=both needs=mtools est=130 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-limits-qemu.sh -- axl_service_start_staged at the edges
# of a trial: the try limit, an invalid candidate, and P lost while a trial
# was recorded (axl-service.h, the resume table). The plain trial rows are in
# test-service-stage-trial-qemu.sh; the two were one suite until it neared
# CI's per-test limit on aa64. Each case stages by putting the candidate at
# P.new and may cut a boot right after a phase is recorded
# (AXL_SERVICE_STAGE_TEST_CUT):
#
#   trial:2 of 2, P.new changed      -> a new candidate: APPLIED, not given up
#   trial recorded, P and P.new gone -> P restored from .bak, v1 runs
#   an invalid P.new                 -> ROLLED_BACK, .failed = it, nothing quarantined
#   two boots die trying it (tries 2) -> given up: ROLLED_BACK, .failed = it
#   trial:N recorded, P.new invalid  -> the record cleared, the old candidate
#                                       neither retried nor quarantined
#
# Usage: ./test/integration/test-service-stage-limits-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
echo "=== staged upgrade: trial limits and invalid candidates ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
V2="$B/upgrade-svc-dxe-q2.efi"
V5="$B/upgrade-svc-dxe-q5.efi"

promoted_to() { img_is "$P" "$1" && img_is "$P.bak" "$V1" && ! img_has "$P.new"; }
failed_is()   { img_is "$P.failed" "$1" && ! img_has "$P.new" && img_is "$P" "$V1"; }

# (First: it leaves the last outcome APPLIED, which the restore case reads.)
# a different candidate when the record has used every try (trial:2 of 2):
# the new one is not charged the old one's dead boots
reset_disk
img_put "$V2" "$P.new"
boot full1 trial
boot full2 trial
check "full2: the second dead boot was trial 2 of 2" has full2 "trying again (2 of 2)"
img_put "$V5" "$P.new"
boot full3
check "trial:2 of 2, a different candidate: applied, not a retry" line full3 "$APPLIED"
check "trial:2 of 2, a different candidate: not given up" not has full3 "died trying"
check "trial:2 of 2, a different candidate: P = v5" promoted_to "$V5"

# trial recorded, then P.new AND P gone, .bak present: P is restored from
# .bak before it is started, on this path as on every other
reset_disk
img_put "$V1" "$P.bak"
img_put "$V2" "$P.new"
boot nop1 trial
img_del "$P.new"; img_del "$P"
boot nop2
check "trial recorded, P and P.new gone: said P was restored" \
    has nop2 "staged upgrade: fs0:\\$P was missing; restored from fs0:\\$P.bak"
check "trial recorded, P and P.new gone: v1 runs" has nop2 "$(up 1 applied)"
check "trial recorded, P and P.new gone: P is v1 again" img_is "$P" "$V1"

# an invalid P.new (not a driver image), nothing recorded
head -c 8192 /dev/urandom > "$TEST_TMPDIR/junk.efi"
JUNK="$TEST_TMPDIR/junk.efi"
reset_disk
img_put "$JUNK" "$P.new"
boot inv0
check "invalid P.new: said so" has inv0 "staged upgrade: fs0:\\$P.new is not a driver image for this machine"
check "invalid P.new: rolled back" line inv0 "$ROLLED"
check "invalid P.new: v1 runs" has inv0 "$(up 1 rolled-back)"
check "invalid P.new: it is .failed, P untouched" failed_is "$JUNK"

# given up after `tries` dead boots
reset_disk
img_put "$V5" "$P.new"
boot give1 trial
boot give2 trial
boot give3
check "two dead boots: the second was a retry" has give2 "trying again (2 of 2)"
check "two dead boots: given up, rolled back" line give3 "$ROLLED"
check "two dead boots: v1 runs" has give3 "$(up 1 rolled-back)"
check "two dead boots: the candidate is .failed" failed_is "$V5"

# an invalid P.new replacing a candidate whose trial is recorded: the record
# is the OLD candidate's -- it is cleared, the invalid file is .failed, and
# the old candidate is neither retried nor quarantined. Last, since v2 would
# be quarantined if this went wrong.
reset_disk
img_put "$V2" "$P.new"
boot ij1 trial
img_put "$JUNK" "$P.new"
boot ij2
check "trial:1, invalid P.new: rolled back" line ij2 "$ROLLED"
check "trial:1, invalid P.new: not a retry" not has ij2 "trying again"
check "trial:1, invalid P.new: it is .failed, P untouched" failed_is "$JUNK"

reset_disk
img_put "$V2" "$P.new"
boot ik1 trial
boot ik2 trial
img_put "$JUNK" "$P.new"
boot ik3
check "trial:2 of 2, invalid P.new: rolled back" line ik3 "$ROLLED"
check "trial:2 of 2, invalid P.new: the old candidate is not given up" not has ik3 "died trying"
check "trial:2 of 2, invalid P.new: it is .failed, P untouched" failed_is "$JUNK"
reset_disk
img_put "$V2" "$P.new"
boot ik4
check "trial:2 of 2, invalid P.new: the old candidate was not quarantined" line ik4 "$APPLIED"

stage_finish "staged upgrade trial limits" 22
