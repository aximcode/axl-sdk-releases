#!/bin/bash
# test-meta: arch=x64 needs= est=110 local-only=1
# test-console-device-takeover-qemu.sh — output take-over and restore: eviction, uninstall-restore, passthrough co-paint,
#
# Output take-over and restore: eviction, uninstall-restore, passthrough co-paint,
# and non-80x25 geometry surviving a shell scroll.
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
#            ./test/integration/test-console-device-takeover-qemu.sh [--arch X64]

CD_NAME="console-device take-over test"
# shellcheck source=lib/console-device-lib.sh
source "$(dirname "$0")/lib/console-device-lib.sh"

# Scenario 1 — take-over: the device evicts GraphicsConsole and delivers the
# shell's output as ops. The `ver` spew is load-bearing for the discriminator,
# not just proof of keyboard passthrough: its multi-line, full-width repaint
# would land in the x>=660 clean region if eviction had failed. SHOT_WAIT captures
# while resident (the driver stays taken over) after the driver is up + `ver` runs.
run_scenario "take-over" "$DRIVER" 16 "v e r ret" "" \
    "take-over verified (x>640 black + grid shows the shell output)"

# Scenario 2 — uninstall-restore: the restore-variant driver takes over (frame
# wiped pure black), then self-uninstalls at SELF_UNINSTALL_MS (re-tag + reconnect
# the firmware console + DisconnectController our own handle so ConSplitter drops
# us cleanly) and drives the restored gST->ConOut directly. A working restore
# paints those lines onto the black frame; a broken restore wedges on the first
# OutputString (the use-after-free this smoke first caught) and the frame stays
# black. No sendkey needed (the driver self-prints); SHOT_WAIT just needs to
# outlast the self-uninstall. That take-over blackened the frame first is
# established by Scenario 1, which shares the identical install path.
run_scenario "restore" "$RESTORE_DRIVER" 18 "" "--restored" \
    "uninstall restored the firmware console (it paints again)"

# Scenario 2b — passthrough (co-paint): the exact inverse of Scenario 1. Installing
# with passthrough_local=true SKIPS the eviction, so ConSplitter should fan to BOTH
# our grid and the firmware GraphicsConsole and the local display keeps working —
# which is what a consumer that only MIRRORS the console needs (evicting would blank
# the local monitor and freeze anything sampling the GOP, e.g. a BMC's KVM stream).
# The driver drives gST->ConOut directly with lines wider than its 80-col grid, so
# ink past x=660 can only be the firmware console's; no sendkey is needed (`ver` is
# far too narrow to reach that region, so it could not discriminate). SHOT_WAIT
# outlasts the driver's 2 s post-take-over probe delay.
run_scenario "passthrough" "$PASSTHRU_DRIVER" 18 "" "--passthrough" \
    "passthrough co-paints (local console alive + ops still delivered)"

# Scenario 3 — non-80x25 geometry survives scrolling (deterministic regression for
# the ConsoleLogger stale-RowsPerScreen bug). AGT's axcon hit an EDK2 ShellPkg
# ConsoleLogger.c(489) assert (CursorRow==RowsPerScreen*ScreenCount-1 -> CpuDeadLoop)
# when a FULL-SCREEN take-over console scrolls: the shell's ConsoleLogger caches its
# scrollback row count at shell start and only refreshes it on a SetMode routed
# through gST->ConOut; our take-over changed the geometry without one, so the stale
# count overflowed on scroll. The wide driver is built -DPRECACHE_SMALL, which forces
# ConsoleLogger to cache 80x25 right before taking over at 142x44 -> the stale-small
# window is deterministic, not the ~1-in-6 timing race AGT saw. We drive `dh` FIVE
# times (output >> the 3-screen history) and assert: (a) run_scenario's fatals grep
# sees NO ASSERT/CpuDeadLoop, AND (b) --wide sees content past x=640 (wide geometry
# active). Confirmed RED: without the install-time SetMode re-sync in
# axl-console-device.c this fires 4/4; with it, 8/8 clean.
run_scenario "wide-geometry" "$WIDE_DRIVER" 24 \
    "d h ret d h ret d h ret d h ret d h ret" "--wide" \
    "non-80x25 (142x44) survives shell scroll (no ConsoleLogger assert, wide render)"

test_host_summary "$CD_NAME (X64)"
