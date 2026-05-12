/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-console.h
 * @brief Interactive console input — single-keystroke read with timeout.
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_CONSOLE_H */
