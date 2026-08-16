#!/bin/bash
# test-meta: arch=x64 needs= est=250 local-only=1
# test-console-device — DEBUG-OVMF smoke for axl-console-device, the take-over
# console producer (output-only). Proves in real firmware what unit tests can't:
# that installing the device evicts the firmware GraphicsConsole and fans the
# Shell's console output through the device -> emit engine -> AxlConsoleOps ->
# a consumer's GOP grid, with the firmware console going silent (not co-painting)
# -- and that uninstalling brings the firmware console back.
#
# Six scenarios, each a separate DEBUG-OVMF boot (all assert zero firmware
# fatals in the debug log):
#
#   1. TAKE-OVER (console-device-smoke.efi): a resident `load`-ed driver renders
#      the ops into an 80x25 grid over x in [0,640), y in [0,400). We type
#      `ver`+Enter at the already-running Shell so it produces output through the
#      taken-over console (which also proves physical-keyboard passthrough
#      survives take_input=false), then analyze-console-device-shot.py asserts
#      x>=660 is pure black (GraphicsConsole evicted) AND the grid has real
#      content (the take-over delivered the output).
#
#   2. UNINSTALL-RESTORE (console-device-restore-smoke.efi, the same source built
#      with -DSELF_UNINSTALL_MS): the driver takes over (frame wiped black), then
#      self-uninstalls, re-tagging + reconnecting the firmware console. We type
#      `ver` AFTER that, and the analyzer's --restored mode asserts the firmware
#      console painted it back onto the frame (a broken restore leaves it black).
#
#   3. WIDE-GEOMETRY (console-device-wide-smoke.efi, -DGRID_COLS=142 -DAUTO_ALT
#      -DPRECACHE_SMALL): a NON-80x25 take-over console. Forces the shell's
#      ConsoleLogger to cache 80x25, then takes over at 142x44 and scrolls (5x `dh`);
#      the --wide mode asserts no ConsoleLogger.c(489) deadloop + the wide geometry
#      is active. Deterministic regression for the SetMode re-sync fix.
#
#   4. INPUT-RELAY (console-device-input-smoke.efi, -DTAKE_INPUT): the device also
#      becomes the sole ConInEx + evicts the raw keyboard + runs its read loop, so a
#      --sendkey `ver` can ONLY reach the shell through our relay. The --input mode
#      asserts the ver OUTPUT painted -> keys were delivered, no double-delivery.
#
#   5. INPUT-RESTORE (console-device-input-restore-smoke.efi, -DTAKE_INPUT
#      -DSELF_UNINSTALL_MS): takes over BOTH output and input, then self-uninstalls,
#      exercising the ConIn teardown (DisconnectController-before-free for our ConInEx,
#      the input mirror of the e051c0db UAF fix) in real firmware. --restored asserts
#      the frame painted -> the teardown was UAF-safe (a wedge would leave it black).
#
# LOCAL-ONLY: needs a GPU-capable QEMU + a DEBUG build of OVMF (release OVMF has
# the asserts compiled out, so it can't gate assert-absence). Point OVMF_CODE /
# OVMF_VARS at a DEBUG build, or stage one at the default path below; absent it,
# the test SKIPs. DEBUG OVMF is x86 (and the driver builds x64), so X64 only.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: [OVMF_CODE=/path/DEBUG/OVMF_CODE.fd OVMF_VARS=...] \
#            ./test/integration/test-console-device-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

# DEBUG OVMF is x86, and the smoke driver is built x64 — this path is X64 only.
if [[ "$TEST_ARCH" != "X64" ]]; then
    echo "console-device test: SKIP (DEBUG OVMF is x86 only; $TEST_ARCH not supported)"
    exit 0
fi

# Locate a DEBUG build of OVMF. Release OVMF compiles the asserts out, so it
# cannot gate the "0 firmware fatals" half of this smoke; require the DEBUG FV.
DBG_FV="$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV"
DBG_CODE="${OVMF_CODE:-$DBG_FV/OVMF_CODE.fd}"
DBG_VARS="${OVMF_VARS:-$DBG_FV/OVMF_VARS.fd}"
if [[ ! -f "$DBG_CODE" || ! -f "$DBG_VARS" ]]; then
    echo "WARN: DEBUG OVMF not found (looked for '$DBG_CODE' + '$DBG_VARS')."
    echo "      Set OVMF_CODE/OVMF_VARS to a DEBUG OVMF build to run this test."
    echo "console-device test: SKIP"
    exit 0
fi

# The analyzer needs PIL; without it we can't render a verdict from the shot.
if ! python3 -c 'import PIL' >/dev/null 2>&1; then
    echo "WARN: python3 PIL (Pillow) not available; the shot analyzer can't run."
    echo "console-device test: SKIP"
    exit 0
fi

RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
DRV_DIR="$(test_build_dir x64)/drivers"
DRIVER="$DRV_DIR/console-device-smoke.efi"
RESTORE_DRIVER="$DRV_DIR/console-device-restore-smoke.efi"
PASSTHRU_DRIVER="$DRV_DIR/console-device-passthrough-smoke.efi"
WIDE_DRIVER="$DRV_DIR/console-device-wide-smoke.efi"
INPUT_DRIVER="$DRV_DIR/console-device-input-smoke.efi"
INPUT_RESTORE_DRIVER="$DRV_DIR/console-device-input-restore-smoke.efi"
WIDE_RESTORE_DRIVER="$DRV_DIR/console-device-wide-restore-smoke.efi"
CYCLE_DRIVER="$DRV_DIR/console-device-cycle-smoke.efi"
# fbcon is the productized tool (a driver in tools/, not a smoke fixture in drivers/).
FBCON_DRIVER="$(test_build_dir x64)/tools/fbcon.efi"
ANALYZER="$TESTS_DIR/analyze-console-device-shot.py"

# Build the three smoke drivers (take-over + restore + wide-geometry). A build
# failure is a real FAIL (they're the test's own fixtures, not a
# missing-capability SKIP) — surface it loudly. The `if` guard keeps set -e /
# pipefail from aborting mid-pipe so we own the message instead of dying with a
# truncated `tail`.
if ! make -C "$PROJECT_DIR" ARCH=x64 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
        console-device-smoke console-device-restore-smoke console-device-wide-smoke \
        console-device-passthrough-smoke \
        console-device-input-smoke console-device-input-restore-smoke \
        console-device-wide-restore-smoke console-device-cycle-smoke fbcon 2>&1 | tail -20; then
    echo "console-device test: FAIL (driver build failed)"
    exit 1
fi
if [[ ! -f "$DRIVER" || ! -f "$RESTORE_DRIVER" || ! -f "$WIDE_DRIVER" \
      || ! -f "$INPUT_DRIVER" || ! -f "$INPUT_RESTORE_DRIVER" || ! -f "$FBCON_DRIVER" \
      || ! -f "$PASSTHRU_DRIVER" ]]; then
    echo "console-device test: FAIL (drivers not produced despite a clean build)"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Run one screenshot scenario and score it: run-qemu detects the driver's PE
# subsystem 11 and `load`s it at the Shell prompt, optional --sendkey types keys
# so the Shell produces output, then we screenshot + capture the DEBUG log. Scores
# three checks (screenshot present, zero firmware fatals, analyzer verdict).
# Args: <label> <driver> <shot_wait> <sendkey-tokens|""> <analyzer_flag> <verdict>.
# An empty sendkey string injects no keys (run-qemu's key loop iterates nothing;
# it still does the small pre-capture settle, which is harmless).
# find_firmware honors OVMF_CODE/OVMF_VARS, pinning the DEBUG FV.
run_scenario() {
    local label="$1" driver="$2" wait="$3" keys="$4" aflag="$5" verdict="$6"
    local shot="$WORK/$label.png" dbg="$WORK/$label.dbg"

    echo "=== $label ==="
    OVMF_CODE="$DBG_CODE" OVMF_VARS="$DBG_VARS" SHOT_WAIT="$wait" \
        timeout 160 "$RUN_QEMU" --arch X64 \
            --screenshot "$shot" --debugcon "$dbg" \
            --sendkey "$keys" \
            "$driver" >/dev/null 2>&1 || true

    if [[ ! -f "$shot" ]]; then
        test_host_fail "[$label] run-qemu produced a screenshot"
        return
    fi
    test_host_pass "[$label] run-qemu produced a screenshot"

    # Zero real firmware fatals in the DEBUG log. Match only unambiguous fatals:
    # NOT bare EXCEPTION/PANIC — OVMF prints benign
    # `InitializeExceptionStackSwitchHandlers` lines, and AHCI/PciSioSerial errors
    # are normal boot spew.
    if [[ -f "$dbg" ]]; then
        local fatals
        fatals=$(grep -aE 'ASSERT|Pool\.c\(721\)|Bad Signature|CpuDeadLoop|DeadLoop' \
                     "$dbg" || true)
        if [[ -z "$fatals" ]]; then
            test_host_pass "[$label] no firmware fatals (ASSERT/DeadLoop/Bad Signature)"
        else
            test_host_fail "[$label] firmware fatals present:"
            echo "$fatals" | head | sed 's/^/    /'
        fi
    else
        test_host_fail "[$label] run-qemu captured a debug log"
    fi

    # $aflag is intentionally unquoted: "" expands to no arg (take-over mode),
    # "--restored" to one.
    local rc
    if python3 "$ANALYZER" $aflag "$shot" > "$WORK/$label.analyze" 2>&1; then
        rc=0
    else
        rc=1
    fi
    sed 's/^/  /' "$WORK/$label.analyze"
    if [[ $rc -eq 0 ]]; then
        test_host_pass "[$label] $verdict"
    else
        test_host_fail "[$label] $verdict — FAILED (see analyzer output above)"
    fi
}

# fbcon self-restore scenarios (Ctrl+\ hotkey, auto-restore on shell exit). The
# generic run_scenario can only assert "no firmware fatal" for these -- fbcon paints
# a full frame either way, so --restored can't tell a torn-down fbcon from a live one.
# The definitive signal is fbcon's own StdErr (serial): it logs the restore reason and
# axl-console-device logs "console device uninstalled". So capture serial and grep it.
# $expect is a regex that MUST appear (the teardown ran); we also require no debugcon
# fatal (the teardown did not assert/wedge).
run_fbcon_serial_scenario() {
    local label="$1" keys="$2" expect="$3" verdict="$4"
    local shot="$WORK/$label.png" dbg="$WORK/$label.dbg" ser="$WORK/$label.serial"

    echo "=== $label ==="
    OVMF_CODE="$DBG_CODE" OVMF_VARS="$DBG_VARS" SHOT_WAIT=22 \
        timeout 160 "$RUN_QEMU" --arch X64 \
            --screenshot "$shot" --debugcon "$dbg" --serial-log "$ser" \
            --setvar AXL_LOG_LEVEL=debug \
            --sendkey "$keys" \
            "$FBCON_DRIVER" >/dev/null 2>&1 || true

    if [[ -f "$dbg" ]] && grep -aqE 'ASSERT|Pool\.c\(721\)|Bad Signature|CpuDeadLoop|DeadLoop' "$dbg"; then
        test_host_fail "[$label] firmware fatal during teardown:"
        grep -aE 'ASSERT|CpuDeadLoop|DeadLoop|Bad Signature' "$dbg" | head | sed 's/^/    /'
    else
        test_host_pass "[$label] no firmware fatals (teardown did not assert/wedge)"
    fi

    if [[ -f "$ser" ]] && grep -aqE "$expect" "$ser" && grep -aq "console device uninstalled" "$ser"; then
        test_host_pass "[$label] $verdict"
    else
        test_host_fail "[$label] $verdict — expected /$expect/ + 'console device uninstalled' in serial"
        [[ -f "$ser" ]] && grep -aE "fbcon:|console device" "$ser" | tail -4 | sed 's/^/    /'
    fi
}

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

test_host_summary "console-device test (X64)"
