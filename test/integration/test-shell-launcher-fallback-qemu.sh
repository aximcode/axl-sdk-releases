#!/bin/bash
# test-meta: arch=x64 needs= est=80 local-only=0
# test-shell-launcher-fallback-qemu.sh — guards the self-healing default.
#
# The AXL shell launcher (test/integration/axl-shell-launcher.c) is staged as
# \EFI\BOOT\BOOTX64.EFI in place of the Shell and chains it with "-delay 0",
# skipping the 5 s startup countdown. It was once observed to hang some
# firmware: the guest loads BOOTX64.EFI and never reaches the Shell. That
# firmware no longer exists on any machine here, so the fault cannot be
# reproduced by finding it again — which is why the launcher stayed opt-in and
# every consumer kept paying ~4.6 s per boot.
#
# This test reproduces the fault CLASS instead of the firmware, via
# AXL_SHELL_LAUNCHER_BIN: stage a binary that loads and never chains, and the
# guest is in exactly the observed state. kbprobe is that binary — it blocks
# forever in axl_console_read_key(UINT64_MAX) with no key to read.
#
# Two distinct failure shapes exist and only one needs the retry (measured):
#   - a launcher that RETURNS is already self-healed by firmware: BdsDxe falls
#     through to Boot0002 "EFI Internal Shell", which runs startup.nsh anyway.
#     The user pays the countdown and nothing else. No retry needed.
#   - a launcher that HANGS never gives BdsDxe control back. Nothing recovers,
#     and this is the shape that was actually reported.
#
# The contract asserted here:
#   1. default ON, working launcher    -> launcher used, no memo written
#   2. default ON, hanging launcher    -> run still SUCCEEDS via a retry, and a
#                                         negative memo is recorded
#   3. memo present                    -> launcher not staged at all
#   4. AXL_SHELL_LAUNCHER=1            -> memo ignored, and no retry (forced-on
#                                         means a failure must stay loud)
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1) — integration test.
#
# Usage: ./test/integration/test-shell-launcher-fallback-qemu.sh [--arch X64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

# hello = the app under test; kbprobe = the hanging fault injection; the
# launcher itself is what scenario 1 exercises. Build up front so a missing
# binary is a loud failure here, not a confusing boot-time fallback.
make -C "$PROJECT_DIR" ARCH="$_native_arch" hello kbprobe shell-launcher 2>&1 | tail -1

PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$_native_arch")"
APP="$PREFIX/hello.efi"
HANG_LAUNCHER="$PREFIX/kbprobe.efi"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Isolate the memo from the developer's real cache, and make it inspectable.
export XDG_CACHE_HOME="$WORK/cache"
MEMO_DIR="$XDG_CACHE_HOME/axl-sdk/shell-launcher-blocklist"

NSH="$WORK/probe.nsh"
printf 'echo AXL_FALLBACK_APP_RAN\n' > "$NSH"

# Pin run-qemu.sh's scratch base so a retry leak is countable. The retry is a
# CALL, not an exec, precisely so the EXIT trap still fires: an exec would leak
# the first attempt's disk image + logs and, under --mount, orphan virtiofsd.
# This is the leak class that once accumulated 8.6 GB from timeout-killed runs
# (see run-qemu.sh's trap).
export AXL_QEMU_TMPDIR="$WORK/qtmp"
mkdir -p "$AXL_QEMU_TMPDIR"
qemu_tmp_count() { find "$AXL_QEMU_TMPDIR" -maxdepth 1 -type d -name 'axl-qemu.*' | wc -l; }

fail=0

# Count memo files without tripping `set -e` on an absent directory.
memo_count() {
    [[ -d "$MEMO_DIR" ]] || { echo 0; return; }
    find "$MEMO_DIR" -type f | wc -l
}

# run_case <label> <serial-log> <stdout-log> <guest-timeout> [env...] — runs the
# app under run-qemu.sh and records the exit status rather than letting `set -e`
# abort. A launcher hang is expected to burn the full guest timeout, so the
# outer timeout must clear it plus a full retry boot.
#
# The guest timeout is per-scenario, not shared, because the two kinds of
# scenario want opposite things from it. A scenario that must SUCCEED needs
# enough headroom that a slow boot under the parallel suite pool is not mistaken
# for the injected fault — which would write a memo against the real launcher
# and fail with a misleading message. A scenario that must FAIL only needs
# enough time to prove the Shell was not reached, so a big timeout there is
# dead wall-clock. 40 s matches the sibling test-shell-launcher-qemu.sh.
run_case() {
    local label="$1" raw="$2" out="$3" guest_timeout="$4"; shift 4
    # stderr, not stdout: stdout IS this function's return value.
    echo "--- $label" >&2
    # -u before "$@": run-integration.sh exports AXL_SHELL_LAUNCHER=1 to every
    # test, so without this the auto-mode scenarios would silently run forced-on
    # — passing standalone and failing only inside the suite.
    env -u AXL_SHELL_LAUNCHER -u AXL_SHELL_LAUNCHER_BIN -u AXL_SHELL_LAUNCHER_RETRIED \
        "$@" timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$TEST_ARCH" --timeout "$guest_timeout" \
        --serial-log "$raw" --nsh "$NSH" "$APP" > "$out" 2>&1 && echo 0 || echo $?
}

# ---------------------------------------------------------------------------
# 1. POSITIVE CONTROL — default (AXL_SHELL_LAUNCHER unset), working launcher.
#    Proves the default is ON, that a good boot is visible to this test, and
#    that a success writes NO memo. Without this control, scenarios 2-4 could
#    all "pass" against a launcher that never worked at all.
# ---------------------------------------------------------------------------
rc=$(run_case "1. default ON, working launcher" "$WORK/s1.log" "$WORK/s1.out" 40)
if [[ "$rc" != "0" ]]; then
    echo "  FAIL: working launcher run exited $rc (expected 0)"; fail=1
fi
if ! grep -q "AXL_FALLBACK_APP_RAN" "$WORK/s1.out"; then
    echo "  FAIL: app output missing — the app did not run"; fail=1
fi
if grep -q "seconds to skip" "$WORK/s1.log"; then
    echo "  FAIL: countdown present — the launcher was NOT used by default"; fail=1
fi
if [[ "$(memo_count)" != "0" ]]; then
    echo "  FAIL: a memo was written for a launcher that worked"; fail=1
fi

# ---------------------------------------------------------------------------
# 2. FAULT INJECTION — default ON, launcher hangs. The run must still succeed,
#    by noticing the Shell was never reached and retrying without the launcher.
# ---------------------------------------------------------------------------
rc=$(run_case "2. default ON, hanging launcher" "$WORK/s2.log" "$WORK/s2.out" 40 \
        "AXL_SHELL_LAUNCHER_BIN=$HANG_LAUNCHER")
if [[ "$rc" != "0" ]]; then
    echo "  FAIL: hanging launcher run exited $rc (expected 0 — retry should heal it)"; fail=1
fi
if ! grep -q "AXL_FALLBACK_APP_RAN" "$WORK/s2.out"; then
    echo "  FAIL: app output missing — the retry did not reach the app"; fail=1
fi
if ! grep -q "did not reach the Shell" "$WORK/s2.out"; then
    echo "  FAIL: no fallback diagnostic printed"; fail=1
fi
# The retry's transcript replaces the first attempt's, so the countdown being
# BACK is the proof that the retry booted the Shell directly.
if ! grep -q "seconds to skip" "$WORK/s2.log"; then
    echo "  FAIL: retry did not boot the Shell directly"; fail=1
fi
if [[ "$(memo_count)" != "1" ]]; then
    echo "  FAIL: expected exactly 1 memo after the fault, found $(memo_count)"; fail=1
fi
# The retried run is the one that spawns a child, so a leak would show here.
if [[ "$(qemu_tmp_count)" != "0" ]]; then
    echo "  FAIL: retry leaked $(qemu_tmp_count) run-qemu temp dir(s)"; fail=1
fi

# ---------------------------------------------------------------------------
# 3. MEMO HONORED — same broken launcher, but the memo now exists. The launcher
#    must not be staged at all: no hang, no retry, no second memo.
# ---------------------------------------------------------------------------
rc=$(run_case "3. memo honored" "$WORK/s3.log" "$WORK/s3.out" 40 \
        "AXL_SHELL_LAUNCHER_BIN=$HANG_LAUNCHER")
if [[ "$rc" != "0" ]]; then
    echo "  FAIL: memo-honored run exited $rc (expected 0)"; fail=1
fi
if ! grep -q "AXL_FALLBACK_APP_RAN" "$WORK/s3.out"; then
    echo "  FAIL: app output missing"; fail=1
fi
# kbprobe's banner is the proof the broken launcher was staged. It must be
# absent: the memo means we never tried it.
if grep -q "kbprobe: press keys" "$WORK/s3.log"; then
    echo "  FAIL: broken launcher was staged despite the memo"; fail=1
fi
if grep -q "did not reach the Shell" "$WORK/s3.out"; then
    echo "  FAIL: retried despite the memo (the memo was not consulted)"; fail=1
fi
if [[ "$(memo_count)" != "1" ]]; then
    echo "  FAIL: memo count changed on a honored memo, found $(memo_count)"; fail=1
fi

# ---------------------------------------------------------------------------
# 4. FORCED ON — AXL_SHELL_LAUNCHER=1 means "definitely use it": the memo is
#    ignored and the failure stays loud. This is what run-integration.sh and
#    test-shell-launcher-qemu.sh rely on, so a silent fallback there would hide
#    a real launcher regression behind a slow-but-green suite.
# ---------------------------------------------------------------------------
rc=$(run_case "4. AXL_SHELL_LAUNCHER=1 overrides the memo" "$WORK/s4.log" "$WORK/s4.out" 20 \
        "AXL_SHELL_LAUNCHER=1" "AXL_SHELL_LAUNCHER_BIN=$HANG_LAUNCHER")
if [[ "$rc" == "0" ]]; then
    echo "  FAIL: forced-on run exited 0 — the failure was silently healed"; fail=1
fi
if ! grep -q "kbprobe: press keys" "$WORK/s4.log"; then
    echo "  FAIL: memo was honored despite AXL_SHELL_LAUNCHER=1"; fail=1
fi
if grep -q "did not reach the Shell" "$WORK/s4.out"; then
    echo "  FAIL: forced-on run retried (it must not)"; fail=1
fi
if [[ "$(memo_count)" != "1" ]]; then
    echo "  FAIL: forced-on run wrote a memo, found $(memo_count)"; fail=1
fi

if (( fail )); then
    echo "FAIL: shell launcher fallback ($TEST_ARCH)"
    exit 1
fi
echo "shell launcher fallback: retry heals a hang, memo honored, forced-on stays loud ($TEST_ARCH)"
exit 0
