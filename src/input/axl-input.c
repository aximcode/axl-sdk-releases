/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-input.c
    axl-input — source registration for UEFI input protocols.

    Mouse via `EFI_SIMPLE_POINTER_PROTOCOL` (Phase 0h, this file).
    Keyboard via thin wrapper over `axl_loop_add_key_press` (Phase 0i).
    Touch via `EFI_ABSOLUTE_POINTER_PROTOCOL` (deferred until consumer).

    Each `axl_input_attach_*` wrapper registers the underlying UEFI
    protocol's WaitForInput event with the existing
    `axl_loop_add_event` / `axl_loop_add_key_press` primitive.  No
    parallel queue — axl-loop already buffers events at the UEFI
    layer via WaitForInput.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-input.h>
#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-time.h>

AXL_LOG_DOMAIN("input");

// ===================================================================
// Keyboard chord helpers (pure — no protocol I/O)
// ===================================================================

char
axl_input_ctrl_letter(uint32_t unicode, uint32_t modifiers)
{
    // Serial console (TerminalDxe): Ctrl+letter arrives folded to a C0
    // control code (Ctrl+A = 0x01 … Ctrl+Z = 0x1A) with modifiers == 0.
    // Exclude the four C0 codes that are real editing keys in their own
    // right — Ctrl+H / I / J / M == Backspace / Tab / LF / CR.
    if (unicode >= 1 && unicode <= 26
        && unicode != 0x08 && unicode != 0x09
        && unicode != 0x0A && unicode != 0x0D) {
        return (char)('a' + (int)unicode - 1);
    }
    // Physical keyboard (Simple Text Input Ex): Ctrl+letter arrives as
    // the printable letter + AXL_INPUT_MOD_CTRL (not folded).  Case-fold
    // so Ctrl+A and Ctrl+Shift+A both resolve to 'a'.
    if ((modifiers & AXL_INPUT_MOD_CTRL) && unicode != 0) {
        uint32_t u = unicode;
        if (u >= 'A' && u <= 'Z') u += 0x20;
        if (u >= 'a' && u <= 'z') return (char)u;
    }
    return 0;
}

// ===================================================================
// Live keyboard modifier state — stamped onto pointer events
// ===================================================================
//
// UEFI delivers keyboard modifier state only WITH a keystroke, so to make
// Shift+wheel / Ctrl+click work the substrate keeps a live modifier state,
// updated from every keystroke — including the modifier-only "partial"
// keystrokes that EFI_KEY_STATE_EXPOSED delivers on shift/ctrl down+up
// (enabled in the backend), which keep it current between character keys.
// Pointer events (mouse + touch) are stamped with this state. It is 0 until
// a keyboard source is attached AND the firmware reports modifiers (no
// SimpleTextInputEx — e.g. a serial console — leaves it 0).

static uint32_t g_kbd_modifiers = 0;

// Track the live modifier state from a keystroke @p key. Returns true if the
// keystroke carries a character / scan code to deliver, false for a
// modifier-only "partial" keystroke (the state was updated; nothing to
// deliver). Pure aside from the module-global update; non-static (no public
// header) so the unit test can drive it.
bool
axl_input_track_modifiers(const AxlInputKey *key)
{
    if (key == NULL) {
        return true;   // never silently swallow input
    }
    g_kbd_modifiers = key->modifiers;
    return !(key->scan_code == 0 && key->unicode_char == 0);
}

// The current live keyboard modifier state (for pointer-event stamping and
// the unit test). Non-static (no public header).
uint32_t
axl_input_live_modifiers(void)
{
    return g_kbd_modifiers;
}

// ===================================================================
// Mouse — EFI_SIMPLE_POINTER_PROTOCOL
// ===================================================================
//
// State tracked per attach:
//   - accumulated cursor position (X, Y) starting at (0, 0)
//   - previous button state (so we can debounce DOWN/UP transitions)
//
// One mouse source per process for v0.1.  Multiple mice / hotplug
// could be added later by allocating MouseSource per attach call
// and supporting axl_loop_remove_source notification.
//
// Spec-typo note: `EFI_SIMPLE_POINTER_PROTOCOL.Mode` is documented
// in UEFI spec text as `EFI_SIMPLE_POINTER_MODE *`, but the struct
// definition in the spec HTML mistakenly uses `EFI_SIMPLE_INPUT_MODE`
// — a name that doesn't exist.  Our generated header falls back to
// `void *` because of that typo.  We cast at use site below.

typedef struct {
    AxlInputCallback              cb;
    void                         *data;
    EFI_SIMPLE_POINTER_PROTOCOL  *protocol;
    int32_t                       cursor_x;
    int32_t                       cursor_y;
    bool                          prev_left;
    bool                          prev_right;
    AxlGesture                    gesture;       ///< click-count / drag recognizer
    AxlLoop                      *loop;          ///< loop (for repeat timers + detach)
    uint32_t                      source_id;     ///< WaitForInput loop source (for detach)
    uint32_t                      repeat_src;    ///< active held-button repeat timer (0 = none)
    uint32_t                      repeat_button; ///< AXL_INPUT_BUTTON_* being repeated
} MouseSource;

// Run the gesture recognizer over @p ev (annotating click_count /
// dragging), then deliver it to the consumer callback.
static bool
mouse_emit(MouseSource *ms, AxlInputEvent *ev)
{
    ev->modifiers = g_kbd_modifiers;   // live keyboard modifiers (Shift+wheel, Ctrl+click)
    axl_input_gesture_feed(&ms->gesture, ev);
    return ms->cb(ev, ms->data);
}

// Held-pointer-button auto-repeat. Firmware never repeats a held mouse
// button (UsbMouseDxe has no repeat), so the substrate synthesizes it on
// a loop timer while the button stays down — for scrollbar arrows,
// spinners, press-and-hold.
static uint32_t g_repeat_delay_ms    = 0;    // 0 = disabled (default)
static uint32_t g_repeat_interval_ms = 50;

void
axl_input_set_button_repeat(uint32_t delay_ms, uint32_t interval_ms)
{
    g_repeat_delay_ms    = delay_ms;
    g_repeat_interval_ms = (interval_ms == 0) ? 50 : interval_ms;
}

// Deliver one synthetic held-button repeat. Bypasses mouse_emit (and so
// the gesture recognizer) on purpose — a repeat must never be counted as
// a click. Returns the consumer's keep/remove decision.
static bool
emit_repeat(MouseSource *ms)
{
    AxlInputEvent ev = {0};
    ev.type         = AXL_INPUT_MOUSE_BUTTON_DOWN;
    ev.timestamp_us = axl_time_get_us();
    ev.x            = ms->cursor_x;
    ev.y            = ms->cursor_y;
    ev.buttons      = ms->repeat_button;
    ev.modifiers    = g_kbd_modifiers;
    ev.dragging     = ms->gesture.dragging;
    ev.repeat       = true;
    return ms->cb(&ev, ms->data);
}

static bool
repeat_tick_cb(void *data)   // periodic, interval phase
{
    MouseSource *ms = (MouseSource *)data;
    if (!emit_repeat(ms)) {
        ms->repeat_src = 0;
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static bool
repeat_initial_cb(void *data)   // one-shot, fires after the initial delay
{
    MouseSource *ms = (MouseSource *)data;
    bool keep = emit_repeat(ms);
    // This one-shot auto-removes; hand off to the periodic interval phase.
    ms->repeat_src = keep
        ? axl_loop_add_timer(ms->loop, g_repeat_interval_ms, repeat_tick_cb, ms)
        : 0;
    return AXL_SOURCE_REMOVE;
}

static void
arm_repeat(MouseSource *ms, uint32_t button)
{
    if (g_repeat_delay_ms != 0 && ms->repeat_src == 0 && ms->loop != NULL) {
        ms->repeat_button = button;
        ms->repeat_src    = axl_loop_add_timeout(ms->loop, g_repeat_delay_ms,
                                                 repeat_initial_cb, ms);
    }
}

static void
disarm_repeat(MouseSource *ms)
{
    if (ms->repeat_src != 0) {
        axl_loop_remove_source(ms->loop, ms->repeat_src);
        ms->repeat_src = 0;
    }
}

static MouseSource mouse_state;
static bool        mouse_state_used = false;

static bool
mouse_dispatch_cb(
    void  *data
    )
{
    MouseSource              *ms = (MouseSource *)data;
    EFI_SIMPLE_POINTER_STATE  state;

    EFI_STATUS st = ms->protocol->GetState(ms->protocol, &state);
    if (st != 0) {
        /* No new data ready — keep source alive for next dispatch. */
        return AXL_SOURCE_CONTINUE;
    }

    /* Accumulate cursor position from relative motion.  GetState
       returns deltas in protocol counts; for v0.1 we treat 1 count =
       1 pixel (no Mode scaling — the spec-typo workaround would cost
       a cast and most real input drivers report ~1 count per pixel
       at default sensitivity anyway).  Callers can clamp to screen
       bounds in their callback. */
    ms->cursor_x += state.RelativeMovementX;
    ms->cursor_y += state.RelativeMovementY;

    uint64_t      ts      = axl_time_get_us();
    uint32_t      buttons = (state.LeftButton  ? AXL_INPUT_BUTTON_LEFT  : 0u)
                          | (state.RightButton ? AXL_INPUT_BUTTON_RIGHT : 0u);

    /* MOUSE_MOVE on non-zero relative motion. */
    if (state.RelativeMovementX != 0 || state.RelativeMovementY != 0) {
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_MOUSE_MOVE;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;
        if (!mouse_emit(ms, &ev)) {
            return AXL_SOURCE_REMOVE;
        }
    }

    /* BUTTON_DOWN / BUTTON_UP on state transitions (debounced). */
    if (state.LeftButton != ms->prev_left) {
        AxlInputEvent ev = {0};
        ev.type         = state.LeftButton
                          ? AXL_INPUT_MOUSE_BUTTON_DOWN
                          : AXL_INPUT_MOUSE_BUTTON_UP;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;   // CURRENT button state (post-edge), per the
                                     // AxlInputEvent contract — NOT just the
                                     // changed bit.  A consumer that derives the
                                     // change (the compositor seat: ptr_buttons ^
                                     // buttons) needs the full mask or a release
                                     // (LEFT^LEFT==0) dispatches nothing.
        ms->prev_left   = state.LeftButton;
        if (!mouse_emit(ms, &ev)) {
            disarm_repeat(ms);
            return AXL_SOURCE_REMOVE;
        }
        if (state.LeftButton) {
            arm_repeat(ms, AXL_INPUT_BUTTON_LEFT);
        } else {
            disarm_repeat(ms);
        }
    }
    if (state.RightButton != ms->prev_right) {
        AxlInputEvent ev = {0};
        ev.type         = state.RightButton
                          ? AXL_INPUT_MOUSE_BUTTON_DOWN
                          : AXL_INPUT_MOUSE_BUTTON_UP;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.buttons      = buttons;   // current state (post-edge) — see the LEFT note
        ms->prev_right  = state.RightButton;
        if (!mouse_emit(ms, &ev)) {
            disarm_repeat(ms);
            return AXL_SOURCE_REMOVE;
        }
        if (state.RightButton) {
            arm_repeat(ms, AXL_INPUT_BUTTON_RIGHT);
        } else {
            disarm_repeat(ms);
        }
    }

    /* MOUSE_WHEEL on non-zero Z motion. */
    if (state.RelativeMovementZ != 0) {
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_MOUSE_WHEEL;
        ev.timestamp_us = ts;
        ev.x            = ms->cursor_x;
        ev.y            = ms->cursor_y;
        ev.wheel_dy     = state.RelativeMovementZ;
        if (!mouse_emit(ms, &ev)) {
            return AXL_SOURCE_REMOVE;
        }
    }

    return AXL_SOURCE_CONTINUE;
}

// Prefer a PHYSICAL pointer device over the ConSplitter console
// aggregator. LocateProtocol returns the FIRST handle publishing @p guid,
// which is ConSplitterDxe's aggregator installed early on
// gST->ConsoleInHandle (alongside SimpleTextIn/InputEx) — and that
// aggregator carries no backing device when the firmware never wired the
// pointer into ConIn (common on OVMF and real platforms). The physical
// device's own protocol instance is installed later, during BDS
// ConnectAll, so it is shadowed; WaitForInput on the aggregator then
// never signals and no pointer input reaches the consumer.
//
// Enumerate every handle publishing @p guid and take the first that is
// NOT the aggregator (the physical device). Fall back to the aggregator,
// then to LocateProtocol, when no separate physical handle exists.
//
// v0.1 attaches a single source; if several physical pointers exist
// (USB mouse + PS/2) this takes the first enumerated — acceptable for now.
//
// Non-static (no public header) so the input regression test can call it
// directly: a dispatch-through-the-loop check can't tell which device was
// bound when the QEMU platform also exposes a real pointer, but a direct
// call can assert the returned interface is never the aggregator's.
void *
axl_input_locate_physical_pointer(EFI_GUID *guid)
{
    EFI_HANDLE *handles = NULL;
    uint64_t    count   = 0;
    void       *iface   = NULL;

    if (axl_bs()->LocateHandleBuffer(ByProtocol, guid, NULL, &count, &handles) == 0
        && handles != NULL) {
        for (uint64_t i = 0; i < count; i++) {
            if (handles[i] == axl_st()->ConsoleInHandle) {
                continue;   /* skip the console aggregator */
            }
            void *cand = NULL;
            if (axl_bs()->HandleProtocol(handles[i], guid, &cand) == 0 && cand != NULL) {
                iface = cand;   /* first physical device wins */
                break;
            }
        }
        if (iface == NULL && count > 0) {
            /* No separate physical handle — keep the old behaviour and
               use the first (the aggregator). */
            (void)axl_bs()->HandleProtocol(handles[0], guid, &iface);
        }
        axl_bs()->FreePool(handles);
    }
    if (iface == NULL) {
        /* LocateHandleBuffer failed outright — last resort. */
        (void)axl_bs()->LocateProtocol(guid, NULL, &iface);
    }
    return iface;
}

uint32_t
axl_input_attach_mouse(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (mouse_state_used) {
        axl_warning("axl_input_attach_mouse: already attached "
                    "(only one mouse source per process for v0.1)");
        return 0;
    }

    EFI_GUID                     guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *sp   =
        (EFI_SIMPLE_POINTER_PROTOCOL *)axl_input_locate_physical_pointer(&guid);
    if (sp == NULL) {
        axl_debug("EFI_SIMPLE_POINTER_PROTOCOL not available "
                  "(headless / no mouse hardware)");
        return 0;
    }

    /* Best-effort reset — clears any stale state from prior consumers.
       Failure is non-fatal; the first GetState may return EFI_NOT_READY
       which the dispatch callback handles. */
    (void)sp->Reset(sp, false);

    mouse_state.cb            = cb;
    mouse_state.data          = data;
    mouse_state.protocol      = sp;
    mouse_state.cursor_x      = 0;
    mouse_state.cursor_y      = 0;
    mouse_state.prev_left     = false;
    mouse_state.prev_right    = false;
    mouse_state.gesture       = (AxlGesture){0};
    mouse_state.loop          = loop;
    mouse_state.repeat_src    = 0;
    mouse_state.repeat_button = 0;
    mouse_state_used          = true;

    mouse_state.source_id = axl_loop_add_event(loop, sp->WaitForInput,
                                               mouse_dispatch_cb, &mouse_state);
    return mouse_state.source_id;
}

void
axl_input_detach_mouse(AxlLoop *loop)
{
    if (!mouse_state_used) {
        return;
    }
    disarm_repeat(&mouse_state);
    if (mouse_state.source_id != 0) {
        axl_loop_remove_source(loop, mouse_state.source_id);
        mouse_state.source_id = 0;
    }
    mouse_state_used = false;
}

// ===================================================================
// Keyboard — thin wrapper over axl_loop_add_key_press
// ===================================================================
//
// axl_loop_add_key_press reads ConIn via SimpleTextInputEx when the
// firmware publishes it (falling back to the basic ReadKeyStroke), and
// hands us an AxlInputKey with scan_code + unicode_char + normalized
// modifiers.  Our wrapper translates that into the unified
// AxlInputEvent shape so callers register one AxlInputCallback for
// all input kinds.
//
// modifiers (Shift/Ctrl/Alt/Meta + caps/num/scroll lock) arrive
// populated from the Ex protocol's KeyShiftState/KeyToggleState, and
// are 0 when there is no Ex protocol (e.g. a serial console). KEY_UP
// is not emitted — UEFI delivers no key-up events. See the Ctrl+letter
// encoding note in <axl/axl-input.h> (keyboard: letter + MOD_CTRL;
// serial: folded C0 control code).

typedef struct {
    AxlInputCallback  cb;
    void             *data;
    AxlKeyDebounce    debounce;   ///< repeat-suppression state (opt-in)
    uint32_t          source_id;  ///< loop source id, for axl_input_detach_key
} KeySource;

static KeySource key_state;
static bool      key_state_used = false;

static bool
key_dispatch_cb(
    AxlInputKey  key,
    void        *data
    )
{
    KeySource     *ks = (KeySource *)data;
    /* Update the live modifier state from every keystroke. A modifier-only
       "partial" keystroke (EFI_KEY_STATE_EXPOSED) has both scan code and
       unicode == 0, so it only refreshes the state — don't deliver a
       KEY_DOWN for it (it would look like a phantom no-char key). */
    if (!axl_input_track_modifiers(&key)) {
        return AXL_SOURCE_CONTINUE;
    }
    AxlInputEvent  ev = {0};
    ev.type         = AXL_INPUT_KEY_DOWN;
    ev.timestamp_us = axl_time_get_us();
    ev.keycode      = key.scan_code;
    ev.unicode      = key.unicode_char;
    ev.modifiers    = key.modifiers;  /* AXL_INPUT_MOD_* (0 if no ConIn-Ex) */
    /* Drop too-fast same-key repeats when debounce is enabled (remote
       consoles); keep the source alive. No-op when disabled (default). */
    if (!axl_input_key_accept(&ks->debounce, &ev)) {
        return AXL_SOURCE_CONTINUE;
    }
    return ks->cb(&ev, ks->data);
}

uint32_t
axl_input_attach_key(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (key_state_used) {
        axl_warning("axl_input_attach_key: already attached "
                    "(only one keyboard source per process for v0.1)");
        return 0;
    }
    key_state.cb       = cb;
    key_state.data     = data;
    key_state.debounce = (AxlKeyDebounce){0};
    key_state_used     = true;
    g_kbd_modifiers    = 0;   /* fresh keyboard session — no held modifiers yet */
    /* Ask the firmware for modifier-only partial keystrokes so the live
       modifier state stays current between character keys (Shift+wheel,
       Ctrl+click). Best-effort; no-op without an Ex protocol. */
    axl_backend_console_expose_modifiers();
    key_state.source_id = axl_loop_add_key_press(loop, key_dispatch_cb,
                                                 &key_state);
    return key_state.source_id;
}

void
axl_input_detach_key(AxlLoop *loop)
{
    if (!key_state_used) {
        return;
    }
    if (key_state.source_id != 0) {
        axl_loop_remove_source(loop, key_state.source_id);
        key_state.source_id = 0;
    }
    key_state_used = false;
}

// ===================================================================
// Touch — EFI_ABSOLUTE_POINTER_PROTOCOL
// ===================================================================
//
// Absolute pointer reports (CurrentX, CurrentY) in the device's native
// EFI_ABSOLUTE_POINTER_MODE AbsoluteMin/Max range; we normalize each event
// to [0, AXL_INPUT_ABS_RANGE) (axl_input_abs_normalize) so the value is
// display-independent — the consumer maps it onto its own surface (axl-input
// stays a sibling of axl-gfx, never learning the screen resolution).
// ActiveButtons is a bitfield: bit 0 = touch contact, higher bits
// device-specific (alt-button on stylus, etc.).
//
// Emit semantics:
//   TOUCH_DOWN — first dispatch where ActiveButtons != 0 after being 0.
//   TOUCH_UP   — transition from non-zero ActiveButtons back to 0.
//   TOUCH_MOVE — whenever (CurrentX, CurrentY) changes, INCLUDING with no
//                button held (hover): a pen / tablet / VNC-tablet reports
//                position continuously, so it drives a pointer.  `buttons`
//                carries the live ActiveButtons (0 = hover, non-zero = drag).

typedef struct {
    AxlInputCallback                cb;
    void                           *data;
    EFI_ABSOLUTE_POINTER_PROTOCOL  *protocol;
    int32_t                         last_x;
    int32_t                         last_y;
    bool                            contact_active;
} TouchSource;

static TouchSource touch_state;
static bool        touch_state_used = false;

// Map a native absolute coord into [0, AXL_INPUT_ABS_RANGE) using the
// device's reported min/max.  Keeps axl-input display-agnostic (no GOP /
// axl-gfx dependency) — the consumer maps the normalized value onto its
// own surface.  A degenerate range (max <= min) yields 0.
int32_t
axl_input_abs_normalize(int64_t v, uint64_t lo, uint64_t hi)
{
    if (hi <= lo) {
        return 0;
    }
    if (v < (int64_t)lo) v = (int64_t)lo;
    if (v > (int64_t)hi) v = (int64_t)hi;
    return (int32_t)(((uint64_t)(v - (int64_t)lo) * (AXL_INPUT_ABS_RANGE - 1u))
                     / (hi - lo));
}

static bool
touch_dispatch_cb(
    void  *data
    )
{
    TouchSource                *tch = (TouchSource *)data;
    EFI_ABSOLUTE_POINTER_STATE  state;

    EFI_STATUS st = tch->protocol->GetState(tch->protocol, &state);
    if (st != 0) {
        return AXL_SOURCE_CONTINUE;
    }

    EFI_ABSOLUTE_POINTER_MODE *mode = tch->protocol->Mode;
    if (mode == NULL) {
        return AXL_SOURCE_CONTINUE;   /* spec requires Mode; be defensive */
    }

    uint64_t now_us = axl_time_get_us();
    int32_t  cx     = (int32_t)state.CurrentX;   /* native device coords */
    int32_t  cy     = (int32_t)state.CurrentY;
    bool     active = (state.ActiveButtons != 0);
    /* Normalized [0, AXL_INPUT_ABS_RANGE) coords for the event payload;
       last_x/last_y stay NATIVE for change detection. */
    int32_t  nx = axl_input_abs_normalize(cx, mode->AbsoluteMinX, mode->AbsoluteMaxX);
    int32_t  ny = axl_input_abs_normalize(cy, mode->AbsoluteMinY, mode->AbsoluteMaxY);

    if (active && !tch->contact_active) {
        /* Transition 0 → contact: emit TOUCH_DOWN. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_DOWN;
        ev.timestamp_us = now_us;
        ev.x            = nx;
        ev.y            = ny;
        ev.buttons      = state.ActiveButtons;
        ev.modifiers    = g_kbd_modifiers;
        tch->contact_active = true;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (!active && tch->contact_active) {
        /* Transition contact → 0: emit TOUCH_UP at the last position. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_UP;
        ev.timestamp_us = now_us;
        ev.x            = axl_input_abs_normalize(tch->last_x, mode->AbsoluteMinX, mode->AbsoluteMaxX);
        ev.y            = axl_input_abs_normalize(tch->last_y, mode->AbsoluteMinY, mode->AbsoluteMaxY);
        ev.buttons      = 0;
        ev.modifiers    = g_kbd_modifiers;
        tch->contact_active = false;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (cx != tch->last_x || cy != tch->last_y) {
        /* Position changed — a contact drag OR a no-button hover (pen /
           tablet / VNC tablet reports position continuously).  Emit
           TOUCH_MOVE either way so an absolute pointer drives a cursor;
           buttons carries the live ActiveButtons (0 = hover). */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_MOVE;
        ev.timestamp_us = now_us;
        ev.x            = nx;
        ev.y            = ny;
        ev.buttons      = state.ActiveButtons;
        ev.modifiers    = g_kbd_modifiers;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    /* No state change worth reporting (position unchanged, no contact
       transition). */
    return AXL_SOURCE_CONTINUE;
}

uint32_t
axl_input_attach_touch(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    )
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }
    if (touch_state_used) {
        axl_warning("axl_input_attach_touch: already attached "
                    "(only one touch source per process for v0.1)");
        return 0;
    }

    EFI_GUID                       guid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap   =
        (EFI_ABSOLUTE_POINTER_PROTOCOL *)axl_input_locate_physical_pointer(&guid);
    if (ap == NULL) {
        axl_debug("EFI_ABSOLUTE_POINTER_PROTOCOL not available "
                  "(no touch / digitizer hardware)");
        return 0;
    }

    (void)ap->Reset(ap, false);  /* best-effort */

    touch_state.cb             = cb;
    touch_state.data           = data;
    touch_state.protocol       = ap;
    touch_state.last_x         = 0;
    touch_state.last_y         = 0;
    touch_state.contact_active = false;
    touch_state_used           = true;

    return axl_loop_add_event(loop, ap->WaitForInput,
                              touch_dispatch_cb, &touch_state);
}
