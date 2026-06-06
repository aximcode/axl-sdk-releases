#!/bin/bash
# test-input-keys-qemu.sh — empirically pin the Ctrl+letter encoding that
# axl-input delivers, across the two UEFI console-input paths.
#
# AGT (and any toolkit) needs to know how a held Ctrl arrives in an
# AxlInputEvent. There is NO single canonical form — it depends on the
# console device, and this test proves the divergence with exact-value
# assertions:
#
#   * Physical keyboard (SimpleTextInputEx — Ps2KeyboardDxe on x86,
#     UsbKeyboardDxe on aa64): Ctrl+letter is delivered as the PRINTABLE
#     LETTER in `unicode` PLUS an AXL_INPUT_MOD_CTRL bit in `modifiers`.
#     It is NOT folded to a C0 control code.
#
#   * Serial console (TerminalDxe): the terminal sends a raw C0 byte, so
#     Ctrl+letter arrives as the FOLDED C0 CONTROL CODE (Ctrl-A=0x01 …
#     Ctrl-Z=0x1A) in `unicode`, with `modifiers == 0` (serial carries
#     no shift state / has no Ex protocol).
#
# How it drives the guest:
#   1. Builds test/integration/input-keys-selftest.c via axl-cc. That EFI
#      attaches axl_input_attach_key and prints one
#      "KEY u=0x.... sc=0x.... m=0x........" line per AXL_INPUT_KEY_DOWN,
#      then "INPUT-DONE" after 5 keys (a 30 s in-app watchdog prints
#      "INPUT-TIMEOUT" and quits so the run can never hang).
#   2. Launches QEMU in the background with serial on a UNIX socket AND a
#      QEMU monitor on a second socket (for `sendkey`). On aa64 it also
#      wires a USB keyboard (the `virt` machine has none by default).
#   3. Waits for "INPUT-READY", then injects 5 keys:
#        - monitor `sendkey ctrl-a`  → keyboard path
#        - monitor `sendkey ctrl-z`  → keyboard path
#        - serial byte 0x01          → serial path (Ctrl-A)
#        - serial byte 0x1A          → serial path (Ctrl-Z)
#        - serial byte 'a'           → serial path (plain letter, sanity)
#   4. Asserts the EXACT KEY lines the guest must print, in order.
#
# NOTE: `sendkey ctrl-c` is deliberately NOT used — the UEFI shell traps
# Ctrl-C as its ExecutionBreak, which axl_loop_run honours as a quit
# (this is the path test-yield-ctrlc.sh exercises). ctrl-a / ctrl-z are
# ordinary keys, so they reach the app's input callback intact.
#
# Usage:
#   scripts/install.sh --arch x64     # one-time, produces out/bin/axl-cc
#   test/integration/test-input-keys-qemu.sh            # X64 (default)
#   test/integration/test-input-keys-qemu.sh --arch AARCH64
#
# AARCH64 additionally needs the staged aa64 SDK (scripts/install.sh
# --arch aa64) and the aarch64 cross toolchain that axl-cc --arch aa64
# uses. Requires: socat, a staged SDK at out/ (from scripts/install.sh).

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,46p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg '$1' (try --help)" >&2; exit 2 ;;
    esac
done

case "$ARCH" in
    X64)     CC_ARCH="x64" ;;
    AARCH64) CC_ARCH="aa64" ;;
    *) echo "ERROR: --arch must be X64 or AARCH64 (got '$ARCH')" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

AXL_CC="$PROJECT_DIR/out/bin/axl-cc"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
DEMO_C="$SCRIPT_DIR/input-keys-selftest.c"

TMP=$(mktemp -d)
EFI="$TMP/input-keys.efi"
SOCK="$TMP/serial.sock"
MON="$TMP/monitor.sock"
QIN="$TMP/stdin.fifo"
LOG="$TMP/serial.log"
RUN_LOG="$TMP/run.out"

cleanup() {
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID"  2>/dev/null || true
    [[ -n "${QEMU_PID:-}"   ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

# Prereqs
if ! command -v socat >/dev/null 2>&1; then
    echo "ERROR: socat is required for this test" >&2
    exit 2
fi
if [[ ! -x "$AXL_CC" ]]; then
    echo "ERROR: $AXL_CC not found; run scripts/install.sh --arch $CC_ARCH first" >&2
    exit 2
fi
if [[ ! -f "$DEMO_C" ]]; then
    echo "ERROR: selftest source missing: $DEMO_C" >&2
    exit 2
fi

# Build the test EFI. --debug so the final leak report appears as an
# extra exit-path signal, matching test-yield-ctrlc.sh.
"$AXL_CC" --arch "$CC_ARCH" --debug "$DEMO_C" -o "$EFI" >/dev/null

# QEMU outer timeout: TCG (no KVM, and always on aa64) needs a much
# larger budget than KVM-x64.
qemu_timeout=90
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    qemu_timeout=240
fi

# Per-key settle delay between injections — give the firmware time to
# process each keystroke before the next. TCG is slower.
key_delay=0.6
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    key_delay=1.0
fi

# Extra QEMU wiring: a monitor socket for `sendkey` (both arches) and,
# on aa64, a USB keyboard since the `virt` machine ships none.
qemu_args=(--qemu-arg "-monitor unix:$MON,server,nowait")
if [[ "$ARCH" == "AARCH64" ]]; then
    qemu_args+=(--qemu-arg "-device qemu-xhci,id=axl_kbd_xhci")
    qemu_args+=(--qemu-arg "-device usb-kbd,bus=axl_kbd_xhci.0")
fi

rm -f "$SOCK" "$MON"
"$RUN_QEMU" \
    --arch "$ARCH" \
    --background --timeout "$qemu_timeout" \
    --serial-socket "$SOCK" \
    "${qemu_args[@]}" \
    "$EFI" > "$RUN_LOG" 2>&1

QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$RUN_LOG" || true)
if [[ -z "$QEMU_PID" ]]; then
    echo "ERROR: could not capture QEMU pid"; cat "$RUN_LOG"; exit 1
fi
echo "launched QEMU pid=$QEMU_PID (arch=$ARCH, serial=$SOCK, monitor=$MON)"

# Wait for QEMU to create the serial socket.
for _ in $(seq 1 50); do
    [[ -S "$SOCK" ]] && break
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; cat "$RUN_LOG"; exit 1; }

mkfifo "$QIN"
# Long-lived FIFO writer on fd 9 so socat's stdin never sees EOF (same
# trick as test-yield-ctrlc.sh).
exec 9<>"$QIN"

# Single bidirectional bridge: host → guest serial (from the FIFO) and
# guest serial → LOG (tee). The serial chardev accepts ONE client, so a
# single socat must carry both directions.
socat - "UNIX-CONNECT:$SOCK" < "$QIN" | tee -a "$LOG" > /dev/null &
BRIDGE_PID=$!

# Wait for the app to publish INPUT-READY (ConIn bound, ready for keys).
ready_iters=200            # 50 s at 0.25 s/iter
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    ready_iters=600        # 150 s for TCG
fi
echo "waiting for INPUT-READY..."
for _ in $(seq 1 "$ready_iters"); do
    grep -aq "INPUT-READY" "$LOG" 2>/dev/null && break
    sleep 0.25
done
if ! grep -aq "INPUT-READY" "$LOG" 2>/dev/null; then
    echo "ERROR: INPUT-READY never appeared; first 80 log lines:"
    sed -n '1,80p' "$LOG"
    exit 1
fi
[[ -S "$MON" ]] || { echo "ERROR: QEMU monitor socket never appeared"; exit 1; }
echo "INPUT-READY seen; injecting keys"

# Keyboard path: inject via the QEMU monitor.
send_kbd() {
    echo "sendkey $1" | socat -t 2 - "UNIX-CONNECT:$MON" >/dev/null 2>&1
    echo "  monitor sendkey $1"
    sleep "$key_delay"
}
# Serial path: write a raw byte into the guest serial via the FIFO.
# %b interprets the \xNN escape AND is format-string-safe regardless of
# the byte spec.
send_serial() {
    printf '%b' "$1" > "$QIN"
    echo "  serial byte $2"
    sleep "$key_delay"
}

send_kbd    ctrl-a                  # keyboard Ctrl-A
send_kbd    ctrl-z                  # keyboard Ctrl-Z
send_serial '\x01' "0x01 (Ctrl-A)"  # serial Ctrl-A (folded C0)
send_serial '\x1a' "0x1A (Ctrl-Z)"  # serial Ctrl-Z (folded C0)
send_serial 'a'    "'a' (plain)"    # serial plain letter

# Wait for INPUT-DONE (all 5 keys observed) or INPUT-TIMEOUT.
echo "waiting for INPUT-DONE..."
for _ in $(seq 1 40); do
    grep -aq "INPUT-DONE\|INPUT-TIMEOUT" "$LOG" 2>/dev/null && break
    sleep 0.25
done

# ------------------------------------------------------------------
# Assertions — EXACT key lines, in order (CLAUDE.md bucket B). Each
# entry is the precise line the guest must have printed.
# ------------------------------------------------------------------
EXPECT=(
    "KEY u=0x0061 sc=0x0000 m=0x00000004"   # kbd Ctrl-A  → 'a' + MOD_LCTRL
    "KEY u=0x007a sc=0x0000 m=0x00000004"   # kbd Ctrl-Z  → 'z' + MOD_LCTRL
    "KEY u=0x0001 sc=0x0000 m=0x00000000"   # serial Ctrl-A → folded C0, no mods
    "KEY u=0x001a sc=0x0000 m=0x00000000"   # serial Ctrl-Z → folded C0, no mods
    "KEY u=0x0061 sc=0x0000 m=0x00000000"   # serial 'a'  → plain letter, no mods
)

echo ""
echo "=== observed KEY lines ==="
grep -aE "INPUT-READY|KEY |INPUT-DONE|INPUT-TIMEOUT" "$LOG" || true
echo ""

if grep -aq "INPUT-TIMEOUT" "$LOG"; then
    echo "=== FAIL: app hit its watchdog (not all keys delivered) ==="
    exit 1
fi

# Pull the KEY lines out in order and compare position-by-position.
mapfile -t GOT < <(grep -aoE "KEY u=0x[0-9a-f]{4} sc=0x[0-9a-f]{4} m=0x[0-9a-f]{8}" "$LOG")

fail=0
if [[ "${#GOT[@]}" -ne "${#EXPECT[@]}" ]]; then
    echo "FAIL: expected ${#EXPECT[@]} KEY lines, got ${#GOT[@]}"
    fail=1
fi
for i in "${!EXPECT[@]}"; do
    if [[ "${GOT[$i]:-<missing>}" != "${EXPECT[$i]}" ]]; then
        echo "FAIL @${i}: expected '${EXPECT[$i]}'  got '${GOT[$i]:-<missing>}'"
        fail=1
    fi
done

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): Ctrl+letter encoding verified ==="
    echo "  keyboard (Ex)  : letter + AXL_INPUT_MOD_CTRL (no C0 fold)"
    echo "  serial (TermDxe): folded C0 control code, modifiers == 0"
    exit 0
else
    echo "=== FAIL ($ARCH): encoding did not match expected exact values ==="
    exit 1
fi
