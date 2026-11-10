#!/bin/bash
# test-meta: arch=both needs= est=60 local-only=0 guest=none
# guest=none: it asserts lsusb's exact output for three USB topologies of
#   its own (--qemu-arg); the standard class's xHCI would change every one.
# test-lsusb-qemu.sh -- lsusb against real QEMU USB topologies, exact output.
#
# Three boots per arch, each with its OWN USB shape (run-qemu.sh adds no USB
# controller on a plain boot, unlike common-test.sh, which always attaches
# one -- so this test drives run-qemu.sh directly):
#
#   none      no USB arguments at all. `lsusb` and `lsusb -t` each print
#             exactly "lsusb: no USB host controller found", exit 0.
#   hc-only   one qemu-xhci with nothing on it. Both print exactly
#             "lsusb: no USB devices found", exit 0.
#   topology  qemu-xhci with usb-kbd (port 1), usb-mouse (port 2), usb-hub
#             (port 3) carrying a usb-tablet (port 3.1), and usb-storage
#             (port 4) over a blank scratch image. Pins the default list,
#             -n, -d and -s filters (a filter that matches nothing: no
#             output, exit 1 -- usbutils' behaviour), the -t -n tree (the
#             tablet nested under the hub, rows in device-path order) and
#             the storage device's -v strings.
#
# Every block is compared WHOLE against its expected text (diff), not
# grepped for a substring: an extra line, a missing line or the usb-ids tip
# appearing after a note all fail. Exit codes come from %lasterror%.
#
# The blank image (no filesystem) keeps the USB stick from taking a shell
# mapping, so fs0: stays the boot disk carrying lsusb.efi and its sidecar.
#
# Usage: ./test/integration/test-lsusb-qemu.sh [X64|AARCH64|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
USB_IDS="$PROJECT_DIR/share/usb-ids.json5"

export TEST_SKIP_RATCHET=1

if [ "${1:-}" = "--arch" ]; then WHICH="${2:-both}"; else WHICH="${1:-both}"; fi
case "$WHICH" in
    X64)     ARCHES=(X64) ;;
    AARCH64) ARCHES=(AARCH64) ;;
    both)    ARCHES=(X64 AARCH64) ;;
    *) echo "usage: $0 [X64|AARCH64|both]" >&2; exit 2 ;;
esac

TMP="$(mktemp -d)"
# LSUSB_KEEP_LOGS=DIR keeps every boot's serial log for a post-mortem.
trap 'if [[ -n "${LSUSB_KEEP_LOGS:-}" ]]; then mkdir -p "$LSUSB_KEEP_LOGS"; cp "$TMP"/*.log "$LSUSB_KEEP_LOGS"/ 2>/dev/null || true; fi; rm -rf "$TMP"' EXIT

# A blank (no filesystem) backing image for usb-storage.
STICK="$TMP/stick.img"
truncate -s 16M "$STICK"

pass=0
fail=0
ok() { echo "  PASS: $1"; pass=$((pass + 1)); }
no() { echo "  FAIL: $1"; fail=$((fail + 1)); }

# Lines strictly between the "==NAME==" marker and the next "==" marker line,
# less the DEBUG build's clean teardown verdict ("mem: no leaks detected"),
# which every tool prints on exit. Only that exact clean line is dropped: a
# leak report stays in the block and fails the comparison.
section() {  # section LOG NAME
    awk -v m="==$2==" '
        $0 == m { on = 1; next }
        on && /^==/ { exit }
        on && /^[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9]+ \[INFO\]  mem: no leaks detected$/ { next }
        on { print }
    ' "$1"
}

# Compare one section WHOLE against expected text.
expect_block() {  # expect_block LOG NAME DESC EXPECTED
    local got
    got="$(section "$1" "$2")"
    if [[ "$got" == "$4" ]]; then
        ok "$3"
    else
        no "$3"
        diff <(printf '%s\n' "$4") <(printf '%s\n' "$got") | sed 's/^/      /' || true
    fi
}

# The exit status line "==RC== RC_NAME=<lasterror>" printed after a command
# (it starts with "==" so section() ends the command's block on it).
expect_rc() {  # expect_rc LOG NAME DESC WANT
    local n
    n="$(grep -cxF "==RC== RC_$2=$4" "$1" || true)"
    if [[ "$n" == "1" ]]; then
        ok "$3"
    else
        no "$3 (got: $(grep -E "^==RC== RC_$2=" "$1" | head -1 || true))"
    fi
}

# Write a startup.nsh running each "NAME|command" entry between markers, with
# its %lasterror% captured as RC_NAME.
write_nsh() {  # write_nsh FILE "NAME|cmd"...
    local f="$1"; shift
    {
        echo '@echo -off'
        echo 'fs0:'
        local e
        for e in "$@"; do
            echo "echo ==${e%%|*}=="
            echo "${e#*|}"
            echo "echo ==RC== RC_${e%%|*}=%lasterror%"
        done
        echo 'echo ==END=='
        echo 'reset -s'
    } > "$f"
}

# boot ARCH EFI LOG NSH [qemu tokens...]
boot() {
    local arch="$1" efi="$2" log="$3" nsh="$4"; shift 4
    local qargs=() t
    for t in "$@"; do qargs+=(--qemu-arg "$t"); done
    local to=60
    [[ "$arch" == "AARCH64" || ! -r /dev/kvm ]] && to=150
    timeout $((to + 60))s "$RUN_QEMU" --arch "$arch" --timeout "$to" \
        --nsh "$nsh" --extra "$USB_IDS" "${qargs[@]}" "$efi" > "$log.raw" 2>&1 || true
    tr -d '\r' < "$log.raw" > "$log"
    if ! grep -qxF '==END==' "$log"; then
        no "$(basename "$log"): boot reached the end of startup.nsh"
        echo "--- serial (tail) ---"; tail -40 "$log"
    fi
}

ROW1='Bus 001 Device 001: ID 46f4:0001'
ROW2='Bus 001 Device 002: ID 0627:0001'
ROW3='Bus 001 Device 003: ID 0627:0001'
ROW4='Bus 001 Device 004: ID 0409:55aa'
ROW5='Bus 001 Device 005: ID 0627:0001'
TABLET='Adomax Technology Co., Ltd QEMU Tablet'

EXP_LIST="$ROW1 QEMU QEMU USB HARDDRIVE
$ROW2 $TABLET
$ROW3 $TABLET
$ROW4 NEC Corp. Hub
$ROW5 $TABLET"

EXP_NUMERIC="$ROW1
$ROW2
$ROW3
$ROW4
$ROW5"

# Tree order is axl_usb_tree_for_each's contract: ascending device path, so
# ascending port chain. OVMF's xHCI paths here are USB(0x3) storage (QEMU
# attaches usb-storage SuperSpeed, on a USB3 port, which qemu-xhci numbers
# before its USB2 ports), USB(0x4) keyboard, USB(0x5) mouse, USB(0x6) hub and
# USB(0x6)/USB(0x0) tablet. Device ordinals are assigned in that same order,
# which is why the storage is Device 001 in the flat list too.
EXP_TREE='Bus 001
  \- Device 001: ID 46f4:0001
    \- Interface 0: 080650
  \- Device 002: ID 0627:0001
    \- Interface 0: 030101
  \- Device 003: ID 0627:0001
    \- Interface 0: 030102
  \- Device 004: ID 0409:55aa
    \- Interface 0: 090000
    \- Device 005: ID 0627:0001
      \- Interface 0: 030000'

run_arch() {
    local arch="$1" native out efi log nsh
    case "$arch" in
        X64)     native="x64" ;;
        AARCH64) native="aa64" ;;
    esac
    out="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$native")"
    efi="$out/tools/lsusb.efi"

    echo "=== lsusb ($arch) ==="
    "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$native" tools 2>&1 | tail -1
    [[ -f "$efi" ]] || { no "$arch: $efi was not built"; return; }

    # --- none: no USB controller at all ---
    nsh="$TMP/none.nsh"; log="$TMP/none-$arch.log"
    write_nsh "$nsh" 'L|lsusb.efi' 'T|lsusb.efi -t'
    boot "$arch" "$efi" "$log" "$nsh"
    expect_block "$log" L "none: lsusb prints the no-controller note" \
        'lsusb: no USB host controller found'
    expect_rc "$log" L "none: lsusb exits 0x0" 0x0
    expect_block "$log" T "none: lsusb -t prints the no-controller note" \
        'lsusb: no USB host controller found'
    expect_rc "$log" T "none: lsusb -t exits 0x0" 0x0

    # --- hc-only: a controller with nothing on it ---
    nsh="$TMP/hc.nsh"; log="$TMP/hc-$arch.log"
    write_nsh "$nsh" 'L|lsusb.efi' 'T|lsusb.efi -t'
    boot "$arch" "$efi" "$log" "$nsh" -device qemu-xhci,id=ux
    expect_block "$log" L "hc-only: lsusb prints the no-device note" \
        'lsusb: no USB devices found'
    expect_rc "$log" L "hc-only: lsusb exits 0x0" 0x0
    expect_block "$log" T "hc-only: lsusb -t prints the no-device note" \
        'lsusb: no USB devices found'
    expect_rc "$log" T "hc-only: lsusb -t exits 0x0" 0x0

    # --- topology: kbd, mouse, hub + tablet, storage ---
    nsh="$TMP/topo.nsh"; log="$TMP/topo-$arch.log"
    write_nsh "$nsh" 'L|lsusb.efi' 'N|lsusb.efi -n' 'D|lsusb.efi -d 0409:55aa' \
        'S|lsusb.efi -s 1:5' 'X|lsusb.efi -d 1234:5678' 'T|lsusb.efi -t -n' \
        'V|lsusb.efi -v -s 1:1'
    boot "$arch" "$efi" "$log" "$nsh" \
        -device qemu-xhci,id=ux \
        -device usb-kbd,bus=ux.0,port=1 \
        -device usb-mouse,bus=ux.0,port=2 \
        -device usb-hub,bus=ux.0,port=3 \
        -device usb-tablet,bus=ux.0,port=3.1 \
        -drive "if=none,id=stick,format=raw,file=$STICK" \
        -device usb-storage,bus=ux.0,port=4,drive=stick,removable=on
    expect_block "$log" L "topology: lsusb lists five named devices" "$EXP_LIST"
    expect_rc "$log" L "topology: lsusb exits 0x0" 0x0
    expect_block "$log" N "topology: lsusb -n lists five numeric rows" "$EXP_NUMERIC"
    expect_block "$log" D "topology: -d 0409:55aa selects the hub only" \
        "$ROW4 NEC Corp. Hub"
    expect_block "$log" S "topology: -s 1:5 selects Device 005 only" \
        "$ROW5 $TABLET"
    expect_block "$log" X "topology: -d 1234:5678 prints nothing" ''
    expect_rc "$log" X "topology: a filter matching nothing exits 0x1" 0x1
    expect_block "$log" T "topology: -t -n nests the tablet under the hub" "$EXP_TREE"
    expect_rc "$log" T "topology: lsusb -t -n exits 0x0" 0x0

    local v
    v="$(section "$log" V)"
    [[ "$(grep -cxF '    iManufacturer  QEMU' <<< "$v" || true)" == 1 ]] \
        && ok "topology: -v -s 1:1 iManufacturer QEMU" \
        || no "topology: -v -s 1:1 iManufacturer QEMU"
    [[ "$(grep -cxF '    iProduct       QEMU USB HARDDRIVE' <<< "$v" || true)" == 1 ]] \
        && ok "topology: -v -s 1:1 iProduct QEMU USB HARDDRIVE" \
        || no "topology: -v -s 1:1 iProduct QEMU USB HARDDRIVE"
    [[ "$(grep -cE '^    iSerial        [0-9]+-0000:00:[0-9a-f]{2}\.[0-9]-[0-9.]+$' <<< "$v" || true)" == 1 ]] \
        && ok "topology: -v -s 1:1 iSerial carries the QEMU serial shape" \
        || no "topology: -v -s 1:1 iSerial carries the QEMU serial shape"
}

for arch in "${ARCHES[@]}"; do
    run_arch "$arch"
done

echo "lsusb: $pass passed, $fail failed"
if (( fail > 0 )); then
    exit 1
fi
echo "All lsusb checks passed."
exit 0
