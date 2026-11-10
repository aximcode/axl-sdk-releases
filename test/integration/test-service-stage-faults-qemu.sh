#!/bin/bash
# test-meta: arch=both needs=mtools est=90 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-faults-qemu.sh -- axl_service_start_staged when the NV
# store refuses a write (AXL_SERVICE_STAGE_TEST_FAIL, the test launcher's
# fault seam). The rule: never trial or promote without the phase recorded.
#
#   the trial record fails        -> no trial: v1 runs, P.new kept, a warning;
#                                    the next start trials it: APPLIED
#   the promote:bak record fails  -> the candidate runs, nothing promoted, the
#                                    trial record stays; the next start counts
#                                    a boot that died: RETRIED, P = v2
#   the promote:swap record fails -> the candidate runs, P.bak made, P not
#                                    swapped; the next start finishes: APPLIED
#   the quarantine fails          -> logged, not retried: ROLLED_BACK, P.failed
#
# Usage: ./test/integration/test-service-stage-faults-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
echo "=== staged upgrade: NV write faults ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
V2="$B/upgrade-svc-dxe-q2.efi"
BROKEN="$B/upgrade-svc-dxe-broken.efi"
PP="fs0:\\$P"

staged_kept() { img_is "$P.new" "$V2" && img_is "$P" "$V1"; }
promoted()    { img_is "$P" "$V2" && img_is "$P.bak" "$V1" && ! img_has "$P.new"; }
unpromoted()  { img_is "$P" "$V1" && img_is "$P.new" "$V2" && ! img_has "$P.bak"; }
bak_only()    { img_is "$P" "$V1" && img_is "$P.new" "$V2" && img_is "$P.bak" "$V1"; }
verdictless() { not has "$1" "upgrade-svc: staged upgrade"; }

# The trial record fails: no trial.
reset_disk
img_put "$V2" "$P.new"
STAGE_FAIL=trial boot trialrec
check "trial record fails: v1 runs" has trialrec "$(up 1 none)"
check "trial record fails: v2 was not started" not has trialrec "$(up_v 2)"
check "trial record fails: said, naming what is kept" has trialrec \
    "could not be recorded; starting $PP, keeping $PP.new"
check "trial record fails: no verdict" verdictless trialrec
check "trial record fails: P.new kept, P untouched" staged_kept
boot trialrec2
check "trial record fails, next start: trialled and applied" line trialrec2 "$APPLIED"
check "trial record fails, next start: P = v2, .bak = v1" promoted

# The promote:bak record fails: the candidate runs, nothing is promoted.
reset_disk
img_put "$V2" "$P.new"
STAGE_FAIL=promote:bak boot bakrec
check "promote:bak record fails: the candidate runs" has bakrec "$(up_v 2)"
check "promote:bak record fails: said" has bakrec \
    "promote:bak could not be recorded; the promotion is left for the next start"
check "promote:bak record fails: no verdict" verdictless bakrec
check "promote:bak record fails: P, P.new untouched, no .bak" unpromoted
boot bakrec2
check "promote:bak record fails, next start: a retry" line bakrec2 "$RETRIED"
check "promote:bak record fails, next start: P = v2, .bak = v1" promoted

# The promote:swap record fails: .bak is made, P is not swapped.
reset_disk
img_put "$V2" "$P.new"
STAGE_FAIL=promote:swap boot swaprec
check "promote:swap record fails: the candidate runs" has swaprec "$(up_v 2)"
check "promote:swap record fails: said" has swaprec \
    "promote:swap could not be recorded; the promotion is left for the next start"
check "promote:swap record fails: no verdict" verdictless swaprec
check "promote:swap record fails: .bak made, P not swapped" bak_only
boot swaprec2
check "promote:swap record fails, next start: applied" line swaprec2 "$APPLIED"
check "promote:swap record fails, next start: P = v2, .bak = v1" promoted

# The quarantine fails: logged, the candidate is not retried.
reset_disk
img_put "$BROKEN" "$P.new"
STAGE_FAIL=quarantine boot quar
check "quarantine fails: rolled back" line quar "$ROLLED"
check "quarantine fails: said" has quar "could not be quarantined; it is not retried"
check "quarantine fails: v1 runs" has quar "$(up 1 rolled-back)"
check "quarantine fails: .failed = the candidate, no P.new" \
    eval 'img_is "$P.failed" "$BROKEN" && ! img_has "$P.new" && img_is "$P" "$V1"'
boot quar2
check "quarantine fails, next start: nothing retried" verdictless quar2
check "quarantine fails, next start: v1 runs" has quar2 "$(up 1 rolled-back)"

stage_finish "staged upgrade faults" 25
