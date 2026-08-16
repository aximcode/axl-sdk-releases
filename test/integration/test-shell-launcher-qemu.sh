#!/bin/bash
# test-meta: arch=x64 needs= est=10 local-only=0
# test-shell-launcher-qemu.sh — regression guard for the AXL shell launcher
# that boots the EDK2 Shell with "-delay 0", skipping its 5-second
# "Press ESC in N seconds to skip startup.nsh" countdown.
#
# The launcher (test/integration/axl-shell-launcher.c) is staged as
# \EFI\BOOT\BOOTX64.EFI in place of the Shell by stage_boot_shell
# (scripts/axl-common.sh); it sibling-loads Shell.efi and starts it with
# LoadOptions "-delay 0". This test asserts the observable contract:
#   1. the startup countdown prompt never prints (the perf win), and
#   2. startup.nsh still runs (the launcher → Shell → startup.nsh chain works).
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

# The launcher is opt-in (stage_boot_shell defaults to booting the Shell
# directly); enable it for this test, which exists to validate it.
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
make -C "$PROJECT_DIR" ARCH="$_native_arch" hello shell-launcher 2>&1 | tail -2

EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$_native_arch")/hello.efi"

nsh="$(mktemp)"
stdout="$(mktemp)"   # non-raw filtered output (what integration tests grep)
raw="$(mktemp)"      # full ANSI-stripped transcript (--serial-log)
trap 'rm -f "$nsh" "$stdout" "$raw"' EXIT

# A marker proving startup.nsh ran (run-qemu appends `reset -s`).
printf 'echo AXL_LAUNCHER_STARTUP_OK\n' > "$nsh"

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

if (( fail )); then
    echo "FAIL: shell launcher -delay 0 ($TEST_ARCH)"
    exit 1
fi
echo "shell launcher -delay 0: countdown skipped, startup.nsh ran ($TEST_ARCH)"
exit 0
