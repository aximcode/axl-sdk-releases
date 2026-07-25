/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-term.h
    Local interactive terminal: the on-screen sink for @ref AxlConsoleOps, and the
    counterpart to @ref axl_console_mirror_install (the REMOTE sink that serializes
    the ops to a VT byte stream). An `AxlConsoleTerm` turns a take-over console's op
    stream — from @ref axl_console_device_install — into a rendered cell grid on the
    GOP framebuffer, with scrollback, drag-select + copy, and font zoom.

    **Mechanism, not policy.** The terminal owns the grid + rendering + interaction
    *mechanisms* (`scroll`, `selection_*`, `set_font`, `render`); the consumer binds
    *gestures* to them (which wheel/key does what). Keys reach the shell through the
    device's input relay; the terminal peeks its own hotkeys via the device's
    `key_filter` (@ref axl_console_term_handle_hotkey). This split is what lets a
    richer consumer (a widget toolkit) reuse the plumbing while keeping custom
    keybindings + theming.

    Not a singleton: a consumer may host several (e.g. tabs). See
    `AXL-Console-Terminal-Design.md`.
**/

#ifndef AXL_CONSOLE_TERM_H
#define AXL_CONSOLE_TERM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>        /* AXL_DEFINE_AUTOPTR_CLEANUP */
#include <axl/axl-console-ops.h>
#include <axl/axl-font.h>
#include <axl/axl-gfx.h>
#include <axl/axl-input.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque local-terminal instance. */
typedef struct AxlConsoleTerm AxlConsoleTerm;

/**
 * @brief Terminal configuration (copied). A zeroed config = auto geometry from the
 *     GOP + default font, the default 16-colour palette, GOP render target, full
 *     bounds, a default scrollback depth.
 */
typedef struct {
    uint32_t       cols;            ///< grid columns (0 = auto from the target/GOP + font)
    uint32_t       rows;            ///< grid rows (0 = auto)
    const AxlFont *font;            ///< monospace font (NULL = axl_gfx_default_font())
    uint32_t       scrollback_rows; ///< history depth in rows (0 = a sensible default)
    const AxlGfxPixel *palette;     ///< 16 console colours (NULL = the default palette)
    AxlGfxBuffer  *target;          ///< render target: NULL = the GOP screen; else an
                                    ///< off-screen buffer a compositor blits/composites.
    uint32_t       x;               ///< render bounds within the target: origin x
    uint32_t       y;               ///< origin y
    uint32_t       w;               ///< width  (0 = to the target/GOP edge)
    uint32_t       h;               ///< height (0 = to the target/GOP edge)
    void (*on_zoom)(void *user, int32_t delta); ///< Ctrl+wheel via handle_pointer; NULL = ignore
    void          *cb_user;         ///< opaque context passed to @a on_zoom
    bool           mouse_cursor;    ///< draw a software mouse-cursor overlay (@ref AxlCursor,
                                    ///< built-in arrow) tracking @ref axl_console_term_set_pointer.
                                    ///< Off by default; enable it when the terminal OWNS the display
                                    ///< (a direct-GOP `target`) — a compositor host draws its own
                                    ///< cursor, so leave it off there to avoid two cursors.
} AxlConsoleTermConfig;

/**
 * @brief Create a terminal. Resolves geometry (from @p cfg or the GOP + font),
 *     allocates the cell grid + scrollback, and wires an @ref AxlConsoleOps sink.
 * @return the instance, or NULL on bad args / allocation failure.
 */
AxlConsoleTerm *
axl_console_term_new(
    const AxlConsoleTermConfig *cfg   ///< configuration (copied); NULL = all defaults
);

/** @brief Destroy a terminal and free its buffers. NULL-safe. */
void
axl_console_term_free(
    AxlConsoleTerm *t   ///< instance (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlConsoleTerm, axl_console_term_free)
#endif

/**
 * @brief The @ref AxlConsoleOps sink to hand to a producer (e.g.
 *     @ref axl_console_device_install). Every console op mutates the cell grid.
 * @return the borrowed ops vtable; @p *user (if non-NULL) receives the context to
 *     pass back as each op's `user` argument.
 */
const AxlConsoleOps *
axl_console_term_ops(
    AxlConsoleTerm  *t,     ///< instance
    void           **user   ///< [out] receives the op context (may be NULL)
);

/* --- Scrollback --------------------------------------------------------------
 * Lines that scroll off the top of the live screen are kept in a history ring
 * (depth = cfg.scrollback_rows). The scroll offset selects what the viewport shows:
 * 0 = live, N = N rows back into history. */

/**
 * @brief Move the scrollback view by @p delta_rows: positive scrolls BACK into
 *     history, negative toward the live screen. Clamps to [0, stored history].
 *     NULL-safe.
 */
void
axl_console_term_scroll(
    AxlConsoleTerm *t,          ///< instance (NULL-safe)
    int32_t         delta_rows  ///< +back into history / -toward live
);

/* --- Rendering ---------------------------------------------------------------
 * Paint the terminal onto its render target (the GOP screen, or the offscreen buffer
 * from cfg.target). Only rows dirtied since the last render are re-blitted. */

/**
 * @brief Blit changed cells to the render target at the configured bounds, then
 *     clear the dirty flags. Uses per-cell damage tracking: within a dirty row only
 *     cells whose drawn appearance actually changed (glyph, colour, selection, or
 *     cursor caret) are re-blitted — so a single-character edit repaints a single
 *     cell, not the whole row. This keeps the framebuffer writes proportional to the
 *     change, which matters on a dirty-tracked display (e.g. VNC) where each write is
 *     costly. The cursor caret is an underline folded into its cell's render (live
 *     view only). Honors scrollback: when scrolled back the shown rows come from
 *     history. Saves the caller's active draw target and restores it on return.
 *
 *     When `cfg.mouse_cursor` is set, the software mouse cursor (@ref AxlCursor) is
 *     composited as the top layer of this frame: the render brackets its cell blits so
 *     the arrow is lifted before and re-laid after, staying atop the cells without a
 *     full-frame redraw (its own footprint is the only extra write). NULL-safe.
 */
void
axl_console_term_render(
    AxlConsoleTerm *t   ///< instance (NULL-safe)
);

/* --- Reflow ------------------------------------------------------------------
 * Adjust the terminal's font, grid size, render bounds, or palette. Each marks the
 * whole viewport dirty so the next render() repaints. */

/**
 * @brief Set the monospace font and re-cache its cell metrics (the grid stays the
 *     same cols x rows; only the pixel size of each cell changes). Marks all rows
 *     dirty. NULL @p t or @p font is a no-op.
 */
void
axl_console_term_set_font(
    AxlConsoleTerm *t,     ///< instance (NULL-safe)
    const AxlFont  *font   ///< new monospace font (borrowed; NULL = no-op)
);

/**
 * @brief Resize the cell grid to @p cols x @p rows. The overlapping top-left region
 *     is preserved; new cells are blank in the current pen; the cursor is clamped
 *     into range. Changing @p cols resets scrollback (history rows are @p cols wide).
 *     Marks all rows dirty. NULL @p t or a zero dimension is a no-op.
 */
void
axl_console_term_resize(
    AxlConsoleTerm *t,     ///< instance (NULL-safe)
    uint32_t        cols,  ///< new column count (0 = no-op)
    uint32_t        rows   ///< new row count (0 = no-op)
);

/**
 * @brief Set the render bounds (origin + extent) within the target. A zero @p w or
 *     @p h means "to the target/GOP edge". Marks all rows dirty. NULL-safe.
 */
void
axl_console_term_set_bounds(
    AxlConsoleTerm *t,   ///< instance (NULL-safe)
    uint32_t        x,   ///< origin x within the target
    uint32_t        y,   ///< origin y within the target
    uint32_t        w,   ///< width  (0 = to the edge)
    uint32_t        h    ///< height (0 = to the edge)
);

/**
 * @brief Replace the 16-colour palette. Marks all rows dirty so the next render
 *     repaints with the new colours. NULL @p t or @p palette is a no-op.
 */
void
axl_console_term_set_palette(
    AxlConsoleTerm    *t,        ///< instance (NULL-safe)
    const AxlGfxPixel *palette   ///< 16 colours (borrowed/copied; NULL = no-op)
);

/* --- Selection + copy --------------------------------------------------------
 * Coordinates are VIEWPORT cells (0,0 = top-left of the visible area). The
 * selection is stored in viewport space; copy resolves each row through the current
 * scroll offset, so it reads history rows when scrolled back. Selected cells render
 * inverted (fg/bg swapped). */

/**
 * @brief Begin a selection, anchoring it at viewport cell (@p col, @p row).
 *     Replaces any prior selection. Marks the viewport dirty. NULL-safe.
 */
void
axl_console_term_selection_start(
    AxlConsoleTerm *t,    ///< instance (NULL-safe)
    uint32_t        col,  ///< anchor column (viewport)
    uint32_t        row   ///< anchor row (viewport)
);

/**
 * @brief Extend the active selection's free end to viewport cell (@p col, @p row)
 *     — the drag-to endpoint. No-op if no selection is active. Marks the viewport
 *     dirty. NULL-safe.
 */
void
axl_console_term_selection_extend(
    AxlConsoleTerm *t,    ///< instance (NULL-safe)
    uint32_t        col,  ///< new free-end column (viewport)
    uint32_t        row   ///< new free-end row (viewport)
);

/** @brief Clear the active selection. Marks the viewport dirty. NULL-safe. */
void
axl_console_term_selection_clear(
    AxlConsoleTerm *t   ///< instance (NULL-safe)
);

/**
 * @brief Copy the selected text to the clipboard (@ref axl_clipboard_set, MIME
 *     `text/plain`). Rows are joined with '\n'; each row's trailing blanks are
 *     trimmed; interior blanks are preserved as spaces.
 * @return AXL_OK on success, AXL_ERR if @p t is NULL, no selection is active, or
 *     the clipboard allocation fails.
 */
AXL_WARN_UNUSED int
axl_console_term_selection_copy(
    AxlConsoleTerm *t   ///< instance
);

/* --- Interaction conveniences ------------------------------------------------
 * Optional helpers that bind common gestures to the mechanisms above. A consumer
 * with its own keybindings/gestures can ignore these and drive scroll/selection
 * directly. */

/**
 * @brief Route a pointer event to the terminal: wheel scrolls history (Ctrl+wheel
 *     invokes `cfg.on_zoom` instead), a left-button press starts a selection, and a
 *     dragging move extends it. A motion event also moves the software mouse cursor
 *     (when `cfg.mouse_cursor` is set), so a consumer that already forwards events
 *     here gets the tracking cursor for free. Pointer coordinates are pixels in the
 *     target's space; they are mapped to cells via the render bounds + font metrics.
 *     Unrecognized events are ignored. NULL-safe.
 */
void
axl_console_term_handle_pointer(
    AxlConsoleTerm      *t,   ///< instance (NULL-safe)
    const AxlInputEvent *e    ///< pointer event (NULL-safe)
);

/**
 * @brief Move the software mouse cursor to target-space pixel (@p px, @p py) and show
 *     it. This is the direct-position feed for a consumer that has a pointer position
 *     but does not route full @ref AxlInputEvent records (e.g. a take-over driver
 *     reading the forwarded pointer). The position is the cursor hotspot, clamped to
 *     the target; the arrow is composited on the next @ref axl_console_term_render.
 *     No-op when `cfg.mouse_cursor` was not set (or the cursor could not be created).
 *     NULL-safe.
 */
void
axl_console_term_set_pointer(
    AxlConsoleTerm *t,    ///< instance (NULL-safe)
    int32_t         px,   ///< hotspot x in target-space pixels
    int32_t         py    ///< hotspot y in target-space pixels
);

/**
 * @brief Hide the software mouse cursor (e.g. no pointer device is present, or the
 *     pointer left the surface). The next render leaves the cells clean where the
 *     arrow was. Idempotent; no-op when `cfg.mouse_cursor` was not set. NULL-safe.
 */
void
axl_console_term_hide_pointer(
    AxlConsoleTerm *t   ///< instance (NULL-safe)
);

/**
 * @brief Offer a key to the terminal's built-in hotkeys: Shift+PgUp / Shift+PgDn
 *     scroll a page back / forward, Ctrl+Shift+C copies the selection. Its signature
 *     matches the device `key_filter` so it can be wired as one directly.
 * @param key firmware key record (an `EFI_KEY_DATA *` passed opaquely, as the device
 *     `key_filter` delivers it).
 * @return true if the key was a recognized hotkey (consume it — do not forward to
 *     the shell); false otherwise.
 */
bool
axl_console_term_handle_hotkey(
    AxlConsoleTerm *t,     ///< instance (NULL-safe)
    const void     *key    ///< opaque firmware key record (EFI_KEY_DATA *)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_TERM_H */
