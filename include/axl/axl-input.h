/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-input.h
    Toolkit-agnostic input event types — codepoints, button + modifier
    bitfields, the unified `AxlInputEvent` discriminated union.

    Source registration follows axl-loop's existing pattern:
      - Keyboard already has `axl_loop_add_key_press` in <axl/axl-loop.h>
        (uses `AxlInputKey` — keep for legacy consumers; new code uses
        the unified `AxlInputEvent` via the wrappers below).
      - Mouse + touch are added in subsequent phases via
        `axl_input_attach_mouse` / `axl_input_attach_touch`, which
        register `EFI_SIMPLE_POINTER_PROTOCOL` /
        `EFI_ABSOLUTE_POINTER_PROTOCOL` as axl-loop sources through
        `axl_loop_add_event`.

    Per substrate discipline rule 3 (docs/AGT-Design.md): this module
    produces raw events only — no widget dispatch, no toolkit-specific
    dialect.  Toolkits translate `AxlInputEvent` into their own model
    (AGT message maps, signal/slot, etc.) at the layer above.

    @code
    static bool on_input(const AxlInputEvent *ev, void *data) {
        (void)data;
        switch (ev->type) {
        case AXL_INPUT_MOUSE_MOVE:        ui_move(ev->x, ev->y); break;
        case AXL_INPUT_MOUSE_BUTTON_DOWN: ui_click(ev->buttons); break;
        case AXL_INPUT_KEY_DOWN:          ui_key(ev->unicode); break;
        default: break;
        }
        return AXL_SOURCE_CONTINUE;
    }
    axl_input_attach_mouse(loop, on_input, NULL);
    axl_input_attach_key(loop, on_input, NULL);
    @endcode
**/

#ifndef AXL_INPUT_H
#define AXL_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===================================================================
// Event types
// ===================================================================

/// Type tag for `AxlInputEvent`.
typedef enum {
    AXL_INPUT_NONE = 0,         ///< Sentinel — not a real event
    AXL_INPUT_MOUSE_MOVE,       ///< Cursor moved to (.x, .y)
    AXL_INPUT_MOUSE_BUTTON_DOWN,///< Mouse button pressed (see .buttons)
    AXL_INPUT_MOUSE_BUTTON_UP,  ///< Mouse button released
    AXL_INPUT_MOUSE_WHEEL,      ///< Scroll wheel rotated (see .wheel_dx/dy)
    AXL_INPUT_KEY_DOWN,         ///< Key pressed (see .keycode, .unicode)
    AXL_INPUT_KEY_UP,           ///< Key released (where the platform reports it)
    AXL_INPUT_TOUCH_DOWN,       ///< Touch contact began at (.x, .y)
    AXL_INPUT_TOUCH_UP,         ///< Touch contact ended
    AXL_INPUT_TOUCH_MOVE,       ///< Touch contact moved
} AxlInputType;

// ===================================================================
// Button + modifier bitfields
// ===================================================================

#define AXL_INPUT_BUTTON_LEFT    (1u << 0)
#define AXL_INPUT_BUTTON_RIGHT   (1u << 1)
#define AXL_INPUT_BUTTON_MIDDLE  (1u << 2)

#define AXL_INPUT_MOD_SHIFT      (1u << 0)
#define AXL_INPUT_MOD_CTRL       (1u << 1)
#define AXL_INPUT_MOD_ALT        (1u << 2)
#define AXL_INPUT_MOD_META       (1u << 3)

// ===================================================================
// Event struct
// ===================================================================

/// Raw input event — unified across keyboard / mouse / touch.  Fields
/// are populated based on `.type`; unused fields are zero.  Passed by
/// `const AxlInputEvent *` into the registered callback; the pointer
/// is valid only for the duration of the call.
typedef struct {
    AxlInputType  type;            ///< Event kind (discriminator)
    uint64_t      timestamp_us;    ///< Wall-clock microseconds since boot
    int32_t       x;               ///< Cursor / touch x in pixels
    int32_t       y;               ///< Cursor / touch y in pixels
    uint32_t      buttons;         ///< Current button state (AXL_INPUT_BUTTON_*)
    int32_t       wheel_dx;        ///< Horizontal wheel delta (notch ticks)
    int32_t       wheel_dy;        ///< Vertical wheel delta
    uint32_t      keycode;         ///< Raw scan code (key events)
    uint32_t      unicode;         ///< Translated codepoint (0 if none)
    uint32_t      modifiers;       ///< Modifier state (AXL_INPUT_MOD_*)
} AxlInputEvent;

/// Callback signature for unified input events.  Return
/// `AXL_SOURCE_CONTINUE` to keep the source active or
/// `AXL_SOURCE_REMOVE` to detach (same convention as
/// `AxlLoopCallback` / `AxlKeyCallback`).
typedef bool (*AxlInputCallback)(
    const AxlInputEvent  *event,  ///< [in] event payload (valid only during call)
    void                 *data    ///< opaque caller data
);

// ===================================================================
// Source registration (axl-loop integration)
// ===================================================================
//
// These wrappers register UEFI input protocols as event sources on an
// axl-loop via the existing `axl_loop_add_event` / `axl_loop_add_key_press`
// primitives.  On dispatch they read the underlying protocol payload,
// translate it into AxlInputEvent, and call @a cb.  Returns the
// axl-loop source ID (use with axl_loop_remove_source to detach), or
// 0 on failure (loop / cb NULL, protocol not available, already attached).

/* Forward-declared to avoid pulling axl-loop.h into every input
 * consumer; toolkits that use attach_* will already include axl-loop.h. */
typedef struct AxlLoop AxlLoop;

/// Register mouse input as an event source on the loop.
///
/// Locates `EFI_SIMPLE_POINTER_PROTOCOL`, registers its `WaitForInput`
/// event with the loop, and on each dispatch reads cursor state via
/// `GetState`.  Emits `AXL_INPUT_MOUSE_MOVE` for non-zero relative
/// motion, `AXL_INPUT_MOUSE_BUTTON_DOWN`/`UP` on button-state
/// transitions (debounced internally), `AXL_INPUT_MOUSE_WHEEL` for
/// non-zero Z motion.  Multiple discrete events may fire per dispatch.
///
/// Cursor position is accumulated relative deltas starting at (0, 0);
/// callers wanting screen-bounded positions should clamp in their
/// callback.  Only one mouse source per process for v0.1.
///
/// @return source ID for axl_loop_remove_source, or 0 on failure
///         (NULL args, EFI_SIMPLE_POINTER_PROTOCOL not available,
///         or a mouse source is already attached).
uint32_t
axl_input_attach_mouse(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    );

/// Register keyboard input as an event source on the loop.
///
/// Thin wrapper over `axl_loop_add_key_press` that translates each
/// `AxlInputKey` (scan_code + unicode_char) into a unified
/// `AxlInputEvent` (`type = AXL_INPUT_KEY_DOWN`, `keycode = scan_code`,
/// `unicode = unicode_char`).  This lets callers register a single
/// `AxlInputCallback` for mouse + keyboard + (future) touch instead
/// of separate per-device callbacks.
///
/// Modifiers (shift / ctrl / alt / meta) and `AXL_INPUT_KEY_UP` events
/// require `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL` — deferred until a
/// consumer asks.  For v0.1, modifiers is always 0 and only KEY_DOWN
/// fires.
///
/// Only one keyboard source per process for v0.1.
///
/// @return source ID for axl_loop_remove_source, or 0 on failure
///         (NULL args or a keyboard source already attached).
uint32_t
axl_input_attach_key(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    );

/// Register touch input as an event source on the loop.
///
/// Locates `EFI_ABSOLUTE_POINTER_PROTOCOL`, registers its
/// `WaitForInput` event with the loop, and on each dispatch reads
/// absolute position via `GetState`.  Emits `AXL_INPUT_TOUCH_DOWN`
/// on first contact (ActiveButtons transition 0 → non-zero),
/// `AXL_INPUT_TOUCH_UP` on contact end (non-zero → 0),
/// `AXL_INPUT_TOUCH_MOVE` while contact is active and position
/// changes.
///
/// Position is reported in the protocol's `(CurrentX, CurrentY)`
/// range — see `EFI_ABSOLUTE_POINTER_MODE`'s AbsoluteMin/Max for
/// the device's native coordinate system.  Callers wanting screen
/// pixels should rescale in their callback.
///
/// Only one touch source per process for v0.1.
///
/// @return source ID for axl_loop_remove_source, or 0 on failure
///         (NULL args, EFI_ABSOLUTE_POINTER_PROTOCOL not available,
///         or a touch source already attached).
uint32_t
axl_input_attach_touch(
    AxlLoop           *loop,
    AxlInputCallback   cb,
    void              *data
    );

#ifdef __cplusplus
}
#endif

#endif /* AXL_INPUT_H */
