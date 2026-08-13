/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-vterm.h
    Parse a real VT/xterm byte stream into structured @ref AxlConsoleOps.

    The second producer behind the @ref AxlConsoleOps contract. Where
    `axl-console-tap` sources ops from UEFI's `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`,
    `axl-vterm` sources the same ops from a byte stream — a serial / SOL console, or
    any pipe carrying xterm escapes. A consumer binds one vtable and renders both.

    Implemented over a vendored libvterm (MIT), Layer 2 only: libvterm's own cell
    grid (Layer 3) is never compiled, because the consumer already owns one. See
    `deps/libvterm/README.md`.

    Because libvterm's Layer 2 hands text out one positioned glyph at a time and the
    pen one attribute at a time, this adapter does the two jobs that make the ops
    honestly cursor-relative and snapshot-shaped: it **coalesces** consecutive
    glyphs at advancing positions into a single @ref AxlConsoleOps::output_text run
    (flushing on a position jump, a pen change, or any other op), and it
    **accumulates** libvterm's incremental `initpen`/`setpenattr` into one
    @ref AxlConsolePen snapshot, emitted lazily via @ref AxlConsoleOps::set_pen.

    `axl-vterm` also drives @ref AxlConsoleOps::set_term_prop values the tap can
    never originate — title / icon-name fragments, mouse-report mode, focus
    reporting, cursor shape, and DECSCNM reverse video — beyond the alt-screen /
    cursor-visible / cursor-blink props both producers share. It never calls
    @ref AxlConsoleOps::set_mode or @ref AxlConsoleOps::clear_screen -- those have no
    Layer-2 callback (a VT stream expresses a full clear as an `erase`). Note that
    libvterm scopes each `scrollrect` rect to the active DECSTBM region, so a
    consumer never needs the region itself.

    @code
    AxlVterm *v = axl_vterm_new(25, 80, &my_grid_ops, my_grid);
    axl_vterm_feed(v, bytes, len);      // drives the ops
    axl_vterm_flush(v);                 // emit any coalesced run at a repaint boundary
    axl_vterm_free(v);
    @endcode
**/

#ifndef AXL_VTERM_H
#define AXL_VTERM_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-console-ops.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque VT-stream parser bound to an @ref AxlConsoleOps sink.
 *
 * Created by @ref axl_vterm_new, torn down by @ref axl_vterm_free. Owns a vendored
 * libvterm `VTerm` + `VTermState` and the coalescing run buffer and pen snapshot.
 */
typedef struct AxlVterm AxlVterm;

/**
 * @brief Create a VT parser that drives @p ops from a byte stream.
 *
 * The @p ops vtable is retained **by pointer** and must outlive the `AxlVterm`
 * (only the pointer is stored, not a copy). `axl-vterm` reports
 * @ref AXL_CONSOLE_CELLS_WIDTH_RESOLVED through @ref AxlConsoleOps::set_cell_rule
 * once, before returning — so a consumer knows to apply @ref axl_vterm_char_width
 * to every @c output_text run before the first byte is fed. This notification is
 * the only way to learn the rule: `axl-vterm`'s output is width-resolved
 * unconditionally, so a consumer that leaves `set_cell_rule` NULL and falls back to
 * @ref AxlConsoleOps's documented default of @ref AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT
 * will silently misdecode any run carrying a double-width or combining codepoint.
 * `set_cell_rule` is invoked **only on the success path** — a call that returns
 * NULL never touches @p ops.
 *
 * Exactly one op fires before returning — `set_cell_rule`; no `set_cursor` or
 * `set_pen` is synthesized at creation. The consumer's grid should already be in
 * its own fresh-terminal state (cursor at (0,0), default pen) before the first byte
 * is fed.
 *
 * **@ref AxlConsoleOps::erase MUST be bound whenever a scroll can ever be
 * declined — including leaving @ref AxlConsoleOps::scrollrect unbound.** A NULL or
 * declining `scrollrect` decomposes the scroll into `moverect` + `erase`, and
 * libvterm's `vterm_scroll_rect()` dereferences the `erase` callback unconditionally
 * (it NULL-guards only `moverect`); an unbound `erase` therefore faults on the first
 * such scroll. The only way to skip binding `erase` is to bind `scrollrect` and
 * have it accept (return non-zero for) every scroll.
 *
 * @param rows number of terminal rows (must be > 0; signed to match
 *     @ref AxlConsoleRect and libvterm's own `int` geometry).
 * @param cols number of terminal columns (must be > 0).
 * @param ops  consumer callbacks (retained by pointer; must outlive the handle).
 * @param user opaque context passed back to every callback.
 * @return the new handle, or NULL on bad arguments (NULL @p ops, or non-positive
 *     @p rows / @p cols) or allocation failure.
 */
AxlVterm *
axl_vterm_new(
    int32_t              rows,   ///< terminal rows (> 0)
    int32_t              cols,   ///< terminal columns (> 0)
    const AxlConsoleOps *ops,    ///< consumer callbacks (retained by pointer)
    void                *user    ///< opaque context for the callbacks
);

/**
 * @brief Free the parser and its libvterm state. NULL-safe.
 *
 * Discards any coalesced run that was not flushed. Always pair with
 * @ref axl_vterm_new.
 */
void
axl_vterm_free(
    AxlVterm *v  ///< handle (NULL-safe)
);

/**
 * @brief Feed a run of terminal bytes, driving the bound ops.
 *
 * Bytes are parsed incrementally; a partial escape or multi-byte sequence at the
 * end of @p bytes is held inside libvterm until the next call. Emitted text is
 * **coalesced** into cursor-relative runs that may remain buffered when this
 * returns — call @ref axl_vterm_flush at a repaint boundary to force the tail run
 * out. No-op if @p v or @p bytes is NULL.
 *
 * @p bytes is untrusted input and need not be well-formed UTF-8. What reaches
 * @ref AxlConsoleOps::output_text always is: ill-formed input becomes U+FFFD
 * REPLACEMENT CHARACTER, including the over-range codepoints (above U+10FFFF)
 * that libvterm's own decoder passes through. A consumer never has to defend
 * against a surrogate or an over-long sequence arriving in a run.
 *
 * @param v     handle.
 * @param bytes terminal bytes (xterm/VT); need not be NUL-terminated.
 * @param len   number of bytes.
 */
void
axl_vterm_feed(
    AxlVterm   *v,      ///< handle
    const char *bytes,  ///< terminal bytes (xterm/VT)
    size_t      len     ///< number of bytes
);

/**
 * @brief Flush any coalesced @ref AxlConsoleOps::output_text run. NULL-safe.
 *
 * The adapter buffers consecutive glyphs into one run and normally flushes on a
 * position jump, a pen change, or any non-text op. At a natural boundary — a
 * repaint, end of an input burst — the tail of the last run has no following op to
 * trigger its flush, so the consumer would miss it. Call this there. Idempotent.
 */
void
axl_vterm_flush(
    AxlVterm *v  ///< handle (NULL-safe)
);

/**
 * @brief Resize the terminal.
 *
 * Flushes any pending run first, then resizes the underlying parser so subsequent
 * wrapping and scrolling use the new geometry.
 *
 * @param v    handle.
 * @param rows new row count (must be > 0).
 * @param cols new column count (must be > 0).
 * @return AXL_OK on success; AXL_ERR on NULL @p v or non-positive @p rows /
 *     @p cols, in which case the terminal's current size is left unchanged.
 */
int
axl_vterm_set_size(
    AxlVterm *v,     ///< handle
    int32_t   rows,  ///< new row count (> 0)
    int32_t   cols   ///< new column count (> 0)
);

/**
 * @brief The display width, in cells, of a Unicode codepoint.
 *
 * @return the number of cells @p codepoint occupies: 2 for double-width (CJK,
 *     emoji), **0 for combining marks, other zero-width codepoints, and C0/C1
 *     control characters**, and 1 for everything else — including any value outside
 *     U+0000..U+10FFFF. There is no error return; the domain is total. A consumer
 *     decoding an @ref AxlConsoleOps::output_text run under
 *     @ref AXL_CONSOLE_CELLS_WIDTH_RESOLVED merges zero-width codepoints into the
 *     preceding cell — libvterm packs a base character and its combining sequence
 *     into a single cell, so a naive `width >= 1` split would give the mark a cell
 *     of its own. This function is the single width authority shared by producer
 *     and consumer; do not carry a second `wcwidth` table.
 *
 * @param codepoint a Unicode codepoint (values outside U+0000..U+10FFFF return 1).
 */
int
axl_vterm_char_width(
    uint32_t codepoint  ///< Unicode codepoint
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_VTERM_H */
