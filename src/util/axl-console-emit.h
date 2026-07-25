/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-emit.h
    Shared SIMPLE_TEXT_OUTPUT -> AxlConsoleOps translation engine (internal).

    Both console *output* producers observe the same firmware protocol
    (`EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`) and must report the same @ref AxlConsoleOps
    for a given call, so a consumer bound to one renders the other unchanged. This
    engine is that translation, factored out of the two producers so they emit
    identically by construction rather than by two hand-kept copies:

    - `axl-console-tap` (swap strategy: wraps `gST->ConOut`), and
    - `axl-console-device` (take-over strategy: installs a fresh ConsoleOut device).

    An @ref AxlConsoleEmit owns the emission-side state a producer must maintain:
    the consumer vtable, the producer's owned `SIMPLE_TEXT_OUTPUT_MODE` (cursor /
    attribute / visibility the guest reads back), the alternate-screen state
    machine, and the text-row tracker its auto-leave heuristic uses. Geometry is
    NOT held here: its resolution differs per producer (the tap falls back to the
    physical console it wrapped; the device advertises its own), so the producer
    computes it and passes it in.

    Not a public header. The producers include it; consumers never see it.
**/

#ifndef AXL_CONSOLE_EMIT_H
#define AXL_CONSOLE_EMIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-console-ops.h>
#include <uefi/axl-uefi.h>

/**
 * @brief Emission-side state shared by the console output producers.
 *
 * Zero-initialize, then @ref axl_console_emit_init. @c mode points at the
 * producer's own `SIMPLE_TEXT_OUTPUT_MODE` (the engine advances its cursor /
 * attribute / visibility); the producer owns that struct's storage and its
 * one-time seeding (MaxMode, initial Mode, etc.).
 */
typedef struct {
    const AxlConsoleOps     *ops;             ///< consumer vtable (borrowed)
    void                    *user;            ///< consumer context
    SIMPLE_TEXT_OUTPUT_MODE  *mode;           ///< producer's owned Mode (advanced here)
    bool                     auto_alt_screen; ///< bracket a full-screen app: enter on a backward cursor jump after a clear, leave on a newline
    bool                     alt_active;      ///< an ALT_SCREEN(true) was reported without a matching false
    bool                     saw_clear;       ///< a clear/set_mode happened; a later backward cursor jump marks a TUI repaint (enter)
} AxlConsoleEmit;

/**
 * @brief Bind a consumer vtable + owned Mode to the engine. Call at producer
 *     init; does NOT emit (report the cell rule separately, once the producer's
 *     output path is live, via @ref axl_console_emit_report_cell_rule).
 */
void
axl_console_emit_init(
    AxlConsoleEmit          *e,
    const AxlConsoleOps     *ops,
    void                    *user,
    SIMPLE_TEXT_OUTPUT_MODE *mode,
    bool                     auto_alt_screen
);

/**
 * @brief Report this producer's cell-boundary rule once, at bind.
 *
 * Emits @ref AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT through @c set_cell_rule:
 * `SIMPLE_TEXT_OUTPUT` is one-cell-per-codepoint by construction, true for both
 * producers. Kept distinct from @ref axl_console_emit_init so a producer can
 * report it at the exact point its output path goes live.
 */
void
axl_console_emit_report_cell_rule(AxlConsoleEmit *e);

/**
 * @brief Emit a UCS-2 run: report it as @c output_text, advance the owned Mode's
 *     cursor, and run the alt-screen auto-leave heuristic.
 *
 * @param res_cols / @param res_rows  RESOLVED geometry (the producer's configured
 *     size, or its physical fallback) — used for cursor autowrap / scroll clamp.
 *     0 disables that dimension's wrap/clamp.
 *
 * Text is sanitized exactly as the firmware sanitizes it (C0 controls except
 * `{NUL,BS,TAB,LF,CR}` become `'?'`; `>= 0x20` including non-ASCII BMP passes) and
 * chunked to UTF-8. No-op for a NULL string.
 */
void
axl_console_emit_text(
    AxlConsoleEmit *e,
    const CHAR16   *s,
    uint32_t        res_cols,
    uint32_t        res_rows
);

/** @brief SetAttribute: latch it into the owned Mode and report @c set_pen
 *     (UEFI nibble -> INDEXED fg 0..15 / bg 0..7). */
void
axl_console_emit_set_attribute(AxlConsoleEmit *e, uint32_t attribute);

/** @brief SetMode: record the mode in the owned Mode, home the cursor, report
 *     @c set_mode. */
void
axl_console_emit_set_mode(AxlConsoleEmit *e, uint32_t mode_number);

/** @brief Report a new grid size to the consumer (@ref AxlConsoleOps::resize).
    Callers update their advertised geometry BEFORE calling, so a consumer that
    reads the size instead of trusting the arguments sees the same answer. */
void
axl_console_emit_resize(AxlConsoleEmit *e, uint32_t cols, uint32_t rows);

/** @brief ClearScreen: report @c clear_screen, home the owned Mode's cursor, and
 *     (if @c auto_alt_screen) arm the alt-screen enter — a following backward
 *     cursor jump is then read as a full-screen repaint. */
void
axl_console_emit_clear_screen(AxlConsoleEmit *e);

/** @brief SetCursorPosition: publish it into the owned Mode and report
 *     @c set_cursor (row, col order). If @c auto_alt_screen and this is a backward
 *     row jump after a clear, enter the alt-screen (a full-screen app repainting). */
void
axl_console_emit_set_cursor(AxlConsoleEmit *e, uint32_t column, uint32_t row);

/** @brief EnableCursor: set the owned Mode's visibility and report
 *     @c set_term_prop(CURSOR_VISIBLE). */
void
axl_console_emit_enable_cursor(AxlConsoleEmit *e, bool visible);

/** @brief Reset / SetMode-style cursor home: owned Mode CursorColumn/Row = 0.
 *     Emits nothing (the producer handles any passthrough). */
void
axl_console_emit_home_cursor(AxlConsoleEmit *e);

/** @brief Enter the alternate screen: report `set_term_prop(ALT_SCREEN, true)`
 *     once (idempotent). NULL-safe. */
void
axl_console_emit_enter_alt_screen(AxlConsoleEmit *e);

/** @brief Leave the alternate screen: report `set_term_prop(ALT_SCREEN, false)`
 *     once (idempotent). NULL-safe. */
void
axl_console_emit_leave_alt_screen(AxlConsoleEmit *e);

/** @brief Whether an enter was reported without a matching leave. NULL-safe. */
bool
axl_console_emit_in_alt_screen(const AxlConsoleEmit *e);

/** @brief Per-session reset of the emission state: leave the alt-screen (emits
 *     the false prop if it was active) and clear the alt-screen detect state.
 *     NULL-safe. */
void
axl_console_emit_reset(AxlConsoleEmit *e);

#endif /* AXL_CONSOLE_EMIT_H */
