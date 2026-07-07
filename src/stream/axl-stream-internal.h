/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-internal.h
    Internal AxlStream layout — shared between the stream backends
    (file, buffer, text wrapper, console) and the dispatch logic in
    `axl-stream.c`. Not a public header.
**/

#ifndef AXL_STREAM_INTERNAL_H
#define AXL_STREAM_INTERNAL_H

#include <stddef.h>
#include <axl/axl-stream.h>

struct AxlStream {
    void         *ctx;
    axl_ssize_t  (*read)(void *ctx, void *buf, size_t count);
    axl_ssize_t  (*write)(void *ctx, const void *buf, size_t count);
    axl_ssize_t  (*pread)(void *ctx, void *buf, size_t count, size_t offset);
    axl_ssize_t  (*pwrite)(void *ctx, const void *buf, size_t count, size_t offset);
    int          (*seek)(void *ctx, int64_t offset, int whence);
    int64_t      (*tell)(void *ctx);
    int          (*flush)(void *ctx);
    void         (*close)(void *ctx);
    bool         eof;
    bool         err;             /* sticky error flag — axl_ferror/axl_clearerr */
    AxlEncoding  encoding;        /* default 0 = AXL_ENC_UTF8 = passthrough */
    /* On read: decoded UTF-8 bytes that didn't fit the caller's buf
       last call (max 3 bytes for a BMP UTF-8 sequence). */
    uint8_t      in_pending[3];
    size_t       in_pending_n;
    /* On write: partial UTF-8 input bytes buffered until enough
       arrive to form a complete codepoint (max 3 bytes for a 4-byte
       UTF-8 sequence still missing its tail). */
    uint8_t      out_pending[3];
    size_t       out_pending_n;
    /* Optional tee target for axl_write — when non-NULL, bytes
       successfully forwarded to this stream are also written to
       the tee. The tee's own `tee` field is intentionally ignored
       at write time so accidental loops just double-write once
       instead of recursing. Typical use: `-o:<file>` log option
       on a tool wired via axl_stream_set_stdout_tee. */
    AxlStream   *tee;
    /* Output buffering (axl_stream_set_buffering / setvbuf family).
       buf_mode defaults to AXL_STREAM_BUF_NONE (0), so an unconfigured
       stream is unbuffered and writes pass straight through. wbuf is
       lazily allocated when LINE/FULL is selected and freed on close /
       switch back to NONE. wbuf_len bytes of raw (pre-transcode) output
       are held pending a flush. */
    AxlStreamBuffering  buf_mode;
    uint8_t            *wbuf;
    size_t              wbuf_cap;
    size_t              wbuf_len;
    /* Interactive / no-EOF source hint (axl_stream_set_interactive) —
       when set, axl_text_stream_wrap skips its classify-time read-ahead
       so an interactive source is never over-read. See the
       line-discipline note in axl-stream.h. */
    bool                interactive;
};

/* Allocate a stream with the default field initializers. Used by
   every backend constructor (file, buffer, text wrapper). */
AxlStream *axl_stream_new(void);

#endif /* AXL_STREAM_INTERNAL_H */
