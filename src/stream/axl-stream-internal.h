/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-internal.h
    Internal AxlStream layout, with exactly ONE consumer: `axl-stream.c`, the
    dispatch logic and console backend that owns this struct. It exports no
    functions at all — every backend and wrapper in `src/stream/` (file,
    buffer, text, compress) builds and serves itself through the PUBLIC
    `axl_stream_open_custom` / `axl_stream_ctx` / `axl_read` and names nothing
    in here, so `src/stream/` has no construction or composition path a
    consumer lacks. See the design doc §11–§14. Not a public header.
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
    /* Diagnostic label reported by axl_stream_name(), drawn from the closed
       set the public header publishes ("file", "buffer", ...). Which of the
       two storage shapes a stream gets follows from HOW it was built, not
       from what it is: anything routed through axl_stream_open_custom gets a
       heap copy, and that is now EVERY constructor -- the only literals left
       are the five static console streams below, which fill this struct
       directly because they are objects in .data rather than allocations.
       name_owned is the discriminator, and axl_fclose frees only when it is
       set, so a literal is never handed to axl_free. NULL reads as "". */
    const char         *name;
    bool                name_owned;   /* name is a heap copy — free on close */
    /* True for every stream axl_stream_new() built, i.e. everything a
       constructor hands back. The five console streams (axl_stdout and
       friends) are STATIC objects in .data and leave this false, which is
       what stops axl_fclose from handing a non-heap address to axl_free.
       The safe default is deliberate: a new static that forgets the field
       is still never freed, whereas a flag meaning "static" would have to be
       remembered to avoid heap corruption. */
    bool                on_heap;
};

#endif /* AXL_STREAM_INTERNAL_H */
