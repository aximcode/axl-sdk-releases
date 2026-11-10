#!/bin/bash
# test-meta: arch=x64 needs= est=50 local-only=0 guest=none
# test-meta: builds=ARCH=x64,console-device-hold-takeover
# guest=none: the boot is the subject: one boot copies ConOut into ErrOut,
#   the next boot of the same NVRAM reads it.
# test-console-device-errout-qemu.sh -- a take-over on firmware whose ErrOut
# lists the display.
#
# OVMF lists only the serial terminal in ErrOut; firmware that also lists the
# screen tags GraphicsConsole as a StdErr device. A take-over evicts the
# firmware consoles from the ConOut fan-out, and the device joins the StdErr
# one -- so a screen left in StdErr would draw every error line a second time,
# over the take-over's own picture. The take-over must move the StdErr tag off
# the screens it evicts (and only those: the serial terminal keeps its tag),
# and put it back when it is uninstalled.
#
# Boot 1 copies ConOut (serial + display) into ErrOut, so on boot 2 -- the same
# NVRAM -- ConPlatform tags the display as a StdErr device, as such firmware
# does. Boot 2, all from startup.nsh:
#   baseline   two StdErr devices, one a screen (`probe.efi tags`, which counts
#              StdErr-tagged handles and those carrying a GOP)
#   take-over  console-device-hold-takeover.efi: two StdErr devices (the
#              serial terminal and the device), no screen
#   stderr     a line another image writes to StdErr reaches the device
#              exactly once, and the serial terminal exactly once
#   release    after the device is uninstalled, two StdErr devices, one a
#              screen again -- the tag is back on the display
#              and a ConOut device again (`probe.efi conout`: two devices,
#              one a screen, before and after)
#
# Boot 3, the same NVRAM: the take-over again, and while it holds the console
# the display's StdErr tag comes back (`probe.efi retag-err`); the release
# must still re-tag the display as a ConOut device.
#
# x64 only: aa64's virt machine has no display device, so there is no screen
# to list. The probe writes its counts to StdErr, which the serial terminal
# stays in through the take-over (StdOut does not: it is evicted from ConOut).
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-console-device-errout-qemu.sh [--arch X64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

if [[ "$TEST_ARCH" != "X64" ]]; then
    echo "console-device errout: SKIP (no display device on $TEST_ARCH)"
    exit 0
fi

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 console-device-hold-takeover 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/drivers/console-device-hold-takeover.efi" hold.efi
test_add_efi "$PREFIX_DIR/console-device-present-probe.efi" probe.efi

echo "=== console-device errout ($TEST_ARCH) ==="

# --- boot 1: the display into ErrOut ---------------------------------------
{
    echo "@echo -off"
    echo "fs0:"
    echo "probe.efi errout"
    echo "reset -s"
} | test_set_startup
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log
if grep -aqE '^PROBE errout=[1-9][0-9]*$' "$TEST_CLEAN_LOG"; then
    test_host_pass "setup: ErrOut now lists ConOut's devices, the display among them"
else
    test_host_fail "setup: ErrOut now lists ConOut's devices (saw '$(grep -a 'PROBE errout' "$TEST_CLEAN_LOG" || true)')"
fi

# --- boot 2: the take-over, on the same NVRAM ------------------------------
{
    echo "@echo -off"
    echo "fs0:"
    echo "set -v T cdh"
    echo "probe.efi tags"
    echo "probe.efi conout"
    echo "load hold.efi"
    echo "probe.efi tags"
    echo "probe.efi say-err errline"
    echo "stall 300000"
    echo "echo %T%-report"
    echo "stall 300000"
    echo "echo %T%-release"
    echo "stall 500000"
    echo "probe.efi tags"
    echo "probe.efi conout"
    echo "probe.efi say-err end"
    echo "reset -s"
} | test_set_startup
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

# The take-over puts the terminal's SetMode sequence on the serial line ahead
# of the next output, so lines are anchored at the END.
tags=$(grep -aoE 'PROBE stderr=[0-9]+ screen=[0-9]+$' "$TEST_CLEAN_LOG" | tr '\n' ';' || true)
if [[ "$tags" == "PROBE stderr=2 screen=1;PROBE stderr=2 screen=0;PROBE stderr=2 screen=1;" ]]; then
    test_host_pass "tags: the display leaves the StdErr fan-out with the take-over and returns after it; the serial terminal stays"
else
    test_host_fail "tags: want 'PROBE stderr=2 screen=1;PROBE stderr=2 screen=0;PROBE stderr=2 screen=1;', saw '$tags'"
fi
conout=$(grep -aoE 'PROBE conout=[0-9]+ screen=[0-9]+$' "$TEST_CLEAN_LOG" | tr '\n' ';' || true)
if [[ "$conout" == "PROBE conout=2 screen=1;PROBE conout=2 screen=1;" ]]; then
    test_host_pass "release: the display is a ConOut device again, as before the take-over"
else
    test_host_fail "release: want 'PROBE conout=2 screen=1;PROBE conout=2 screen=1;', saw '$conout'"
fi
if grep -aqE 'HOLD-READY$' "$TEST_CLEAN_LOG" && grep -aqE 'HOLD-RELEASED$' "$TEST_CLEAN_LOG"; then
    test_host_pass "hold: the take-over installed and released"
else
    test_host_fail "hold: the take-over installed and released (READY=$(grep -acE 'HOLD-READY$' "$TEST_CLEAN_LOG" || true) RELEASED=$(grep -acE 'HOLD-RELEASED$' "$TEST_CLEAN_LOG" || true))"
fi
seen=$(grep -aoE 'HOLD-SEEN errline=[0-9]+$' "$TEST_CLEAN_LOG" | tr '\n' ';' || true)
if [[ "$seen" == "HOLD-SEEN errline=1;" ]]; then
    test_host_pass "stderr: another image's StdErr line reaches the take-over device exactly once"
else
    test_host_fail "stderr: another image's StdErr line reaches the take-over device exactly once (saw '$seen')"
fi
n=$(grep -acE '(^|[^-])cdh-errline$' "$TEST_CLEAN_LOG" || true)
if [[ "$n" == 1 ]]; then
    test_host_pass "stderr: and the serial terminal (still in StdErr) shows it exactly once"
else
    test_host_fail "stderr: and the serial terminal shows it exactly once (saw $n)"
fi
if grep -aqE 'cdh-end$' "$TEST_CLEAN_LOG"; then
    test_host_pass "errout: the script ran to its end"
else
    test_host_fail "errout: the script ran to its end (no cdh-end line)"
fi

rc=0
test_check_leaks || rc=1

# --- boot 3: a StdErr tag back on the display mid-take-over ----------------
# While the take-over holds the console the display's StdErr tag comes back
# on its own (probe.efi retag-err, as a reconnect could). The release must
# still put the display's ConOut tag back: one combined install of both tags
# fails whole on the tag already there, and the display would leave the
# ConOut fan-out for good. Its own boot, so boot 2's release proves the
# StdErr re-tag the restore does itself.
{
    echo "@echo -off"
    echo "fs0:"
    echo "set -v T cdh"
    echo "load hold.efi"
    echo "probe.efi retag-err"
    echo "stall 300000"
    echo "echo %T%-report"
    echo "stall 300000"
    echo "echo %T%-release"
    echo "stall 500000"
    echo "probe.efi conout"
    echo "probe.efi say-err end3"
    echo "reset -s"
} | test_set_startup
test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 40
test_clean_log

n=$(grep -acE 'PROBE retagged=1$' "$TEST_CLEAN_LOG" || true)
if [[ "$n" == 1 ]]; then
    test_host_pass "retag: the display's StdErr tag came back during the take-over"
else
    test_host_fail "retag: the display's StdErr tag came back during the take-over (saw '$(grep -a 'PROBE retagged' "$TEST_CLEAN_LOG" || true)')"
fi
conout=$(grep -aoE 'PROBE conout=[0-9]+ screen=[0-9]+$' "$TEST_CLEAN_LOG" | tr '\n' ';' || true)
if [[ "$conout" == "PROBE conout=2 screen=1;" ]]; then
    test_host_pass "retag: the release still makes the display a ConOut device again"
else
    test_host_fail "retag: want 'PROBE conout=2 screen=1;', saw '$conout'"
fi
if grep -aqE 'cdh-end3$' "$TEST_CLEAN_LOG"; then
    test_host_pass "retag: the script ran to its end"
else
    test_host_fail "retag: the script ran to its end (no cdh-end3 line)"
fi

test_host_summary "console-device errout ($TEST_ARCH)" || rc=1
test_check_leaks || rc=1
exit "$rc"
