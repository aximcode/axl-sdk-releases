#!/bin/bash
# test-meta: arch=both needs=mtools est=150 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-grace-qemu.sh -- the staged upgrade touched DURING a
# trial's commit grace (axl-service.h, axl_service_upgrade_in_flight).
#
# THE DEFECT. The launcher trials P.new, then waits the grace period at
# TPL_APPLICATION with the candidate already serving -- its handlers run above
# the launcher, and one that takes uploads can stage or unstage in that
# window. promote() then acted on whatever P.new held at that moment:
#   - a restage put an UNTRIALLED image in place as P (the trialled one lost,
#     P.bak the image before it);
#   - an unstage made the swap fail with promote:swap still recorded, and the
#     next start reported the good candidate ROLLED_BACK.
# Now a stage or unstage while the trial is in flight is AXL_BUSY, the last
# outcome is AXL_BUSY (no verdict yet), and promote() re-keys P.new before it
# copies and before it swaps, so a P.new written past the API is not promoted.
#
# Each case puts a candidate gN at P.new; gN, once up, acts 1 s into the
# test launcher's 3 s grace and prints
#   USVC: vN in grace: in_flight=<0|1> last_rc=<rc> rc=<rc>
#   g8   restages fs0:\restage.efi        -> AXL_BUSY; v8 promoted
#   g9   unstages                         -> AXL_BUSY; v9 promoted
#   g10  writes restage.efi over P.new    -> v10 NOT promoted, P stays v1,
#                                            the next start trials restage.efi
# The hand and clobber cases also check the record was cleared: the next
# start would otherwise name the stale record it found.
#   v2, and the test hook writes restage.efi over P.new right after
#   promote:swap is recorded (STAGE_CLOBBER) -- past the first re-key, at the
#   second, just before the swap -> not promoted, the record cleared
#
# Usage: ./test/integration/test-service-stage-grace-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
echo "=== staged upgrade: inside the commit grace ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
V5="$B/upgrade-svc-dxe-q5.efi"
G8="$B/upgrade-svc-dxe-g8.efi"
G9="$B/upgrade-svc-dxe-g9.efi"
G10="$B/upgrade-svc-dxe-g10.efi"
BUSY=-10   # AXL_BUSY

promoted_to() { img_is "$P" "$1" && img_is "$P.bak" "$V1" && ! img_has "$P.new"; }
# grace TAG VERSION IN_FLIGHT LAST_RC RC -- the candidate's in-grace line, exactly
grace() { line "$1" "USVC: v$2 in grace: in_flight=$3 last_rc=$4 rc=$5"; }

# g8: a restage inside the grace is refused; the trialled candidate is promoted.
# The Shell is held 8 s, so the candidate also reports once the launcher is
# done: nothing in flight, and the verdict is this attempt's (APPLIED = 1).
reset_disk
img_put "$G8" "$P.new"
img_put "$V5" restage.efi
STAGE_STALL=8000000 boot restage
check "restage: refused AXL_BUSY while in flight (no verdict yet)" \
    grace restage 8 1 "$BUSY" "$BUSY"
check "restage: after the grace, nothing in flight and the verdict is APPLIED" \
    line restage "USVC: v8 after grace: in_flight=0 last_rc=0 last=1"
check "restage: applied" line restage "$APPLIED"
check "restage: P = v8 (the trialled one), .bak = v1, nothing staged" promoted_to "$G8"
boot restage2
check "restage: the next boot runs v8, applied" has restage2 "$(up 8 applied)"

# g9: an unstage inside the grace is refused; the candidate is promoted
reset_disk
img_put "$G9" "$P.new"
img_put "$V5" restage.efi
boot unstage
check "unstage: refused AXL_BUSY while in flight" grace unstage 9 1 "$BUSY" "$BUSY"
check "unstage: applied" line unstage "$APPLIED"
check "unstage: P = v9, .bak = v1, nothing staged" promoted_to "$G9"
boot unstage2
check "unstage: the next boot runs v9, applied (not rolled back)" has unstage2 "$(up 9 applied)"

# g10: P.new written past the API inside the grace -- promote() re-keys it
reset_disk
img_put "$G10" "$P.new"
img_put "$V5" restage.efi
boot hand
check "hand: the write itself went through" grace hand 10 1 "$BUSY" 0
check "hand: the replacement is named" \
    has hand "$P.new changed during the trial of"
check "hand: no verdict line (NONE)" not has hand "upgrade-svc: staged upgrade"
check "hand: P is still v1, no .bak, P.new is the replacement" \
    eval 'img_is "$P" "$V1" && ! img_has "$P.bak" && img_is "$P.new" "$V5"'
boot hand2
# A trial record left set would make it "not the candidate trial 1 recorded".
check "hand: the record was cleared (no stale trial record found)" \
    not has hand2 "is not the candidate trial"
check "hand: the next start trials the replacement: applied" line hand2 "$APPLIED"
check "hand: v5 runs" has hand2 "$(up_v 5)"
check "hand: P = v5, .bak = v1" promoted_to "$V5"

# The same race at the last moment: P.new replaced after promote:swap is
# recorded, so only the re-key just before the swap can see it.
reset_disk
img_put "$B/upgrade-svc-dxe-q2.efi" "$P.new"
img_put "$V5" restage.efi
STAGE_CLOBBER=promote:swap boot clobber
check "clobber: the hook wrote P.new at promote:swap" \
    has clobber "written over P.new at promote:swap (rc=0)"
check "clobber: the replacement is named" has clobber "$P.new changed during the trial of"
check "clobber: no verdict line (NONE)" not has clobber "upgrade-svc: staged upgrade"
check "clobber: P is still v1 (.bak its copy), P.new is the replacement" \
    eval 'img_is "$P" "$V1" && img_is "$P.bak" "$V1" && img_is "$P.new" "$V5"'
boot clobber2
# A promote:swap record left set would be dropped here, by name.
check "clobber: the record was cleared (no stale promotion record found)" \
    not has clobber2 "was replaced during the promotion of"
check "clobber: the next start trials the replacement: applied" line clobber2 "$APPLIED"
check "clobber: P = v5, .bak = v1" promoted_to "$V5"

stage_finish "stage-grace" 24
