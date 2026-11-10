#!/bin/bash
# test-meta: arch=both needs= est=119 local-only=0 builds=sshd guest=none
# guest=none: NICs whose links are down, or no NIC driver at all: a machine
#   no class gives.
# test-sshd-start-nonet-qemu.sh -- one part of the sshd start suite:
# how it starts with NICs whose links are down, and with no NIC driver.
#
# The setup and the helpers are lib/sshd-start-lib.sh, whose header says
# how the suite is split and why.
#
# Boots and scenarios, in run order:
#   linkdown   NIC 0 link-down, NIC 1 leasing.
#              skip      `sshd -v` names NIC 0 as link-down and skips it,
#                        runs DHCP on NIC 1 and serves on its address,
#                        without waiting on NIC 0
#   alldown    two link-down NICs: `sshd` fails at once, saying so; with
#              `-n 1` it names that NIC
#   nonic      a NIC no firmware driver binds (a ROM-less e1000) and no NIC
#              driver on any volume: `sshd -v` names where it would sweep
#              and the firmware pass, and fails after the link wait (a NIC
#              may still appear during it) with the no-network message
#              naming what it searched
#
# Usage: ./test/integration/test-sshd-start-nonet-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/lib/sshd-start-lib.sh"

echo "=== sshd-start nonet ($TEST_ARCH) ==="

# --- linkdown, alldown ------------------------------------------------------------
if [[ "$LD_AVAILABLE" -eq 0 ]]; then
    # Balanced: as many SKIPs as the two boots' checks.
    for i in $(seq 1 11); do
        echo "SKIP: link-down boots need the QEMU x-link-down patch or python3 qemu.qmp"
    done
    SKIPPED=11
else
    SKIPPED=0
    SOCK="$TEST_TMPDIR/ld-qmp.sock"
    rm -f "$SOCK"
    SSHD_NET=(-qmp "unix:$SOCK,server,nowait"
              -device "virtio-net-pci,netdev=n0,mac=52:54:00:aa:00:00,${LD_XLD}romfile="
              -netdev "user,id=n0,net=10.10.0.0/24"
              -device "virtio-net-pci,netdev=net0,mac=52:54:00:bb:00:01,romfile="
              -netdev "user,id=net0@HOSTFWD@")
    INJ=$(ld_inject "$SOCK" n0)
    sshd_boot ""
    send "sshd -v -P $PW\\r"
    wait_guest "$STARTING" 1 30 || true
    t0=$(now)
    check "linkdown/skip: serves on the link-up NIC's address" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 40
    t1=$(now)
    el=$(secs "$t0" "$t1")
    check "linkdown/skip: without waiting on the link-down NIC ($el s)" test "$el" -le 10
    check "linkdown/skip: -v names the link-down NIC and skips it" \
        test "$(guest_re "${DBG_MSG_RE}NIC 52:54:00:aa:00:00 link down -- skipping\$")" = 1
    check "linkdown/skip: -v names the NIC it runs DHCP on" \
        test "$(guest_re "${DBG_MSG_RE}DHCP on NIC 52:54:00:bb:00:01 \\(up to 1[0-5] s\\)\$")" = 1
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 1 30 || true
    ld_inject_stop "$INJ"
    end_boot linkdown

    rm -f "$SOCK"
    SSHD_NET=(-qmp "unix:$SOCK,server,nowait"
              -device "virtio-net-pci,netdev=n0,mac=52:54:00:aa:00:00,${LD_XLD}romfile="
              -netdev "user,id=n0"
              -device "virtio-net-pci,netdev=n1,mac=52:54:00:aa:00:01,${LD_XLD}romfile="
              -netdev "user,id=n1")
    INJ=$(ld_inject "$SOCK" n0 n1)
    sshd_boot ""
    send "sshd -P $PW\\r"
    wait_guest "$STARTING" 1 30 || true
    t0=$(now)
    check "alldown: fails, saying no NIC has a link" \
        wait_guest "sshd: no network: no NIC has a link (2 link down)" 1 30
    t1=$(now)
    el=$(secs "$t0" "$t1")
    check "alldown: at once -- the link wait, not DHCP ($el s)" test "$el" -le 10
    send "sshd -n 1 -P $PW\\r"
    check "alldown: -n 1 with its link down names that NIC" \
        wait_guest "sshd: no network: NIC 1 (52:54:00:aa:00:01) has no link" 1 30
    ld_inject_stop "$INJ"
    end_boot alldown
fi

# --- nonic ------------------------------------------------------------------------
SSHD_NET=(-device "e1000,netdev=n0,romfile=" -netdev "user,id=n0")
sshd_boot ""
RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
wait_guest "$STARTING" 1 30 || true
t0=$(now)
check "nonic: fails with the no-network message, naming what it searched" \
    wait_guest "sshd: no network: no NIC driver produced an interface (searched the firmware and \\drivers\\$NAT on every volume)" 1 30
t1=$(now)
el=$(secs "$t0" "$t1")
check "nonic: after the link wait, not a DHCP ($el s)" test "$el" -le 8
check_phases "nonic: -v names the drivers it would sweep, the firmware pass and its result" \
    "p4f-v-$RAND" "sshd: no network:" \
    "$(printf '%s\n' \
        'bringing up the network (NIC auto, up to 60 s)' \
        "no drivers directory found -- sweeping \\drivers\\$NAT on every volume" \
        "firmware pass: connecting the firmware's NIC drivers" \
        'firmware pass: no NIC with a link')"
# Options that do not fit the driver's LoadOptions are refused saying so: a
# 1500-character password of '!', each URL-encoded to the 3 bytes %21 --
# 4500 bytes against the 4088 LoadOptions holds after its 8-byte magic.
# Typing it costs ~15 s, but it cannot be shorter: under ~1360 characters
# it fits (600 would encode to 1800), and the check would prove nothing.
LONG=$(printf '!%.0s' $(seq 1 1500))
send "sshd -P $LONG\\r"
check "nonic/options: options over the LoadOptions limit are refused, naming the cause" \
    wait_guest_re "\\[ERROR\\] +service: service 'sshd': its options \\(URL-encoded\\) do not fit the 4096-byte LoadOptions -- shorten a long path or string option\$" 1 30
check "nonic/options: the launcher then says the server did not start" \
    wait_guest "$NOSTART" 2 10
end_boot nonic

sshd_start_finish "sshd-start nonet" $((18 - SKIPPED))
