/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-console.h
 * @brief The active console: interactive key input (single-keystroke read
 *        with timeout) and text-output mode enumeration / selection.
 *
 * `axl_stdin` (in axl-stream.h) is shell-pipe input only:
 * bytes the shell captured from the left-hand side of a `|`. Tools
 * that need to wait on a real keystroke (`y` / `n` prompts, "press
 * any key", arrow-key menus) reach for the SimpleTextInputProtocol
 * `ReadKeyStroke` path, which axl-console wraps with a timeout so
 * callers don't have to manage timer events by hand.
 *
 * @code
 * AxlKey k;
 * axl_print("Continue? [y/n]: ");
 * int rc = axl_console_read_key(5000, &k);   // 5-second timeout
 * if (rc == 0 && (k.unicode_char == 'y' || k.unicode_char == 'Y')) {
 *     // proceed
 * }
 * @endcode
 */

#ifndef AXL_CONSOLE_H
#define AXL_CONSOLE_H

#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One decoded keystroke, in the shape the UEFI Simple Text Input
/// Protocol delivers it. Exactly one of scan_code and unicode_char carries the user's intent: printable keys leave
/// scan_code = 0; special keys (arrows, F-keys, Esc, Home/End, etc.)
/// leave unicode_char = 0.
typedef struct {
    uint16_t  scan_code;     ///< UEFI scan code (0 for printable keys)
    uint16_t  unicode_char;  ///< UCS-2 character (0 for special keys)
} AxlKey;

/**
 * @brief Read one keystroke from the console, blocking up to @p timeout_ms.
 *
 * Three timeout modes:
 *   - `0`            — non-blocking. Returns -1 immediately if no
 *                      key is already buffered.
 *   - `UINT64_MAX`   — block forever until a keystroke arrives.
 *   - any other      — block at most @p timeout_ms milliseconds;
 *                      returns -1 on timeout with @p out untouched.
 *
 * Internally creates a timer event (when @p timeout_ms is finite),
 * waits on the union of {ConIn WaitForKey, timer}, then reads one
 * keystroke. The timer is closed before return.
 *
 * **At raised TPL** (called from an @ref axl_loop_attach_driver pump
 * callback at `TPL_CALLBACK`): the wait is raised-TPL-safe but
 * busy-holds `TPL_CALLBACK` until it returns. Pass a finite
 * @p timeout_ms so it self-limits; a `UINT64_MAX` block-forever read
 * there spins until a key arrives, starving the pump — so don't
 * block-forever on input from a pump callback.
 *
 * @return AXL_OK on key read (with @p out populated), -1 on timeout,
 *     no console available, or backend error.
 */
int
axl_console_read_key(
    uint64_t   timeout_ms,   ///< 0 / UINT64_MAX / millisecond bound
    AxlKey    *out           ///< [out] decoded keystroke (must be non-NULL)
);

/**
 * @brief Drain any buffered keystrokes.
 *
 * Discards every keystroke currently in the ConIn queue. Useful
 * before a prompt to eat type-ahead, or after a long-running
 * operation that may have accumulated stray keys. NULL-safe and
 * no-op on consoles where ConIn is unavailable.
 */
void
axl_console_flush_input(
    void
);

// ===================================================================
// Text-console modes (SimpleTextOutput QueryMode / SetMode) — the
// graphics-free peer of the AxlGfx display-mode API
// (`axl_gfx_query_mode` / `axl_gfx_set_mode`). This is the surface the
// UEFI Shell's `mode` command exposes: enumerate the character-cell
// geometries (80x25, 80x50, 100x31, ...) the active output console
// supports, and switch between them.
//
// All of these operate on the *active* console (`gST->ConOut`). Under an
// installed @ref AxlConsoleMirror the active console is the mirror's
// wrapper, which reports its single fixed remote geometry — so until the
// mirror is uninstalled, enumeration reflects the remote terminal size,
// not the hardware's mode list. Boot-services console state; valid until
// the firmware tears the console down.
//
// Robustness note: UEFI guarantees only mode 0 (80x25); higher modes are
// optional and a conformant console may reject `QueryMode` on a mode that
// is still within `[0, count)`. Such a mode is reported as AXL_ERR and
// skipped by the inventory-walking helpers (@ref axl_console_text_find_mode,
// @ref axl_console_text_max_mode). The current-mode index can also be
// "unset" (the firmware's pre-init state) — that is surfaced as AXL_ERR,
// not a bogus index.
// ===================================================================

/// One enumerable text-console mode (see axl_console_text_query_mode).
/// Geometry only — the character-cell pixel size and font are the
/// firmware's concern, not the caller's when picking a size.
typedef struct {
    uint32_t  index;     ///< mode number — pass to axl_console_text_set_mode
    uint32_t  columns;   ///< text columns (character cells across)
    uint32_t  rows;      ///< text rows (character cells down)
} AxlConsoleTextMode;

/**
 * @brief Number of text-console modes the active output enumerates.
 *
 * The console's `Mode->MaxMode`. Mode 0 (80x25) is the only mode UEFI
 * guarantees, so a working console returns at least 1.
 *
 * @return mode count, or 0 if there is no output console or it reports no
 *     usable modes (`MaxMode <= 0` — the signed field is clamped to 0).
 */
uint32_t
axl_console_text_mode_count(
    void
);

/**
 * @brief Query the geometry of mode @p index without switching to it.
 *
 * @return AXL_OK with @p out populated; AXL_ERR if there is no console,
 *     @p index is out of range (>= @ref axl_console_text_mode_count),
 *     @p out is NULL, or the console's `QueryMode` rejected this mode
 *     (legal for an optional mode — treat it as "skip and continue" in a
 *     walk over `[0, count)`). @p out is untouched on AXL_ERR.
 */
AXL_WARN_UNUSED int
axl_console_text_query_mode(
    uint32_t             index,  ///< mode number in [0, axl_console_text_mode_count())
    AxlConsoleTextMode  *out     ///< [out] receives the mode geometry
);

/**
 * @brief The currently-active text-console mode index.
 *
 * @return AXL_OK with @p out_index set (in `[0, count)`); AXL_ERR if there
 *     is no console, @p out_index is NULL, the console reports no mode
 *     currently set (`Mode->Mode == -1`, the firmware's pre-init state), or
 *     the reported current mode falls outside `[0, count)` (malformed
 *     firmware). @p out_index is untouched on AXL_ERR.
 */
AXL_WARN_UNUSED int
axl_console_text_current_mode(
    uint32_t  *out_index  ///< [out] receives the active mode number
);

/**
 * @brief Find the first enumerated mode matching @p columns x @p rows.
 *
 * Walks `[0, count)` in order, so when several modes share a geometry the
 * lowest-numbered match is returned. Skips any mode whose `QueryMode`
 * fails, so a malformed optional mode in range never aborts the search.
 * (Under an installed @ref AxlConsoleMirror only the mirror's single remote
 * geometry enumerates — see the section note.)
 *
 * @return AXL_OK with @p out_index set; AXL_ERR if there is no console, no
 *     mode matches, or @p out_index is NULL. @p out_index is untouched on
 *     AXL_ERR.
 */
AXL_WARN_UNUSED int
axl_console_text_find_mode(
    uint32_t   columns,    ///< desired column count
    uint32_t   rows,       ///< desired row count
    uint32_t  *out_index   ///< [out] receives the matching mode number
);

/**
 * @brief Find the largest enumerated mode.
 *
 * Ranks by cell area (`columns * rows`); ties broken by the greater
 * `columns`, then by the lower index — so the result is deterministic even
 * when modes share a geometry. The "use the biggest console" pick; its
 * `index` feeds @ref axl_console_text_set_mode. Skips modes whose
 * `QueryMode` fails or that report a zero dimension. (Under an installed
 * @ref AxlConsoleMirror only the mirror's single remote geometry
 * enumerates — see the section note.)
 *
 * @return AXL_OK with @p out populated; AXL_ERR if there is no console, no
 *     mode queried successfully, or @p out is NULL. @p out is untouched on
 *     AXL_ERR.
 */
AXL_WARN_UNUSED int
axl_console_text_max_mode(
    AxlConsoleTextMode  *out   ///< [out] receives the largest mode
);

/**
 * @brief Switch the active console to text mode @p index.
 *
 * The index is range-checked before the firmware call. On success the
 * firmware clears the screen (the cursor returns to row 0, column 0), so
 * the caller must repaint. A failed `SetMode` leaves the current mode
 * unchanged.
 *
 * @return AXL_OK on success; AXL_ERR if there is no console, @p index is
 *     out of range, or `SetMode` failed.
 */
AXL_WARN_UNUSED int
axl_console_text_set_mode(
    uint32_t  index  ///< mode number in [0, axl_console_text_mode_count())
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_H */
