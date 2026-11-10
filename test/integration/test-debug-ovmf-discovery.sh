#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-debug-ovmf-discovery.sh -- where a DEBUG OVMF is found, and what a
# test does without one.
#
# Tests that need a DEBUG OVMF (pool cleared on free, ASSERTs compiled in)
# find it through ONE point, find_debug_ovmf (scripts/axl-common.sh):
# $AXL_OVMF_DEBUG_DIR when it is set -- and then only there -- else the EDK2
# build tree under $EDK2_DIR, else $HOME/uefi. Without one a test SKIPs, which
# passes a suite; with AXL_REQUIRE_DEBUG_OVMF=1 (release-suite.sh sets it) it
# FAILS, so a release is never certified by a run that skipped it.
#
# No firmware boots here: the DEBUG OVMF files are empty stand-ins, and
# test-stderr-stale-qemu.sh is only run where it stops for want of one.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { if [[ "$2" == "$3" ]]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/dbg" "$TMP/empty" "$TMP/home/uefi/Build/OvmfX64/DEBUG_GCC5/FV" \
         "$TMP/edk2/Build/OvmfX64/DEBUG_GCC5/FV"
for d in "$TMP/dbg" "$TMP/home/uefi/Build/OvmfX64/DEBUG_GCC5/FV" \
         "$TMP/edk2/Build/OvmfX64/DEBUG_GCC5/FV"; do
    : > "$d/OVMF_CODE.fd"; : > "$d/OVMF_VARS.fd"
done

# find_debug_ovmf in a clean shell, with only the variables given (HOME
# included: probe_nohome leaves it unset); prints its status and the
# CODE/VARS it chose, or why each place it looked had none.
probe() { probe_nohome HOME="$TMP/home" "$@"; }
probe_nohome() {
    env -i PATH="$PATH" "$@" bash -c '
        source "$1/scripts/axl-common.sh" >/dev/null 2>&1
        if find_debug_ovmf; then echo "ok $DEBUG_OVMF_CODE $DEBUG_OVMF_VARS"
        else echo "none $DEBUG_OVMF_SEARCHED"; fi' _ "$PROJECT_DIR"
}

echo "=== find_debug_ovmf ==="
check "AXL_OVMF_DEBUG_DIR names the firmware" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/dbg")" \
    "ok $TMP/dbg/OVMF_CODE.fd $TMP/dbg/OVMF_VARS.fd"
check "...and only there: an empty one finds none, though \$HOME has one" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/empty")" "none $TMP/empty: no CODE file"
check "unset, the EDK2 build tree under \$EDK2_DIR comes first" \
    "$(probe EDK2_DIR="$TMP/edk2")" \
    "ok $TMP/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd $TMP/edk2/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd"
check "unset with no \$EDK2_DIR, \$HOME/uefi" \
    "$(probe)" \
    "ok $TMP/home/uefi/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd $TMP/home/uefi/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd"
check "a VARS without its CODE is none" \
    "$(rm "$TMP/dbg/OVMF_CODE.fd"; probe AXL_OVMF_DEBUG_DIR="$TMP/dbg")" \
    "none $TMP/dbg: no CODE file"

echo ""
echo "=== why none: \"not there\" is not \"could not look\" ==="
check "a directory that does not exist is 'missing'" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/nowhere")" "none $TMP/nowhere: missing"
mkdir -p "$TMP/codeonly"; : > "$TMP/codeonly/OVMF_CODE.fd"
check "a CODE without its VARS is 'no VARS file'" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/codeonly")" "none $TMP/codeonly: no VARS file"
mkdir -p "$TMP/locked"; : > "$TMP/locked/OVMF_CODE.fd"; : > "$TMP/locked/OVMF_VARS.fd"
chmod 000 "$TMP/locked"
check "a directory it may not read is 'unreadable', not missing" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/locked")" "none $TMP/locked: unreadable"
chmod 755 "$TMP/locked"
check "control: the same directory, readable, is found" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/locked")" \
    "ok $TMP/locked/OVMF_CODE.fd $TMP/locked/OVMF_VARS.fd"
chmod 000 "$TMP/locked/OVMF_CODE.fd"
check "a firmware file it may not read is 'unreadable'" \
    "$(probe AXL_OVMF_DEBUG_DIR="$TMP/locked")" "none $TMP/locked: unreadable"
chmod 644 "$TMP/locked/OVMF_CODE.fd"
check "every place looked is named, with its own reason" \
    "$(probe EDK2_DIR="$TMP/noedk2" HOME="$TMP/nohome")" \
    "none $TMP/noedk2/Build/OvmfX64/DEBUG_GCC5/FV: missing; $TMP/nohome/uefi/Build/OvmfX64/DEBUG_GCC5/FV: missing"
check "HOME unset: /uefi is not searched, and that is said" \
    "$(probe_nohome)" \
    "none \$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV: not searched, HOME is unset"

echo ""
echo "=== a test without one: SKIP, or FAIL when required ==="
STALE="$PROJECT_DIR/test/integration/test-stderr-stale-qemu.sh"
out=$(AXL_OVMF_DEBUG_DIR="$TMP/empty" AXL_REQUIRE_DEBUG_OVMF= "$STALE" 2>&1); rc=$?
check "test-stderr-stale-qemu.sh SKIPs (77) without a DEBUG OVMF" "$rc" "77"
check "...saying where it looked, and why it had none" "$(tail -1 <<< "$out")" \
    "SKIP: no DEBUG OVMF -- $TMP/empty: no CODE file (set AXL_OVMF_DEBUG_DIR)"
out=$(AXL_OVMF_DEBUG_DIR="$TMP/empty" AXL_REQUIRE_DEBUG_OVMF=1 "$STALE" 2>&1); rc=$?
check "...and FAILS (1) when a DEBUG OVMF is required" "$rc" "1"
check "...saying so" "$(tail -1 <<< "$out")" \
    "FAIL: no DEBUG OVMF -- $TMP/empty: no CODE file (set AXL_OVMF_DEBUG_DIR) -- this run requires one"

echo ""
echo "debug-ovmf-discovery: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
