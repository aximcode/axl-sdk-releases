#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=none
# test-meta: builds=console-device-hold
# guest=none: the driver reads a file from its 10 ms loop tick (TPL_CALLBACK),
#   and a leased guest's files are on its virtio-fs share, whose firmware
#   driver (VirtioFsDxe) does not serialize: a read that preempts one of the
#   Shell's own requests (it checks its current directory after every
#   command) leaves the Shell polling the ring forever. See the refusal below.
# test-console-device-hold-qemu.sh — a resident driver holding the console
# take-over, seen from a second image and from the Shell.
#
# console-device-hold.efi is a resident service driver: its setup installs the
# take-over device and a tick on the service's own loop, steered by marker lines
# the Shell prints (see the fixture's header). console-device-present-probe.efi is a SEPARATE image that
# never installs a device, so its axl_console_device_present() answer can only
# come from the firmware's handle database: the per-image singleton cannot see
# another image's take-over, which is the case the probe exists for.
#
# The Ctrl-C scenario is the one a remote console delivers: the key is injected
# through the device while a script loops, with a resident service loop ticking
# every 10 ms. That loop must neither swallow the Shell's break nor be stopped
# by it.
#
# Serial only (-nographic, the harness's own firmware): passthrough_local keeps
# the serial console painting, so every line below is on the log.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-console-device-hold-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

# Not on a leased guest. OVMF's VirtioFsDxe raises no TPL around a request,
# unlike FAT (which takes a TPL_CALLBACK lock), and VirtioFlush waits for the
# used index to equal the one it published -- so when the driver's tick reads
# busy.nsh while the Shell is inside one of its own share requests, the tick's
# requests carry the index past the Shell's and the Shell spins in Stall for
# good (measured: the Shell's expected index 11 short of the ring's, 11 being
# the tick's read; 12 of 78 pooled runs under load, 0 of 30 with the same
# read on the FAT boot disk, 0 of 30 booted on both arches). The read is
# legal UEFI and ordinary for a resident driver; it is the share's driver
# that cannot take it.
_test_unpooled "a file system a driver can read from its loop tick (the share is virtio-fs, which does not serialize)"

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" console-device-hold 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/drivers/console-device-hold.efi" console-device-hold.efi
test_add_efi "$PREFIX_DIR/console-device-present-probe.efi" probe.efi

# The guest's commands live in test-console-device-hold-qemu.nsh.
# Booted, test_use_nsh appends the reset -s that powers the guest off.
test_use_nsh

# A script the Shell can break: its runner checks the break flag between
# lines, while a single `stall` never does. 100 x 100 ms runs ~10 s unbroken.
cat > "$TEST_STAGING/loop.nsh" << 'NSH'
@echo -off
for %a run (1 100)
  echo %T%-iter-%a
  stall 100000
endfor
echo %T%-loop-done
NSH
# The same, broken while the driver does library work under the break.
cat > "$TEST_STAGING/busy.nsh" << 'NSH'
@echo -off
for %a run (1 100)
  echo %T%-biter-%a
  stall 100000
endfor
echo %T%-busy-done
NSH

test_build_image

echo "=== console-device hold ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

# Whole lines, in order. The probe prints exactly one line per run.
probes=$(grep -aE '^PROBE present=[01]$' "$TEST_CLEAN_LOG" | tr '\n' ' ' || true)

# x64's take-over puts the terminal's SetMode sequence (ESC[=3h) on the serial
# line just before the next output, so HOLD-READY is anchored at the END only.
if grep -aqE 'HOLD-READY$' "$TEST_CLEAN_LOG"; then
    test_host_pass "hold: the driver installed the take-over"
else
    test_host_fail "hold: the driver installed the take-over (no HOLD-READY line)"
fi
if grep -aqFx 'HOLD-RELEASED' "$TEST_CLEAN_LOG"; then
    test_host_pass "hold: the driver released the take-over on its marker"
else
    test_host_fail "hold: the driver released the take-over on its marker (no HOLD-RELEASED line)"
fi
if [[ "$probes" == "PROBE present=0 PROBE present=1 PROBE present=0 " ]]; then
    test_host_pass "present: false before, true while another image holds it, false after"
else
    test_host_fail "present: false before, true while another image holds it, false after (saw '$probes')"
fi
# A SimpleTextOut handle whose device path is a bare End node is a prefix of
# every LocateDevicePath query; a probe that accepted any hit would say true.
if grep -aqFx 'PROBE decoy present=0' "$TEST_CLEAN_LOG"; then
    test_host_pass "present: an empty-path SimpleTextOut decoy is not a take-over"
else
    test_host_fail "present: an empty-path SimpleTextOut decoy is not a take-over (saw '$(grep -a 'PROBE decoy' "$TEST_CLEAN_LOG" | tr -d '\r' || true)')"
fi
# The Shell's Ctrl-C, delivered while a service loop is resident, breaks the
# running script (the Shell prints nothing on a break: the script just stops)
# and leaves the service loop running.
n_iter=$(grep -acE '^cdh-iter-[0-9]+$' "$TEST_CLEAN_LOG" || true)
if grep -aqE 'HOLD-INJECTED-CTRLC rc=0$' "$TEST_CLEAN_LOG" \
   && ! grep -aqFx 'cdh-loop-done' "$TEST_CLEAN_LOG" \
   && (( n_iter >= 1 && n_iter < 100 )); then
    test_host_pass "break: an injected Ctrl-C stops a looping script under a resident service loop"
else
    test_host_fail "break: an injected Ctrl-C stops a looping script under a resident service loop (iterations=$n_iter, loop-done=$(grep -acFx 'cdh-loop-done' "$TEST_CLEAN_LOG" || true))"
fi
if grep -aqFx 'cdh-after-loop' "$TEST_CLEAN_LOG"; then
    test_host_pass "break: only the broken script stopped; the calling script went on"
else
    test_host_fail "break: only the broken script stopped; the calling script went on (no cdh-after-loop line)"
fi
if grep -aqE 'HOLD-PONG$' "$TEST_CLEAN_LOG"; then
    test_host_pass "break: the service loop still dispatches after the Shell's Ctrl-C"
else
    test_host_fail "break: the service loop still dispatches after the Shell's Ctrl-C (no HOLD-PONG line)"
fi
# The second break lands while the DRIVER is busy in ordinary library work: a
# short-lived sync wait (its own, never-attached loop) and a file read (which
# polls the break). In a driver image neither may read the Shell's break, latch
# an interrupt, or try to exit the image -- an exit cannot succeed for an image
# that is not the current one, so it would spin with the machine wedged.
busy_line() { grep -aoE "HOLD-BUSY $1=-?[0-9]+ interrupted=[01]\$" "$TEST_CLEAN_LOG" | tr -d '\r' | tr '\n' ';' || true; }
if [[ "$(busy_line wait)" == "HOLD-BUSY wait=0 interrupted=0;" ]]; then
    test_host_pass "busy: a driver's sync wait runs its full time under the Shell's break and latches nothing"
else
    test_host_fail "busy: a driver's sync wait runs its full time under the Shell's break and latches nothing (saw '$(busy_line wait)')"
fi
if [[ "$(busy_line read)" == "HOLD-BUSY read=0 interrupted=0;" ]]; then
    test_host_pass "busy: a driver's file read under the Shell's break neither latches nor exits"
else
    test_host_fail "busy: a driver's file read under the Shell's break neither latches nor exits (saw '$(busy_line read)')"
fi
n_biter=$(grep -acE '^cdh-biter-[0-9]+$' "$TEST_CLEAN_LOG" || true)
if grep -aqE 'HOLD-BUSY-INJECTED rc=0$' "$TEST_CLEAN_LOG" \
   && ! grep -aqFx 'cdh-busy-done' "$TEST_CLEAN_LOG" \
   && (( n_biter >= 1 && n_biter < 100 )); then
    test_host_pass "busy: the Shell still breaks the looping script the driver worked under"
else
    test_host_fail "busy: the Shell still breaks the looping script the driver worked under (iterations=$n_biter, busy-done=$(grep -acFx 'cdh-busy-done' "$TEST_CLEAN_LOG" || true))"
fi
n_pong=$(grep -acE 'HOLD-PONG$' "$TEST_CLEAN_LOG" || true)
if grep -aqFx 'cdh-after-busy' "$TEST_CLEAN_LOG" && [[ "$n_pong" == 2 ]]; then
    test_host_pass "busy: the calling script went on and the driver still answers"
else
    test_host_fail "busy: the calling script went on and the driver still answers (after-busy=$(grep -acFx 'cdh-after-busy' "$TEST_CLEAN_LOG" || true), pongs=$n_pong)"
fi
if grep -aqFx 'cdh-end' "$TEST_CLEAN_LOG"; then
    test_host_pass "hold: the script ran to its end"
else
    test_host_fail "hold: the script ran to its end (no cdh-end line)"
fi

rc=0
test_host_summary "console-device hold ($TEST_ARCH)" || rc=1
# The leak gate directly: these images print no "=== Results:" footer, so
# test_count_results has no assertion count to read.
test_check_leaks || rc=1
exit "$rc"
