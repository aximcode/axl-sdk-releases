#!/bin/bash
# test-meta: arch=both needs= est=68 local-only=0 builds=sshd guest=none
# guest=none: a second NIC with no DHCP server behind it, and typed input: a
#   machine no class gives.
# test-sshd-start-nolease-qemu.sh -- one part of the sshd start suite:
# how its bring-up ends when no DHCP server answers -- at the timeout, at a
# shorter --net-timeout, at a Ctrl-C.
#
# The setup and the helpers are lib/sshd-start-lib.sh, whose header says
# how the suite is split and why.
#
# Boots and scenarios, in run order:
#   nolease    two NICs whose links are up and that no DHCP server answers,
#              and an sshd.conf saying AxlNetworkTimeout 25.
#              timeout   `sshd -T` shows the file's 25 s; `sshd -v` names
#                        where it would sweep drivers, the firmware pass and
#                        each NIC it runs DHCP on, in order, all at once with
#                        the one 15 s DHCP budget; after that budget -- well
#                        inside the 25 s deadline -- it fails, saying no
#                        NIC leased, deploys nothing, stops the DHCP it
#                        started (`ifconfig -l` shows the policy static
#                        again), and the Shell runs the next command
#              cut       `sshd -v --net-timeout 10`: the flag wins over the
#                        file; both DHCPs are cut to what the deadline left
#                        and it gives up at 10 s with its message
#              ctrl-c    a Ctrl-C on the serial console while `sshd` waits
#                        for DHCP stops it at once, with its message, and
#                        its DHCP too; the control shows `ifconfig -l` does
#                        see a DHCP policy
#
# Usage: ./test/integration/test-sshd-start-nolease-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/lib/sshd-start-lib.sh"

echo "=== sshd-start nolease ($TEST_ARCH) ==="

# --- nolease --------------------------------------------------------------------
# Two NICs, each on a hub with no other port: every link is up and nothing
# ever answers DHCP. The first DHCP gives up after its own 15 s, and the
# deadline (sshd.conf's 25 s) cuts the second's to what is left.
SSHD_NO_DHCP=1
SSHD_NET=()
printf 'AxlNetworkTimeout 25\r\n' > "$TEST_STAGING/sshd.conf"
for k in 3 4; do
    SSHD_NET+=(-device "virtio-net-pci,netdev=net$k,mac=52:54:00:12:00:0$k,romfile="
               -netdev "hubport,id=net$k,hubid=$((4 + k))")
done
sshd_boot ""

# policy_is LABEL WANT -- eth0's IP4Config2 policy, as `ifconfig -l` says.
policy_is() {
    local tag
    tag=$(rand_tag)
    send "echo %T%-pol-$tag\\r"
    send 'ifconfig -l eth0\r'
    send "echo %T%-pol-end-$tag\\r"
    wait_guest "p4f-pol-end-$tag" 1 20 || true
    test_clean_log
    test_slice_log "p4f-pol-$tag" "p4f-pol-end-$tag" "$TEST_TMPDIR/pol.txt"
    check "$1" test "$(grep -acE "^policy +: $2\$" "$TEST_TMPDIR/pol.txt" || true)" = 1
}

send 'sshd -T\r'
check "nolease/-T: AxlNetworkTimeout is sshd.conf's 25 s" \
    wait_guest "axlnetworktimeout 25" 1 20
RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
wait_guest "$STARTING25" 1 30 || true
t0=$(now)
check "nolease/timeout: gives up, saying no NIC with a link leased" \
    wait_guest "sshd: no network: no DHCP lease on any NIC with a link (2 tried)" 1 60
t1=$(now)
el=$(secs "$t0" "$t1")
check "nolease/timeout: after the one DHCP budget, not one per NIC: 14-24 s ($el s)" \
    test "$el" -ge 14 -a "$el" -le 24
check "nolease/timeout: the launcher then says the server did not start" \
    wait_guest "$NOSTART" 1 10
check_phases "nolease/timeout: -v named the drivers it would sweep, the firmware pass and each NIC it ran DHCP on, in order" \
    "p4f-v-$RAND" "sshd: no network:" \
    "$(printf '%s\n' \
        'bringing up the network (NIC auto, up to 25 s)' \
        "no drivers directory found -- sweeping \\drivers\\$NAT on every volume" \
        "firmware pass: connecting the firmware's NIC drivers" \
        'DHCP on NIC 52:54:00:12:00:03 (up to N s)' \
        'DHCP on NIC 52:54:00:12:00:04 (up to N s)' \
        'firmware pass: NIC 52:54:00:12:00:04 got no address')"
check "nolease/timeout: both DHCPs share the one 15 s budget" \
    test "$(guest_re "${DBG_MSG_RE}DHCP on NIC 52:54:00:12:00:0[34] \\(up to 15 s\\)\$")" = 2
send "echo %T%-after-to-$RAND\\r"
check "nolease/timeout: the Shell runs the next command" \
    wait_guest "p4f-after-to-$RAND" 1 20
send 'sshd status\r'
check "nolease/timeout: nothing was deployed" wait_guest "sshd: stopped" 1 20
policy_is "nolease/timeout: the DHCP it started is stopped (policy static again)" static

# --net-timeout 10, under the file's 25: the flag wins, and the deadline cuts
# the DHCP budget short of its 15 s.
RAND=$(rand_tag)
send "echo %T%-c-$RAND\\r"
wait_guest "p4f-c-$RAND" 1 20 || true
send "sshd -v --net-timeout 10 -P $PW\\r"
wait_guest "sshd: starting -- bringing up the network (up to 10 s)" 1 30 || true
t0=$(now)
check "nolease/cut: --net-timeout 10 gives up at its deadline, with its message" \
    wait_guest "sshd: network bring-up timed out after 10 s" 1 40
t1=$(now)
el=$(secs "$t0" "$t1")
check "nolease/cut: after 9-15 s ($el s)" test "$el" -ge 9 -a "$el" -le 15
check "nolease/cut: both DHCPs were cut to what the deadline left" \
    test "$(phases_between "p4f-c-$RAND" "sshd: network bring-up timed out" \
            | grep -cE '^DHCP on NIC 52:54:00:12:00:0[34] \(up to (9|10) s\)$')" = 2
policy_is "nolease/cut: the DHCP it started is stopped (policy static again)" static

send "sshd -P $PW\\r"
wait_guest "$STARTING25" 2 30 || true
sleep 3
t0=$(now)
send '\003'
check "nolease/ctrl-c: a serial Ctrl-C stops the bring-up, with its message" \
    wait_guest "sshd: network bring-up stopped (Ctrl-C)" 1 15
t1=$(now)
el=$(secs "$t0" "$t1")
check "nolease/ctrl-c: at once ($el s)" test "$el" -le 3
check "nolease/ctrl-c: the launcher then says the server did not start" \
    wait_guest "$NOSTART" 2 10
send "echo %T%-after-cc-$RAND\\r"
check "nolease/ctrl-c: the Shell runs the next command" \
    wait_guest "p4f-after-cc-$RAND" 1 20
send 'sshd status\r'
check "nolease/ctrl-c: nothing was deployed" wait_guest "sshd: stopped" 2 20
policy_is "nolease/ctrl-c: the DHCP it started is stopped (policy static again)" static
# The control: the probe sees a DHCP policy when there is one.
send 'ifconfig -s eth0 dhcp\r'
policy_is "nolease/control: \`ifconfig -l\` shows a DHCP policy as dhcp" dhcp
end_boot nolease
rm -f "$TEST_STAGING/sshd.conf"

sshd_start_finish "sshd-start nolease" 22
