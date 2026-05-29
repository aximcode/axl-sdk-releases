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
} MouseSource;

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
        if (!ms->cb(&ev, ms->data)) {
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
        ev.buttons      = AXL_INPUT_BUTTON_LEFT;
        ms->prev_left   = state.LeftButton;
        if (!ms->cb(&ev, ms->data)) {
            return AXL_SOURCE_REMOVE;
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
        ev.buttons      = AXL_INPUT_BUTTON_RIGHT;
        ms->prev_right  = state.RightButton;
        if (!ms->cb(&ev, ms->data)) {
            return AXL_SOURCE_REMOVE;
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
        if (!ms->cb(&ev, ms->data)) {
            return AXL_SOURCE_REMOVE;
        }
    }

    return AXL_SOURCE_CONTINUE;
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
    EFI_SIMPLE_POINTER_PROTOCOL *sp   = NULL;
    EFI_STATUS                   st   =
        axl_bs()->LocateProtocol(&guid, NULL, (void **)&sp);
    if (st != 0 || sp == NULL) {
        axl_debug("EFI_SIMPLE_POINTER_PROTOCOL not available "
                  "(headless / no mouse hardware)");
        return 0;
    }

    /* Best-effort reset — clears any stale state from prior consumers.
       Failure is non-fatal; the first GetState may return EFI_NOT_READY
       which the dispatch callback handles. */
    (void)sp->Reset(sp, false);

    mouse_state.cb         = cb;
    mouse_state.data       = data;
    mouse_state.protocol   = sp;
    mouse_state.cursor_x   = 0;
    mouse_state.cursor_y   = 0;
    mouse_state.prev_left  = false;
    mouse_state.prev_right = false;
    mouse_state_used       = true;

    return axl_loop_add_event(loop, sp->WaitForInput,
                              mouse_dispatch_cb, &mouse_state);
}

// ===================================================================
// Keyboard — thin wrapper over axl_loop_add_key_press
// ===================================================================
//
// axl_loop_add_key_press uses ConIn's WaitForKey + ReadKeyStroke, which
// returns the basic EFI_INPUT_KEY (scan_code + unicode_char) wrapped
// as AxlInputKey.  Our wrapper translates that into the unified
// AxlInputEvent shape so callers register one AxlInputCallback for
// all input kinds.
//
// Modifiers (Shift/Ctrl/Alt/Meta) and KEY_UP events require
// EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL — deferred until a consumer asks.
// v0.1 always emits KEY_DOWN with modifiers == 0.

typedef struct {
    AxlInputCallback  cb;
    void             *data;
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
    AxlInputEvent  ev = {0};
    ev.type         = AXL_INPUT_KEY_DOWN;
    ev.timestamp_us = axl_time_get_us();
    ev.keycode      = key.scan_code;
    ev.unicode      = key.unicode_char;
    ev.modifiers    = 0;  /* EX protocol required for modifier state */
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
    key_state_used     = true;
    return axl_loop_add_key_press(loop, key_dispatch_cb, &key_state);
}

// ===================================================================
// Touch — EFI_ABSOLUTE_POINTER_PROTOCOL
// ===================================================================
//
// Absolute pointer reports (CurrentX, CurrentY) in the protocol's
// native coordinate system (see EFI_ABSOLUTE_POINTER_MODE for the
// device's AbsoluteMin/Max range — callers rescale to screen).
// ActiveButtons is a bitfield: bit 0 = touch contact, higher bits
// device-specific (alt-button on stylus, etc.).
//
// Emit semantics:
//   TOUCH_DOWN — first dispatch where ActiveButtons != 0 after being 0.
//   TOUCH_UP   — transition from non-zero ActiveButtons back to 0.
//   TOUCH_MOVE — while contact is active and (CurrentX, CurrentY)
//                changes between dispatches.

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

    uint64_t now_us = axl_time_get_us();
    int32_t  cx     = (int32_t)state.CurrentX;
    int32_t  cy     = (int32_t)state.CurrentY;
    bool     active = (state.ActiveButtons != 0);

    if (active && !tch->contact_active) {
        /* Transition 0 → contact: emit TOUCH_DOWN. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_DOWN;
        ev.timestamp_us = now_us;
        ev.x            = cx;
        ev.y            = cy;
        ev.buttons      = state.ActiveButtons;
        tch->contact_active = true;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (!active && tch->contact_active) {
        /* Transition contact → 0: emit TOUCH_UP. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_UP;
        ev.timestamp_us = now_us;
        ev.x            = tch->last_x;   /* report last known position */
        ev.y            = tch->last_y;
        ev.buttons      = 0;
        tch->contact_active = false;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    if (active && (cx != tch->last_x || cy != tch->last_y)) {
        /* Contact still active, position changed: emit TOUCH_MOVE. */
        AxlInputEvent ev = {0};
        ev.type         = AXL_INPUT_TOUCH_MOVE;
        ev.timestamp_us = now_us;
        ev.x            = cx;
        ev.y            = cy;
        ev.buttons      = state.ActiveButtons;
        tch->last_x = cx;
        tch->last_y = cy;
        return tch->cb(&ev, tch->data) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
    }
    /* No state change worth reporting (e.g. inactive + position
       unchanged, or contact wobble below the device's tolerance). */
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
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap   = NULL;
    EFI_STATUS                     st   =
        axl_bs()->LocateProtocol(&guid, NULL, (void **)&ap);
    if (st != 0 || ap == NULL) {
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
