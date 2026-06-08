/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-compress-stream.c
    AxlCompress stream filters — AxlStream adapters over the one-shot
    codec in src/data/axl-compress.c.

    The codec (sdefl/sinfl) is whole-buffer, so these filters buffer:
      - the writer accumulates plaintext in memory and compresses it
        into the sink when finalized;
      - the reader eagerly drains its source, decompresses, and serves
        the plaintext from an in-memory buffer stream (so it inherits
        the buffer backend's read/seek/tell for free).

    Both borrow their sink/source — neither is closed by the filter.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-compress.h>
#include <axl/axl-stream.h>
#include <axl/axl-mem.h>
#include "axl-stream-internal.h"

// ---------------------------------------------------------------------------
// Compressing writer
// ---------------------------------------------------------------------------

typedef struct {
    AxlStream        *sink;       /* borrowed destination */
    AxlStream        *accum;      /* in-memory plaintext accumulator */
    AxlCompressFormat fmt;
    int               level;
    bool              finished;
    int               finish_rc;  /* cached result, for idempotent finish */
} CompressWriterCtx;

static int
writer_do_finish(CompressWriterCtx *c)
{
    if (c->finished) {
        return c->finish_rc;
    }
    c->finished = true;

    size_t      n    = 0;
    const void *data = axl_bufdata(c->accum, &n);

    void  *out = NULL;
    size_t out_len = 0;
    if (axl_compress(c->fmt, data, n, &out, &out_len, c->level) != AXL_OK) {
        c->finish_rc = AXL_ERR;
        return AXL_ERR;
    }

    int    rc  = AXL_OK;
    size_t off = 0;
    while (off < out_len) {
        axl_ssize_t w = axl_write(c->sink, (const uint8_t *)out + off,
                                  out_len - off);
        if (w <= 0) {
            rc = AXL_ERR;
            break;
        }
        off += (size_t)w;
    }
    axl_free(out);
    c->finish_rc = rc;
    return rc;
}

static axl_ssize_t
compress_writer_write(void *vctx, const void *buf, size_t count)
{
    CompressWriterCtx *c = (CompressWriterCtx *)vctx;
    if (c->finished) {
        return -1;  /* no writes after finalize */
    }
    return axl_write(c->accum, buf, count);
}

static void
compress_writer_close(void *vctx)
{
    CompressWriterCtx *c = (CompressWriterCtx *)vctx;
    if (!c->finished) {
        (void)writer_do_finish(c);  /* implicit finalize, best-effort */
    }
    axl_fclose(c->accum);           /* does NOT close c->sink */
    axl_free(c);
}

AxlStream *
axl_compress_writer(AxlCompressFormat fmt, AxlStream *sink, int level)
{
    if (sink == NULL
        || (fmt != AXL_COMPRESS_GZIP && fmt != AXL_COMPRESS_ZLIB
            && fmt != AXL_COMPRESS_DEFLATE_RAW)) {
        return NULL;
    }

    AxlStream         *accum = axl_bufopen();
    CompressWriterCtx *c     = axl_calloc(1, sizeof(CompressWriterCtx));
    AxlStream         *s     = axl_stream_new();
    if (accum == NULL || c == NULL || s == NULL) {
        axl_fclose(accum);
        axl_free(c);
        axl_free(s);
        return NULL;
    }

    c->sink  = sink;
    c->accum = accum;
    c->fmt   = fmt;
    c->level = level;

    s->ctx   = c;
    s->write = compress_writer_write;
    s->close = compress_writer_close;
    return s;
}

int
axl_compress_writer_finish(AxlStream *s)
{
    /* Identify a compressing writer by its write vtable slot. */
    if (s == NULL || s->write != compress_writer_write) {
        return AXL_ERR;
    }
    return writer_do_finish((CompressWriterCtx *)s->ctx);
}

// ---------------------------------------------------------------------------
// Decompressing reader
// ---------------------------------------------------------------------------

AxlStream *
axl_compress_reader(AxlCompressFormat fmt, AxlStream *src)
{
    if (src == NULL
        || (fmt != AXL_COMPRESS_GZIP && fmt != AXL_COMPRESS_ZLIB
            && fmt != AXL_COMPRESS_DEFLATE_RAW)) {
        return NULL;
    }

    /* Drain the whole compressed source. */
    AxlStream *acc = axl_bufopen();
    if (acc == NULL) {
        return NULL;
    }
    uint8_t tmp[1024];
    for (;;) {
        axl_ssize_t got = axl_read(src, tmp, sizeof(tmp));
        if (got <= 0) {
            break;
        }
        if (axl_write(acc, tmp, (size_t)got) < 0) {
            axl_fclose(acc);
            return NULL;
        }
    }

    size_t      clen = 0;
    const void *comp = axl_bufdata(acc, &clen);
    void       *plain = NULL;
    size_t      plain_len = 0;
    int rc = axl_decompress(fmt, comp, clen, &plain, &plain_len);
    axl_fclose(acc);
    if (rc != AXL_OK) {
        return NULL;
    }

    /* Serve the plaintext from a rewound buffer stream. */
    AxlStream *out = axl_bufopen();
    if (out == NULL) {
        axl_free(plain);
        return NULL;
    }
    if (plain_len > 0 && axl_write(out, plain, plain_len) < 0) {
        axl_free(plain);
        axl_fclose(out);
        return NULL;
    }
    axl_free(plain);
    axl_fseek(out, 0, AXL_SEEK_SET);
    return out;
}

// ---------------------------------------------------------------------------
// gzip convenience wrappers
// ---------------------------------------------------------------------------

AxlStream *
axl_gzip_writer(AxlStream *sink, int level)
{
    return axl_compress_writer(AXL_COMPRESS_GZIP, sink, level);
}

AxlStream *
axl_gzip_reader(AxlStream *src)
{
    return axl_compress_reader(AXL_COMPRESS_GZIP, src);
}
