#!/bin/bash
# test-meta: arch=x64 needs= est=10 local-only=0 builds=hello,shell-launcher guest=none
# guest=none: the boot is the subject: the AXL Shell launcher's -delay 0
#   boot.
# test-shell-launcher-qemu.sh — regression guard for the AXL shell launcher
# that boots the EDK2 Shell with "-delay 0", skipping its 5-second
# "Press ESC in N seconds to skip startup.nsh" countdown.
#
# The launcher (test/integration/axl-shell-launcher.c) is staged as
# \EFI\BOOT\BOOTX64.EFI in place of the Shell by stage_boot_shell
# (scripts/axl-common.sh); it sibling-loads Shell.efi and starts it with
# LoadOptions "-delay 0". This test asserts the observable contract:
#   1. the startup countdown prompt never prints (the perf win), and
#   2. startup.nsh still runs (the launcher → Shell → startup.nsh chain works),
#   3. AXL_SHELL_OPTIONS reaches the Shell: stage_boot_shell writes it to
#      axl-shell.opt beside the launcher, which appends it to "-delay 0". A
#      boot with "-nonest" must see `nonesting` read True; the first boot, with
#      no options, is the control that must see False -- so the True is the
#      option's doing, not the Shell's default.
# A silent regression here (launcher fails to skip the delay, or falls back and
# the Shell boots with the countdown) leaves the suite passing but slow — which
# is exactly what this guard catches.
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) — integration test.
#
# Usage: ./test/integration/test-shell-launcher-qemu.sh [--arch X64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

# =1 is needed here for the OPPOSITE reason it once was. run-qemu.sh now uses
# the launcher by default and heals itself when the Shell is not reached, which
# would turn a real launcher regression into a silent slow-but-green pass. =1
# means "always, ignore the cached verdict, no fallback", so the failure this
# test exists to catch stays a failure.
export AXL_SHELL_LAUNCHER=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

# The staging (stage_boot_shell) builds the launcher on demand, but build the
# app + launcher up front so a missing-binary failure is loud here, not a
# confusing boot-time fallback.
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_native_arch" hello shell-launcher 2>&1 | tail -2

EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$_native_arch")/hello.efi"

nsh="$(mktemp)"
stdout="$(mktemp)"   # non-raw filtered output (what integration tests grep)
raw="$(mktemp)"      # full ANSI-stripped transcript (--serial-log)
stdout2="$(mktemp)"  # the AXL_SHELL_OPTIONS boot's output
trap 'rm -f "$nsh" "$stdout" "$raw" "$stdout2"' EXIT

# A marker proving startup.nsh ran (run-qemu appends `reset -s`), and the
# Shell's nesting mode, which AXL_SHELL_OPTIONS=-nonest must change.
printf 'echo AXL_LAUNCHER_STARTUP_OK\necho NONEST=%%nonesting%%\n' > "$nsh"

# Run in NON-raw mode on purpose: that path anchors app-output extraction on the
# startup sentinel, which the launcher's countdown removal would otherwise break.
# --serial-log keeps the full transcript so the countdown check can see it (the
# non-raw filter strips the countdown lines from stdout).
timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" \
    --timeout 40 --serial-log "$raw" --nsh "$nsh" "$EFI" > "$stdout" 2>&1 || true

fail=0
# 1. Countdown prompt must be gone (Delay=0 → the countdown loop never prints).
#    Checked in the raw transcript — the non-raw filter strips countdown lines.
if grep -q "seconds to skip" "$raw"; then
    echo "  HIT: startup countdown still present (launcher did not skip it)"
    fail=1
fi
# 2. The app's output must survive the non-raw filter — this guards the sentinel
#    that replaced the countdown anchor. A broken filter swallows all app output.
if ! grep -q "AXL_LAUNCHER_STARTUP_OK" "$stdout"; then
    echo "  MISS: startup marker absent from non-raw output (filter/sentinel broken)"
    fail=1
fi
# 3. The Shell itself must have started (guards against a load/start failure).
if ! grep -q "UEFI Interactive Shell" "$raw"; then
    echo "  MISS: Shell did not start"
    fail=1
fi
# 4. The launcher must not have printed an error/fallback warning.
if grep -q "axl-shell-launcher:" "$raw"; then
    echo "  HIT: launcher reported an error"
    grep "axl-shell-launcher:" "$raw" | sed 's/^/    /'
    fail=1
fi
# 5. Control: with no options the Shell nests (the EDK2 default).
if ! grep -qx "NONEST=False" <(tr -d '\r' < "$stdout"); then
    echo "  MISS: control boot did not print NONEST=False"
    grep -a "NONEST" "$stdout" | sed 's/^/    /'
    fail=1
fi

# 6. AXL_SHELL_OPTIONS=-nonest reaches the Shell.
AXL_SHELL_OPTIONS="-nonest" timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" \
    --arch "$TEST_ARCH" --timeout 40 --nsh "$nsh" "$EFI" > "$stdout2" 2>&1 || true
if ! grep -qx "NONEST=True" <(tr -d '\r' < "$stdout2"); then
    echo "  MISS: AXL_SHELL_OPTIONS=-nonest boot did not print NONEST=True"
    grep -aE "NONEST|axl-shell-launcher:|AXL_SHELL_OPTIONS" "$stdout2" | sed 's/^/    /'
    fail=1
fi

# 7. Options with the launcher switched off cannot apply: an error, never a
#    silent boot of a Shell without them.
rc7=0
out7=$(AXL_SHELL_LAUNCHER=0 AXL_SHELL_OPTIONS="-nonest" timeout 90s \
    "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$TEST_ARCH" --timeout 40 \
    --nsh "$nsh" "$EFI" 2>&1) || rc7=$?
if (( rc7 == 0 )) || ! grep -q "needs the Shell launcher" <<< "$out7" \
   || grep -q "NONEST=" <<< "$out7"; then
    echo "  MISS: AXL_SHELL_OPTIONS with AXL_SHELL_LAUNCHER=0 did not refuse (rc=$rc7)"
    tail -3 <<< "$out7" | sed 's/^/    /'
    fail=1
fi

if (( fail )); then
    echo "FAIL: shell launcher -delay 0 ($TEST_ARCH)"
    exit 1
fi
echo "shell launcher -delay 0: countdown skipped, startup.nsh ran, options reach the Shell ($TEST_ARCH)"
exit 0
