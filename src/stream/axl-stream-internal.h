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
};

/* Allocate a stream with the default field initializers. Used by
   every backend constructor (file, buffer, text wrapper). */
AxlStream *axl_stream_new(void);

#endif /* AXL_STREAM_INTERNAL_H */
