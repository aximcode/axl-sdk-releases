/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-cursor.h
 *
 * Software mouse-cursor compositor for axl-gfx. GOP exposes no
 * hardware-cursor API (the GPU usually has a cursor plane, but UEFI
 * surfaces only a framebuffer + Blt — no portable way to drive it), so a
 * sprite that tracks the pointer must be composited in software — and
 * since exactly one cursor owns the screen, that belongs in one shared
 * place rather than re-invented per consumer.
 *
 * The cursor is bound to the back-buffer "scene" being scanned out (the
 * source of truth for the pixels under it). Moving it touches only the
 * cursor region: the old position is erased by re-presenting the clean
 * scene there, and the sprite is composited over the scene at the new
 * position — no full-frame redraw (validated; see
 * docs/AXL-Pointer-Cursor-Design.md, Option C).
 *
 * AxlCursor owns *the sprite on the screen and the current position*. It
 * never routes input — hit-testing and widget behavior stay with the
 * consumer. (Under a future surface compositor the cursor becomes the
 * topmost overlay and input routing moves to a seat; this same sprite +
 * compositing logic carries over.)
 *
 * @code
 * AxlGfxBuffer *scene = axl_gfx_buffer_new(w, h);   // your back-buffer
 * // ... draw your frame into scene, present it ...
 * AxlCursor *cur = axl_cursor_new(scene);           // built-in arrow
 * axl_cursor_attach(cur, loop, on_input, &app);     // tracks the pointer
 * // when you re-present a new frame:
 * axl_cursor_lift(cur);
 * axl_gfx_buffer_present(scene, 0, 0);
 * axl_cursor_drop(cur);
 * @endcode
 */

#ifndef AXL_CURSOR_H
#define AXL_CURSOR_H

#include <stdint.h>
#include <stdbool.h>

#include <axl/axl-gfx-surface.h>   /* AxlGfxBuffer */
#include <axl/axl-input.h>         /* AxlInputCallback */
#include <axl/axl-loop.h>          /* AxlLoop */

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque cursor compositor.
typedef struct AxlCursor AxlCursor;

/**
 * @brief Create a cursor bound to a back-buffer scene.
 *
 * @p scene is the buffer being scanned out — the source of truth for the
 * pixels under the cursor (today a consumer's back-buffer; under a future
 * compositor, its composited output). It must outlive the cursor. The
 * cursor starts with the built-in arrow, hidden, at (0, 0).
 *
 * Pass a NULL @p scene for direct-to-screen "save-under" mode (Option B):
 * the screen itself is the scene, so the cursor captures the screen pixels
 * under it (`axl_gfx_capture`) and restores them on move. This needs a
 * live framebuffer — it returns NULL if graphics output is unavailable.
 * The bracket discipline still applies: when the consumer presents a new
 * frame while the cursor is up, the saved pixels go stale, so wrap the
 * present in axl_cursor_lift / axl_cursor_drop. Most consumers have a
 * back-buffer; prefer the (non-NULL scene) Option-C path when you do.
 *
 * @return the cursor, or NULL on allocation failure (or, for save-under
 *     mode, if graphics output is unavailable).
 */
AxlCursor *
axl_cursor_new(
    AxlGfxBuffer  *scene   ///< back-buffer being scanned out, or NULL for save-under mode; if non-NULL must outlive the cursor
);

/**
 * @brief Destroy a cursor. NULL-safe. Does not free the scene.
 *
 * If the cursor is visible, hide it (restore the scene under it) first.
 */
void
axl_cursor_free(
    AxlCursor  *c   ///< cursor (NULL-safe)
);

/**
 * @brief Set the cursor sprite and hotspot.
 *
 * @p sprite is an RGBA buffer composited source-over the scene; its
 * pixels are copied, so the buffer need not outlive the call. @p hot_x /
 * @p hot_y are the hotspot offset within the sprite (the pixel that sits
 * exactly on the pointer position). Pass a NULL @p sprite to restore the
 * built-in arrow.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments / allocation
 *     failure.
 */
int
axl_cursor_set_image(
    AxlCursor           *c,        ///< cursor
    const AxlGfxBuffer  *sprite,   ///< RGBA sprite (copied); NULL = built-in arrow
    int32_t              hot_x,    ///< hotspot x within the sprite
    int32_t              hot_y     ///< hotspot y within the sprite
);

/**
 * @brief Show the cursor (composite it at the current position).
 */
void
axl_cursor_show(
    AxlCursor  *c   ///< cursor
);

/**
 * @brief Hide the cursor (restore the scene under it).
 */
void
axl_cursor_hide(
    AxlCursor  *c   ///< cursor
);

/**
 * @brief Whether the cursor is currently shown.
 *
 * @return true if visible.
 */
bool
axl_cursor_visible(
    const AxlCursor  *c   ///< cursor (NULL-safe; NULL → false)
);

/**
 * @brief Move the hotspot to (@p x, @p y) in screen pixels.
 *
 * Erases the cursor at its old position (re-presents the clean scene
 * there) and composites it at the new one — touching only the cursor
 * region(s), not the whole frame. The position is clamped to the
 * framebuffer. No-op visually while hidden (position is still tracked).
 */
void
axl_cursor_move(
    AxlCursor  *c,   ///< cursor
    int32_t     x,   ///< hotspot x in screen pixels
    int32_t     y    ///< hotspot y in screen pixels
);

/**
 * @brief Move the hotspot by a relative delta from its current position.
 *
 * Applies (@p dx, @p dy) to the current hotspot and clamps the result to
 * the framebuffer — so the cursor stops at an edge but moves the instant
 * the delta reverses. This is the right way to drive the cursor from a
 * relative pointer (`EFI_SIMPLE_POINTER`): feeding the device's raw
 * *accumulated* position to axl_cursor_move instead would let the cursor
 * get stuck off-screen (the accumulator can drift far past an edge, and
 * the clamped cursor then can't recover until the accumulator climbs all
 * the way back). axl_cursor_attach tracks the pointer through this.
 */
void
axl_cursor_move_rel(
    AxlCursor  *c,   ///< cursor
    int32_t     dx,  ///< delta x in screen pixels
    int32_t     dy   ///< delta y in screen pixels
);

/**
 * @brief Get the current hotspot position.
 */
void
axl_cursor_position(
    const AxlCursor  *c,   ///< cursor (NULL-safe)
    int32_t          *x,   ///< [out] hotspot x (may be NULL)
    int32_t          *y    ///< [out] hotspot y (may be NULL)
);

/**
 * @brief Bracket-open: fold the cursor into the scene before the consumer
 *        flushes it, so one present carries the cursor atomically.
 *
 * For a bound scene (Option C), this composites the sprite INTO the scene as
 * its top layer (saving the pixels it overwrites), so the consumer's
 * `axl_gfx_buffer_present(scene, …)` — or the compositor's damage flush —
 * presents scene+cursor in a SINGLE operation. This is the flicker fix: there
 * is no separate erase/redraw to the GOP, so there is never an intermediate
 * frame showing the scene without the cursor (the failure mode at low present
 * rates). It mirrors how software-cursor compositors composite the cursor as
 * the last layer before one commit (wlroots, Qt's QFbCursor).
 *
 * For save-under (NULL scene) there is no buffer to fold into, so this falls
 * back to restoring the screen pixels under the cursor; axl_cursor_drop then
 * redraws it after the present.
 *
 * Pair with axl_cursor_drop after the present. A lift/drop with no present in
 * between is harmless. No-op if already lifted/hidden.
 */
void
axl_cursor_lift(
    AxlCursor  *c   ///< cursor
);

/**
 * @brief Bracket-close: counterpart to axl_cursor_lift.
 *
 * For a bound scene, unfolds the cursor — restores the saved scene pixels so
 * the scene is byte-clean for the next partial-damage repaint. For save-under,
 * re-composites the cursor over the freshly-presented screen. No-op if hidden.
 */
void
axl_cursor_drop(
    AxlCursor  *c   ///< cursor
);

/**
 * @brief Which pointer sources axl_cursor_attach_ex binds, and how it reads
 *        the absolute one.
 *
 * Zero-initialize (`AxlCursorConfig cfg = {0};`) for the common case — bind
 * BOTH pointers with library-default absolute-read settings — then override
 * only what you need (e.g. `cfg.touch_drain = 8;`). The source flags are
 * *skip* flags, and `touch_all_handles` is likewise an opt-OUT, precisely so
 * the all-zero config matches the library defaults: bind both pointers,
 * EVENT_AND_POLL, **ConsoleIn-only**, 30 ms poll, no coalesce.
 *
 * The cursor tracks both pointer kinds when both are bound: the absolute
 * pointer (`EFI_ABSOLUTE_POINTER` — a touchscreen, a digitizer, or a
 * remote-console / BMC virtual mouse, and QEMU's usb-tablet over VNC) reports
 * a position directly. Once ANY absolute event arrives it is latched
 * AUTHORITATIVE for cursor position for the lifetime of the attach: from then
 * on relative-mouse motion no longer moves the cursor (it still reaches @p cb
 * for button / wheel handling). Before the first absolute event — and forever
 * on bare metal with no absolute device — the relative mouse drives position.
 * Absolute coordinates are mapped onto the cursor's scene extent (the bound
 * back-buffer's size, or the live GOP resolution in save-under / NULL-scene
 * mode) — the same extent axl_cursor_move clamps to.
 *
 * The touch_* fields configure the absolute source exactly as
 * axl_input_set_touch_config / axl_input_set_touch_drain do. Passing this
 * config to axl_cursor_attach_ex applies them PROCESS-GLOBALLY (overwriting
 * any prior axl_input_set_touch_* state, even for fields you left at their
 * default) before binding the touch source. They are the levers for the
 * remote-console "catch-up lag": some firmware queues absolute states FIFO and
 * a one-read-per-poll drain trails a fast move, so raise touch_drain to
 * coalesce the backlog to the latest position. See <axl/axl-input.h>.
 */
typedef struct {
    bool                 skip_mouse;          ///< do NOT bind the relative pointer (default 0: bind it)
    bool                 skip_touch;          ///< do NOT bind the absolute pointer (default 0: bind it)
    AxlInputTouchMethod  touch_method;        ///< absolute read mechanism (0 = EVENT_AND_POLL)
    bool                 touch_all_handles;   ///< bind ALL absolute handles, not just gST->ConsoleInHandle (default 0 = ConsoleIn-only; opt out only where the pointer is on a separate physical handle)
    uint32_t             touch_poll_ms;       ///< absolute poll-fallback interval in ms (0 = 30 ms default)
    uint32_t             touch_drain;         ///< coalesce up to N queued absolute states per read (0/1 = no coalesce)
} AxlCursorConfig;

/**
 * @brief Track the physical pointer on a loop — both relative and absolute.
 *
 * Attaches the relative mouse (axl_input_attach_mouse) AND the absolute
 * pointer (axl_input_attach_touch), moving the cursor on motion from either
 * and showing it, then forwarding every event to @p cb so the consumer still
 * does hit-testing / widget logic. The absolute source is authoritative for
 * position once seen, so a remote-console / VNC / touch pointer tracks
 * correctly (see AxlCursorConfig for the exact rule). Binds with the current
 * process-global absolute-read settings — use axl_cursor_attach_ex to choose
 * sources and set those settings in one call.
 *
 * @note Behavior change: this now ALSO binds the absolute pointer, claiming
 *     the single process-wide absolute-pointer slot. Two consequences for
 *     existing callers: (1) a single saved source ID no longer tears the
 *     cursor down — use axl_cursor_detach (it removes both sources); (2) do
 *     not combine with axl_compositor_attach_touch or a direct
 *     axl_input_attach_touch in the same process — whichever binds the
 *     absolute slot second gets 0. If another component owns the absolute
 *     pointer, attach via axl_cursor_attach_ex with `cfg.skip_touch = true`.
 *
 * @return a non-zero source ID on success (a source for axl_loop_remove_source
 *     — but prefer axl_cursor_detach, which tears down BOTH sources), or 0 if
 *     neither source could be bound.
 */
AxlSourceId
axl_cursor_attach(
    AxlCursor         *c,      ///< cursor
    AxlLoop           *loop,   ///< event loop (caller-owned)
    AxlInputCallback   cb,     ///< consumer callback for forwarded events (may be NULL)
    void              *data    ///< opaque data for @p cb
);

/**
 * @brief Track the physical pointer on a loop, choosing which source(s) to
 *        bind and how to read the absolute one.
 *
 * Like axl_cursor_attach, but @p cfg selects the relative and/or absolute
 * source and configures the absolute read path (method / ConsoleIn-only /
 * poll interval / drain). A NULL @p cfg behaves exactly like
 * axl_cursor_attach (bind both, current global touch settings, none applied).
 * With a non-NULL @p cfg the touch_* fields are applied PROCESS-GLOBALLY
 * before the absolute source is bound — including fields left at default — so
 * a config built only to select sources still resets the global touch tunables
 * to its (default) touch_* values; set them deliberately. The same
 * single-absolute-slot caveat as axl_cursor_attach applies (use skip_touch
 * when another component owns the absolute pointer).
 *
 * @return a non-zero source ID on success (prefer axl_cursor_detach to tear
 *     down), or 0 if no requested source could be bound.
 */
AxlSourceId
axl_cursor_attach_ex(
    AxlCursor              *c,      ///< cursor
    AxlLoop                *loop,   ///< event loop (caller-owned)
    AxlInputCallback        cb,     ///< consumer callback for forwarded events (may be NULL)
    void                   *data,   ///< opaque data for @p cb
    const AxlCursorConfig  *cfg     ///< sources + absolute-read config (NULL = both, current settings)
);

/**
 * @brief Stop tracking the pointer (counterpart to axl_cursor_attach /
 *        axl_cursor_attach_ex).
 *
 * Detaches whichever of the relative and absolute sources this cursor bound.
 */
void
axl_cursor_detach(
    AxlCursor  *c,     ///< cursor
    AxlLoop    *loop   ///< the loop passed to axl_cursor_attach
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CURSOR_H */
