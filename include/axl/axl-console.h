/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-console.h
 * @brief The active console: interactive key input (single-keystroke read
 *        with timeout) and text-output mode enumeration / selection.
 *
 * Two kinds of console input live here:
 *
 *   - **Single keystroke** (`axl_console_read_key`) — for `y`/`n`
 *     prompts, "press any key", arrow-key menus: one decoded keystroke
 *     off the SimpleTextInputProtocol `ReadKeyStroke` path, wrapped with
 *     a timeout so callers don't hand-manage timer events.
 *   - **A whole line** (`axl_console_readline`) — the interactive,
 *     echoed, Backspace-editable, Enter-terminated line a tool needs
 *     when it prompts a human (`do -f`, a REPL, a "name? " prompt).
 *
 * This is distinct from `axl_stdin` / `axl_readline` (in axl-stream.h),
 * which read the shell's StdIn *handle* — the bytes captured from the
 * left of a `|` or from `< file`. When StdIn is redirected that is
 * exactly right; but when a command is typed with no redirection StdIn
 * *is* the console, which is not a byte-addressable file. `axl_readline`
 * now bridges that gap automatically (see axl_stdin_is_interactive()):
 * on an interactive StdIn it delegates to `axl_console_readline` so a
 * prompt "just works" whether piped or typed. Call `axl_console_readline`
 * directly when you always want the console (never the pipe).
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
#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// One decoded keystroke. Exactly one of scan_code and unicode_char carries
/// the user's intent: printable keys leave scan_code = 0; special keys (arrows,
/// F-keys, Esc, Home/End, etc.) leave unicode_char = 0.
///
/// @a modifiers carries the shift/lock state at the moment of the keystroke
/// (`AXL_INPUT_MOD_*` bits from `<axl/axl-input.h>`). It is populated from the
/// console's Simple Text Input **Ex** protocol when present, and is 0 on
/// consoles that don't publish it (a serial terminal / TerminalDxe carries no
/// shift bits over the wire) — treat 0 as "no modifiers / unknown".
typedef struct {
    uint16_t  scan_code;     ///< UEFI scan code (0 for printable keys)
    uint16_t  unicode_char;  ///< UCS-2 character (0 for special keys)
    uint32_t  modifiers;     ///< AXL_INPUT_MOD_* shift/lock bits; 0 if unavailable
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
 * **At raised TPL** (called from an @c axl_loop_attach_driver pump
 * callback at `TPL_CALLBACK`): the wait is raised-TPL-safe but
 * busy-holds `TPL_CALLBACK` until it returns. Pass a finite
 * @p timeout_ms so it self-limits; a `UINT64_MAX` block-forever read
 * there spins until a key arrives, starving the pump — so don't
 * block-forever on input from a pump callback.
 *
 * Reads through the console's Simple Text Input **Ex** protocol when available
 * (so @p out->modifiers reflects the shift/lock state), transparently falling
 * back to the basic protocol (modifiers = 0) on consoles without it. A
 * modifier-only "partial" keystroke (both scan_code and unicode_char 0, seen
 * only when another layer has enabled modifier exposure) is drained so this
 * always returns a real keystroke.
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

/**
 * @brief Read one line of interactive input from the console.
 *
 * The line-level peer of axl_console_read_key() — reads keystrokes
 * from the active console (ConIn), echoes printable characters to
 * ConOut, erases the last character on Backspace, and returns the
 * accumulated text (WITHOUT the terminating CR/LF) when the user
 * presses Enter. This is what a tool prompting a human wants; unlike
 * `axl_readline` on `axl_stdin`, it targets the console unconditionally
 * — never the shell pipe.
 *
 * **On Enter** a CRLF is echoed to ConOut (leaving the cursor at
 * column 0 of the next line), so the caller's next output starts
 * cleanly — do not print your own newline after the read.
 *
 * **Keys handled:** printable characters (accumulated + echoed),
 * Backspace (erases the last character, echoing `\b \b`), and Enter
 * (CR or LF — terminates). All other keys (arrows, Tab, Esc, Del,
 * function keys, Ctrl-letter) are ignored: there is no mid-line
 * cursor editing.
 *
 * On success @p out_line receives a heap-allocated, NUL-terminated
 * UTF-8 string the caller must free with axl_free() (an immediate
 * Enter yields ""). On any non-AXL_OK return @p out_line is set to NULL
 * (never left holding a stale pointer — deliberately unlike the
 * "@p out untouched on error" convention elsewhere in this header, so
 * an ignored error can't feed a dangling free).
 *
 * @p timeout_ms is a **whole-line** deadline — the total budget to
 * finish the line, not a per-keystroke bound:
 *   - `0`          — non-blocking: returns a line only if a complete
 *                    Enter-terminated line is already buffered.
 *   - `UINT64_MAX` — block until the user presses Enter.
 *   - any other    — allow at most @p timeout_ms for the whole line.
 *
 * **Timeout discards input.** UEFI ConIn cannot push keystrokes back,
 * so on expiry (or the `0` no-complete-line case) every character read
 * so far is consumed and permanently lost, and the call returns -1.
 * This makes a short timeout unsuitable for polling a half-typed line —
 * a caller that needs resumable / incremental input must use
 * @ref axl_console_read_key and buffer the keystrokes itself. A pressed
 * Ctrl-C (shell ExecutionBreak) also aborts with -1.
 *
 * **At raised TPL** (from a pump callback): pass a finite @p timeout_ms;
 * a `UINT64_MAX` read busy-holds the TPL until Enter arrives, starving
 * the pump — same caveat as @ref axl_console_read_key.
 *
 * Equivalent to @ref axl_console_readline_ex with an unbounded length
 * and echo enabled.
 *
 * @return AXL_OK with @p out_line populated; -1 on timeout, Ctrl-C, no
 *     console (ConIn unavailable), or allocation failure.
 */
AXL_WARN_UNUSED int
axl_console_readline(
    uint64_t   timeout_ms,   ///< 0 / UINT64_MAX / whole-line millisecond deadline
    char     **out_line      ///< [out] heap UTF-8 line (caller frees); must be non-NULL
);

/**
 * @brief Read one interactive line, with a length cap and echo control.
 *
 * The full form behind @ref axl_console_readline. Same key handling,
 * Enter/CRLF echo, @p timeout_ms (whole-line deadline) and
 * @p out_line ownership semantics; adds:
 *   - @p max_len — maximum number of characters accepted into the line
 *     (BMP code units, i.e. keystrokes). Keystrokes past the cap are
 *     ignored (not echoed, not stored); Backspace and Enter still work.
 *     `0` means unbounded. The returned UTF-8 string may exceed
 *     @p max_len *bytes* when input contains multi-byte characters —
 *     the cap counts characters, not bytes.
 *   - @p echo — when false, typed characters are not echoed to ConOut
 *     (password-style entry) and Backspace edits the buffer silently;
 *     the terminating CRLF on Enter is **still** echoed so the next
 *     prompt isn't glued to the hidden line. When true, behaves like
 *     @ref axl_console_readline.
 *
 * @return AXL_OK with @p out_line populated; -1 on timeout, Ctrl-C, no
 *     console (ConIn unavailable), or allocation failure.
 */
AXL_WARN_UNUSED int
axl_console_readline_ex(
    uint64_t   timeout_ms,   ///< 0 / UINT64_MAX / whole-line millisecond deadline
    size_t     max_len,      ///< max characters accepted (0 = unbounded)
    bool       echo,         ///< echo typed characters to ConOut (false = hidden)
    char     **out_line      ///< [out] heap UTF-8 line (caller frees); must be non-NULL
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

// ===================================================================
// Output pagination (the shell `-b` page-break convention)
// ===================================================================

/**
 * @brief Enable or disable the shell's page-break (screen-at-a-time) mode.
 *
 * Paging is a **shell service**, not something the SDK reimplements. AXL
 * tools write to the UEFI console (gST->ConOut), which the shell wraps;
 * this call flips the shell's page-break switch
 * (EFI_SHELL_PROTOCOL.EnablePageBreak / DisablePageBreak) so the shell
 * itself paginates that output — its own `-- More --` prompt, key
 * reading, screen geometry, and (crucially) its own redirect/interactive
 * detection, so a redirected or piped stream is never paused.
 *
 * @ref axl_args_run recognizes a universal `-b` / `--page` option and
 * toggles this on for the duration of a tool's run, so most tools get
 * paging for free without declaring the flag — a tool that declares its
 * own `-b` keeps it (the universal option defers). Tools rarely need to
 * call this directly.
 *
 * **No-op when no page-break service is reachable:** the legacy EFI 1.x
 * shell publishes SHELL_ENVIRONMENT rather than EFI_SHELL_PROTOCOL, and a
 * non-shell context has no shell at all — in both, enabling is a silent
 * no-op (output is simply not paginated). Safe to call in any context.
 *
 * @param enable  true to enable page break, false to disable.
 */
void
axl_console_set_page_break(
    bool  enable
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_H */
