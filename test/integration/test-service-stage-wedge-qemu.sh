#!/bin/bash
# test-meta: arch=both needs=mtools est=160 local-only=0 builds=upgrade-svc-test guest=none
# guest=none: the boot is the subject: the UEFI watchdog resets a wedged
#   trial, across real boots.
# test-service-stage-wedge-qemu.sh -- a staged candidate that comes UP (it
# publishes the service GUID) and then wedges a callback during the commit
# grace period. The launcher is inside its grace-period wait, so nothing but
# the watchdog the trial re-armed at publish (grace + 60 s) can end the boot.
# The launcher here allows ONE try (upgrade-svc-t1.efi), so one wedge is
# enough to give the candidate up:
#   boot 1: v7 is up, then wedges; no promotion; the watchdog resets -- the
#           re-armed one, ~62 s after the wedge (the trial's first 30 s
#           watchdog alone would fire ~30 s after it; asserted >= 45 s)
#   boot 2: a boot died trying it, tries is 1: given up -- ROLLED_BACK, v7 is
#           .failed and not started again, v1 runs
#
# Usage: ./test/integration/test-service-stage-wedge-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/stage-lib.sh"

test_parse_args "$@"
test_setup
stage_setup
img_put "$B/upgrade-svc-t1.efi" upgrade-svc.efi
# A wedged boot ends only on the re-armed watchdog: 3 s grace + 60 s.
STAGE_BOOT_SECS=$((STAGE_BOOT_SECS + 75))
echo "=== staged upgrade: a candidate that wedges after publishing ($TEST_ARCH) ==="

V1="$B/upgrade-svc-dxe-q1.efi"
WEDGE="$B/upgrade-svc-dxe-wedge.efi"
gave_up() { img_is "$P.failed" "$WEDGE" && ! img_has "$P.new" && img_is "$P" "$V1"; }
untouched() { img_is "$P" "$V1" && img_is "$P.new" "$WEDGE" && ! img_has "$P.bak"; }

reset_disk
img_put "$WEDGE" "$P.new"
# When the wedge starts, by the host clock: the reset must come from the
# watchdog RE-ARMED at publish (grace 3 s + 60 s), not the trial's first one
# (30 s from the trial's start, which would still be running without the
# re-arm and fire well under 30 s after the wedge).
rm -f "$TEST_TMPDIR/wedge_at"
(
    for ((i = 0; i < STAGE_BOOT_SECS * 10; i++)); do
        if grep -aqF "USVC: v7 wedging a callback on purpose" "$TEST_TMPDIR/wedge1.raw" 2>/dev/null; then
            date +%s > "$TEST_TMPDIR/wedge_at"
            exit 0
        fi
        sleep 0.1
    done
) &
WATCH_PID=$!
boot wedge1
ended_at=$(date +%s)
kill "$WATCH_PID" 2>/dev/null || true
wait "$WATCH_PID" 2>/dev/null || true
wedged_for() {   # the boot ended at least $1 s after the wedge began
    [[ -s "$TEST_TMPDIR/wedge_at" ]] || return 1
    local d=$((ended_at - $(cat "$TEST_TMPDIR/wedge_at")))
    echo "  (the reset came ${d}s after the wedge)"
    (( d >= $1 ))
}
check "boot 1: the reset came from the grace-period watchdog (>= 45 s after the wedge)" \
    wedged_for 45
check "boot 1: the candidate came up (published)" has wedge1 "$(up_v 7)"
check "boot 1: then wedged a callback" has wedge1 "USVC: v7 wedging a callback on purpose"
check "boot 1: the launcher never returned" not has wedge1 "BOOT-END"
check "boot 1: no verdict" not has wedge1 "upgrade-svc: staged upgrade"
check "boot 1: the watchdog reset the box (not the harness timeout)" ended_itself wedge1
check "boot 1: nothing was promoted" untouched
boot wedge2
check "boot 2: given up after one dead boot" has wedge2 "1 boot(s) died trying"
check "boot 2: rolled back" line wedge2 "$ROLLED"
check "boot 2: it was not started again" not has wedge2 "$(up_v 7)"
check "boot 2: v1 runs" has wedge2 "$(up 1 rolled-back)"
check "boot 2: the candidate is .failed, P untouched" gave_up

stage_finish "staged upgrade wedge" 12
