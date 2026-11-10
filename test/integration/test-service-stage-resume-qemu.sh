#!/bin/bash
# test-meta: arch=both needs=mtools est=190 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: a staged upgrade across real resets,
#   with the boot disk rewritten (mcopy) between boots.
# test-service-stage-resume-qemu.sh -- axl_service_start_staged's step 1:
# what the launcher does with what a power loss during a PROMOTION left, row
# by row (axl-service.h, the resume table). Each case cuts a boot right after
# a phase is recorded (AXL_SERVICE_STAGE_TEST_CUT), may damage the files as a
# FAT failure would (mtools, between boots), and boots again:
#
#   promote:bak, P.new present         -> redo the copy, finish: APPLIED, P = v2, .bak = v1
#   promote:bak, P.new gone            -> ROLLED_BACK, v1 runs
#   promote:swap, P.new present        -> finish the swap: APPLIED, P = v2
#   promote:bak/swap, P.new replaced   -> the record dropped, the new one trialled from 1
#   swap done, record not cleared      -> APPLIED, P = v2 (no second copy over .bak)
#   promote:swap, P.new gone, P = v1   -> the rename lost it: ROLLED_BACK, v1 runs
#   promote:swap, P and P.new gone     -> P restored from .bak: ROLLED_BACK, v1 runs
#   promote:swap, P, P.new, .bak gone  -> no fallback: the start fails (rc -5)
#   nothing recorded, P gone, .bak     -> P restored from .bak, v1 runs (NONE)
#   asides: one equal to .bak is removed, one equal to nothing is kept
#
# Usage: ./test/integration/test-service-stage-resume-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
echo "=== staged upgrade: resuming a promotion ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
V2="$B/upgrade-svc-dxe-q2.efi"

# The power-loss cut and the NV fault seam are the TEST launcher's
# (upgrade-svc-t.efi links the hook that reads their Shell variables): the
# library and the example as shipped carry no trace of either. bytes_in FILE TEXT: 0 present, 1 absent, 2 FILE unreadable --
# an unreadable file must not read as "absent".
bytes_in() {
    [[ -s "$1" ]] || return 2
    grep -qaF -- "$2" "$1"
    local rc=$?
    (( rc <= 1 )) && return "$rc"
    return 2
}
absent_from() { bytes_in "$1" "$2"; [[ $? -eq 1 ]]; }
for var in AXL_SERVICE_STAGE_TEST_CUT AXL_SERVICE_STAGE_TEST_FAIL; do
    check "the test launcher carries $var (control)" bytes_in "$B/upgrade-svc-t.efi" "$var"
    check "libaxl.a carries no trace of $var" absent_from "$B/lib/libaxl.a" "$var"
    check "the shipped example launcher carries no trace of $var" \
        absent_from "$B/upgrade-svc.efi" "$var"
done

promoted()   { img_is "$P" "$V2" && img_is "$P.bak" "$V1" && ! img_has "$P.new"; }
bak_is_v1()  { img_is "$P.bak" "$V1"; }
p_is_v1()    { img_is "$P" "$V1"; }

# cut_at PHASE TAG -- stage v2 on a clean disk and cut the boot at PHASE.
cut_at() {
    reset_disk
    img_put "$V2" "$P.new"
    boot "$2" "$1"
    check "$2: the boot was cut at $1" has "$2" "staged upgrade: test cut at $1 -- resetting"
}

# promote:bak, P.new present
cut_at promote:bak bak1
boot bak2
check "promote:bak + P.new: the launcher finishes the promotion" line bak2 "$APPLIED"
check "promote:bak + P.new: v2 runs" has bak2 "$(up 2 applied)"
check "promote:bak + P.new: P is v2, .bak is v1, no .new" promoted

# promote:bak, P.new gone (FAT damage)
cut_at promote:bak bakgone1
img_del "$P.new"
boot bakgone2
check "promote:bak, P.new gone: rolled back" line bakgone2 "$ROLLED"
check "promote:bak, P.new gone: v1 runs" has bakgone2 "$(up 1 rolled-back)"

# promote:swap, P.new present
cut_at promote:swap swap1
boot swap2
check "promote:swap + P.new: the launcher finishes the swap" line swap2 "$APPLIED"
check "promote:swap + P.new: v2 runs from P" has swap2 "$(up 2 applied)"
check "promote:swap + P.new: P is v2, .bak is v1, no .new" promoted

# the swap was done, the record not yet cleared
cut_at swapped done1
boot done2
check "swap done, record left: applied" line done2 "$APPLIED"
check "swap done, record left: v2 runs" has done2 "$(up 2 applied)"
check "swap done, record left: .bak is still v1" bak_is_v1

# promote:swap, the rename lost P.new (P still v1)
cut_at promote:swap lost1
img_del "$P.new"
boot lost2
check "promote:swap, P.new lost: rolled back" line lost2 "$ROLLED"
check "promote:swap, P.new lost: v1 runs" has lost2 "$(up 1 rolled-back)"
check "promote:swap, P.new lost: said why" \
    has lost2 "the promotion of"

# promote:swap, P and P.new both gone
cut_at promote:swap both1
img_del "$P.new"; img_del "$P"
boot both2
check "promote:swap, P and P.new gone: rolled back" line both2 "$ROLLED"
check "promote:swap, P and P.new gone: v1 runs, restored from .bak" has both2 "$(up 1 rolled-back)"
check "promote:swap, P and P.new gone: P is v1 again" p_is_v1

# ...and with no .bak either: no fallback
cut_at promote:swap none1
img_del "$P.new"; img_del "$P"; img_del "$P.bak"
boot none2
check "promote:swap, no image left at all: the start fails (rc -5)" \
    line none2 "upgrade-svc: start failed \(rc=-5\)"
check "promote:swap, no image left at all: said there is no fallback" \
    has none2 "and there is no fallback"

# promote:bak or promote:swap recorded, then P.new REPLACED (a restage after
# the cut): the record is the old candidate's, so it is cleared and the new
# one is trialled from 1 -- here a broken one, which a promotion without a
# trial would have put in place as P. Each row gets its own broken image (v3,
# v6): the first is quarantined by its trial, and would not be started again.
broken_failed() { img_is "$P.failed" "$1" && img_is "$P" "$V1" && ! img_has "$P.new"; }
for row in "promote:bak 3 upgrade-svc-dxe-broken.efi" "promote:swap 6 upgrade-svc-dxe-broken2.efi"; do
    read -r ph ver img <<< "$row"
    t="chg${ph#promote:}"
    cut_at "$ph" "${t}1"
    img_put "$B/$img" "$P.new"
    boot "${t}2"
    check "$ph, P.new replaced: the new candidate is trialled" \
        has "${t}2" "USVC: v$ver setup failing on purpose"
    check "$ph, P.new replaced: rolled back" line "${t}2" "$ROLLED"
    check "$ph, P.new replaced: v1 runs" has "${t}2" "$(up 1 rolled-back)"
    check "$ph, P.new replaced: P is v1, .failed is the new candidate" broken_failed "$B/$img"
done

# nothing recorded, P gone, .bak present
reset_disk
img_put "$V1" "$P.bak"
img_del "$P"
boot restore
check "P missing with .bak: v1 runs, restored" has restore "$(up 1 rolled-back)"
check "P missing with .bak: P is v1 again" p_is_v1

# asides
reset_disk
img_put "$V1" "$P.bak"
img_put "$V1" "~AXL0001.TMP"
head -c 4096 /dev/urandom > "$TEST_TMPDIR/junk.bin"
img_put "$TEST_TMPDIR/junk.bin" "~AXL0002.TMP"
boot asides
check "asides: the one equal to .bak is removed" not img_has "~AXL0001.TMP"
check "asides: the one equal to nothing is kept" img_has "~AXL0002.TMP"
check "asides: and logged by name" has asides "~AXL0002.TMP matches no image here; left in place"

stage_finish "staged upgrade resume" 47
