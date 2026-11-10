#!/bin/bash
# test-meta: arch=both needs=sftp est=80 cpu=2 local-only=0 builds=sshd,sshd-rtk,sshd-full-rtk,release-drivers,devhandle-driver guest=none
# guest=none: it types into the guest Shell (test_send_stdin), and a leased
#   guest takes no typed serial input.
# test-sshd-rtk-qemu.sh -- sshd-rtk.efi, sshd with a NIC driver inside it
# (`scripts/axl-build.sh sshd-rtk RTK_USB_UNDI=...`). Realtek's driver cannot
# run in QEMU and never enters this tree, so the build hook is given a stand-in: on x64 the
# e1000 driver cut from QEMU's ROM (lib/e1000-lib.sh), for a ROM-less e1000
# the firmware has no driver for; on aarch64, where QEMU ships no e1000
# driver, devhandle-driver.efi as DevProbe.efi, which binds no NIC and says
# whether its loaded image was given a DeviceHandle.
#
# Boot 1: the stand-in NIC (x64) or none (aarch64), no DHCP in the startup,
# both sshd.efi and sshd-rtk.efi at the root, and drivers\<arch> beside them
# holding one dummy (UsbCdcNcm.efi as Aaa.efi, which binds no NIC).
#   embedded  `sshd-rtk -v` names the embedded driver as the first candidate,
#             after the firmware pass and before drivers\<arch>: x64 -- it
#             brings the e1000 up, sshd serves on its address, and Aaa.efi is
#             never tried; aarch64 -- it brings nothing up and is unloaded,
#             Aaa.efi is tried after it, and the start fails
#   shared    (x64) the two launchers are one service: with sshd-rtk serving,
#             `sshd status` says it runs and `sshd stop` stops it; with sshd
#             serving, `sshd-rtk stop` stops it
#   resident  (x64) after `sshd stop` the embedded driver and its lease stay:
#             the next `sshd -v` uses the addressed NIC as it is -- no sweep
# Boot 2: the same, with NO drivers directory anywhere -- one file on the
# stick, sshd-rtk's own use case. Then sshd-full-rtk.efi, built around the
# same stand-in: on x64 it serves and an sftp(1) `get` is answered by its
# embedded SFTP helper; on aarch64 it fails as sshd-rtk does, naming its
# embedded driver.
#   nodir     the embedded driver is still given a volume as its DeviceHandle
#             (the launcher's): x64 -- iPXE's e1000 driver, which refuses to
#             start without one, starts and sshd serves; aarch64 -- DevProbe
#             says `DEVHANDLE: set`, and the start fails for want of a NIC
#
# Usage: ./test/integration/test-sshd-rtk-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/e1000-lib.sh"

test_parse_args "$@"
test_setup
declare -A _NAT=([X64]=x64 [AARCH64]=aa64)
NAT="${_NAT[$TEST_ARCH]:-x64}"

PW=rtk-pw
STARTING='sshd: starting -- bringing up the network (up to 60 s)'
NOSTART='sshd: could not start the server'
DBG_MSG_RE='\[DEBUG\] sshd:[a-z_0-9]+:[0-9]+: '
TOTAL=19
SKIPPED=0
skip() { echo "  SKIP: $1"; SKIPPED=$((SKIPPED + 1)); }

sshd_build
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        release-drivers devhandle-driver > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "FAIL: building the release drivers and devhandle-driver.efi"
    exit 1
fi
RDRV="$TEST_BUILD_DIR/release-drivers/$NAT"

e1000_driver || true
e1000_required_in_ci
if [[ -n "$E1000" ]]; then
    # Not iPXE-named: an embedded iPXE driver is swept after every other
    # candidate, and this one stands in for a driver that goes first.
    STANDIN="$E1000"   # e1000.efi
else
    STANDIN="$TEST_TMPDIR/DevProbe.efi"
    cp "$TEST_BUILD_DIR/devhandle-driver.efi" "$STANDIN"
fi
EMB=$(basename "$STANDIN")
# The driver is the build's ENVIRONMENT, not a make word: the runner
# pre-builds `sshd-rtk` without it (a no-op), and this builds the real one.
# Built into this test's own directory (SSHD_RTK_OUT), never the shared
# tools/: tool-version and redirect-pipe enumerate tools/*.efi, and a
# stand-in build left there broke them in a full run.
RTK_OUT="$TEST_TMPDIR/rtk"
BUILT_RE="^  Built: ${RTK_OUT//./\\.}/sshd-rtk\.efi \(sshd \+ embedded ${EMB//./\\.}\)\$"
if ! RTK_USB_UNDI="$STANDIN" SSHD_RTK_OUT="$RTK_OUT" "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        sshd-rtk > "$TEST_TMPDIR/make-rtk.log" 2>&1 \
   || ! grep -qE "$BUILT_RE" "$TEST_TMPDIR/make-rtk.log"; then
    tail -20 "$TEST_TMPDIR/make-rtk.log"
    echo "FAIL: building sshd-rtk.efi around $EMB"
    exit 1
fi
test_add_efi "$RTK_OUT/sshd-rtk.efi"
FULL_BUILT_RE="^  Built: ${RTK_OUT//./\\.}/sshd-full-rtk\.efi \(sshd \+ SftpServer\.efi \+ ${EMB//./\\.}\)\$"
if ! RTK_USB_UNDI="$STANDIN" SSHD_RTK_OUT="$RTK_OUT" "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NAT" \
        sshd-full-rtk > "$TEST_TMPDIR/make-full-rtk.log" 2>&1 \
   || ! grep -qE "$FULL_BUILT_RE" "$TEST_TMPDIR/make-full-rtk.log"; then
    tail -20 "$TEST_TMPDIR/make-full-rtk.log"
    echo "FAIL: building sshd-full-rtk.efi around $EMB"
    exit 1
fi
test_add_efi "$RTK_OUT/sshd-full-rtk.efi"
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/authorized_keys"
printf 'over sshd-full-rtk\n' > "$TEST_STAGING/full.txt"
mkdir -p "$TEST_STAGING/drivers/$NAT"
cp "$RDRV/UsbCdcNcm.efi" "$TEST_STAGING/drivers/$NAT/Aaa.efi"

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

echo "=== sshd-rtk ($TEST_ARCH, embedding $EMB) ==="
SSHD_NO_DHCP=1
SSHD_NET=(-nic none)   # aarch64: no NIC at all (an empty array would get the default)
if [[ -n "$E1000" ]]; then
    SSHD_NET=(-device "e1000,netdev=net0,mac=52:54:00:ee:00:01,romfile="
              -netdev "user,id=net0@HOSTFWD@")
fi
sshd_boot ""

# --- embedded --------------------------------------------------------------------
RAND=$(rand_tag)
send "echo %T%-v-$RAND\\r"
wait_guest "p4f-v-$RAND" 1 20 || true
send "sshd-rtk -v -P $PW\\r"
check "embedded: sshd-rtk says it is starting" wait_guest "$STARTING" 1 30
PHASES=(
    'bringing up the network (NIC auto, up to 60 s)'
    "drivers directory: FS0:\\drivers\\$NAT"
    "firmware pass: connecting the firmware's NIC drivers"
    'firmware pass: no NIC with a link'
    "trying embedded driver $EMB"
)
if [[ -n "$E1000" ]]; then
    check "embedded: serves on the embedded driver's NIC" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 60
    check_phases "embedded: -v names the embedded driver first, and it wins" \
        "p4f-v-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' "${PHASES[@]}" \
            'DHCP on NIC 52:54:00:ee:00:01 (up to N s)' \
            "embedded driver $EMB brought NIC 52:54:00:ee:00:01 up with 10.0.2.15" \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
else
    check "embedded: with no NIC, fails saying no driver produced an interface" \
        wait_guest "sshd: no network: no NIC driver produced an interface (searched the firmware and FS0:\\drivers\\$NAT)" 1 60
    check_phases "embedded: -v names the embedded driver first, then the directory's" \
        "p4f-v-$RAND" "sshd: no network:" \
        "$(printf '%s\n' "${PHASES[@]}" \
            "embedded driver $EMB: no NIC with a link -- unloaded" \
            'trying driver Aaa.efi' \
            'driver Aaa.efi: no NIC with a link -- unloaded')"
fi
check "embedded: drivers\\$NAT's Aaa.efi is tried only when the embedded one did not win" \
    test "$(guest_re "${DBG_MSG_RE}trying driver Aaa\\.efi\$")" = "$([[ -n "$E1000" ]] && echo 0 || echo 1)"

# --- shared, resident -------------------------------------------------------------
if [[ -n "$E1000" ]]; then
    send 'sshd status\r'
    check "shared: \`sshd status\` sees sshd-rtk's server" \
        wait_guest "sshd: running on 10.0.2.15" 1 20
    send 'sshd stop\r'
    check "shared: \`sshd stop\` stops sshd-rtk's server" \
        wait_guest "sshd: stopped" 1 30
    RAND=$(rand_tag)
    send "echo %T%-r-$RAND\\r"
    wait_guest "p4f-r-$RAND" 1 20 || true
    send "sshd -v -P $PW\\r"
    check "resident: plain sshd serves after the stop" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 2 60
    check_phases "resident: the embedded driver and its lease outlived the stop -- no sweep" \
        "p4f-r-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' \
            'bringing up the network (NIC auto, up to 60 s)' \
            'NIC 52:54:00:ee:00:01 has 10.0.2.15 -- using it' \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
    send 'sshd-rtk stop\r'
    check "shared: \`sshd-rtk stop\` stops sshd's server" \
        wait_guest "sshd: stopped" 2 30
    send 'sshd status\r'
    check "shared: \`sshd status\` then says it is stopped" \
        wait_guest "sshd: stopped" 3 20
else
    # Six x64 checks here; this one takes the place of the first.
    check "embedded: the launcher then says the server did not start" \
        wait_guest "$NOSTART" 1 10
    for l in "shared: sshd stop stops sshd-rtk" "resident: plain sshd serves after the stop" \
             "resident: no sweep" "shared: sshd-rtk stop stops sshd" "shared: then stopped"; do
        skip "$l -- needs a NIC the embedded driver serves on ($WHY_NO_E1000)"
    done
fi

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
end_boot one

# --- nodir --------------------------------------------------------------------
rm -rf "$TEST_STAGING/drivers"
sshd_boot ""
RAND=$(rand_tag)
send "echo %T%-n-$RAND\\r"
wait_guest "p4f-n-$RAND" 1 20 || true
send "sshd-rtk -v -P $PW\\r"
PHASES=(
    'bringing up the network (NIC auto, up to 60 s)'
    "no drivers directory found -- sweeping \\drivers\\$NAT on every volume"
    "firmware pass: connecting the firmware's NIC drivers"
    'firmware pass: no NIC with a link'
    "trying embedded driver $EMB"
)
if [[ -n "$E1000" ]]; then
    check "nodir: the embedded iPXE driver starts (it has a device) and sshd serves" \
        wait_guest "sshd: serving the UEFI Shell on 10.0.2.15 port 22" 1 60
    check_phases "nodir: -v names the embedded driver, and it wins" \
        "p4f-n-$RAND" "sshd: serving the UEFI Shell on" \
        "$(printf '%s\n' "${PHASES[@]}" \
            'DHCP on NIC 52:54:00:ee:00:01 (up to N s)' \
            "embedded driver $EMB brought NIC 52:54:00:ee:00:01 up with 10.0.2.15" \
            'taking over the console' \
            'building the server (host key, authorized keys)' \
            'listening on port 22' \
            'the log is not written while serving: every serial port carries the console')"
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 1 30 || true

    # --- full: sshd-full-rtk, on the NIC the stand-in left up ----------------------
    send "sshd-full-rtk -P $PW $SSHD_LOG_HERE\\r"
    check "full: sshd-full-rtk serves" \
        wait_guest_re "sshd: serving the UEFI Shell on 10\\.0\\.2\\.15 port 22( |\$)" 2 60
    rm -f "$TEST_TMPDIR/full.txt"
    printf 'get /fs0/full.txt %s\n' "$TEST_TMPDIR/full.txt" > "$TEST_TMPDIR/full.batch"
    timeout 120 sftp -b "$TEST_TMPDIR/full.batch" -P "$HOST_PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
        -o IdentityAgent=none -o LogLevel=ERROR -i "$TEST_TMPDIR/key" anyone@127.0.0.1 \
        > "$TEST_TMPDIR/full.out" 2>&1 < /dev/null || true
    check "full: an sftp get returns the file, byte for byte" \
        cmp -s "$TEST_TMPDIR/full.txt" "$TEST_STAGING/full.txt"
    check "full: the SFTP helper was the embedded copy" \
        wait_guest_re '\[INFO\]  sftp: SFTP helper loaded from \(embedded\)$' 1 10
    send 'sshd stop\r'
    wait_guest "sshd: stopped" 2 30 || true
else
    check "nodir: the embedded driver was given a DeviceHandle" \
        wait_guest "DEVHANDLE: set" 1 60
    wait_guest "sshd: no network: no NIC driver produced an interface (searched the firmware and \\drivers\\$NAT on every volume)" 1 60 || true
    check_phases "nodir: -v names the embedded driver; with no NIC the start fails" \
        "p4f-n-$RAND" "sshd: no network:" \
        "$(printf '%s\n' "${PHASES[@]}" \
            "embedded driver $EMB: no NIC with a link -- unloaded")"

    # --- full: sshd-full-rtk runs its own embedded NIC driver too --------------------
    send "sshd-full-rtk -P $PW\\r"
    check "full: sshd-full-rtk fails as sshd-rtk does, with no NIC" \
        wait_guest "sshd: no network: no NIC driver produced an interface (searched the firmware and \\drivers\\$NAT on every volume)" 2 60
    for l in "full: an sftp get returns the file" "full: the SFTP helper was the embedded copy"; do
        skip "$l -- needs a NIC the embedded driver serves on ($WHY_NO_E1000)"
    done
fi
end_boot nodir

echo ""
WANT=$((TOTAL - SKIPPED))
printf "sshd-rtk: %d passed, %d failed, %d skipped (%s)\n" "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
[[ $FAIL -eq 0 && $PASS -eq $WANT ]] && exit 0 || exit 1
