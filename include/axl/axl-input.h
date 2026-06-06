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
    AXL_INPUT_KEY_DOWN,         ///< Key pressed (see .keycode, .unicode, .modifiers; Ctrl+letter encoding below)
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

// Keyboard modifier / lock state, reported in AxlInputEvent.modifiers
// on KEY_DOWN events. Left/right variants are distinct bits; the
// unmodified names (SHIFT/CTRL/ALT/META) are masks matching EITHER
// side, so `mods & AXL_INPUT_MOD_SHIFT` works regardless of which
// shift was held. META is the "logo" key (Windows / Command).
//
// modifiers is 0 when nothing is held OR when the platform can't
// report modifier state (no EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL — e.g.
// over a serial console) — treat absent modifiers as "none". Reported
// only on KEY_DOWN; UEFI delivers no key-up / standalone-modifier
// events.
#define AXL_INPUT_MOD_LSHIFT      (1u << 0)
#define AXL_INPUT_MOD_RSHIFT      (1u << 1)
#define AXL_INPUT_MOD_LCTRL       (1u << 2)
#define AXL_INPUT_MOD_RCTRL       (1u << 3)
#define AXL_INPUT_MOD_LALT        (1u << 4)
#define AXL_INPUT_MOD_RALT        (1u << 5)
#define AXL_INPUT_MOD_LMETA       (1u << 6)
#define AXL_INPUT_MOD_RMETA       (1u << 7)

// Toggle-lock state (active/inactive), from the keyboard's
// KeyToggleState. Like the held modifiers, 0 when unavailable.
// NOTE: attaching a keyboard source resets the caps/num/scroll-lock
// state (UEFI has no GetState to preserve it — see
// axl_backend_console_expose_modifiers). Lock changes made AFTER attach
// are tracked; the pre-attach lock state is not recoverable.
#define AXL_INPUT_MOD_CAPS_LOCK   (1u << 8)
#define AXL_INPUT_MOD_NUM_LOCK    (1u << 9)
#define AXL_INPUT_MOD_SCROLL_LOCK (1u << 10)

// Side-agnostic masks — match either the left or right key.
#define AXL_INPUT_MOD_SHIFT  (AXL_INPUT_MOD_LSHIFT | AXL_INPUT_MOD_RSHIFT)
#define AXL_INPUT_MOD_CTRL   (AXL_INPUT_MOD_LCTRL  | AXL_INPUT_MOD_RCTRL)
#define AXL_INPUT_MOD_ALT    (AXL_INPUT_MOD_LALT   | AXL_INPUT_MOD_RALT)
#define AXL_INPUT_MOD_META   (AXL_INPUT_MOD_LMETA  | AXL_INPUT_MOD_RMETA)

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
    uint32_t      unicode;         ///< Translated codepoint (0 if none) — see Ctrl+letter note below
    uint32_t      modifiers;       ///< Modifier state (AXL_INPUT_MOD_*)
    uint32_t      click_count;     ///< Mouse-button events: 1/2/3 for single/double/triple click within the multi-click window+threshold; 0 otherwise
    bool          dragging;        ///< Mouse events: true once a held-button press moves past the drag threshold, until release
    bool          repeat;          ///< Mouse-button events: true if this is a synthetic held-button auto-repeat, not a fresh press
} AxlInputEvent;

// ===================================================================
// Click gestures + held-button auto-repeat
//
// axl_input_attach_mouse runs a built-in recognizer that annotates each
// event's `click_count` / `dragging` and, when enabled, synthesizes
// held-button auto-repeat. Tunables are global (one pointer per process
// for v0.1). Keyboard repeat is NOT here: UEFI delivers no key-up, so
// repeat-while-held can't be synthesized — held-key repeat comes from
// firmware typematic as repeated KEY_DOWN events.
// ===================================================================

/// Click-gesture recognizer state. A plain value type — zero-initialize
/// (`AxlGesture g = {0};`) before the first feed. One instance per
/// pointer stream. axl_input_attach_mouse keeps one internally; this is
/// exposed so a consumer with its own event stream (or a unit test) can
/// run the same recognition.
typedef struct {
    uint64_t  last_down_us;   ///< timestamp of the last BUTTON_DOWN
    int32_t   last_down_x;    ///< x of the last BUTTON_DOWN
    int32_t   last_down_y;    ///< y of the last BUTTON_DOWN
    uint32_t  streak;         ///< current consecutive-click count
    uint32_t  held;           ///< buttons currently held (AXL_INPUT_BUTTON_*)
    int32_t   press_x;        ///< x where the current press began
    int32_t   press_y;        ///< y where the current press began
    bool      dragging;       ///< drag latched for the current press
} AxlGesture;

/// Feed one event through the recognizer, annotating `ev->click_count`
/// and `ev->dragging` in place and updating @p g. Pure: it reads only
/// `ev->type` / `timestamp_us` / `x` / `y` / `buttons`, so synthetic
/// events drive it deterministically in tests. Uses the global tuning
/// (axl_input_set_click_tuning).
void
axl_input_gesture_feed(
    AxlGesture     *g,    ///< recognizer state (zero-initialized before first call)
    AxlInputEvent  *ev    ///< [in,out] event to annotate
);

/// Set the multi-click window and drag threshold the recognizer uses.
/// @p multi_click_ms is the largest gap between successive BUTTON_DOWNs
/// that still counts as a double/triple click; @p drag_threshold_px is
/// how far the pointer must move under a held button before `dragging`
/// latches. Pass 0 for either to restore its default (400 ms / 4 px).
void
axl_input_set_click_tuning(
    uint32_t  multi_click_ms,    ///< max inter-click gap in ms (0 = default 400)
    int32_t   drag_threshold_px  ///< drag-latch distance in px (0 = default 4)
);

/// Enable held-pointer-button auto-repeat for axl_input_attach_mouse.
/// While a button stays held, a synthetic MOUSE_BUTTON_DOWN (with
/// `repeat == true`) is emitted after @p delay_ms, then every
/// @p interval_ms — for scrollbar arrows, spinners, press-and-hold.
/// @p delay_ms == 0 disables repeat (the default).
void
axl_input_set_button_repeat(
    uint32_t  delay_ms,     ///< delay before the first repeat in ms (0 = disabled)
    uint32_t  interval_ms   ///< gap between repeats in ms
);

// ===================================================================
// Keyboard debounce / repeat suppression
//
// Over a high-latency remote console (iDRAC Virtual Console, IPMI SOL),
// a single intended keypress can register as held long enough that the
// firmware's typematic fires, so one character arrives several times.
// UEFI offers no way to turn firmware typematic off, so the fix is a
// software filter: drop a same-key KEY_DOWN that repeats faster than a
// human would. Off by default; a consumer enables it for text entry.
// ===================================================================

/// Key-debounce recognizer state. Zero-initialize (`AxlKeyDebounce d =
/// {0};`) before the first call; one per key stream. axl_input_attach_key
/// keeps one internally; exposed for reuse / unit testing.
typedef struct {
    uint32_t  last_keycode;   ///< keycode of the last key seen
    uint32_t  last_unicode;   ///< unicode of the last key seen
    uint64_t  last_us;        ///< timestamp of the last key seen
} AxlKeyDebounce;

/// Decide whether to deliver @p ev (a KEY_DOWN). Returns true to deliver,
/// false to drop it as a too-fast same-key repeat. Pure — reads only
/// `ev->type` / `keycode` / `unicode` / `timestamp_us` and updates @p d —
/// so synthetic events drive it in tests. Non-KEY_DOWN events always
/// return true. Uses the global tuning (axl_input_set_key_debounce).
bool
axl_input_key_accept(
    AxlKeyDebounce  *d,   ///< state (zero-initialized before first call)
    AxlInputEvent   *ev   ///< [in] event to test (not modified)
);

/// Configure keyboard debounce. @p min_repeat_ms is the minimum gap below
/// which a repeat of the *same* key is dropped; 0 disables (the default).
/// When @p printable_only is true, the filter applies only to printable
/// characters (unicode >= 0x20) — navigation/editing keys (arrows,
/// Backspace, Delete, Page/Home/End, whose unicode is 0 or a control
/// code) keep their firmware repeat, so held-key navigation still works.
/// Recommended starting point: ~40 ms (above the ~20 ms USB typematic
/// rate, below the ~60 ms human same-key minimum); tune from a capture.
void
axl_input_set_key_debounce(
    uint32_t  min_repeat_ms,  ///< min same-key gap in ms (0 = disabled)
    bool      printable_only  ///< true: exempt navigation/editing keys
);

/// Normalized coordinate span for absolute-pointer (touch) events.  A
/// `TOUCH_*` event's `x` / `y` are rescaled from the device's native
/// `EFI_ABSOLUTE_POINTER_MODE` `AbsoluteMin/Max` into `[0, AXL_INPUT_ABS_RANGE)`,
/// so the value is display-independent: the consumer maps it onto its own
/// surface (`px = x * surface_w / AXL_INPUT_ABS_RANGE`).  axl-input stays a
/// sibling of axl-gfx with no dependency on it — it never learns the screen
/// resolution; mapping to pixels is the (display-owning) caller's job.
#define AXL_INPUT_ABS_RANGE 0x10000u

/// Normalize a native absolute coordinate @p value (in the device's
/// `[lo, hi]` range, from `EFI_ABSOLUTE_POINTER_MODE`) to `[0,
/// AXL_INPUT_ABS_RANGE)`.  This is the exact mapping the touch source
/// applies to each `TOUCH_*` event; exposed so it can be unit-tested and
/// reused.  @p value is clamped to `[lo, hi]`; a degenerate range
/// (`hi <= lo`) yields 0.
int32_t
axl_input_abs_normalize(int64_t value, uint64_t lo, uint64_t hi);

// -------------------------------------------------------------------
// Ctrl+letter encoding (KEY_DOWN) — there is NO single canonical form;
// it depends on the UEFI console-input device, and a portable consumer
// must handle BOTH. Verified empirically on QEMU OVMF (x64,
// Ps2KeyboardDxe) and AAVMF (aa64, UsbKeyboardDxe) — see
// test/integration/test-input-keys-qemu.sh.
//
//   * Physical keyboard (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL): Ctrl+letter
//     arrives as the PRINTABLE LETTER in `unicode` (Ctrl+A → 'a' =
//     0x61) with AXL_INPUT_MOD_CTRL set in `modifiers`. It is NOT folded
//     to a C0 control code. (Lock/toggle state may add CAPS/NUM/SCROLL
//     bits, but no C0 fold occurs.)
//
//   * Serial console (TerminalDxe): the terminal transmits a raw C0
//     byte, so Ctrl+letter arrives as the FOLDED C0 CONTROL CODE in
//     `unicode` (Ctrl+A = 0x01 … Ctrl+Z = 0x1A) with `modifiers == 0`
//     (serial has no Ex protocol and carries no shift state).
//
// Recommended detection (covers both paths). Fold case first so the
// keyboard branch also catches Ctrl+letter delivered uppercase when
// Shift / Caps Lock is active (terminals fold both Ctrl+A and
// Ctrl+Shift+A to 0x01, so case-folding keeps the two paths consistent):
//     uint32_t u = ev->unicode;
//     if (u >= 'A' && u <= 'Z') u += 0x20;                     // fold case
//     bool is_ctrl_letter =
//         ((ev->modifiers & AXL_INPUT_MOD_CTRL) &&
//          u >= 'a' && u <= 'z')                               // keyboard
//      || (ev->unicode >= 0x01 && ev->unicode <= 0x1A);        // serial
//     // 1-based letter index (A=1 … Z=26), valid ONLY when is_ctrl_letter:
//     uint32_t n = (ev->unicode >= 0x01 && ev->unicode <= 0x1A)
//                ? ev->unicode : (u - 'a' + 1);
//
// Do not assume one form: dropping the folded-C0 branch breaks the
// serial console; dropping the letter+MOD_CTRL branch breaks the
// physical keyboard.
// -------------------------------------------------------------------

/// Decode a Ctrl+\<letter\> chord from a KEY_DOWN event, collapsing the
/// two device-dependent encodings documented above into a single letter
/// so a consumer can match Ctrl-chord shortcuts portably.  Pass the
/// event's `unicode` and `modifiers`.
///
/// @return the lowercase letter 'a'..'z' the chord names (Ctrl+A → 'a'),
///         or 0 when the inputs are not a Ctrl+\<letter\> chord.  The four
///         chords whose C0 codes double as dedicated editing keys —
///         Ctrl+H / I / J / M (Backspace / Tab / LF / CR) — return 0, so
///         a consumer never shadows those keys with a chord.
char
axl_input_ctrl_letter(uint32_t unicode, uint32_t modifiers);

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

/// Detach the mouse source previously attached with
/// axl_input_attach_mouse. Removes the loop event source, cancels any
/// pending held-button auto-repeat timer, and frees the
/// single-mouse-per-process slot so a later axl_input_attach_mouse can
/// succeed. NULL-safe and idempotent (no-op if no mouse is attached).
void
axl_input_detach_mouse(
    AxlLoop  *loop  ///< the loop the mouse was attached to
    );

/// Register keyboard input as an event source on the loop.
///
/// Thin wrapper over `axl_loop_add_key_press` that translates each
/// `AxlInputKey` (scan_code + unicode_char) into a unified
/// `AxlInputEvent` (`type = AXL_INPUT_KEY_DOWN`, `keycode = scan_code`,
/// `unicode = unicode_char`, `modifiers = AXL_INPUT_MOD_*`).  This lets
/// callers register a single `AxlInputCallback` for mouse + keyboard +
/// (future) touch instead of separate per-device callbacks.
///
/// `event.modifiers` carries held shift/ctrl/alt/meta (left/right
/// distinct, plus side-agnostic masks) and caps/num/scroll lock state
/// when the firmware publishes `EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`; it
/// is 0 when modifiers can't be read (no ex protocol, e.g. serial).
/// Only `AXL_INPUT_KEY_DOWN` fires — UEFI delivers no key-up or
/// standalone-modifier events.
///
/// For how a held Ctrl is encoded — it differs between the physical
/// keyboard (letter + `AXL_INPUT_MOD_CTRL`) and the serial console
/// (folded C0 control code, no modifiers) — see the Ctrl+letter note on
/// `AxlInputEvent.unicode` above; a portable consumer must handle both.
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

/// Detach the keyboard source previously attached with
/// axl_input_attach_key. Removes the loop event source and frees the
/// single-keyboard-per-process slot so a later axl_input_attach_key can
/// succeed — the mirror of axl_input_detach_mouse. Pass the same @a loop
/// you gave axl_input_attach_key (the source is removed from it).
/// Idempotent: a no-op if no keyboard is attached.
void
axl_input_detach_key(
    AxlLoop  *loop  ///< the loop the keyboard was attached to
    );

/// Register touch input as an event source on the loop.
///
/// Locates `EFI_ABSOLUTE_POINTER_PROTOCOL`, registers its
/// `WaitForInput` event with the loop, and on each dispatch reads
/// absolute position via `GetState`.  Emits `AXL_INPUT_TOUCH_DOWN`
/// on first contact (ActiveButtons transition 0 → non-zero),
/// `AXL_INPUT_TOUCH_UP` on contact end (non-zero → 0), and
/// `AXL_INPUT_TOUCH_MOVE` whenever the position changes — INCLUDING
/// while no button is held (hover), so a pen / tablet / VNC-tablet that
/// reports position without contact still drives a pointer.  (A bare
/// touchscreen simply never emits the hover moves.)  `buttons` on a MOVE
/// is the live `ActiveButtons` (0 on hover), so a consumer can tell a
/// drag from a hover.
///
/// Position is normalized to `[0, AXL_INPUT_ABS_RANGE)` from the device's
/// native `EFI_ABSOLUTE_POINTER_MODE` range — display-independent; the
/// caller maps it onto its surface.  See `AXL_INPUT_ABS_RANGE`.
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
