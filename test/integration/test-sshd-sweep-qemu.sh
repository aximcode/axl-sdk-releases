#!/bin/bash
# test-meta: arch=both needs= est=100 local-only=0 builds=sshd,release-drivers,driver guest=none
# guest=none: a second disk (TEST_DISK2) of drivers, and typed input: a
#   machine no class gives.
# test-sshd-sweep-qemu.sh -- `sshd` alone gets a box online the way
# `netload -a` would: the firmware's NICs first, then a crash-safe sweep of
# the drivers in drivers\<arch> BESIDE sshd.efi, under the one deadline.
#
# The guests are the shape of a lab server with a USB NIC dongle: the
# firmware has an IP stack and a NIC driver, but its NIC cannot get an
# address, and the NIC that is cabled needs a driver the firmware lacks,
# which is on the stick. On x64 that NIC is a ROM-less e1000 whose driver --
# iPXE's, cut from QEMU's efi-e1000.rom -- is staged only on the stick.
# QEMU ships no aarch64 e1000 driver, so on aarch64 the sweep finds no NIC
# and each boot checks the failure instead.
#
# The dummy candidates are copies of UsbCdcNcm.efi, which binds no NIC. The
# companion is sdk/examples/driver.efi, which prints `PASS: driver-unload`
# when it is unloaded.
#
# Boots and scenarios, in run order:
#   A (a link-down firmware NIC; drivers\<arch> beside \sshd.efi, with a
#      netload-drivers.json5 naming Comp.efi as the companion of Aaa.efi
#      and of the e1000 driver;
#      `netload --_mark Bbb.efi` leaves the breadcrumb a crash would)
#     ctrl-c   \cc\sshd.efi, whose drivers\<arch> holds twenty dummies: a
#              serial Ctrl-C while it sweeps them stops it at once, with its
#              message, before the last dummy; nothing is deployed and the
#              Shell runs the next command
#     sweep    \sshd.efi -v names the drivers directory, the firmware pass,
#              the link-down firmware NIC, the companion it co-loads, each
#              driver it tries, the quarantined one it skips (netload's
#              quarantine) -- exactly, in order; the companion stays when
#              the winner needs it, else is unloaded. x64: then
#              the e1000 driver, DHCP on its NIC, and it serves. aarch64:
#              fails, saying no NIC has a link
#   B (a link-UP firmware NIC no DHCP server answers; NO drivers directory
#      on sshd.efi's volume -- the drivers are on a second disk)
#     nolease  the firmware NIC's DHCP gives up after its 15 s and the sweep
#              runs: the driver, started from memory, sweeps every volume's
#              drivers\<arch>. x64: the e1000 driver on the second disk
#              brings its NIC up and sshd serves, within the deadline.
#              aarch64: fails, saying no NIC leased
#   C (THREE link-up firmware NICs no DHCP server answers; the e1000 driver
#      in drivers\<arch> beside sshd.efi)
#     race     the firmware pass runs DHCP on the three at once, under ONE 15 s
#              budget, so the sweep starts after one DHCP wait, not three: x64
#              -- the e1000 driver's NIC leases and sshd serves within 35 s
#              (one at a time it took over 50 s); aarch64 -- fails, saying no
#              NIC leased, within 30 s
#
# Not tested here: the reverse quarantine direction (a driver sshd's sweep
# crashed on is skipped by netload). It needs a driver that resets the box
# mid-load and a second boot on the same NVRAM; netload's --_mark seam only
# writes the breadcrumb from netload's side. Both sides bind the namespace
# through the one net_drvq_init(), which is what makes it one quarantine.
#
# Usage: ./test/integration/test-sshd-sweep-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/linkdown-lib.sh"
source "$(dirname "$0")/lib/e1000-lib.sh"

test_parse_args "$@"
test_setup
declare -A _NAT=([X64]=x64 [AARCH64]=aa64)
NAT="${_NAT[$TEST_ARCH]:-x64}"

PW=sweep-pw
STARTING='sshd: starting -- bringing up the network (up to 60 s)'
NOSTART='sshd: could not start the server'
DBG_MSG_RE='\[DEBUG\] sshd:[a-z_0-9]+:[0-9]+: '
TOTAL=23
SKIPPED=0
skip() { echo "  SKIP: $1"; SKIPPED=$((SKIPPED + 1)); }

sshd_build
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        tools release-drivers driver > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "FAIL: building netload, the release drivers and the companion"
    exit 1
fi
ld_setup "$TEST_ARCH" "$TEST_TMPDIR"
if [[ "$LD_AVAILABLE" -eq 0 ]]; then
    missing_tool "a link-down NIC needs the QEMU x-link-down patch or python3 qemu.qmp"
fi
RDRV="$TEST_BUILD_DIR/release-drivers/$NAT"

e1000_driver || true
e1000_required_in_ci

now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%d", b - a }'; }

# The sshd-domain DEBUG messages between the line that is exactly $1 and the
# first line after it starting with $2, with a DHCP line's budget as N.
phases_between() {
    test_clean_log
    awk -v from="$1" -v stop="$2" '
        $0 == from { on = 1; next }
        on && index($0, stop) == 1 { exit }
        on { print }' "$TEST_CLEAN_LOG" \
        | { grep -aoE -- "${DBG_MSG_RE}.*\$" || true; } | sed -E "s/^${DBG_MSG_RE}//" \
        | sed -E 's/^(DHCP on NIC [0-9a-f:]+ \(up to )[0-9]+( s\))$/\1N\2/'
}
check_phases() {
    local got
    got=$(phases_between "$2" "$3")
    if [[ "$got" == "$4" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$4") <(printf '%s\n' "$got") | sed 's/^/    /' || true
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

echo "=== sshd sweep ($TEST_ARCH) ==="
[[ -z "$E1000" ]] && echo "  (no e1000 driver: $WHY_NO_E1000)"
SSHD_NO_DHCP=1

# --- boot A -------------------------------------------------------------------
# The stick: sshd.efi and netload.efi at the root, with drivers\<arch> beside
# them; a second sshd.efi in \cc with twenty dummies beside it.
test_add_efi "$TEST_BUILD_DIR/tools/netload.efi"
DRV="$TEST_STAGING/drivers/$NAT"
mkdir -p "$DRV" "$TEST_STAGING/cc/drivers/$NAT"
cp "$TEST_BUILD_DIR/driver.efi" "$DRV/Comp.efi"
cp "$RDRV/UsbCdcNcm.efi" "$DRV/Aaa.efi"
cp "$RDRV/UsbCdcNcm.efi" "$DRV/Bbb.efi"
printf '%s\n' "{ schema: 1, drivers: [ { name: 'Aaa.efi', requires: [ 'Comp.efi' ] }, { name: 'ipxe-e1000.efi', requires: [ 'Comp.efi' ] } ] }" \
    > "$DRV/netload-drivers.json5"
[[ -n "$E1000" ]] && cp "$E1000" "$DRV/ipxe-e1000.efi"
cp "$TEST_BUILD_DIR/tools/sshd.efi" "$TEST_STAGING/cc/sshd.efi"
for i in $(seq -w 1 20); do
    cp "$RDRV/UsbCdcNcm.efi" "$TEST_STAGING/cc/drivers/$NAT/D$i.efi"
done

SOCK="$TEST_TMPDIR/ld-qmp.sock"
rm -f "$SOCK"
SSHD_NET=(-qmp "unix:$SOCK,server,nowait"
          -device "virtio-net-pci,netdev=n0,mac=52:54:00:aa:00:00,${LD_XLD}romfile="
          -netdev "user,id=n0,net=10.10.0.0/24")
if [[ -n "$E1000" ]]; then
    SSHD_NET+=(-device "e1000,netdev=net0,mac=52:54:00:ee:00:01,romfile="
               -netdev "user,id=net0@HOSTFWD@")
fi
INJ=$(ld_inject "$SOCK" n0)
sshd_boot "$(printf 'netload --clear\nnetload --_mark Bbb.efi')"

# Run from \cc, so the Shell finds THAT sshd.efi first (send() reads
# backslash escapes, so no `\c` can be typed in a path).
send 'cd cc\r'
send "sshd.efi -v -P $PW\\r"
wait_guest "$STARTING" 1 30 || true
check "A/ctrl-c: the sweep starts on the dummies beside \\cc\\sshd.efi" \
    wait_guest_re "${DBG_MSG_RE}trying driver D02\\.efi\$" 1 40
t0=$(now)
send '\003'
check "A/ctrl-c: a serial Ctrl-C stops the sweep, with its message" \
    wait_guest "sshd: network bring-up stopped (Ctrl-C)" 1 15
t1=$(now)
el=$(secs "$t0" "$t1")
check "A/ctrl-c: at once ($el s)" test "$el" -le 3
TRIED=$(guest_re "${DBG_MSG_RE}trying driver D[0-9]+\\.efi\$")
check "A/ctrl-c: the sweep stopped part-way ($TRIED of 20 dummies tried)" \
    test "$TRIED" -ge 2 -a "$TRIED" -lt 20
check "A/ctrl-c: the launcher then says the server did not start" wait_guest "$NOSTART" 1 10
RAND=$(rand_tag)
send "echo %T%-after-cc-$RAND\\r"
check "A/ctrl-c: the Shell runs the next command" wait_guest "p4f-after-cc-$RAND" 1 20
send 'sshd status\r'
check "A/ctrl-c: nothing was deployed" wait_guest "sshd: stopped" 1 20
send 'cd ..\r'

RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
PHASES=(
    'bringing up the network (NIC auto, up to 60 s)'
    "drivers directory: FS0:\\drivers\\$NAT"
    "firmware pass: connecting the firmware's NIC drivers"
    'NIC 52:54:00:aa:00:00 link down -- skipping'
    'firmware pass: no NIC with a link'
    'loaded companion driver Comp.efi'
    'trying driver Aaa.efi'
    'driver Aaa.efi: no NIC with a link -- unloaded'
    "skipping driver Bbb.efi -- quarantined (it crashed a prior run; 'netload --clear' retries it)"
)
if [[ -n "$E1000" ]]; then
    check "A/sweep: serves on the swept NIC's address" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 60
    check_phases "A/sweep: -v names each step, exactly and in order" \
        "p4f-v-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' "${PHASES[@]}" \
            'trying driver ipxe-e1000.efi' \
            'DHCP on NIC 52:54:00:ee:00:01 (up to N s)' \
            'driver ipxe-e1000.efi brought NIC 52:54:00:ee:00:01 up with 10.0.2.15' \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
    send 'sshd status\r'
    check "A/sweep: \`sshd status\` names the address" \
        wait_guest "sshd: running on 10.0.2.15" 1 20
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 2 30 || true
else
    skip "A/sweep: serving on the swept NIC needs the e1000 driver ($WHY_NO_E1000)"
    check "A/sweep: with no driver for a cabled NIC, fails saying no NIC has a link" \
        wait_guest "sshd: no network: no NIC has a link (1 link down)" 1 60
    check_phases "A/sweep: -v names each step, exactly and in order" \
        "p4f-v-$RAND" "sshd: no network:" "$(printf '%s\n' "${PHASES[@]}")"
fi
# Comp.efi is a companion of both Aaa.efi (which brings nothing up) and the
# e1000 driver: loaded once, reused, and kept only when the winner needs it.
if [[ -n "$E1000" ]]; then
    check "A/sweep: the winner's companion stays resident (loaded once, never unloaded)" \
        test "$(guest_re 'PASS: driver-unload$')" = 0 -a \
             "$(guest_re "${DBG_MSG_RE}loaded companion driver Comp\\.efi\$")" = 1
else
    check "A/sweep: with no winner, the companion is unloaded at the sweep's end" \
        test "$(guest_re 'PASS: driver-unload$')" = 1
fi
ld_inject_stop "$INJ"
end_boot A

# --- boot B -------------------------------------------------------------------
# Nothing on the stick's volume but the tools; the drivers on a second disk.
rm -rf "$TEST_STAGING/drivers" "$TEST_STAGING/cc"
mkdir -p "$TEST_TMPDIR/disk2/drivers/$NAT"
cp "$RDRV/UsbCdcNcm.efi" "$TEST_TMPDIR/disk2/drivers/$NAT/Zz.efi"
[[ -n "$E1000" ]] && cp "$E1000" "$TEST_TMPDIR/disk2/drivers/$NAT/ipxe-e1000.efi"
export TEST_DISK2="$TEST_TMPDIR/disk2.img"
qemu_stage_disk "$TEST_TMPDIR/disk2" "$TEST_DISK2" D2 || { echo "FAIL: staging the second disk"; exit 1; }

SSHD_NET=(-device "virtio-net-pci,netdev=n0,mac=52:54:00:12:00:03,romfile="
          -netdev "hubport,id=n0,hubid=7")
if [[ -n "$E1000" ]]; then
    SSHD_NET+=(-device "e1000,netdev=net0,mac=52:54:00:ee:00:01,romfile="
               -netdev "user,id=net0@HOSTFWD@")
fi
sshd_boot ""
RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
wait_guest "$STARTING" 1 30 || true
t0=$(now)
PHASES=(
    'bringing up the network (NIC auto, up to 60 s)'
    "no drivers directory found -- sweeping \\drivers\\$NAT on every volume"
    "firmware pass: connecting the firmware's NIC drivers"
    'DHCP on NIC 52:54:00:12:00:03 (up to N s)'
    'firmware pass: NIC 52:54:00:12:00:03 got no address'
    'trying driver Zz.efi'
    'driver Zz.efi: no NIC with a link -- unloaded'
)
if [[ -n "$E1000" ]]; then
    check "B/nolease: serves on the swept NIC's address" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 90
    t1=$(now)
    el=$(secs "$t0" "$t1")
    check "B/nolease: within the deadline ($el s)" test "$el" -le 60
    check_phases "B/nolease: -v shows the firmware NIC's DHCP giving up, then the sweep of every volume" \
        "p4f-v-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' "${PHASES[@]}" \
            'trying driver ipxe-e1000.efi' \
            'DHCP on NIC 52:54:00:ee:00:01 (up to N s)' \
            'driver ipxe-e1000.efi brought NIC 52:54:00:ee:00:01 up with 10.0.2.15' \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 1 30 || true
else
    skip "B/nolease: serving on the swept NIC needs the e1000 driver ($WHY_NO_E1000)"
    check "B/nolease: fails, saying no NIC with a link got a lease" \
        wait_guest "sshd: no network: no DHCP lease on any NIC with a link (1 tried)" 1 90
    check_phases "B/nolease: -v shows the firmware NIC's DHCP giving up, then the sweep of every volume" \
        "p4f-v-$RAND" "sshd: no network:" "$(printf '%s\n' "${PHASES[@]}")"
fi
end_boot B

# --- boot C -------------------------------------------------------------------
unset TEST_DISK2
mkdir -p "$TEST_STAGING/drivers/$NAT"
[[ -n "$E1000" ]] && cp "$E1000" "$TEST_STAGING/drivers/$NAT/ipxe-e1000.efi"
SSHD_NET=()
for k in 3 4 5; do
    SSHD_NET+=(-device "virtio-net-pci,netdev=n$k,mac=52:54:00:12:00:0$k,romfile="
               -netdev "hubport,id=n$k,hubid=$((10 + k))")
done
if [[ -n "$E1000" ]]; then
    SSHD_NET+=(-device "e1000,netdev=net0,mac=52:54:00:ee:00:01,romfile="
               -netdev "user,id=net0@HOSTFWD@")
fi
sshd_boot ""
RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd -v -P $PW\\r"
wait_guest "$STARTING" 1 30 || true
t0=$(now)
PHASES=(
    'bringing up the network (NIC auto, up to 60 s)'
    "drivers directory: FS0:\\drivers\\$NAT"
    "firmware pass: connecting the firmware's NIC drivers"
    'DHCP on NIC 52:54:00:12:00:03 (up to N s)'
    'DHCP on NIC 52:54:00:12:00:04 (up to N s)'
    'DHCP on NIC 52:54:00:12:00:05 (up to N s)'
    'firmware pass: NIC 52:54:00:12:00:05 got no address'
)
if [[ -n "$E1000" ]]; then
    check "C/race: serves on the swept NIC's address" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 90
    t1=$(now)
    el=$(secs "$t0" "$t1")
    check "C/race: after one DHCP budget, not three ($el s)" test "$el" -le 35
    check_phases "C/race: -v runs DHCP on the three firmware NICs at once, then sweeps" \
        "p4f-v-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' "${PHASES[@]}" \
            'trying driver ipxe-e1000.efi' \
            'DHCP on NIC 52:54:00:ee:00:01 (up to N s)' \
            'driver ipxe-e1000.efi brought NIC 52:54:00:ee:00:01 up with 10.0.2.15' \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 1 30 || true
else
    check "C/race: fails, saying no NIC with a link leased" \
        wait_guest "sshd: no network: no DHCP lease on any NIC with a link (3 tried)" 1 90
    t1=$(now)
    el=$(secs "$t0" "$t1")
    check "C/race: after one DHCP budget, not three ($el s)" test "$el" -le 30
    check_phases "C/race: -v runs DHCP on the three firmware NICs at once" \
        "p4f-v-$RAND" "sshd: no network:" "$(printf '%s\n' "${PHASES[@]}")"
fi
end_boot C

echo ""
WANT=$((TOTAL - SKIPPED))
printf "sshd-sweep: %d passed, %d failed, %d skipped (%s)\n" "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
[[ $FAIL -eq 0 && $PASS -eq $WANT ]] && exit 0 || exit 1
