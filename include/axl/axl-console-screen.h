/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-screen.h
    A server-side terminal screen model with a self-contained snapshot
    serializer, for late-join repaint of a shared console.

    A remote viewer that joins a live console session mid-stream (a
    serial-over-LAN WebSocket client, an `axl-console-mirror` browser tab) sees a
    blank pane until the guest happens to repaint. `AxlConsoleScreen` closes that
    gap: feed it the same VT/xterm byte stream every viewer receives, and on a new
    connection ask it to @ref axl_console_screen_snapshot the *current* screen as a
    single self-contained VT "repaint" — clear, then the visible cells with their
    colours, then the cursor and alt-screen state. The late joiner applies that one
    burst to its blank terminal and immediately sees exactly what everyone else
    sees.

    The model is driven by @ref AxlVterm (the Layer-2 VT parser) into an owned
    rows x cols cell grid; the snapshot walks that grid. It is therefore the
    inverse of @ref axl-console-mirror.h, which serializes the *other* producer
    (`axl-console-tap`) to the same VT wire — a snapshot is a mirror's worth of
    escapes coalesced down to one screenful. See
    `docs/AXL-console-screen-snapshot-handoff.md`.

    @code
    static void to_ws(const char *bytes, size_t len, void *user) {
        axl_ws_send(user, bytes, len);   // ship to the browser terminal
    }

    AxlConsoleScreen *scr = axl_console_screen_new(25, 80);
    // ... on every rx burst from the serial console:
    axl_console_screen_feed(scr, rx, rx_len);   // (still broadcast rx live too)
    // ... when a new viewer connects, before joining it to the live stream:
    axl_console_screen_snapshot(scr, to_ws, new_client);
    // ... on teardown:
    axl_console_screen_free(scr);
    @endcode
**/

#ifndef AXL_CONSOLE_SCREEN_H
#define AXL_CONSOLE_SCREEN_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque server-side terminal screen model.
 *
 * Created by @ref axl_console_screen_new, torn down by
 * @ref axl_console_screen_free. Owns an internal @ref AxlVterm parser and two
 * rows x cols cell grids — the primary and the alternate screen — kept current
 * with every byte fed, swapping on the guest's `DECSET/DECRST 1049` just as a real
 * terminal does, so the primary screen survives a full-screen app and is intact
 * again when it exits. Not thread-safe: feed and snapshot from one context (the
 * console's event loop).
 */
typedef struct AxlConsoleScreen AxlConsoleScreen;

/**
 * @brief Sink that receives a snapshot's serialized VT bytes.
 *
 * Called (possibly many times) during @ref axl_console_screen_snapshot with
 * consecutive chunks of the repaint stream; concatenating every chunk yields the
 * whole self-contained repaint. A chunk boundary is a byte offset, not a token
 * boundary — a single VT escape may be split across two calls — so feed the bytes
 * to a streaming parser (or reassemble), never treat one call as a self-contained
 * frame. The bytes are UTF-8 text interleaved with ANSI/VT control sequences,
 * ready for an xterm.js / VT100 terminal, and are **only valid for the duration of
 * the call** — copy, do not retain the pointer. It has the
 * same signature as the mirror's `AxlConsoleSinkFn` but is declared here so this
 * header need not depend on `axl-console-mirror.h`; the two are
 * structurally compatible, so one sink function serves both APIs.
 *
 * @param bytes serialized VT bytes for this chunk (not NUL-terminated).
 * @param len   number of bytes in this chunk.
 * @param user  the opaque context passed to @ref axl_console_screen_snapshot.
 */
typedef void (*AxlConsoleScreenSink)(
    const char *bytes,  ///< serialized VT bytes (not NUL-terminated)
    size_t      len,    ///< byte count for this chunk
    void       *user    ///< opaque context from the snapshot call
) AXL_CB_NOEXCEPT;

/**
 * @brief Create a screen model of @p rows x @p cols cells.
 *
 * The grid starts blank in the default pen (default fg/bg, no styles), the cursor
 * home at (0,0) and visible, the primary (non-alternate) screen active — i.e. the
 * state a real terminal boots in, so a snapshot taken before any byte is fed is a
 * clean clear. Internally binds an @ref AxlVterm parser to the grid.
 *
 * @param rows terminal rows (must be > 0).
 * @param cols terminal columns (must be > 0).
 * @return the new handle, or NULL on a zero @p rows / @p cols or allocation
 *     failure.
 */
AxlConsoleScreen *
axl_console_screen_new(
    uint32_t           rows,  ///< terminal rows (> 0)
    uint32_t           cols   ///< terminal columns (> 0)
);

/**
 * @brief Free the screen model and its parser. NULL-safe.
 *
 * Always pair with @ref axl_console_screen_new.
 */
void
axl_console_screen_free(
    AxlConsoleScreen *s  ///< handle (NULL-safe)
);

/**
 * @brief Feed a run of terminal bytes, updating the screen model.
 *
 * The bytes are the raw VT/xterm stream from the console (a serial / SOL pipe):
 * arbitrary bytes, need not be NUL-terminated, and a partial escape or multi-byte
 * sequence at the end is held internally until the next call. The bytes are
 * **consumed, not retained** — the caller keeps ownership of @p bytes. The grid is
 * fully up to date when this returns, so a @ref axl_console_screen_snapshot may
 * follow immediately. No-op if @p s or @p bytes is NULL.
 *
 * @param s     handle.
 * @param bytes terminal bytes (xterm/VT); need not be NUL-terminated.
 * @param len   number of bytes.
 */
void
axl_console_screen_feed(
    AxlConsoleScreen *s,      ///< handle
    const uint8_t    *bytes,  ///< terminal bytes (xterm/VT)
    size_t            len     ///< number of bytes
);

/**
 * @brief Serialize the current screen as a self-contained VT repaint.
 *
 * Emits, through @p sink, one burst of VT bytes that — applied to a **blank
 * terminal of the same @c rows x @c cols size** — reproduces the currently
 * displayed screen: the same visible glyphs, the same per-cell colours and text
 * attributes, the cursor at the same cell with the same visibility, and the same
 * primary/alternate-screen selection. When the alternate screen is active, the
 * intact primary is repainted first and saved (`DECSET 1049`), then the alternate
 * is repainted on top — so the joiner holds *both* buffers and restoring the
 * primary (the guest's `DECRST 1049` on app exit) reveals the real screen, not a
 * blank one. A late joiner that feeds this burst into a fresh terminal and then
 * joins the live stream sees precisely what every existing viewer sees. The output
 * is deterministic for a given screen state.
 *
 * **Carried** in the repaint: the visible cell glyphs; each cell's full pen
 * (default / indexed / 24-bit RGB foreground and background, plus bold, italic,
 * underline, blink, reverse, conceal, strike); the cursor row/col; cursor
 * visibility; the active-vs-alternate screen selection; and whole-screen reverse
 * video (DECSCNM). **Not carried** (a late joiner starts these at their defaults):
 * cursor blink/shape, the window title / icon name, mouse-reporting and
 * focus-reporting modes, and — a known limitation — the right-margin *pending
 * wrap* state (a cursor parked past the last column repaints at the last column,
 * so the joiner's very next wrapped glyph may land one row earlier than on a
 * viewer that saw the live stream).
 *
 * **Self-contained but not self-sizing.** The burst clears and repaints but does
 * not set the terminal size, so the caller must ensure the receiving terminal is
 * already @c rows x @c cols before feeding it (SoftBMC sizes the WebSocket terminal
 * on connect).
 *
 * **Coalesced, not a cell dump.** Blank cells in the default background emit no
 * bytes and fully-blank rows are skipped entirely, so a mostly-empty 80x25 screen
 * serializes to a handful of bytes, not ~2 KB of spaces. Consecutive cells that
 * share a pen are emitted under a single SGR escape (run-length), and the pen is
 * only re-stated when it changes.
 *
 * This is a read-only view of the screen (the model is unchanged) and allocates
 * nothing; it may be called any number of times.
 *
 * @param s    handle.
 * @param sink receives the serialized VT bytes in one or more chunks; the
 *     concatenation is the whole repaint. Must not be NULL.
 * @param user opaque context passed back to @p sink.
 * @return AXL_OK once the whole repaint has been handed to @p sink; AXL_ERR only
 *     on a NULL @p s / @p sink, in which case nothing is written to @p sink.
 */
int
axl_console_screen_snapshot(
    const AxlConsoleScreen *s,     ///< handle
    AxlConsoleScreenSink    sink,  ///< receives the serialized repaint
    void                   *user   ///< opaque context for the sink
);

/**
 * @brief Resize the screen model to @p rows x @p cols.
 *
 * The overlapping top-left region of both the primary and alternate grids is
 * preserved cell-for-cell; cells exposed by a grow start blank in the current pen,
 * and the cursor is clamped into the new bounds. Wrapped lines are **not**
 * reflowed — this truncates or pads, matching how a terminal emulator resizes its
 * grid. The underlying parser is resized too, so subsequent wrapping and scrolling
 * use the new geometry. On failure both grids and the current size are left
 * unchanged.
 *
 * @param s    handle.
 * @param rows new row count (must be > 0).
 * @param cols new column count (must be > 0).
 * @return AXL_OK on success; AXL_ERR on a NULL @p s, a zero @p rows / @p cols, or
 *     allocation failure.
 */
int
axl_console_screen_resize(
    AxlConsoleScreen *s,     ///< handle
    uint32_t          rows,  ///< new row count (> 0)
    uint32_t          cols   ///< new column count (> 0)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_SCREEN_H */
