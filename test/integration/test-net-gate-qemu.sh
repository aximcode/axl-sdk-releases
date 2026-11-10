#!/bin/bash
# test-meta: arch=both needs=qemu.qmp est=95 local-only=0 builds=net-gate-probe,release-drivers,driver,sshd guest=none
# guest=none: it types into the guest Shell (test_send_stdin) and drops the
#   link over QMP; a leased guest takes no typed serial input.
# test-net-gate-qemu.sh -- the bring-up engine's link gate
# (axl_net_auto_init_opts with a link wait), driven by net-gate-probe.efi,
# which prints every progress event and the result as exact lines.
#
# Boot "gate" (no NIC at all, a USB controller), its scenarios in run order:
#   refuse a sweep of a directory whose one candidate binds no NIC and whose
#          Unload REFUSES (refuse-unload-driver.efi, as Ref.efi), with a
#          companion (sdk/examples/driver.efi, as Comp.efi) the sidecar says
#          it requires: the sweep co-loads the companion, tries the
#          candidate, fails to unload it (it stays resident), and at its end
#          unloads the companion anyway -- in that order. The box stays
#          sound: `dh -v` walks every handle, the stranded image's included,
#          without a CPU exception, and the Shell runs the next command
#   unload the same refusing driver loaded from MEMORY (as an embedded driver
#          is): after the refused unload its loaded-image identity is
#          intact -- the LoadedImageDevicePath answers with the same bytes,
#          LoadedImage->FilePath is still set
#   embedded a sweep with two embedded drivers given iPXE-named first, and a
#          sidecar naming the other as a dependency of the directory's
#          Aaa.efi: the non-iPXE embedded one is tried first (a dependency
#          name does not hide an embedded candidate), the directory's next,
#          the embedded iPXE one last
#   late   no NIC when the bring-up starts; the host hot-plugs a USB NIC
#          (QEMU's usb-net, bound by the release's UsbRndis + NetworkCommon,
#          loaded beforehand) on seeing the probe's first line -- a NIC whose
#          interface appears while the gate waits for a link, as a USB
#          dongle still enumerating does. The firmware pass finds it during
#          its link wait and runs DHCP on it (QEMU's RNDIS NIC never leases,
#          which is what lets the gate's verdict on it show: CONFIGURING,
#          then LINK_NO_LEASE -- not NO_NIC at once). x64 only: under the
#          aarch64 firmware usb-net binds no driver (two balanced SKIPs)
# Boot "static": NIC A (no DHCP server) set static with a manual address,
#   gateway and DNS server; NIC B leasing.
#   static the DHCP race: B wins; A loses and comes back static WITH its
#          manual address, gateway and DNS server
# Boot "reject": two leasing NICs.
#   reject the race with a verifier that rejects the first NIC it is asked
#          about (and restarts the other's DHCP, so it leases seconds
#          later): the race keeps waiting and the other NIC wins; the
#          rejected NIC ends with no lease, as it was before the race
# Boot "reject1": ONE leasing NIC.
#   reject1 the verifier rejects its lease (a 60 s DHCP under a 40 s
#          deadline): with no NIC left in the race the pass ends at once,
#          NO_REACH, rather than waiting out the DHCP budget
#
# Usage: ./test/integration/test-net-gate-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

test_parse_args "$@"
test_setup
declare -A _NAT=([X64]=x64 [AARCH64]=aa64)
NAT="${_NAT[$TEST_ARCH]:-x64}"

TEST_BUILD_DIR="$(test_build_dir)"
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        net-gate-probe release-drivers driver > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "FAIL: building net-gate-probe.efi, the release drivers and the companion"
    exit 1
fi
# shellcheck source=lib/qmp-lib.sh
source "$(dirname "$0")/lib/qmp-lib.sh"
axl_qmp_available || missing_tool "no python3 qemu.qmp on the host (needs=qemu.qmp)"
test_add_efi "$TEST_BUILD_DIR/net-gate-probe.efi"
RDRV="$TEST_BUILD_DIR/release-drivers/$NAT"
cp "$RDRV/NetworkCommon.efi" "$RDRV/UsbRndis.efi" "$TEST_STAGING/"
mkdir -p "$TEST_STAGING/sweepc"
cp "$TEST_BUILD_DIR/refuse-unload-driver.efi" "$TEST_STAGING/sweepc/Ref.efi"
cp "$TEST_BUILD_DIR/driver.efi" "$TEST_STAGING/sweepc/Comp.efi"
printf '%s\n' "{ schema: 1, drivers: [ { name: 'Ref.efi', requires: [ 'Comp.efi' ] } ] }" \
    > "$TEST_STAGING/sweepc/netload-drivers.json5"
mkdir -p "$TEST_STAGING/sweepd" "$TEST_STAGING/embsrc"
cp "$RDRV/UsbCdcNcm.efi" "$TEST_STAGING/sweepd/Aaa.efi"
cp "$RDRV/UsbCdcNcm.efi" "$TEST_STAGING/embsrc/Ncm.efi"
printf '%s\n' "{ schema: 1, drivers: [ { name: 'Aaa.efi', requires: [ 'Emb.efi' ] } ] }" \
    > "$TEST_STAGING/sweepd/netload-drivers.json5"


# The probe's GATE: lines between the line that is exactly $1 (a marker
# echoed before the run) and its result line, in order.
gate_lines() {
    test_clean_log
    awk -v from="$1" '
        $0 == from { on = 1; next }
        on && /^GATE: / { print }
        on && /^GATE: rc=/ { exit }' "$TEST_CLEAN_LOG"
}

# run_probe ARGS -- the probe, after a marker line left in MARK; waits for its
# result line (the $PROBES-th of the boot).
PROBES=0
run_probe() {
    MARK="p4f-g-$(rand_tag)"
    send "echo %T%-${MARK#p4f-}\\r"
    wait_guest "$MARK" 1 20 || true
    PROBES=$((PROBES + 1))
    send "net-gate-probe.efi $1\\r"
}
wait_result() {
    wait_guest_re '^GATE: rc=' "$PROBES" 60 || true
}

# check_gate LABEL MARKER EXPECTED -- the probe's lines, exactly.
check_gate() {
    local got
    got=$(gate_lines "$2")
    if [[ "$got" == "$3" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$3") <(printf '%s\n' "$got") | sed 's/^/    /' || true
    fi
}

# End a boot: power off, judge its leak verdicts, keep its log.
end_boot() {
    sshd_poweroff
    test_clean_log
    if test_check_leaks; then
        pass "$1: every leak verdict in the boot is clean"
    else
        fail "$1: a leak or a missing verdict"
        tail -40 "$TEST_CLEAN_LOG"
    fi
    n=$(heap_corruptions)
    check "$1: no heap corruption reported ($n)" test "$n" = 0
    cp -f "$TEST_LOG" "$TEST_TMPDIR/boot-$1.log"
}

echo "=== net gate ($TEST_ARCH) ==="
SSHD_NO_DHCP=1

SOCK="$TEST_TMPDIR/qmp.sock"
rm -f "$SOCK"
SSHD_NET=(-qmp "unix:$SOCK,server,nowait" -device "qemu-xhci,id=xhci"
          -netdev "user,id=u0")
sshd_boot ""

# --- refuse -------------------------------------------------------------------
run_probe 'sweep FS0:\\sweepc'
wait_result
check_gate "refuse: the companion is co-loaded, the candidate tried and bound no NIC" "$MARK" \
    "$(printf '%s\n' \
        'GATE: nics=0' \
        'GATE: ev UP Comp.efi - dep' \
        'GATE: ev TRYING Ref.efi -' \
        'GATE: ev NO_NIC Ref.efi -' \
        'GATE: rc=-1 online=0 via=- ip=-')"
# The drivers' own lines, in order, between the marker and the result.
test_clean_log
GOT=$(awk -v from="$MARK" '
        $0 == from { on = 1; next }
        on && /^GATE: rc=/ { exit }
        on && /^(REFUSE: |PASS: driver-unload$)/ { print }' "$TEST_CLEAN_LOG")
WANT_LINES=$(printf '%s\n' 'REFUSE: loaded' 'REFUSE: unload refused' 'PASS: driver-unload')
if [[ "$GOT" == "$WANT_LINES" ]]; then
    pass "refuse: the candidate's unload is refused once, and the companion is unloaded after it"
else
    fail "refuse: the candidate's unload is refused once, and the companion is unloaded after it"
    diff <(printf '%s\n' "$WANT_LINES") <(printf '%s\n' "$GOT") | sed 's/^/    /' || true
fi
RAND=$(rand_tag)
send 'dh -v\r'
send "echo %T%-after-dh-$RAND\\r"
check "refuse: the Shell runs on after \`dh -v\` walks the stranded image" \
    wait_guest "p4f-after-dh-$RAND" 1 60
n=$(cpu_exceptions)
check "refuse: no CPU exception ($n)" test "$n" = 0

# --- unload -------------------------------------------------------------------
send "net-gate-probe.efi unload-refuse FS0:\\\\sweepc\\\\Ref.efi\\r"
check "unload: a refused unload of a memory-loaded image leaves its identity intact" \
    wait_guest "GATE: unload rc=-1 dp=intact filepath=set" 1 30

# --- embedded -----------------------------------------------------------------
run_probe 'sweep-emb FS0:\\sweepd ipxe-a.efi=FS0:\\embsrc\\Ncm.efi Emb.efi=FS0:\\embsrc\\Ncm.efi'
wait_result
check_gate "embedded: non-iPXE embedded first, a dependency name no bar, iPXE last" "$MARK" \
    "$(printf '%s\n' \
        'GATE: nics=0' \
        'GATE: ev TRYING Emb.efi - emb' \
        'GATE: ev NO_NIC Emb.efi - emb' \
        'GATE: ev LOAD_FAIL Emb.efi - dep' \
        'GATE: ev TRYING Aaa.efi -' \
        'GATE: ev NO_NIC Aaa.efi -' \
        'GATE: ev TRYING ipxe-a.efi - emb' \
        'GATE: ev NO_NIC ipxe-a.efi - emb' \
        'GATE: rc=-1 online=0 via=- ip=-')"

# --- late ---------------------------------------------------------------------
if [[ "$NAT" == aa64 ]]; then
    # Measured: under the aarch64 firmware QEMU's usb-net binds no driver even
    # when present at boot (UsbRndis/UsbCdcEcm load, `connect -r`, no
    # interface), so there is no USB NIC to hot-plug. Balanced: one SKIP per
    # check below.
    for i in 1 2; do
        echo "  SKIP: late: QEMU's usb-net binds no driver under the aarch64 firmware"
    done
    SKIPPED=2
else
    SKIPPED=0
    send 'load NetworkCommon.efi\r'
    send 'load UsbRndis.efi\r'
    send "echo %T%-loaded-$RAND\\r"
    wait_guest "p4f-loaded-$RAND" 1 20 || true
    run_probe late
    wait_guest "GATE: nics=0" 3 30 || true   # the refuse and embedded sweeps printed two
    axl_qmp "$SOCK" device_add \
            '{"driver":"usb-net","id":"hot0","bus":"xhci.0","netdev":"u0","mac":"52:54:00:12:00:41"}' \
            > "$TEST_TMPDIR/qmp.out" 2>&1 \
        || echo "  QMP device_add failed: $(cat "$TEST_TMPDIR/qmp.out")"
    wait_result
    check_gate "late: a NIC that appears during the link wait is found and tried" "$MARK" \
        "$(printf '%s\n' \
            'GATE: nics=0' \
            'GATE: ev TRYING firmware -' \
            'GATE: ev CONFIGURING firmware 52:54:00:12:00:41' \
            'GATE: ev LINK_NO_LEASE firmware 52:54:00:12:00:41' \
            'GATE: rc=-1 online=0 via=- ip=-')"
    # The control: the hot-plugged NIC is there, whatever the gate made of it.
    send 'ifconfig -l\r'
    check "late/control: the hot-plugged USB NIC is a Shell interface" \
        wait_guest 'mac addr     : 52:54:00:12:00:41' 1 20
fi
end_boot gate

# ifconfig_block MAC -- "policy ipv4 gateway dns" of that NIC, from `ifconfig -l`.
ifconfig_block() {
    local tag
    tag=$(rand_tag)
    send "echo %T%-if-$tag\\r"
    send 'ifconfig -l\r'
    send "echo %T%-if-end-$tag\\r"
    wait_guest "p4f-if-end-$tag" 1 20 || true
    test_clean_log
    test_slice_log "p4f-if-$tag" "p4f-if-end-$tag" "$TEST_TMPDIR/if.txt" || return 0
    awk -v mac="$1" '
        /^name +:/              { if (hit) exit; pol = ip = gw = dns = "-"; hit = 0; dnsnext = 0 }
        /^policy +:/            { pol = $3 }
        /^mac addr +:/          { hit = ($4 == mac) }
        /^ipv4 address +:/      { ip = $4 }
        /^default gateway *:/   { gw = $NF }
        dnsnext && NF > 0       { if ($1 ~ /^[0-9.]+$/) dns = $1; dnsnext = 0 }
        /^DNS server +:/        { dnsnext = 1 }
        END { if (hit) print pol, ip, gw, dns }' "$TEST_TMPDIR/if.txt"
}

# --- static ---------------------------------------------------------------------
# New MACs per boot: IP4Config2 keeps a NIC's policy in NVRAM, by MAC.
PROBES=0
SSHD_NET=(-device "virtio-net-pci,netdev=n0,mac=52:54:00:12:00:51,romfile="
          -netdev "hubport,id=n0,hubid=21"
          -device "virtio-net-pci,netdev=net0,mac=52:54:00:12:00:52,romfile="
          -netdev "user,id=net0")
sshd_boot "$(printf '%s\n' 'ifconfig -s eth0 static 192.168.77.5 255.255.255.0 192.168.77.1' \
                            'ifconfig -s eth0 dns 192.168.77.53')"
BEFORE=$(ifconfig_block 52:54:00:12:00:51)
echo "  (NIC A before the race: $BEFORE)"
run_probe race
wait_result
check_gate "static: the leasing NIC wins the race over the static one" "$MARK" \
    "$(printf '%s\n' \
        'GATE: nics=2' \
        'GATE: ev TRYING firmware -' \
        'GATE: ev CONFIGURING firmware 52:54:00:12:00:51' \
        'GATE: ev CONFIGURING firmware 52:54:00:12:00:52' \
        'GATE: ev UP firmware 52:54:00:12:00:52' \
        'GATE: rc=0 online=1 via=firmware ip=10.0.2.15')"
got=$(ifconfig_block 52:54:00:12:00:51)
check "static: the loser is static again with its address, gateway and DNS ($got)" \
    test "$got" = "static 192.168.77.5 192.168.77.1 192.168.77.53" -a "$got" = "$BEFORE"
end_boot static

# --- reject ---------------------------------------------------------------------
PROBES=0
SSHD_NET=(-device "virtio-net-pci,netdev=net0,mac=52:54:00:12:00:61,romfile="
          -netdev "user,id=net0"
          -device "virtio-net-pci,netdev=n1,mac=52:54:00:12:00:62,romfile="
          -netdev "user,id=n1,net=10.0.3.0/24,dhcpstart=10.0.3.15")
sshd_boot ""
BEFORE61=$(ifconfig_block 52:54:00:12:00:61)
BEFORE62=$(ifconfig_block 52:54:00:12:00:62)
run_probe race-reject
wait_result
test_clean_log
REJ=$({ grep -aoE '^GATE: verify 52:54:00:12:00:6[12] reject$' "$TEST_CLEAN_LOG" || true; } \
      | head -1 | awk '{print $3}')
if [[ "$REJ" == 52:54:00:12:00:61 ]]; then
    WIN=52:54:00:12:00:62; WIN_IP=10.0.3.15
else
    WIN=52:54:00:12:00:61; WIN_IP=10.0.2.15
fi
check_gate "reject: the race goes on past a rejected lease, and the other NIC wins" "$MARK" \
    "$(printf '%s\n' \
        'GATE: nics=2' \
        'GATE: ev TRYING firmware -' \
        'GATE: ev CONFIGURING firmware 52:54:00:12:00:61' \
        'GATE: ev CONFIGURING firmware 52:54:00:12:00:62' \
        "GATE: verify ${REJ:-?} reject" \
        "GATE: verify $WIN accept" \
        "GATE: ev UP firmware $WIN" \
        "GATE: rc=0 online=1 via=firmware ip=$WIN_IP")"
if [[ "$REJ" == 52:54:00:12:00:61 ]]; then BEFORE_REJ="$BEFORE61"; else BEFORE_REJ="$BEFORE62"; fi
got=$(ifconfig_block "${REJ:-?}")
check "reject: the rejected NIC has no lease and its settings as before ($got)" \
    test -n "$got" -a "$got" = "$BEFORE_REJ" -a "${got#static 0.0.0.0 }" != "$got"
end_boot reject

# --- reject1 --------------------------------------------------------------------
PROBES=0
SSHD_NET=(-device "virtio-net-pci,netdev=net0,mac=52:54:00:12:00:71,romfile="
          -netdev "user,id=net0")
sshd_boot ""
run_probe race-reject-long
wait_result
check_gate "reject1: the only NIC rejected ends the pass at once, NO_REACH" "$MARK" \
    "$(printf '%s\n' \
        'GATE: nics=1' \
        'GATE: ev TRYING firmware -' \
        'GATE: ev CONFIGURING firmware 52:54:00:12:00:71' \
        'GATE: verify 52:54:00:12:00:71 reject' \
        'GATE: ev NO_REACH firmware 52:54:00:12:00:71' \
        'GATE: rc=-1 online=0 via=- ip=-')"
end_boot reject1

echo ""
WANT=$((21 - SKIPPED))
printf "net-gate: %d passed, %d failed, %d skipped (%s)\n" "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
[[ $FAIL -eq 0 && $PASS -eq $WANT ]] && exit 0 || exit 1
