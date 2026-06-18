#!/bin/bash
# test-meta: arch=x64 needs=socat est=18 local-only=1
# test-input-modifiers-qemu.sh — prove that a HELD keyboard modifier is
# stamped onto POINTER events through the real OVMF firmware path:
# Ctrl+click and Shift+wheel.
#
# Why this can't live in the unit suite: UEFI delivers modifier state only
# WITH a keystroke. axl-input (commit 569508ea) enables
# EFI_KEY_STATE_EXPOSED so the firmware emits a modifier-only "partial
# keystroke" when a modifier alone goes down/up, tracks that live state,
# and stamps it onto every pointer event. The serial unit harness has no
# SimpleTextInputEx, so it can deliver neither modifiers nor partial
# keystrokes — the whole chain is only exercisable against a real UEFI
# keyboard + pointer stack, which OVMF provides under QEMU.
#
# How it drives the guest (the part `sendkey` cannot do — it is an atomic
# press+release and so can never HOLD a modifier while a pointer event
# arrives):
#   * QMP `input-send-event` with `key {down:true}` HOLDS a modifier down,
#     a separate `btn`/wheel event fires while it is held, then
#     `key {down:false}` releases it. Closing the QMP connection does NOT
#     undo a held key (the state lives in QEMU's device model), so each
#     phase reconnects fresh and negotiates capabilities again.
#
# Verified mechanism (EDK2 + QEMU source):
#   * UsbKbDxe/Ps2KeyboardDxe emit a partial EFI_KEY_DATA (scan=0,uni=0)
#     for a lone modifier ONLY when EFI_KEY_STATE_EXPOSED is set
#     (MdeModulePkg/Bus/Usb/UsbKbDxe/KeyBoard.c:1678), which axl enables in
#     axl_backend_console_expose_modifiers (called from attach_key).
#   * QEMU `usb-mouse` packs the wheel into HID report byte 3
#     (hw/input/hid.c), which UsbMouseDxe surfaces as RelativeMovementZ ->
#     axl wheel_dy (UsbMouse.c:834, axl-input.c:300).
#
# Phases (assertions are exact-VALUE on the modifier bitfield):
#   0. baseline: plain click + plain wheel, NO modifier -> m=0x00000000
#      (the discriminating control: proves the modifier bits in phases 1/2
#      actually come from the held key, not a spurious default).
#   1. Ctrl+click:  hold ctrl, left click       -> BTN-DOWN m=0x00000004 (LCTRL)
#   2. Shift+wheel: hold shift, wheel-up         -> WHEEL    m=0x00000001 (LSHIFT)
#      (a lingering ctrl from phase 1 would make this 0x05 and fail, so it
#      also proves the modifier-UP partial is delivered.)
#
# The wheel MAGNITUDE/sign passes through QEMU's dz clamp + OVMF byte-3
# accumulation — layers not under test — so only `dy != 0` is asserted for
# the wheel; the modifier is the pinned value.
#
# aa64 note: AAVMF under QEMU binds no relative EFI_SIMPLE_POINTER, so the
# guest prints "PTR-UNAVAILABLE" and this test records a SKIP there. The
# modifier-stamping logic itself is unit-tested on BOTH arches (the 12 unit
# tests added with 569508ea); this integration test validates the real
# firmware path, which is realistically an x64/OVMF concern under QEMU.
#
# Usage:
#   scripts/install.sh --arch x64                 # one-time, builds axl-cc
#   test/integration/test-input-modifiers-qemu.sh            # X64 (default)
#   test/integration/test-input-modifiers-qemu.sh --arch AARCH64
#
# Requires: socat, a staged SDK at out/ (from scripts/install.sh).

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,57p' "$0"; exit 0 ;;
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
DEMO_C="$SCRIPT_DIR/input-modifiers-selftest.c"

TMP=$(mktemp -d)
EFI="$TMP/input-modifiers.efi"
SOCK="$TMP/serial.sock"
QMP="$TMP/qmp.sock"
LOG="$TMP/serial.log"
RUN_LOG="$TMP/run.out"

cleanup() {
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID"  2>/dev/null || true
    [[ -n "${QEMU_PID:-}"   ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
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

# Build the test EFI. --debug so the final leak report appears as an extra
# exit-path signal, matching test-input-keys-qemu.sh.
"$AXL_CC" --arch "$CC_ARCH" --debug "$DEMO_C" -o "$EFI" >/dev/null

# QEMU outer timeout + per-injection settle: TCG (always on aa64, and on
# x64 without KVM) needs a much larger budget than KVM-x64.
qemu_timeout=90
settle=0.6
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    qemu_timeout=240
    settle=1.0
fi

# Extra QEMU wiring: a QMP socket (for input-send-event), a relative
# usb-mouse on its own xHCI (OVMF binds UsbMouseDxe -> EFI_SIMPLE_POINTER
# with a wheel), and on aa64 a usb-kbd since `virt` ships none. NO
# usb-tablet: its absolute EFI_ABSOLUTE_POINTER steals the pointer and
# delivers nothing under OVMF (see run-qemu.sh comments).
# A usb-kbd is added on BOTH arches: QMP `input-send-event` key events
# route to the USB HID keyboard, not the x64 q35 PS/2 controller (unlike
# HMP `sendkey`), so the PS/2 default alone receives nothing here.
# One --qemu-arg per token: run-qemu.sh appends each --qemu-arg value
# verbatim (no word-splitting), so a flag + its value are two --qemu-arg's.
qemu_args=(
    --qemu-arg -qmp    --qemu-arg "unix:$QMP,server,nowait"
    --qemu-arg -device --qemu-arg "qemu-xhci,id=axl_hid_xhci"
    --qemu-arg -device --qemu-arg "usb-mouse,bus=axl_hid_xhci.0,id=axl_mouse"
    --qemu-arg -device --qemu-arg "usb-kbd,bus=axl_hid_xhci.0"
)

rm -f "$SOCK" "$QMP"
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
echo "launched QEMU pid=$QEMU_PID (arch=$ARCH, serial=$SOCK, qmp=$QMP)"

# Wait for QEMU to create the serial socket.
for _ in $(seq 1 50); do
    [[ -S "$SOCK" ]] && break
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; cat "$RUN_LOG"; exit 1; }

# Drain guest serial -> LOG (read-only; this test injects no serial bytes).
socat -u "UNIX-CONNECT:$SOCK" - | tee -a "$LOG" > /dev/null &
BRIDGE_PID=$!

# Wait for the app to publish PTR-READY or PTR-UNAVAILABLE.
ready_iters=200            # 50 s at 0.25 s/iter
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    ready_iters=600        # 150 s for TCG
fi
echo "waiting for PTR-READY / PTR-UNAVAILABLE..."
for _ in $(seq 1 "$ready_iters"); do
    grep -aq "PTR-READY\|PTR-UNAVAILABLE" "$LOG" 2>/dev/null && break
    sleep 0.25
done

if grep -aq "PTR-UNAVAILABLE" "$LOG" 2>/dev/null; then
    echo ""
    echo "=== SKIP ($ARCH): no relative EFI_SIMPLE_POINTER under this firmware ==="
    echo "  (expected on aa64/AAVMF under QEMU — the modifier-stamping logic"
    echo "   is unit-tested cross-arch; this test validates the OVMF path.)"
    exit 0
fi
if ! grep -aq "PTR-READY" "$LOG" 2>/dev/null; then
    echo "ERROR: PTR-READY never appeared; first 80 log lines:"
    sed -n '1,80p' "$LOG"
    exit 1
fi
[[ -S "$QMP" ]] || { echo "ERROR: QEMU QMP socket never appeared"; exit 1; }
echo "PTR-READY seen; injecting events"

# One QMP command per connection: send qmp_capabilities to leave
# negotiation, then the event, then close. Held key state persists across
# connection close (it lives in QEMU's device model, not the session).
qmp() {
    {
        printf '%s\n' '{"execute":"qmp_capabilities"}'
        printf '%s\n' "$1"
        sleep 0.2
    } | socat -t 2 - "UNIX-CONNECT:$QMP" >/dev/null 2>&1
}
key()   { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"; }
btn()   { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$2,\"button\":\"$1\"}}]}}"; }
click() { btn left true; btn left false; }
wheel() { btn wheel-up true; btn wheel-up false; }

# Phase 0 — baseline (no modifier held): expect m=0x00000000.
echo "  phase 0: baseline click + wheel"
click; sleep "$settle"
wheel; sleep "$settle"

# Phase 1 — Ctrl+click: hold ctrl, click, release. Expect BTN-DOWN LCTRL.
echo "  phase 1: Ctrl+click"
key ctrl true;  sleep "$settle"   # hold -> partial keystroke updates live mods
click;          sleep "$settle"
key ctrl false; sleep "$settle"   # release -> mods back to 0

# Phase 2 — Shift+wheel: hold shift, wheel, release. Expect WHEEL LSHIFT.
echo "  phase 2: Shift+wheel"
key shift true;  sleep "$settle"
wheel;           sleep "$settle"
key shift false; sleep "$settle"

# Phase 3 — NumLock toggle: tap NumLock (a sticky toggle, delivered as a
# modifier-only partial keystroke), then click. The toggle-lock state must
# surface as AXL_INPUT_MOD_NUM_LOCK on the pointer event. Tap again to
# leave NumLock off. This guards the toggle-state plumbing
# (efi_keystate_to_axl_mods) and confirms locks toggled AFTER attach are
# tracked live (the pre-attach lock state is reset at attach — a documented
# UEFI limitation; see axl_backend_console_expose_modifiers).
tap() { key "$1" true; key "$1" false; }
echo "  phase 3: NumLock toggle + click"
tap num_lock; sleep "$settle"   # toggle ON
click;        sleep "$settle"
tap num_lock; sleep "$settle"   # toggle OFF (leave clean)

# Give the guest a moment to flush the last lines.
sleep "$settle"

# Serial output carries trailing CR (\r); strip it so '$'-anchored exact
# assertions match.
STRIPPED="$TMP/serial.stripped"
tr -d '\r' < "$LOG" > "$STRIPPED"

echo ""
echo "=== observed PTR lines ==="
grep -aE "PTR-READY|PTR (BTN|WHEEL)|INPUT-(DONE|TIMEOUT)" "$STRIPPED" || true
echo ""

if grep -aq "INPUT-TIMEOUT" "$STRIPPED"; then
    echo "=== FAIL: app hit its watchdog (events not delivered) ==="
    exit 1
fi

# ------------------------------------------------------------------
# Assertions — existence of EXACT modifier-value lines. Existence (not
# position) is used because pointer streams carry incidental motion /
# extra reports; the discrimination comes from requiring BOTH the
# baseline m=0 lines AND the modified lines: a leak would make baseline
# nonzero, a broken stamp would make the modified lines m=0.
# ------------------------------------------------------------------
fail=0
need() {  # need <description> <grep-ERE>
    if grep -aqE "$2" "$STRIPPED"; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1  (no line matching /$2/)"
        fail=1
    fi
}

need "baseline click has no modifiers"  '^PTR BTN-DOWN b=0x00000001 m=0x00000000$'
need "baseline wheel has no modifiers"  '^PTR WHEEL    dy=-?[1-9][0-9]* m=0x00000000$'
need "Ctrl+click stamps LCTRL"          '^PTR BTN-DOWN b=0x00000001 m=0x00000004$'
need "Shift+wheel stamps LSHIFT"        '^PTR WHEEL    dy=-?[1-9][0-9]* m=0x00000001$'
need "NumLock toggle stamps NUM_LOCK"   '^PTR BTN-DOWN b=0x00000001 m=0x00000200$'

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "=== PASS ($ARCH): held modifiers stamped onto pointer events ==="
    echo "  Ctrl+click  -> AXL_INPUT_MOD_LCTRL on the button event"
    echo "  Shift+wheel -> AXL_INPUT_MOD_LSHIFT on the wheel event"
    exit 0
else
    echo "=== FAIL ($ARCH): modifier stamping did not match expected values ==="
    exit 1
fi
