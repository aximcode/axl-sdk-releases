#!/bin/bash
# test-meta: arch=x64 needs= est=95 local-only=1
# test-console-device-fbcon-qemu.sh — fbcon's rendering pipeline: a taken-over shell rendered as a cell grid, typing
#
# fbcon's rendering pipeline: a taken-over shell rendered as a cell grid, typing
# into `edit` through the pointer proxy, and a guest Ctrl+C not freezing the
# render loop.
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
#            ./test/integration/test-console-device-fbcon-qemu.sh [--arch X64]

CD_NAME="console-device fbcon render test"
# shellcheck source=lib/console-device-lib.sh
source "$(dirname "$0")/lib/console-device-lib.sh"

# Scenario 6 — fbcon: the PRODUCTIZED graphical terminal (tools/fbcon.efi), not a
# smoke fixture. It installs the take-over device with take_input + read_physical +
# the terminal's key_filter hotkey hook, then renders the op stream as a FULL-SCREEN
# AxlConsoleTerm cell grid. We --sendkey `ver` so the shell produces output through
# the terminal. Because fbcon paints edge-to-edge (unlike the 80x25 smokes), the
# x>640-black eviction discriminator doesn't apply; --fbcon instead asserts the
# terminal rendered a screenful of content. Together with the zero-firmware-fatals
# check this proves the whole pipeline — take-over, AxlConsoleTerm render, the
# key_filter/input relay, and uninstall — runs fatal-free in real firmware. Pointer
# selection/zoom + fine-grained key-delivery stay unit-tested (disclosed gap).
run_scenario "fbcon" "$FBCON_DRIVER" 20 "v e r ret" "--fbcon" \
    "fbcon renders the taken-over shell + delivers keystrokes through the relay"

# Scenario 6b — TYPING INTO edit under fbcon (pointer-proxy keyboard regression guard).
# fbcon takes over with take_pointer=true, which evicts the real SimplePointer(s) and
# interposes a yielding proxy so a guest's GetState poll (edit busy-polls it) idles the
# CPU. The bug this guards: the proxy forwarding a spurious "mouse active" every poll
# (from the evicted ConSplitter aggregator) made edit starve its keyboard read, so
# typed characters never appeared. We --sendkey `edit <ret>` then a run of letters; the
# --fbcon-edit analyzer asserts the text row (cols 1+, skipping edit's parked cursor
# block) rendered them. A blank row = the keystrokes never reached the guest.
run_scenario "fbcon-edit-type" "$FBCON_DRIVER" 22 \
    "e d i t ret a b c d e f g h" "--fbcon-edit" \
    "typing into edit under fbcon renders the characters (proxy did not starve the keyboard)"

# Scenario 6c — guest Ctrl+C does not freeze fbcon's render loop (loop-interception
# regression guard). Our read loop routes a guest Ctrl+C to the Shell's registered
# notify, which signals ExecutionBreak. fbcon's render/pointer loop must NOT treat that
# as its OWN Ctrl+C=quit (it calls axl_loop_set_intercept_break(false)) -- otherwise the
# loop quits, the render + pointer pump stop, and every line after the Ctrl+C (echoed
# input AND command output) never paints while the display + mouse freeze. We --sendkey
# `echo 1 <ret>`, a Ctrl+C, then more `echo N <ret>` lines; --fbcon-ctrlc asserts the
# post-Ctrl+C lines rendered below the freeze point. A blank lower region = the loop quit.
run_scenario "fbcon-ctrlc" "$FBCON_DRIVER" 22 \
    "e c h o spc 1 ret ctrl-c e c h o spc 2 ret e c h o spc 3 ret e c h o spc 4 ret" \
    "--fbcon-ctrlc" \
    "a guest Ctrl+C does not freeze fbcon's render loop (loop did not self-quit on ExecutionBreak)"

test_host_summary "$CD_NAME (X64)"
