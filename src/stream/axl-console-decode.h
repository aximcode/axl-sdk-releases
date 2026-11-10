/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-decode.h
    Internal: an incremental decoder for raw bytes bound for the console.

    axl_stdout_raw / axl_stderr_raw bytes whose Shell handle IS the console
    file are shown as text (axl-stream.c). A writer may hand them over in any
    pieces -- `cat --raw` in buffer-sized chunks, `sum -z` a byte at a time --
    so the encoding and any partial character are carried from one write to
    the next:

      - the encoding is decided by the first bytes: FF FE is UTF-16LE
        (the mark dropped), EF BB BF is UTF-8 (the mark dropped), anything
        else UTF-8. In UTF-8 with nothing held, a later write that starts
        FF FE switches to UTF-16LE (a second image after the first: that
        pair is never UTF-8);
      - up to three bytes are held back between writes: an odd byte or a
        high surrogate waiting for its low half (UTF-16LE), or the start of
        an incomplete UTF-8 sequence.

    Pure: no allocation, no firmware call, so AxlTestIo drives it with exact
    bytes. NOT a public API. */

#ifndef AXL_CONSOLE_DECODE_H
#define AXL_CONSOLE_DECODE_H

#include <stddef.h>
#include <stdint.h>

/** Decoder state for one sink; all zero is a fresh decoder. */
typedef struct {
    uint8_t  mode;      ///< AXL_CONSOLE_DECODE_* below
    uint8_t  npend;     ///< bytes held back, 0..3
    uint8_t  pend[3];   ///< the held-back bytes, oldest first
} AxlConsoleDecoder;

#define AXL_CONSOLE_DECODE_UNKNOWN  0u   ///< no byte seen yet
#define AXL_CONSOLE_DECODE_UTF16LE  1u
#define AXL_CONSOLE_DECODE_UTF8     2u

/** Capacity @p out must have for axl_console_decode() of @p len bytes:
    every input byte, and each held one, can become U+FFFD (3 bytes). */
#define AXL_CONSOLE_DECODE_BOUND(len)  (3u * ((len) + 3u))

/**
 * @brief Back to a fresh decoder: the encoding undecided, nothing held.
 *
 * Bytes held back are dropped, not emitted as U+FFFD: a reset means the
 * stream they belonged to has ended (a new handle, a new launcher), and its
 * output may no longer be the console this decoder writes to.
 */
void
axl_console_decoder_reset(
    AxlConsoleDecoder *d   ///< decoder
    );

/**
 * @brief Decode the next @p len bytes of the stream to UTF-8.
 *
 * Writes the text every complete character so far makes (the held-back
 * bytes of the previous call first) to @p out, which must hold
 * AXL_CONSOLE_DECODE_BOUND(@p len) bytes, and holds back an incomplete
 * one. Not NUL-terminated. What can never decode becomes U+FFFD: an
 * unpaired UTF-16 surrogate, and each maximal subpart of ill-formed UTF-8
 * (Unicode 3.9): a valid prefix cut short is one U+FFFD (E1 80 41 is
 * U+FFFD "A"), and a byte no valid prefix begins with -- an overlong form
 * such as C0 80, a surrogate, a value past U+10FFFF -- is one each. Only
 * well-formed text, never the NUL that C0 80 would convert to, reaches the
 * console.
 *
 * @return the number of bytes written to @p out.
 */
size_t
axl_console_decode(
    AxlConsoleDecoder *d,      ///< decoder
    const void        *in,     ///< the next bytes; may be NULL when @p len is 0
    size_t             len,    ///< their count
    char              *out     ///< receives the text
    );

#endif /* AXL_CONSOLE_DECODE_H */
