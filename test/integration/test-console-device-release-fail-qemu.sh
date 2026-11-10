#!/bin/bash
# test-meta: arch=both needs= est=50 local-only=0 guest=none
# test-meta: builds=console-device-hold
# guest=none: it types into the guest Shell (test_send_stdin), and a leased
#   guest takes no typed serial input.
# test-console-device-release-fail-qemu.sh -- an uninstall the firmware
# refuses must leave the keyboard working, and a later uninstall resumes.
#
# console-device-hold.efi installs the take-over device with take_input +
# read_physical: the firmware's ConIn devices (here the serial terminal) are
# evicted, and the device's read loop is the ONLY path from a typed key to the
# Shell. A marker arms one of the library's test points and has the fixture
# call axl_console_device_uninstall, which must return AXL_ERR, keep the
# instance, and leave input working. Two refusals, one boot each:
#
#   disconnect       DisconnectController of our handle is refused, BEFORE
#                    the firmware keyboards are re-admitted: the read loop
#                    must run again;
#   conin-uninstall  the uninstall of our ConIn protocols is refused, AFTER
#                    the firmware keyboards are re-admitted: uninstall must
#                    not go on to free the instance those protocols point
#                    into.
#
# Then the host types at the Shell's prompt over serial (test_send_stdin):
#
#   echo kb-alive-%T%   must print `kb-alive-cdh`: keys reach the Shell after
#                       the refusal;
#   echo %T%-release    a retry, which must succeed (HOLD-RELEASED) -- after
#                       a refused ConIn uninstall the device no longer sees
#                       the Shell's output, so the fixture retries by itself,
#                       15 s after the refusal;
#   echo kb-back-%T%    must print `kb-back-cdh`: the restored firmware
#                       keyboard reaches the Shell.
#
# The typed command line echoes the literal `%T%`, so only the command's
# OUTPUT can match `-cdh`.
#
# RED (before the fixes): uninstall stopped the read loop before the
# disconnect and returned AXL_ERR with it stopped, so no key reached the
# Shell; and it ignored a refused ConIn uninstall, cleared its record of it
# and freed the instance, reporting success.
#
# In the conin-uninstall boot the kb-alive step does NOT tell the fix from
# the bug: the firmware keyboards are re-admitted before that refusal, so keys
# reach the Shell either way. What it shows is that a refused uninstall,
# with the instance kept, leaves the restored keyboard working -- and the
# test asserts from the timestamped log that it ran BEFORE the retry. The
# discriminating assertion there is the refusal count.
#
# AXL_ERR needs firmware that refuses, which OVMF never does: the refusal is
# the fixture's strong _axl_console_device_test_point (the library's weak one
# never fires). It is a CLEAN refusal -- nothing of the refused step happens
# -- which is one of the shapes EDK2 can produce, not the only one. What
# follows the refusal is real.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-console-device-release-fail-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export TEST_SKIP_RATCHET=1
# shellcheck source=common-test.sh
source "$SCRIPT_DIR/common-test.sh"
test_parse_args "$@"
test_setup

declare -A _MK=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_MK[$TEST_ARCH]}" console-device-hold 2>&1 | tail -2

PREFIX_DIR="$(test_build_dir)"
test_add_efi "$PREFIX_DIR/drivers/console-device-hold.efi" console-device-hold.efi

# Each step types one command at the prompt and waits for what it must print.
# A step that times out leaves the rest unreachable, so they are reported as
# failed rather than skipped: the keyboard IS what is under test.
step() {   # step <label> <keys> <pattern> <secs> <description>
    test_send_stdin "$2"
    if test_wait_for "$3" "$4"; then
        test_host_pass "$1: $5"
    else
        test_host_fail "$1: $5 (no '$3' within ${TEST_WAITED_S}s)"
    fi
}

# scenario <test point> <marker> <the library's warning for that refusal>
#          <keys that ask for the retry, or '' when the fixture retries itself>
scenario() {
    local point="$1" marker="$2" warning="$3" retry_keys="$4"

    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        echo "set -v T cdh"
        echo "load console-device-hold.efi"
        echo "echo %T%-$marker"
        echo "stall 1000000"
        echo "echo CDRF-SCRIPT-DONE"
    } | test_set_startup
    test_build_image

    echo "=== console-device release failure: $point ($TEST_ARCH) ==="

    test_build_qemu_cmd
    test_add_no_network
    test_enable_stdin
    test_run_background

    if test_wait_boot "CDRF-SCRIPT-DONE" 90; then
        test_host_pass "$point: the script ran to the end"
    else
        test_host_fail "$point: the script ran to the end (no CDRF-SCRIPT-DONE within ${TEST_WAITED_S}s)"
    fi
    # The prompt follows the script's end; give the Shell a moment to reach it.
    sleep 2

    step "$point" 'echo kb-alive-%T%\r' 'kb-alive-cdh' 20 \
        "after the refused uninstall, typed keys still reach the Shell"
    step "$point" "$retry_keys" 'HOLD-RELEASED' 40 \
        "a retried uninstall succeeds"
    step "$point" 'echo kb-back-%T%\r' 'kb-back-cdh' 20 \
        "after the release, the restored keyboard reaches the Shell"
    test_send_stdin 'reset -s\r'
    sleep 2

    test_clean_log

    # The refusal really happened, exactly once, through the library's path,
    # and the first release reported it rather than success.
    local n_failed n_warned n_released
    n_failed=$(grep -acE 'HOLD-RELEASE-FAILED rc=-1$' "$TEST_CLEAN_LOG" || true)
    n_warned=$(grep -acF "$warning" "$TEST_CLEAN_LOG" || true)
    # Anchored at the END only: x64's take-over can put the terminal's mode
    # sequence on the serial line just before the next output.
    n_released=$(grep -acE 'HOLD-RELEASED$' "$TEST_CLEAN_LOG" || true)
    if [[ "$n_failed" == 1 && "$n_warned" == 1 && "$n_released" == 1 ]]; then
        test_host_pass "$point: the first uninstall was refused (AXL_ERR), the retry released"
    else
        test_host_fail "$point: the first uninstall was refused (AXL_ERR), the retry released (failed=$n_failed warned=$n_warned released=$n_released)"
    fi

    # The keyboard step ran in the refused state, not after the retry: the
    # raw log's line order (the timestamps share one clock) puts kb-alive
    # first.
    local l_alive l_released
    l_alive=$(grep -anF 'kb-alive-cdh' "$TEST_LOG" | grep -av 'echo kb-alive' | head -1 | cut -d: -f1)
    l_released=$(grep -anE 'HOLD-RELEASED' "$TEST_LOG" | head -1 | cut -d: -f1)
    if [[ -n "$l_alive" && -n "$l_released" && "$l_alive" -lt "$l_released" ]]; then
        test_host_pass "$point: the keys reached the Shell before the retry released the device"
    else
        test_host_fail "$point: the keys reached the Shell before the retry released the device (kb-alive line ${l_alive:-none}, released line ${l_released:-none})"
    fi

    kill "$TEST_QEMU_PID" 2>/dev/null || true
    wait "$TEST_QEMU_PID" 2>/dev/null || true
    TEST_QEMU_PID=0
    exec {TEST_STDIN_FD}>&-
    unset TEST_STDIN_FD
}

# After a refused disconnect the device still receives the Shell's output,
# so a typed marker asks for the retry. After a refused ConIn uninstall the
# firmware consoles are back and the device sees nothing, so the fixture
# retries by itself 3 s later.
scenario disconnect fail-release \
    'console device uninstall: could not disconnect our console handle' \
    'echo %T%-release\r'
scenario conin-uninstall fail-conin \
    'console device uninstall: could not uninstall our console input protocols' \
    ''

test_host_summary "console-device release failure ($TEST_ARCH)"
