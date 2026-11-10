#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 builds=release-drivers guest=none
# guest=none: its machine has exactly its own two NICs (a USB one and an
#   option-ROM-less virtio one) and asserts the whole set of NIC driver
#   lines; a pooled guest always has the agent's NIC too, a third line.
# test-nic-driver-name-qemu.sh -- axl_net_get_driver_info names the NIC's OWN
# driver, for a USB NIC as well as a PCI one.
#
# One boot with two NICs:
#   usb  QEMU's usb-net on a qemu-xhci controller, bound by the release's
#        UsbRndis + NetworkCommon (loaded by startup.nsh). Its device path is
#        PciRoot/Pci(xHCI)/USB(..)/MAC(..), so a lookup that asks for the PCI
#        bus controller FIRST stops at the xHCI controller and names the host
#        controller's driver ("Usb Xhci Driver") instead of the NIC's. The
#        deepest bus controller on the path -- the USB one -- owns the NIC.
#   pci  virtio-net-pci (no option ROM), bound by the firmware's VirtioNetDxe.
#        The control: a PCI NIC still names its PCI driver.
# `netinfo -v --no-load list` prints one "NIC[i] [layer] driver=NAME" line
# per NIC; the lines are matched exactly. Their order is the firmware's
# handle order, so the set is compared sorted.
#
# x64 only for the USB half: under the aarch64 firmware QEMU's usb-net binds
# no driver (see test-net-gate-qemu.sh), so the USB check SKIPs there (one
# SKIP for the one check); the PCI control and the set check run on both.
#
# Usage: ./test/integration/test-nic-driver-name-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup
declare -A _NAT=([X64]=x64 [AARCH64]=aa64)
NAT="${_NAT[$TEST_ARCH]:-x64}"

TEST_BUILD_DIR="$(test_build_dir)"
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        all tools release-drivers > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "FAIL: building netinfo.efi and the release drivers"
    exit 1
fi
test_add_efi "$TEST_BUILD_DIR/tools/netinfo.efi"
RDRV="$TEST_BUILD_DIR/release-drivers/$NAT"
cp "$RDRV/NetworkCommon.efi" "$RDRV/UsbRndis.efi" "$TEST_STAGING/"

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \
load NetworkCommon.efi
load UsbRndis.efi
connect -r
stall 2000000
echo === TEST-DRIVERS ===
netinfo.efi -v --no-load list
echo === TEST-END ===
reset -s
NSHEOF
test_build_image

echo "=== NIC driver names ($TEST_ARCH) ==="
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device "qemu-xhci,id=xhci"
    -device "usb-net,bus=xhci.0,netdev=u0,mac=52:54:00:12:00:81"
    -netdev "user,id=u0"
    -device "virtio-net-pci,netdev=p0,mac=52:54:00:12:00:82,romfile="
    -netdev "user,id=p0"
)
test_run_foreground 60
test_clean_log

PASS=0
FAIL=0
SKIPPED=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# The driver lines of the "NIC Drivers" section, the NIC index stripped (it
# follows the firmware's handle order), sorted.
GOT=$(sed -n '/^=== TEST-DRIVERS ===/,/^=== TEST-END ===/p' "$TEST_CLEAN_LOG" \
        | awk '/^=== NIC Drivers/ { on = 1; next }
               on && /^=== / { exit }
               on && /^  NIC\[[0-9]+\] / { sub(/^  NIC\[[0-9]+\] /, ""); print }' \
        | LC_ALL=C sort)
echo "  driver lines:"
printf '%s\n' "$GOT" | sed 's/^/    /'

PCI_LINE='[SNP] driver=Virtio PCI Driver'
# UsbRndis is loaded from disk, so it is named by the path it was loaded from
# (AxlNetDriverInfo.driver's first state); the firmware's drivers by name.
USB_LINE='[NII3.1] driver=\UsbRndis.efi'

if grep -qxF -- "$PCI_LINE" <<< "$GOT"; then
    pass "pci: the virtio-net NIC names its PCI driver ($PCI_LINE)"
else
    fail "pci: the virtio-net NIC names its PCI driver ($PCI_LINE)"
fi

if [[ "$NAT" == aa64 ]]; then
    # One check skipped, one SKIP. The set check still runs: with no USB NIC
    # bound, the PCI NIC's line is the whole set.
    echo "  SKIP: usb: QEMU's usb-net binds no driver under the aarch64 firmware"
    SKIPPED=1
    WANT_SET="$PCI_LINE"
else
    if grep -qxF -- "$USB_LINE" <<< "$GOT"; then
        pass "usb: the USB NIC names its own driver, not the host controller's ($USB_LINE)"
    else
        fail "usb: the USB NIC names its own driver, not the host controller's ($USB_LINE)"
    fi
    WANT_SET=$(printf '%s\n' "$PCI_LINE" "$USB_LINE" | LC_ALL=C sort)
fi

# The whole set, exactly: no third NIC, no line naming a host controller.
if [[ "$GOT" == "$WANT_SET" ]]; then
    pass "set: exactly one driver line per NIC, as expected"
else
    fail "set: exactly one driver line per NIC, as expected"
    diff <(printf '%s\n' "$WANT_SET") <(printf '%s\n' "$GOT") | sed 's/^/    /' || true
fi

if test_check_leaks; then
    pass "leaks: netinfo's leak verdict is clean"
else
    fail "leaks: a leak or a missing verdict"
fi

echo ""
WANT=$((4 - SKIPPED))
printf "nic-driver-name: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
if [[ $FAIL -ne 0 || $PASS -ne $WANT ]]; then
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
