#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0 builds=hello-minimal guest=none
# guest=none: the boot is the subject: what the firmware's NIC option ROM
#   does, or does not, load at boot.
# test-net-no-ipxe-autoload-qemu.sh -- axl_net_ensure_drivers never loads an
# iPXE driver on its own.
#
# It used to: when its core list (NetworkCommon, the USB-network and vendor
# UNDI drivers) brought up no SNP, it fell back to loading ipxe-all.efidrv,
# ipxe-intel.efi and ipxe-broadcom.efi from ANY mounted volume and connecting
# them. iPXE takes the USB host controller, so on a BMC console the virtual
# keyboard and the virtual-media stick -- both USB -- vanished under a tool
# that only wanted a NIC. An iPXE driver now loads only when someone names it
# (netload -p), never by this fallback.
#
# The boot has no NIC and no core driver, and the three iPXE names staged in
# drivers\<arch>\ are a marker image (hello-minimal-c.efi: it prints
# "hello, (no arg)" when started and returns success, so a load would be both
# SEEN and counted). `netinfo list` runs axl_net_ensure_drivers:
#
#   no-load    the marker line never appears after netinfo starts
#   not-found  netinfo reports NOT_FOUND ("no NIC drivers found"), not
#              NO_LINK ("drivers loaded but no NIC came up") -- with an iPXE
#              name counted as loaded it would say the latter
#   control    the same marker image run BY NAME prints its line, so the
#              no-load check is reading a log the marker does reach
#
# Usage: ./test/integration/test-net-no-ipxe-autoload-qemu.sh [--arch X64|AARCH64]
set -euo pipefail
ARCH="X64"
while [[ $# -gt 0 ]]; do case "$1" in --arch) ARCH="$2"; shift 2;; *) echo "bad arg $1"; exit 2;; esac; done
case "$ARCH" in X64) NAT=x64;; AARCH64) NAT=aa64;; *) echo "bad arch"; exit 2;; esac
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NAT")"
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" tools hello-minimal > "${TMPDIR:-/tmp}/net-no-ipxe-make.$$" 2>&1; then
    tail -20 "${TMPDIR:-/tmp}/net-no-ipxe-make.$$"; rm -f "${TMPDIR:-/tmp}/net-no-ipxe-make.$$"
    echo "ERROR: building the fixtures failed"; exit 1
fi
rm -f "${TMPDIR:-/tmp}/net-no-ipxe-make.$$"
MARKER_EFI="$PREFIX/hello-minimal-c.efi"
for f in "$PREFIX/tools/netinfo.efi" "$MARKER_EFI"; do
    [[ -f "$f" ]] || { echo "ERROR: $f not built"; exit 1; }
done
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"; LOG="$TMP/serial.log"

{
    echo '@echo -off'; echo 'fs0:'
    echo 'echo MARK_NETINFO'; echo 'netinfo.efi list'
    echo 'echo MARK_CONTROL'; echo "fs0:\\drivers\\$NAT\\ipxe-intel.efi"
    echo 'echo MARK_DONE';    echo 'reset -s'
} > "$NSH"
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout 60 --nsh "$NSH" \
    --extra "$MARKER_EFI:drivers/$NAT/ipxe-all.efidrv" \
    --extra "$MARKER_EFI:drivers/$NAT/ipxe-intel.efi" \
    --extra "$MARKER_EFI:drivers/$NAT/ipxe-broadcom.efi" \
    "$PREFIX/tools/netinfo.efi" > "$LOG" 2>&1 || true

sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$LOG" | tr -d '\r' > "$LOG.clean"
sect() { awk -v a="$1" -v b="$2" '$0 == a { on = 1; next } $0 == b { on = 0 } on' "$LOG.clean"; }
PASS=0 FAIL=0
check() {   # label want got
    if [[ "$3" == "$2" ]]; then echo "PASS: $1 ($3)"; PASS=$((PASS + 1))
    else echo "FAIL: $1 ($3, want $2)"; FAIL=$((FAIL + 1)); fi
}
MARKER="hello, (no arg)"
check "the boot reached the end (markers present)" 3 \
    "$(grep -cxE 'MARK_(NETINFO|CONTROL|DONE)' "$LOG.clean" || true)"
check "control: the marker image run by name prints its line" 1 \
    "$(sect MARK_CONTROL MARK_DONE | grep -cxF -- "$MARKER" || true)"
check "no-load: netinfo's driver search started no iPXE-named image" 0 \
    "$(sect MARK_NETINFO MARK_CONTROL | grep -cxF -- "$MARKER" || true)"
check "not-found: ensure_drivers found no NIC driver to load" 1 \
    "$(sect MARK_NETINFO MARK_CONTROL | grep -cxF -- \
        "NetInfo: warning: no NIC drivers found in drivers/<arch>/ on any mounted volume." || true)"
check "not-found: it did not count a driver as loaded (no NO_LINK line)" 0 \
    "$(sect MARK_NETINFO MARK_CONTROL | grep -cF -- "drivers loaded but no NIC" || true)"

echo "net-no-ipxe-autoload: $PASS passed, $FAIL failed ($ARCH)"
# Every check ran: a count short of 5 is a check that never reported.
if (( FAIL > 0 || PASS != 5 )); then echo "--- log ---"; tail -40 "$LOG.clean"; exit 1; fi
exit 0
