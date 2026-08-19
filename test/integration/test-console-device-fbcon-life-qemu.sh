#!/bin/bash
# test-meta: arch=x64 needs= est=125 local-only=1
# test-console-device-fbcon-life-qemu.sh — fbcon's teardown and relaunch: the ctrl+\\ hotkey, auto-restore when the hosted
#
# fbcon's teardown and relaunch: the Ctrl+\\ hotkey, auto-restore when the hosted
# shell exits, reaping a lingering instance on re-run, and the launcher forwarding
# its -d/-g input-gate options to the resident driver.
#
# ONE OF FOUR. test-console-device-qemu.sh held all 15 DEBUG-OVMF boots and cost
# 379 s -- 11% of the X64 suite and 100% of the --only-local run's critical path,
# because the pool parallelises across tests and never within one. The scenarios
# cannot be merged (each loads a different driver and screenshots it), so they are
# split four ways; see test/integration/lib/console-device-lib.sh for why four and
# not more, and AXL-CI-Release-Speed-Design.md §12.11 for the measurement.
#
# Shared setup, the DEBUG-OVMF/Pillow guards and both scenario runners live in the
# lib. This file is the scenario list and nothing else.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: [OVMF_CODE=/path/DEBUG/OVMF_CODE.fd OVMF_VARS=...] \
#            ./test/integration/test-console-device-fbcon-life-qemu.sh [--arch X64]

CD_NAME="console-device fbcon lifecycle test"
# shellcheck source=lib/console-device-lib.sh
source "$(dirname "$0")/lib/console-device-lib.sh"

# Scenario 7 — fbcon Ctrl+\ hotkey: the key_filter peeks Ctrl+\ (folds to FS 0x1C),
# flags a leave, and the render timer tears the take-over down + restores the firmware
# console. Proves the hotkey reaches fbcon through the input relay AND that the
# render-timer-context uninstall is clean (the ConSplitter mode-assert fix is what
# makes fbcon's full-screen 160x50 restore not wedge here).
run_fbcon_serial_scenario "fbcon-leave" "ctrl-backslash" \
    "fbcon: Ctrl.*restoring the firmware console" \
    "Ctrl+\\ drops the take-over and restores the firmware console"

# Scenario 8 — fbcon auto-restore on shell exit: fbcon takes over the ONE shell it was
# loaded into; typing `exit` makes that shell's EFI_SHELL_PROTOCOL disappear, which the
# render timer detects (axl_shell_kind) and self-restores -- so BDS / a re-launched
# "EFI Internal Shell" gets a working console instead of fbcon squatting on it.
run_fbcon_serial_scenario "fbcon-exit" "e x i t ret" \
    "fbcon: hosted shell exited -- restoring the firmware console" \
    "typing exit auto-restores the firmware console (EFI Internal Shell re-entry)"

# Scenario 9 — fbcon RE-RUN (launcher reap + fresh take-over): run fbcon.efi -> Ctrl+\
# (restore, driver lingers resident with its presence marker) -> run `fbcon.efi` again.
# fbcon.efi is the subsystem-10 LAUNCHER app (it embeds + loads the take-over driver),
# so it runs as a command. The second run must REAP the lingering driver (UnloadImage
# via the presence marker -- "reaped N prior fbcon instance(s)") and start a fresh one:
# two "console device installed", no firmware fatal, no leaked instance. This is the
# image-lifecycle a single take-over misses.
FBCON_RELOAD_SHOT="$WORK/fbcon-reload.png"; FBCON_RELOAD_DBG="$WORK/fbcon-reload.dbg"
FBCON_RELOAD_SER="$WORK/fbcon-reload.serial"
echo "=== fbcon-reload ==="
OVMF_CODE="$DBG_CODE" OVMF_VARS="$DBG_VARS" SHOT_WAIT=30 \
    timeout 160 "$RUN_QEMU" --arch X64 \
        --screenshot "$FBCON_RELOAD_SHOT" --debugcon "$FBCON_RELOAD_DBG" --serial-log "$FBCON_RELOAD_SER" \
        --setvar AXL_LOG_LEVEL=debug \
        --sendkey "ctrl-backslash f b c o n dot e f i ret" \
        "$FBCON_DRIVER" >/dev/null 2>&1 || true
if [[ -f "$FBCON_RELOAD_DBG" ]] && grep -aqE 'ASSERT|Pool\.c\(721\)|Bad Signature|CpuDeadLoop|DeadLoop' "$FBCON_RELOAD_DBG"; then
    test_host_fail "[fbcon-reload] firmware fatal during reload:"
    grep -aE 'ASSERT|CpuDeadLoop|DeadLoop|Bad Signature' "$FBCON_RELOAD_DBG" | head | sed 's/^/    /'
else
    test_host_pass "[fbcon-reload] no firmware fatals across leave + reload"
fi
reload_installs=$(grep -ac "console device installed" "$FBCON_RELOAD_SER" 2>/dev/null || echo 0)
if [[ "$reload_installs" -ge 2 ]]; then
    test_host_pass "[fbcon-reload] re-running fbcon.efi re-takes-over cleanly (2 take-overs)"
else
    test_host_fail "[fbcon-reload] expected >=2 'console device installed' (got $reload_installs)"
    grep -aE "fbcon:|console device" "$FBCON_RELOAD_SER" 2>/dev/null | tail -6 | sed 's/^/    /'
fi
# The launcher must REAP the lingering prior instance (no leak) rather than pile a
# second one on top -- the whole point of the app-launches-driver restructure.
if grep -aq "reaped [1-9][0-9]* prior fbcon" "$FBCON_RELOAD_SER" 2>/dev/null; then
    test_host_pass "[fbcon-reload] the second run reaped the lingering prior instance (no leak)"
else
    test_host_fail "[fbcon-reload] expected the launcher to reap the prior fbcon instance"
    grep -aE "fbcon:|reaped" "$FBCON_RELOAD_SER" 2>/dev/null | tail -6 | sed 's/^/    /'
fi

# Scenario 10 — input-gate CLI knobs (`fbcon.efi -d <ms> -g <ms>`). The launcher forwards
# its LoadOptions to the resident driver, which parses -d (same-key debounce) / -g
# (all-key min-gap) into the console-device gate. Verifies the launcher->driver arg
# plumbing end-to-end: run with `-d 55 -g 12` and assert the driver logged the parsed
# values (StdErr -> serial). This is the cheap-re-run knob for the real-HW key-bounce A/B.
FBCON_GATE_DBG="$WORK/fbcon-gate.dbg"; FBCON_GATE_SER="$WORK/fbcon-gate.serial"
echo "=== fbcon-gate-opts ==="
OVMF_CODE="$DBG_CODE" OVMF_VARS="$DBG_VARS" SHOT_WAIT=12 \
    timeout 160 "$RUN_QEMU" --arch X64 \
        --screenshot "$WORK/fbcon-gate.png" --debugcon "$FBCON_GATE_DBG" --serial-log "$FBCON_GATE_SER" \
        "$FBCON_DRIVER" -d 55 -g 12 >/dev/null 2>&1 || true
if [[ -f "$FBCON_GATE_DBG" ]] && grep -aqE 'ASSERT|Pool\.c\(721\)|Bad Signature|CpuDeadLoop|DeadLoop' "$FBCON_GATE_DBG"; then
    test_host_fail "[fbcon-gate-opts] firmware fatal:"
    grep -aE 'ASSERT|CpuDeadLoop|DeadLoop|Bad Signature' "$FBCON_GATE_DBG" | head | sed 's/^/    /'
else
    test_host_pass "[fbcon-gate-opts] no firmware fatals with -d/-g args"
fi
if grep -aq "input gate: debounce=55 ms, min_gap=12 ms" "$FBCON_GATE_SER" 2>/dev/null; then
    test_host_pass "[fbcon-gate-opts] -d/-g reach the device gate through the launcher"
else
    test_host_fail "[fbcon-gate-opts] expected 'input gate: debounce=55 ms, min_gap=12 ms' in the log"
    grep -aE "fbcon:|input gate|console device" "$FBCON_GATE_SER" 2>/dev/null | tail -6 | sed 's/^/    /'
fi

test_host_summary "$CD_NAME (X64)"
