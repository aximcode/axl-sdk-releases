#!/bin/bash
# test-meta: arch=both needs= est=58 local-only=0 builds=sshd guest=none
# guest=none: two leased NICs racing for DHCP, and typed input: a machine no
#   class gives.
# test-sshd-start-qemu.sh -- one part of the sshd start suite:
# where it serves with two leased NICs, and which NIC wins a DHCP race.
#
# The setup and the helpers are lib/sshd-start-lib.sh, whose header says
# how the suite is split and why.
#
# Boots and scenarios, in run order:
#   two        two leased NICs (10.0.2.15, 10.0.3.15).
#              plain     `sshd -T` says AxlNetworkTimeout is 60 s by
#                        default; `sshd` says it is starting (with that
#                        deadline) before anything else, then serves ON BOTH
#                        addresses -- the launcher's line, the driver's log
#                        line (`--log-port 0`: the console's own port, asked
#                        for, since by default nothing is written to a port
#                        the console draws on) and `sshd status` -- and logs
#                        no DEBUG line
#              verbose   `sshd -v`: the driver names each step at DEBUG
#                        before it runs, exactly and in order, including
#                        the NIC it uses
#   race       two NICs that both lease, neither configured before sshd.
#              first     `sshd -v` runs DHCP on BOTH at once (both DHCP lines
#                        before the result) and serves on the first to lease,
#                        only; the other NIC's DHCP is stopped -- `ifconfig
#                        -l` shows its policy static again and no address --
#                        and the winner's stays dhcp
#
# Usage: ./test/integration/test-sshd-start-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/lib/sshd-start-lib.sh"

echo "=== sshd-start ($TEST_ARCH) ==="

# --- two ------------------------------------------------------------------------
SSHD_NET=(-device "virtio-net-pci,netdev=net0,mac=52:54:00:12:00:01"
          -netdev "user,id=net0@HOSTFWD@"
          -device "virtio-net-pci,netdev=n1,mac=52:54:00:12:00:02"
          -netdev "user,id=n1,net=10.0.3.0/24,dhcpstart=10.0.3.15")
sshd_boot "$(printf 'ifconfig -s eth1 dhcp\nstall 3000000')"
ADDRS="10.0.2.15, 10.0.3.15"

send 'sshd -T\r'
check "two/-T: AxlNetworkTimeout is 60 s by default" \
    wait_guest "axlnetworktimeout 60" 1 20

send "sshd $SSHD_LOG_HERE -P $PW\\r"
check "two/plain: the launcher serves on both addresses" \
    wait_guest "sshd: serving the UEFI Shell on $ADDRS port 22" 1 60
check "two/plain: it said it was starting, with the deadline, once" \
    test "$(guest_lines "$STARTING")" = 1
check "two/plain: the driver's log line names both addresses" \
    test "$(guest_re "${LOG_PFX}sshd: serving the UEFI Shell on 10\\.0\\.2\\.15, 10\\.0\\.3\\.15 port 22 \\(console [0-9]+x[0-9]+\\)\$")" = 1
check "two/plain: without -v the driver logs no DEBUG line" \
    test "$(guest_re "$DBG_MSG_RE")" = 0
send 'sshd status\r'
check "two/plain: \`sshd status\` names both addresses" \
    wait_guest "sshd: running on $ADDRS" 1 20
send 'sshd stop\r'
wait_guest "sshd: stopped" 1 30 || true

RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
check "two/verbose: \`sshd -v\` serves" \
    wait_guest "sshd: serving the UEFI Shell on $ADDRS port 22" 2 60
check_phases "two/verbose: the driver names each step before it runs, in order" \
    "p4f-v-$RAND" "sshd: serving the UEFI Shell on $ADDRS" \
    "$(printf '%s\n' \
        'bringing up the network (NIC auto, up to 60 s)' \
        'NIC 52:54:00:12:00:01 has 10.0.2.15 -- using it' \
        'taking over the console' \
        'building the server (host key, authorized keys)' \
        'listening on port 22' \
        'the log is not written while serving: every serial port carries the console')"
send 'sshd stop\r'
wait_guest "sshd: stopped" 2 30 || true
end_boot two

# --- race -----------------------------------------------------------------------
# Two NICs on two slirp networks, both leasing, nothing configured first. New
# MACs: the "two" boot left DHCP policies for its NICs in NVRAM.
SSHD_NO_DHCP=1
SSHD_NET=(-device "virtio-net-pci,netdev=net0,mac=52:54:00:12:00:11,romfile="
          -netdev "user,id=net0@HOSTFWD@"
          -device "virtio-net-pci,netdev=n1,mac=52:54:00:12:00:12,romfile="
          -netdev "user,id=n1,net=10.0.3.0/24,dhcpstart=10.0.3.15")
sshd_boot ""

# ifconfig_policy MAC -- "<policy> <ipv4>" of the NIC with that MAC, as
# `ifconfig -l` prints it.
ifconfig_policy() {
    local tag
    tag=$(rand_tag)
    send "echo %T%-if-$tag\\r"
    send 'ifconfig -l\r'
    send "echo %T%-if-end-$tag\\r"
    wait_guest "p4f-if-end-$tag" 1 20 || true
    test_clean_log
    test_slice_log "p4f-if-$tag" "p4f-if-end-$tag" "$TEST_TMPDIR/if.txt" || return 0
    awk -v mac="$1" '
        /^name +:/          { pol = ""; hit = 0 }
        /^policy +:/        { pol = $3 }
        /^mac addr +:/      { hit = ($4 == mac) }
        hit && /^ipv4 address +:/ { print pol, $4; exit }' "$TEST_TMPDIR/if.txt"
}

RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
check "race/first: serves on one NIC" \
    wait_guest_re '^sshd: serving the UEFI Shell on 10\.0\.[23]\.15 port 22$' 1 60
test_clean_log
WIN_IP=$({ grep -aoE '^sshd: serving the UEFI Shell on 10\.0\.[23]\.15 port 22$' "$TEST_CLEAN_LOG" \
           || true; } | head -1 | sed -E 's/.* on (10\.0\.[23]\.15) port.*/\1/')
if [[ "$WIN_IP" == 10.0.3.15 ]]; then
    WIN_MAC=52:54:00:12:00:12; LOSE_MAC=52:54:00:12:00:11
else
    WIN_MAC=52:54:00:12:00:11; LOSE_MAC=52:54:00:12:00:12
fi
check_phases "race/first: -v runs DHCP on both NICs at once, then names the first to lease" \
    "p4f-v-$RAND" "sshd: serving the UEFI Shell on" \
    "$(printf '%s\n' \
        'bringing up the network (NIC auto, up to 60 s)' \
        "no drivers directory found -- sweeping \\drivers\\$NAT on every volume" \
        "firmware pass: connecting the firmware's NIC drivers" \
        'DHCP on NIC 52:54:00:12:00:11 (up to N s)' \
        'DHCP on NIC 52:54:00:12:00:12 (up to N s)' \
        "firmware pass: NIC $WIN_MAC up with $WIN_IP" \
        'taking over the console' \
        'building the server (host key, authorized keys)' \
        'listening on port 22' \
        'the log is not written while serving: every serial port carries the console')"
got=$(ifconfig_policy "$LOSE_MAC")
check "race/first: the losing NIC's DHCP is stopped: policy static, no address ($got)" \
    test "$got" = "static 0.0.0.0"
got=$(ifconfig_policy "$WIN_MAC")
check "race/first: the winner keeps DHCP and its lease ($got)" \
    test "$got" = "dhcp $WIN_IP"
send 'sshd stop\r'
wait_guest "sshd: stopped" 1 30 || true
end_boot race

sshd_start_finish "sshd-start" 16
