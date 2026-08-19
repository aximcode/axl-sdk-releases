#!/bin/bash
# test-meta: arch=x64 needs= est=110 local-only=1
# test-console-device-input-qemu.sh — input ownership and teardown: the coninex relay, its uninstall (the input mirror
#
# Input ownership and teardown: the ConInEx relay, its uninstall (the input mirror
# of the ConOut UAF), the wide-geometry uninstall, and three take-over/restore
# cycles in one boot.
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
#            ./test/integration/test-console-device-input-qemu.sh [--arch X64]

CD_NAME="console-device input test"
# shellcheck source=lib/console-device-lib.sh
source "$(dirname "$0")/lib/console-device-lib.sh"

# Scenario 4 — input relay (take_input=true). The device becomes the sole ConInEx
# AND evicts the raw firmware keyboard, then runs its internal read loop. So the
# ONLY path from a --sendkey `ver` to the shell is our read-loop -> inject ->
# ConInEx relay: if `ver` runs and its output paints into the grid, input ownership
# + delivery work AND there is no double-delivery (the raw keyboard is gone, so a
# doubled keystroke can't come from it). --input asserts the ver-output region (the
# rows below the prompt) is non-blank -> the command executed; a broken relay leaves
# it blank. The uninstall on unload also exercises the ConIn teardown (the input
# mirror of the e051c0db UAF fix). The Ctrl+letter fold itself is unit-tested
# (test_console_device_input: Simple read folds Ctrl+C to 0x03).
run_scenario "input-relay" "$INPUT_DRIVER" 20 "v e r ret" "--input" \
    "take_input delivers keystrokes through the relay (ver runs, no double-delivery)"

# Scenario 5 — input-relay UNINSTALL (take_input + self-uninstall). The driver takes
# over BOTH output and input, then self-uninstalls, running the ConIn teardown
# (DisconnectController-before-free for our ConInEx + re-admit the keyboard) in real
# firmware -- the input mirror of the ConOut UAF fixed in e051c0db. If that teardown
# wedged (a ConInEx pointer still fanned after free), the uninstall would hang before
# it drives the restored gST->ConOut and the frame would stay black; --restored
# asserts the frame painted, so a clean pass proves the input teardown is UAF-safe.
run_scenario "input-restore" "$INPUT_RESTORE_DRIVER" 18 "" "--restored" \
    "take_input uninstall tears down the ConIn relay cleanly (no UAF; console restored)"

# Scenario 5b — WIDE self-uninstall (142x44 + self-uninstall). Regression guard for
# the ConSplitter mode-reconstruction assert: re-adding GraphicsConsole while our
# single non-80x25 device was still a fan-out member tripped ASSERT(!EFI_ERROR) in
# ConSplitterAddGraphicsOutputMode (ConSplitter.c:2983 -> CpuDeadLoop under DEBUG
# OVMF; silent wedge under RELEASE), leaving the frame black. axl_console_device_-
# uninstall now disconnects our device BEFORE re-adding the firmware console, so the
# reconstruction sees only firmware consoles. The 80x25 restore-smokes could not
# catch this (80x25 IS the fallback BaseMode). --restored asserts the frame repainted
# after restore, so a clean pass proves the wide-geometry uninstall no longer wedges.
run_scenario "wide-restore" "$WIDE_RESTORE_DRIVER" 18 "" "--restored" \
    "non-80x25 (142x44) uninstall restores the console (no ConSplitter mode assert)"

# Scenario 5c — MULTI-CYCLE (142x44, take over -> restore -> re-take-over x3 in one
# boot). Guards cumulative-state bugs a single uninstall misses: a second uninstall,
# re-eviction of the just-restored GraphicsConsole, and any ConSplitter state carried
# across cycles (the mode assert would fire on every re-add). The console is left
# restored after the last cycle; a wedge in ANY cycle stops the loop before the final
# restore, so --restored (frame repainted) passing proves all 3 cycles completed clean.
run_scenario "cycle" "$CYCLE_DRIVER" 18 "" "--restored" \
    "3x take-over/restore cycles in one boot leave the console live (no cumulative wedge)"

test_host_summary "$CD_NAME (X64)"
