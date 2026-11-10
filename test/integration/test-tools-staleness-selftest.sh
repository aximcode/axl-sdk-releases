#!/bin/bash
# test-tools-staleness-selftest.sh -- host-only checks (no QEMU) for the guard
# that tells tool-enumerating tests a tools/*.efi is not from the current build,
# and for the places that used to put a foreign one there.
#
# THE INCIDENT. A hand-built sshd-rtk.efi (the hardware test release's RTK
# image, which embeds a driver this tree does not carry) sat in the shared
# tools/ from the uefi-tools tarball script's --with-rtk (now make-release-assets.sh). test-tool-version and
# test-tool-redirect-pipe test every .efi there, so they tested it -- stale,
# and unnoticed -- and failed on a missing name mapping nobody could connect to
# a leftover. The release path now builds into its own directory, and
# tools_dir_stale names any tool older than libaxl.a.
#
# CONTROLS: a stale tool is named; a fresh one is not (the detector can say
# "clean"); a missing library is an error, not "nothing stale".
# Excluded from discovery by name (*-selftest.sh); run by run-selftests.sh.
set -uo pipefail
cd "$(dirname "$0")" || exit 1
PROJECT_DIR="$(cd ../.. && pwd)"
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=$((fail + 1)); }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT TERM

echo "=== tools staleness self-test ==="

mkdir -p "$work/p/tools" "$work/p/lib"
touch -d '2026-01-01 00:00:00' "$work/p/tools/old.efi"
touch -d '2026-06-01 00:00:00' "$work/p/lib/libaxl.a"
touch -d '2026-09-01 00:00:00' "$work/p/tools/new.efi"
out=$(tools_dir_stale "$work/p/tools"); rc=$?
if [[ $rc -eq 0 && "$out" == "old.efi" ]]; then
    pass "a tool older than libaxl.a is named, a newer one is not"
else
    bad "expected exactly old.efi (rc 0), got rc=$rc out='$out'"
fi

rm "$work/p/tools/old.efi"
out=$(tools_dir_stale "$work/p/tools"); rc=$?
if [[ $rc -eq 0 && -z "$out" ]]; then
    pass "CONTROL: with only current tools nothing is reported"
else
    bad "CONTROL: current tools reported stale: rc=$rc out='$out'"
fi

rm "$work/p/lib/libaxl.a"
out=$(tools_dir_stale "$work/p/tools" 2>&1); rc=$?
if [[ $rc -ne 0 && "$out" == *"no "*"libaxl.a to compare against"* ]]; then
    pass "no library to compare against is an error, not 'nothing stale'"
else
    bad "missing library: rc=$rc out='$out'"
fi

# The release path must not build into the shared tools/. Asserted on the
# text of the script that builds it and the rule that collects it: a real
# --with-rtk run builds a whole tarball.
tb="$PROJECT_DIR/scripts/make-release-assets.sh"
rules="$PROJECT_DIR/packaging/uefi-tools/CMakeLists.txt"
if grep -q 'SSHD_RTK_OUT="\$AXL_RTK_OUT"' "$tb" \
    && grep -q 'FILES \\"\\$ENV{AXL_RTK_OUT}/' "$rules" \
    && ! grep -q 'tools/\\${_axl_t}.efi.*rtk' "$rules"; then
    pass "make-release-assets.sh uefi-tools --with-rtk builds into its own dir and collects from it"
else
    bad "make-release-assets.sh uefi-tools --with-rtk still uses the shared tools/"
fi

# Both enumerating tests map the RTK names to the one service name and call the
# guard.
for t in test-tool-version-qemu.sh test-tool-redirect-pipe-qemu.sh; do
    if grep -q '\[sshd-rtk\]=sshd' "$PROJECT_DIR/test/integration/$t" \
        && grep -q 'tools_dir_stale' "$PROJECT_DIR/test/integration/$t"; then
        pass "$t maps sshd-rtk to sshd and checks for stale tools"
    else
        bad "$t lacks the sshd-rtk name mapping or the staleness check"
    fi
done

if [[ $fail -eq 0 ]]; then echo "PASS"; exit 0; fi
echo "FAIL: $fail"
exit 1
