# shellcheck shell=bash
# console-device-lib.sh — shared fixture setup and scenario runners for the
# test-console-device-*-qemu.sh group.
#
# WHY THIS IS FOUR TESTS AND NOT ONE. It was one, holding 15 serial DEBUG-OVMF
# boots at 25.3 s each -- 379 s, which was 11% of the whole X64 suite AND 100%
# of the --only-local run's critical path. run-integration's pool parallelises
# ACROSS tests and never WITHIN one, so those 15 boots were 15 units of work
# pinned to a single worker.
#
# The scenarios cannot be MERGED into fewer boots: each loads a DIFFERENT driver
# at the shell prompt and screenshots the result, and several deliberately need
# clean firmware state. So they are split instead, and the split is sized rather
# than guessed: the --only-local set is 733 s of work over 6 workers, a 122 s
# floor, so the longest piece only has to get under that. Four groups puts the
# largest at ~107 s; a fifth would buy nothing, because the floor is then the
# binding constraint and not this test. (AXL-CI-Release-Speed-Design.md §12.11.)
#
# Each group sets CD_NAME, sources this file, runs its scenarios, and calls
# test_host_summary. The guards below exit 0 on a SKIP, so a machine without
# DEBUG OVMF or Pillow skips all four identically.
#
# Usage from a group script:
#   CD_NAME="console-device take-over"
#   source "$(dirname "$0")/lib/console-device-lib.sh"

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

# DEBUG OVMF is x86, and the smoke driver is built x64 — this path is X64 only.
if [[ "$TEST_ARCH" != "X64" ]]; then
    echo "$CD_NAME: SKIP (DEBUG OVMF is x86 only; $TEST_ARCH not supported)"
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
    echo "$CD_NAME: SKIP"
    exit 0
fi

# The analyzer needs PIL; without it we can't render a verdict from the shot.
if ! python3 -c 'import PIL' >/dev/null 2>&1; then
    echo "WARN: python3 PIL (Pillow) not available; the shot analyzer can't run."
    echo "$CD_NAME: SKIP"
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
# flock, because the four group scripts run CONCURRENTLY under run-integration's
# pool and would otherwise drive `make` at the same targets at the same time --
# two `ld -o` on one path. The build is incremental, so the three that wait pay
# a no-op make once the first finishes. Degrades to an unlocked build when flock
# is absent (a standalone run, where there is no concurrency to guard).
_cd_lock="${TMPDIR:-/tmp}/axl-console-device-fixtures.lock"
_cd_flock=(); command -v flock >/dev/null 2>&1 && _cd_flock=(flock "$_cd_lock")
if ! "${_cd_flock[@]}" make -C "$PROJECT_DIR" ARCH=x64 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
        console-device-smoke console-device-restore-smoke console-device-wide-smoke \
        console-device-passthrough-smoke \
        console-device-input-smoke console-device-input-restore-smoke \
        console-device-wide-restore-smoke console-device-cycle-smoke fbcon 2>&1 | tail -20; then
    echo "$CD_NAME: FAIL (driver build failed)"
    exit 1
fi
if [[ ! -f "$DRIVER" || ! -f "$RESTORE_DRIVER" || ! -f "$WIDE_DRIVER" \
      || ! -f "$INPUT_DRIVER" || ! -f "$INPUT_RESTORE_DRIVER" || ! -f "$FBCON_DRIVER" \
      || ! -f "$PASSTHRU_DRIVER" ]]; then
    echo "$CD_NAME: FAIL (drivers not produced despite a clean build)"
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

