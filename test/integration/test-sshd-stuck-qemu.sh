#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 guest=none
# test-meta: builds=sshd-faults
# guest=none: sshd's fault hooks read fixed fs0:\sshd-fault-* files, and on
#   a leased guest fs0: is not the test's disk.
# test-sshd-stuck-qemu.sh -- sshd whose console take-over cannot be undone:
# what `sshd stop`, `sshd status` and `sshd` say, and that a later stop
# finishes the job.
#
# sshd-t.efi is a TEST build of sshd (goal `sshd-faults`): the driver also
# links test/integration/sshd-test-hook.c, whose strong test points are armed
# by files on the boot volume. No shipped sshd links it.
#
#   teardown  sshd serves; with the console device's disconnect refused,
#             `sshd stop` fails (exit 1) and the service is STOPPING:
#             `sshd status` prints the stuck line and exits 5. Disarmed, the
#             next `sshd stop` finishes the teardown, and status says stopped.
#   setup     with the disconnect refused AND setup failing right after the
#             take-over, `sshd` cannot give the console back: setup returns
#             AXL_SERVICE_SETUP_HELD, the start prints the stuck line and
#             exits 1, and status exits 5. Disarmed, `sshd stop` finishes it.
#
# Not tested: the third stuck path, a release refused after the Shell exits
# while sshd stays up (DEGRADED). It needs the Shell to exit under a running
# sshd with the fault armed, then a new Shell to ask -- what
# test-sshd-lifecycle-qemu.sh's remote-exit does through the boot manager --
# and it is left documented as untested.
#
# AXL_ERR from the disconnect needs firmware that refuses it, which OVMF never
# does: that is what the hook supplies. Everything after the refusal is real.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-sshd-stuck-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" sshd-faults 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/sshd-t.efi"

STUCK="sshd: stuck - the console take-over could not be undone; 'sshd stop' tries again"

# label | the Shell command(s) | the sshd line that must appear | %lasterror%
STEPS=(
    "START|sshd-t.efi -P stuck-pw|sshd: serving the UEFI Shell on 10.0.2.15 port 22|0x0"
    "STOP-1|echo x > fs0:\\sshd-fault-disconnect; sshd-t.efi stop|sshd: could not stop the server|0x1"
    "STATUS-1|sshd-t.efi status|$STUCK|0x5"
    "STOP-2|del fs0:\\sshd-fault-disconnect; sshd-t.efi stop|sshd: stopped|0x0"
    "STATUS-2|sshd-t.efi status|sshd: stopped|0x0"
    "HELD|echo x > fs0:\\sshd-fault-disconnect; echo x > fs0:\\sshd-fault-setup; sshd-t.efi -P stuck-pw|$STUCK|0x1"
    "STATUS-3|sshd-t.efi status|$STUCK|0x5"
    "STOP-3|del fs0:\\sshd-fault-disconnect; del fs0:\\sshd-fault-setup; sshd-t.efi stop|sshd: stopped|0x0"
    "STATUS-4|sshd-t.efi status|sshd: stopped|0x0"
)
LABELS=()
for st in "${STEPS[@]}"; do LABELS+=("${st%%|*}"); done
LABELS+=(DONE)

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    for st in "${STEPS[@]}"; do
        IFS='|' read -r label cmds _ _ <<< "$st"
        echo "echo SSTK-$label"
        IFS=';' read -ra parts <<< "$cmds"
        for c in "${parts[@]}"; do
            echo "${c# }"
        done
        echo "echo SSTK-RC=%lasterror%"
        echo "stall 500000"
    done
    echo "echo SSTK-DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== sshd stuck console ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_network
test_run_foreground 150
test_clean_log

step_out() {
    local i out="$TEST_TMPDIR/step-$1.log"
    for i in "${!LABELS[@]}"; do
        if [[ "${LABELS[$i]}" == "$1" ]]; then
            test_slice_log "SSTK-$1" "SSTK-${LABELS[$((i + 1))]}" "$out"
            cat "$out"
            return
        fi
    done
}

if grep -aqxF 'SSTK-DONE' "$TEST_CLEAN_LOG"; then
    test_host_pass "the script ran to the end"
else
    test_host_fail "the script ran to the end"
fi

for st in "${STEPS[@]}"; do
    IFS='|' read -r label cmds want_line want_rc <<< "$st"
    out=$(step_out "$label")
    got_rc=$(grep -aE '^SSTK-RC=' <<< "$out" | sed 's/^SSTK-RC=//' || true)
    # The line may follow a log line's tail on the same row (x64's take-over
    # puts a terminal sequence on the serial line), so it is anchored at the
    # end and must start a field.
    if grep -aqE "(^|[^[:alnum:]])$(sed 's/[][\.*^$()+?{}|/]/\\&/g' <<< "$want_line")\$" <<< "$out" \
       && [[ "$got_rc" == "$want_rc" ]]; then
        test_host_pass "$label: '$cmds' -> '$want_line', exit $want_rc"
    else
        test_host_fail "$label: '$cmds' -> '$want_line', exit $want_rc (exit '$got_rc')"
        sed 's/^/    /' <<< "$out" | tail -8
    fi
done

# The driver's own account: the teardown and the setup each refused, once.
if [[ -n "$(step_out STOP-1 | grep -aF 'unload refused: the console take-over is still in place')" ]]; then
    test_host_pass "teardown: the driver refused its unload because the take-over is still in place"
else
    test_host_fail "teardown: the driver refused its unload because the take-over is still in place"
fi
if [[ -n "$(step_out HELD | grep -aF "setup failed and the console take-over could not be undone")" ]]; then
    test_host_pass "setup: the driver's setup failed holding the take-over"
else
    test_host_fail "setup: the driver's setup failed holding the take-over"
fi

test_host_summary "sshd stuck console ($TEST_ARCH)"
