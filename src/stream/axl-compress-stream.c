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

/* The filter rule from axl-stream.h, applied to the peer these filters move
   bytes through. A compressed stream is BINARY: a peer at any encoding but
   AXL_ENC_UTF8 transcodes it code point by code point, which on the write
   side produces an archive no decompressor can read while every call still
   reports success, and on the read side hands axl_decompress rubble and a
   failure that says nothing about the real cause. Refusing is the only
   answer that names the mistake — a filter's bytes are not characters. */
static bool
peer_is_untranscoded(AxlStream *s)
{
    return axl_stream_get_encoding(s) == AXL_ENC_UTF8;
}

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
    /* Re-checked here and not only at construction: this is the moment the
       compressed bytes actually move, and the sink can have acquired an
       encoding since. Deliberately NOT latched into finish_rc — the writer is
       still finalizable once the sink is put back, matching how the text
       wrapper treats the same mistake. */
    if (!peer_is_untranscoded(c->sink)) {
        return AXL_ERR;
    }
    c->finished = true;

    size_t      n    = 0;
    const void *data = axl_bufdata(c->accum, &n);

    void  *out = NULL;
    size_t out_len = 0;
    if (axl_compress(c->fmt, data, n, c->level, &out, &out_len) != AXL_OK) {
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
        writer_do_finish(c);  /* implicit finalize, best-effort */
    }
    axl_fclose(c->accum);           /* does NOT close c->sink */
    axl_free(c);
}

/* The compressing writer's operations, filled in ONE place — the constructor
   opens with these and axl_compress_writer_finish names them again to recover
   the context, so the two must be the same set. A helper rather than two
   literal blocks is what guarantees that: a slot added to one and not the
   other would not fail to compile, it would silently make every finish() call
   answer "not one of mine".

   `flush` stays NULL from AXL_STREAM_OPS_INIT deliberately, and it is not an
   oversight to revisit: the accumulated plaintext cannot go onward until it is
   compressed, and compressing is a one-shot that also finalizes. Pushing at
   flush time would end the stream. A NULL flush slot is contractually AXL_OK,
   which is the right answer for "nothing can move yet". */
static AxlStreamOps
compress_writer_ops(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    ops.write = compress_writer_write;
    ops.close = compress_writer_close;
    return ops;
}

AxlStream *
axl_compress_writer(AxlCompressFormat fmt, AxlStream *sink, int level)
{
    if (sink == NULL
        || (fmt != AXL_COMPRESS_GZIP && fmt != AXL_COMPRESS_ZLIB
            && fmt != AXL_COMPRESS_DEFLATE_RAW)
        || !peer_is_untranscoded(sink)) {
        return NULL;
    }

    AxlStreamOps       ops   = compress_writer_ops();
    AxlStream         *accum = axl_bufopen();
    CompressWriterCtx *c     = axl_calloc(1, sizeof(CompressWriterCtx));
    if (accum == NULL || c == NULL) {
        axl_fclose(accum);
        axl_free(c);
        return NULL;
    }

    c->sink  = sink;
    c->accum = accum;
    c->fmt   = fmt;
    c->level = level;

    AxlStream *s = axl_stream_open_custom(c, &ops, "compress");
    if (s == NULL) {
        /* A refused open never calls `close`, so releasing the context is
           ours. Spelled out rather than delegated to compress_writer_close:
           that callback FINALIZES on the way out, so calling it here would
           emit an empty compressed member into the sink for a stream that
           never existed. The file backend can delegate; this one must not. */
        axl_fclose(c->accum);
        axl_free(c);
        return NULL;
    }
    return s;
}

int
axl_compress_writer_finish(AxlStream *s)
{
    /* Identify a compressing writer by its operations. Both slots are
       file-static, so no stream built anywhere else can present them --
       the same unspoofability the old direct vtable comparison had, now
       expressed through the public getter. */
    AxlStreamOps       ops = compress_writer_ops();
    CompressWriterCtx *c   = (CompressWriterCtx *)axl_stream_ctx(s, &ops);

    if (c == NULL) {
        return AXL_ERR;
    }
    return writer_do_finish(c);
}

// ---------------------------------------------------------------------------
// Decompressing reader
// ---------------------------------------------------------------------------

AxlStream *
axl_compress_reader(AxlCompressFormat fmt, AxlStream *src)
{
    if (src == NULL
        || (fmt != AXL_COMPRESS_GZIP && fmt != AXL_COMPRESS_ZLIB
            && fmt != AXL_COMPRESS_DEFLATE_RAW)
        || !peer_is_untranscoded(src)) {
        /* Refused before the drain below, so a rejected source is left at the
           position the caller handed it over at. Otherwise "refused" and
           "read it all and failed to decode it" would be indistinguishable. */
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
