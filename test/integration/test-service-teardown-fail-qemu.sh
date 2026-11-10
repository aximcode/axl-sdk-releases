#!/bin/bash
# test-meta: arch=both needs= est=50 local-only=0 guest=none
# test-meta: builds=svc-tdfail
# guest=none: its second boot's subject is a machine reset -- `stop --reset`
#   reboots the guest and its startup.nsh runs again from fs0:\tdreset.flg;
#   a pooled run has neither the reboot nor a boot-time startup.nsh.
# test-service-teardown-fail-qemu.sh -- a service whose teardown FAILS stays
# whole and resident, reports itself STOPPING, and a later stop resumes the
# teardown; plus the DEGRADED state a service reports about itself, and
# `stop --reset`.
#
# svc_tdfail.efi is an AXL_SERVICE launcher with its driver embedded
# (test/integration/svc-tdfail.c); the driver's teardown fails --fails N times
# before it succeeds. Every launcher run prints one verb line and its exit
# code (%lasterror%); each step's are asserted exactly.
#
#   A  --fails 3: two refused stops, status `stopping` (exit 5), a start that
#      refuses, a reload over it (AXL_DENIED, nothing started), and the third
#      retry that finishes the teardown.
#   B  --fails 1 --attach-fail: setup succeeds, the tick attach fails (the
#      library's "attach" test point) and the rollback teardown fails. The
#      driver stays resident -- DriverEntry returning an error would have the
#      firmware free it without Unload -- and start reports `stopping`.
#   H  --setup-held: setup returns AXL_SERVICE_SETUP_HELD. Same state as B,
#      and the next stop's teardown frees what setup kept.
#   D  --degrade 1: degraded from setup. status `degraded (...)` exit 0, start
#      says already running, and a reload's replacement starts healthy.
#   T  --degrade 2: degraded from a loop tick (TPL_CALLBACK).
#   C  --degrade 3: degraded in setup, cleared from the first tick.
#   G  --degrade 4 / S --degrade 5: a degraded tag published by hand with a
#      reason struct LARGER (a later SDK) and SMALLER with no terminator (an
#      earlier one): status reads the reason from either.
#   R1 --setup-rc 1: a positive setup return that is not
#      AXL_SERVICE_SETUP_HELD is an ordinary failure -- the image unloads.
#   N  --held-no-teardown: HELD from a service with no teardown is refused,
#      with a warning, as an ordinary failure (nothing could ever finish it).
#
# A second boot (no -no-reboot, so a reset reboots) checks `stop --reset`: a
# stop that succeeds resets nothing, and one whose teardown still fails warns
# and resets the machine -- the script after it never runs, and the reboot
# runs startup.nsh again, which sees the flag file it left and says so. That
# rerun then loads a FOREIGN image publishing the same name, which will not
# unload and is not stopping: `stop --reset` must report the failed stop and
# NOT reset.
#
# The teardown also logs how often the loop has ticked: a half-stopped
# service's loop must not run between two attempts. And no retry may detach a
# loop that is already detached.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-service-teardown-fail-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" svc-tdfail 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/svc_tdfail.efi"
test_add_efi "$PREFIX_DIR/svc_tdfail_foreign.efi"

STOPPING="stopping (teardown failed; 'stop' retries; a reset clears it)"

# label | launcher arguments | the verb line it must print ("" = none) | %lasterror%
STEPS=(
    "START-A|start --detach --fails 3||0x0"
    "STOP-1|stop|svc-tdfail: stop failed (rc=-1)|0x1"
    "STATUS-1|status|svc-tdfail: $STOPPING|0x5"
    "START-A2|start --detach|svc-tdfail: $STOPPING|0x1"
    "STOP-2|stop|svc-tdfail: stop failed (rc=-1)|0x1"
    "STATUS-2|status|svc-tdfail: $STOPPING|0x5"
    "RELOAD|reload|svc-tdfail: reload denied (state 3)|0x1"
    "STATUS-3|status|svc-tdfail: $STOPPING|0x5"
    "STOP-3|stop|svc-tdfail: stopped|0x0"
    "STATUS-4|status|svc-tdfail: stopped|0x1"
    "START-B|start --detach --fails 1 --attach-fail|svc-tdfail: $STOPPING|0x1"
    "STATUS-B1|status|svc-tdfail: $STOPPING|0x5"
    "STOP-B|stop|svc-tdfail: stopped|0x0"
    "STATUS-B2|status|svc-tdfail: stopped|0x1"
    "START-H|start --detach --fails 0 --setup-held|svc-tdfail: $STOPPING|0x1"
    "STATUS-H1|status|svc-tdfail: $STOPPING|0x5"
    "STOP-H|stop|svc-tdfail: stopped|0x0"
    "STATUS-H2|status|svc-tdfail: stopped|0x1"
    "START-D|start --detach --fails 0 --degrade 1||0x0"
    "STATUS-D1|status|svc-tdfail: degraded (set in setup)|0x0"
    "START-D2|start --detach|svc-tdfail: already running (degraded: set in setup)|0x0"
    "RELOAD-D|reload|svc-tdfail: reload rc=0|0x0"
    "STATUS-D3|status|svc-tdfail: running|0x0"
    "STOP-D|stop|svc-tdfail: stopped|0x0"
    "START-T|start --detach --fails 0 --degrade 2||0x0"
    "STATUS-T|status|svc-tdfail: degraded (set in a tick)|0x0"
    "STOP-T|stop|svc-tdfail: stopped|0x0"
    "START-C|start --detach --fails 0 --degrade 3||0x0"
    "STATUS-C|status|svc-tdfail: running|0x0"
    "STOP-C|stop|svc-tdfail: stopped|0x0"
    "START-G|start --detach --fails 0 --degrade 4||0x0"
    "STATUS-G|status|svc-tdfail: degraded (grown struct)|0x0"
    "STOP-G|stop|svc-tdfail: stopped|0x0"
    "START-S|start --detach --fails 0 --degrade 5||0x0"
    "STATUS-S|status|svc-tdfail: degraded (short)|0x0"
    "STOP-S|stop|svc-tdfail: stopped|0x0"
    "START-R1|start --detach --fails 0 --setup-rc 1|svc-tdfail: start failed (rc=-5)|0x1"
    "STATUS-R1|status|svc-tdfail: stopped|0x1"
    "START-N|start --detach --fails 0 --held-no-teardown|svc-tdfail: start failed (rc=-5)|0x1"
    "STATUS-N|status|svc-tdfail: stopped|0x1"
)
LABELS=()
for st in "${STEPS[@]}"; do LABELS+=("${st%%|*}"); done
LABELS+=(DONE)

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    for st in "${STEPS[@]}"; do
        IFS='|' read -r label args _ _ <<< "$st"
        echo "echo TDFAIL-$label"
        echo "svc_tdfail.efi $args"
        echo "echo TDFAIL-RC=%lasterror%"
        echo "stall 300000"
    done
    echo "echo TDFAIL-DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== service teardown failure ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 90
test_clean_log
cp "$TEST_CLEAN_LOG" "$TEST_TMPDIR/boot1.log"

count() { grep -acE -- "$1" "$TEST_TMPDIR/boot1.log" || true; }

# The output of step $1: everything up to the next step's marker.
step_out() {
    local i out="$TEST_TMPDIR/step-$1.log"
    for i in "${!LABELS[@]}"; do
        if [[ "${LABELS[$i]}" == "$1" ]]; then
            test_slice_log "TDFAIL-$1" "TDFAIL-${LABELS[$((i + 1))]}" "$out"
            cat "$out"
            return
        fi
    done
}

# The line the launcher's verb printed (not the driver's log lines).
verb_line() {
    step_out "$1" | grep -aE '^svc-tdfail: (stopped|running|not running|stop failed|start failed|reload|stopping|degraded|already running)' || true
}

expect_driver() {   # expect_driver <step> <regex> <description>
    if [[ -n "$(step_out "$1" | grep -aE -- "$2")" ]]; then
        test_host_pass "$3"
    else
        test_host_fail "$3 (no line matching '$2')"
    fi
}

if [[ "$(count '^TDFAIL-DONE$')" == 1 ]]; then
    test_host_pass "the script ran to the end"
else
    test_host_fail "the script ran to the end"
fi

# Every step: its exact verb line and exit code.
for st in "${STEPS[@]}"; do
    IFS='|' read -r label args want_line want_rc <<< "$st"
    got_line=$(verb_line "$label")
    got_rc=$(step_out "$label" | grep -aE '^TDFAIL-RC=' | sed 's/^TDFAIL-RC=//' || true)
    if [[ "$got_line" == "$want_line" && "$got_rc" == "$want_rc" ]]; then
        test_host_pass "$label: '$args' -> '${want_line:-<nothing>}', exit $want_rc"
    else
        test_host_fail "$label: '$args' -> '${want_line:-<nothing>}', exit $want_rc (saw '$got_line', exit '$got_rc')"
    fi
done

# --- What the driver did -----------------------------------------------------

expect_driver STOP-1 'svc-tdfail: teardown 1 failing on purpose ticks=[0-9]+$' "A: stop 1 ran teardown 1, which failed"
expect_driver STOP-2 'svc-tdfail: teardown 2 failing on purpose ticks=[0-9]+$' "A: stop 2 re-ran teardown, which failed"
expect_driver RELOAD 'svc-tdfail: teardown 3 failing on purpose ticks=[0-9]+$' "A: the reload's stop re-ran teardown, which failed"
expect_driver STOP-3 'svc-tdfail: teardown 4 ok ticks=[0-9]+$' "A: stop 3 re-ran teardown, which succeeded"

slice_a="$TEST_TMPDIR/scenario-a.log"
test_slice_log "TDFAIL-START-A" "TDFAIL-START-B" "$slice_a"
n_td=$(grep -acE 'svc-tdfail: teardown [0-9]+ ' "$slice_a" || true)
n_setup=$(grep -acE 'svc-tdfail: setup ' "$slice_a" || true)
if [[ "$n_td" == 4 && "$n_setup" == 1 ]]; then
    test_host_pass "A: four teardowns and one setup (neither the start nor the reload started anything)"
else
    test_host_fail "A: four teardowns and one setup (teardowns=$n_td setups=$n_setup)"
fi
ticks=$(grep -aoE 'svc-tdfail: teardown [0-9]+ .*ticks=[0-9]+' "$slice_a" \
        | sed -E 's/.*ticks=//' | sort -u)
if [[ "$(wc -l <<< "$ticks")" == 1 && "$ticks" =~ ^[1-9][0-9]*$ ]]; then
    test_host_pass "A: the loop ticked before the first stop ($ticks) and not after it"
else
    test_host_fail "A: the loop ticked before the first stop and not after it (ticks: $(tr '\n' ' ' <<< "$ticks"))"
fi

expect_driver START-B 'svc-tdfail: setup fails=1 attach-fail=1$' "B: setup ran with the attach set to fail"
expect_driver START-B 'svc-tdfail: teardown 1 failing on purpose ticks=0$' "B: the failed attach ran the rollback teardown, which failed"
expect_driver STOP-B 'svc-tdfail: teardown 2 ok ticks=0$' "B: the stop finished the teardown; the loop never ticked"

expect_driver START-H 'svc-tdfail: setup failing, holding its block$' "H: setup returned AXL_SERVICE_SETUP_HELD"
expect_driver STOP-H 'svc-tdfail: teardown 1 ok ticks=0$' "H: the stop's teardown freed what setup held; the loop never ticked"
if [[ -z "$(step_out START-H | grep -aE 'svc-tdfail: teardown ')" ]]; then
    test_host_pass "H: no teardown ran at entry (setup said it holds, the framework kept it)"
else
    test_host_fail "H: no teardown ran at entry"
fi

expect_driver START-D 'svc-tdfail: degrade from setup rc=0$' "D: set_degraded from setup returned AXL_OK"
expect_driver START-T 'svc-tdfail: degrade from a tick rc=0$' "T: set_degraded from a loop tick returned AXL_OK"
expect_driver START-C 'svc-tdfail: degrade from setup rc=0$' "C: set_degraded from setup returned AXL_OK (so the clear had a tag to remove)"
expect_driver START-C 'svc-tdfail: clear from a tick rc=0$' "C: clearing from a loop tick returned AXL_OK"
expect_driver START-G 'svc-tdfail: hand tag 4 rc=0$' "G: the larger degraded tag was published"
expect_driver START-S 'svc-tdfail: hand tag 5 rc=0$' "S: the smaller degraded tag was published"
expect_driver START-R1 'svc-tdfail: setup returning 1$' "R1: setup returned 1"
if [[ -z "$(step_out START-R1 | grep -aE 'svc-tdfail: teardown ')" ]]; then
    test_host_pass "R1: no teardown ran for a setup that returned 1 (an ordinary failure)"
else
    test_host_fail "R1: no teardown ran for a setup that returned 1 (an ordinary failure)"
fi
expect_driver START-N "service 'svc-tdfail': setup returned AXL_SERVICE_SETUP_HELD with no teardown to finish it - treating it as a failure" \
    "N: HELD with no teardown is refused, with a warning"

n_detach=$(count 'detaching its loop failed')
if [[ "$n_detach" == 0 ]]; then
    test_host_pass "no retry tried to detach an already-detached loop"
else
    test_host_fail "no retry tried to detach an already-detached loop (saw $n_detach)"
fi

# A failed teardown is reported ONCE by the driver image: A's three and B's
# one. H's held setup is not a teardown failure.
n_warn=$(count "service 'svc-tdfail': teardown failed - ")
n_old=$(count "service 'svc-tdfail': teardown returned -1")
if [[ "$n_warn" == 4 && "$n_old" == 0 ]]; then
    test_host_pass "each of the four failed teardowns is reported by one driver line"
else
    test_host_fail "each of the four failed teardowns is reported by one driver line (driver lines=$n_warn, helper lines=$n_old)"
fi

# One verdict per launcher run, and one per driver that unloaded: A, B, H,
# D and its reload's replacement, T, C, G, S. R1 and N failed at entry, which
# prints no verdict. No leak report anywhere.
want_clean=$(( ${#STEPS[@]} + 9 ))
n_clean=$(count 'mem: no leaks detected$')
n_any=$(count '=== AxlMem leak report: ')
if [[ "$n_clean" == "$want_clean" && "$n_any" == 0 ]]; then
    test_host_pass "verdicts: $want_clean clean (${#STEPS[@]} launcher exits + 9 driver unloads), no leak"
else
    test_host_fail "verdicts: $want_clean clean (${#STEPS[@]} launcher exits + 9 driver unloads), no leak (clean=$n_clean reports=$n_any)"
fi

# --- Boot 2: stop --reset ----------------------------------------------------

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "if exist fs0:\\tdreset.flg then"
    echo "  echo TDFAIL-REBOOTED"
    echo "  svc_tdfail.efi status"
    echo "  load svc_tdfail_foreign.efi"
    echo "  echo TDFAIL-FOREIGN"
    echo "  svc_tdfail.efi stop --reset"
    echo "  echo TDFAIL-FOREIGN-RC=%lasterror%"
    echo "  reset -s"
    echo "endif"
    echo "svc_tdfail.efi start --detach --fails 1"
    echo "svc_tdfail.efi stop"
    echo "echo TDFAIL-R-SOFT"
    echo "svc_tdfail.efi stop --reset"
    echo "echo TDFAIL-R-SOFT-RC=%lasterror%"
    echo "svc_tdfail.efi start --detach --fails 9"
    echo "echo x > fs0:\\tdreset.flg"
    echo "echo TDFAIL-R-HARD"
    echo "svc_tdfail.efi stop --reset"
    echo "echo TDFAIL-R-NOT-RESET"
    echo "reset -s"
} | test_set_startup
test_build_image

echo "=== service stop --reset ($TEST_ARCH) ==="

test_build_qemu_cmd
_cmd=()
for a in "${TEST_QEMU_CMD[@]}"; do [[ "$a" == -no-reboot ]] || _cmd+=("$a"); done
TEST_QEMU_CMD=("${_cmd[@]}")
test_run_foreground 120
test_clean_log
boot2="$TEST_CLEAN_LOG"

slice_soft="$TEST_TMPDIR/r-soft.log"
test_slice_log "TDFAIL-R-SOFT" "TDFAIL-R-HARD" "$slice_soft"
if grep -aqxF 'svc-tdfail: stopped' "$slice_soft" \
   && grep -aqxF 'TDFAIL-R-SOFT-RC=0x0' "$slice_soft" \
   && ! grep -aqF 'resetting the machine' "$slice_soft"; then
    test_host_pass "stop --reset: a retried teardown that succeeds stops the service and resets nothing"
else
    test_host_fail "stop --reset: a retried teardown that succeeds stops the service and resets nothing"
    sed 's/^/    /' "$slice_soft"
fi

n_warned=$(grep -acF "svc-tdfail: the teardown still fails - resetting the machine" "$boot2" || true)
n_not=$(grep -acxF 'TDFAIL-R-NOT-RESET' "$boot2" || true)
n_reboot=$(grep -acxF 'TDFAIL-REBOOTED' "$boot2" || true)
if [[ "$n_warned" == 1 && "$n_not" == 0 && "$n_reboot" == 1 ]]; then
    test_host_pass "stop --reset: a teardown that still fails warns and resets the machine"
else
    test_host_fail "stop --reset: a teardown that still fails warns and resets the machine (warned=$n_warned not-reset=$n_not rebooted=$n_reboot)"
fi
slice_foreign="$TEST_TMPDIR/r-foreign.log"
test_slice_log "TDFAIL-FOREIGN" "" "$slice_foreign"
if grep -aqxF 'svc-tdfail: stop failed (rc=-1)' "$slice_foreign" \
   && grep -aqxF "svc-tdfail: not resetting - its state is running, not stopping" "$slice_foreign" \
   && grep -aqxF 'TDFAIL-FOREIGN-RC=0x1' "$slice_foreign" \
   && ! grep -aqF 'resetting the machine' "$slice_foreign"; then
    test_host_pass "stop --reset: a foreign image that will not unload is reported, and the machine is NOT reset"
else
    test_host_fail "stop --reset: a foreign image that will not unload is reported, and the machine is NOT reset"
    sed 's/^/    /' "$slice_foreign"
fi
if [[ "$n_reboot" == 1 && -n "$(sed -n '/^TDFAIL-REBOOTED$/,$p' "$boot2" | grep -axF 'svc-tdfail: stopped')" ]]; then
    test_host_pass "stop --reset: after the reset nothing is resident"
else
    test_host_fail "stop --reset: after the reset nothing is resident"
fi

test_host_summary "service teardown failure ($TEST_ARCH)"
