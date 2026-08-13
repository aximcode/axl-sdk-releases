/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-json-io.c
    Sources and sinks — where a parse reads from, and where a write goes.

    Two mirrored vtables and their built-in implementations, kept together
    because they are one idea seen from both ends. What they buy is Jansson's
    eight I/O entry points (`loads`/`loadb`/`loadf`/`load_callback`,
    `dumps`/`dumpb`/`dumpf`/`dump_callback`) without eight functions each
    carrying its own flags-and-error plumbing.

    The reader and writer proper live in axl-json-parse.c and
    axl-json-build.c; this file only describes the ends of the pipe.
**/

#include <axl/axl-json.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-string.h>

#include "axl-json-internal.h"

/* Mark a sink as unusable. axl_json_writer_init_sink() refuses a sink with no
   write function and reports AXL_JSON_ERR_INVALID_ARGUMENT, so a malformed
   initializer call fails at INIT, with the code that names it a caller bug,
   rather than at the first write with the code that names an I/O one. */
static void
sink_refuse(AxlJsonSink *snk)
{
    snk->write = NULL;
    snk->ctx   = NULL;
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

/* Where a pull-mode accumulator starts when the source offers no hint.
   It only sets the number of doublings, not the outcome: a document larger
   than this is not a problem, just one more realloc. */
#define JSON_SOURCE_SEED  4096

void
axl_json_source_init_mem(AxlJsonSource *src, const char *data, size_t len)
{
    if (src == NULL) {
        return;
    }
    src->data = data;
    src->len  = len;
    src->read = NULL;
    src->ctx  = NULL;
    src->hint = len;
}

/* An exact mirror of AxlJsonReadFn, which is why that typedef is signed:
   axl_read() already means bytes / 0 = EOF / -1 = error, so the stream source
   forwards and holds no policy of its own -- including for a NULL stream,
   which axl_read already answers with -1. */
static axl_ssize_t
source_stream_read(void *ctx, void *buf, size_t max)
{
    return axl_read((AxlStream *)ctx, buf, max);
}

void
axl_json_source_init_stream(AxlJsonSource *src, AxlStream *s)
{
    if (src == NULL) {
        return;
    }
    /* Refused HERE, exactly as axl_json_sink_init_stream refuses a NULL
       stream. Left to the read it would surface as AXL_JSON_ERR_IO -- the
       code for the world being broken -- when what happened is a caller bug,
       which is AXL_JSON_ERR_INVALID_ARGUMENT. The two sides of the mirror
       must not disagree about that. */
    if (s == NULL) {
        src->data = NULL;
        src->len  = 0;
        src->read = NULL;
        src->ctx  = NULL;
        src->hint = 0;
        return;
    }
    src->data = NULL;
    src->len  = 0;
    src->read = source_stream_read;
    src->ctx  = s;
    /* No seek probe to size the stream up front. It would save reallocs on a
       regular file and cost correctness on everything else -- a socket or a
       console is not seekable, and a stream that "succeeds" at a meaningless
       seek would be left at the wrong position. Callers who KNOW the size say
       so through axl_json_source_init_callback()'s hint. */
    src->hint = 0;
}

void
axl_json_source_init_callback(AxlJsonSource *src, AxlJsonReadFn fn, void *ctx,
                              size_t hint)
{
    if (src == NULL) {
        return;
    }
    src->data = NULL;
    src->len  = 0;
    src->read = fn;
    src->ctx  = ctx;
    src->hint = hint;
}

/* Read a pull source to EOF into one growing buffer.
 *
 * @return the buffer (caller owns) with *out_len set, or NULL with *out_code
 *     set to the failure.
 *
 * The whole document is retained on purpose and cannot be otherwise: tokens
 * are int32_t OFFSETS, so every byte they index has to stay resident at a
 * stable position for the reader's whole life. A sliding window would break
 * the token model, not merely complicate it -- which is why the streaming win
 * here is ergonomic (one object to free) and not spatial.
 */
static char *
source_slurp(const AxlJsonSource *src, size_t *out_len,
             AxlJsonErrorCode *out_code)
{
    size_t cap = (src->hint > 0) ? src->hint : JSON_SOURCE_SEED;
    size_t len = 0;
    char  *buf = axl_malloc(cap);

    if (buf == NULL) {
        *out_code = AXL_JSON_ERR_NO_MEMORY;
        return NULL;
    }

    for (;;) {
        if (len == cap) {
            char *grown;

            if (cap > (size_t)-1 / 2) {
                axl_free(buf);
                *out_code = AXL_JSON_ERR_NO_MEMORY;
                return NULL;
            }
            grown = axl_realloc(buf, cap * 2);
            if (grown == NULL) {
                axl_free(buf);
                *out_code = AXL_JSON_ERR_NO_MEMORY;
                return NULL;
            }
            buf  = grown;
            cap *= 2;
        }

        const axl_ssize_t n = src->read(src->ctx, buf + len, cap - len);

        if (n == 0) {
            break;                              /* end of input */
        }
        /* A count larger than the room offered is a caller's read function
           misbehaving, and believing it would walk the accumulator past the
           allocation -- a heap overflow driven from outside the library. It
           is refused for the same reason the writer refuses an over-reporting
           sink, and it costs one comparison. */
        if (n < 0 || (size_t)n > cap - len) {
            axl_free(buf);
            *out_code = AXL_JSON_ERR_IO;
            return NULL;
        }
        len += (size_t)n;
    }

    if (len == 0) {
        /* Not INVALID_ARGUMENT: nobody passed a bad argument. A source that
           yields nothing is a document that has not arrived, which is what
           INCOMPLETE means and what tells the caller to come back. */
        axl_free(buf);
        *out_code = AXL_JSON_ERR_INCOMPLETE;
        return NULL;
    }

    /* Give back the doubling slack. The accumulator can hold up to twice the
       document -- and does so exactly when the hint was RIGHT, since a hint
       equal to the document fills the buffer, trips the grow check, and only
       then reads the 0 that ends the loop. The reader keeps this buffer for
       its whole life, so on a firmware heap the excess is worth one realloc.
       A shrink that fails is not a failure: the oversized buffer is still
       correct, and `buf` stays valid because axl_realloc leaves it alone. */
    if (len < cap) {
        char *trimmed = axl_realloc(buf, len);

        if (trimmed != NULL) {
            buf = trimmed;
        }
    }

    *out_len = len;
    return buf;
}

bool
axl_json_parse_source(const AxlJsonSource *src, AxlJsonFlags flags,
                      AxlJsonReader *r)
{
    AxlJsonErrorCode code = AXL_JSON_ERR_UNKNOWN;
    size_t           len  = 0;
    char            *buf;

    if (r == NULL) {
        return false;
    }
    if (src == NULL || (src->data == NULL && src->read == NULL)) {
        axl_json_reader_fail(r, AXL_JSON_ERR_INVALID_ARGUMENT);
        return false;
    }

    /* The contiguous view is the fast path and stays exactly what it was: the
       parser reads the caller's bytes in place, tokens index straight into
       them, and nothing is allocated or copied. */
    if (src->data != NULL) {
        return axl_json_parse(src->data, src->len, flags, r);
    }

    buf = source_slurp(src, &len, &code);
    if (buf == NULL) {
        axl_json_reader_fail(r, code);
        return false;
    }

    if (!axl_json_parse(buf, len, flags, r)) {
        /* Keep the parse error -- position, line, column and all -- and drop
           only the view, because the bytes it points at are about to go. The
           reader comes back json == NULL / owns_json == false exactly as it
           already comes back tokens == NULL / owns_tokens == false, so a
           failed streamed parse leaves nothing for the caller to free and
           nothing dangling for them to read. */
        axl_free(buf);
        r->json      = NULL;
        r->json_len  = 0;
        r->owns_json = false;
        return false;
    }

    r->json      = buf;
    r->owns_json = true;
    return true;
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

/* An AxlString grows on demand, so the only way it declines a write is that
   the allocator did. That is a broken sink, not a full one: -1, which halts
   the writer, rather than a short count, which would merely be counted and
   reported at finish. */
static axl_ssize_t
sink_string_write(void *ctx, const char *buf, size_t len)
{
    AxlString *out = (AxlString *)ctx;

    if (out == NULL || axl_string_append_len(out, buf, len) != AXL_OK) {
        return -1;
    }
    return (axl_ssize_t)len;
}

void
axl_json_sink_init_string(AxlJsonSink *snk, AxlString *out)
{
    if (snk == NULL) {
        return;
    }
    snk->write = sink_string_write;
    snk->ctx   = out;
}

/* The one sink for which "full" is an ordinary outcome. It stores what fits,
   returns the SHORT COUNT, and never -1: capacity is a fact about the buffer,
   not a malfunction, and latching on it would halt the writer at the first
   fragment over and strand axl_json_writer_needed() there. The writer sums the
   difference and reports it once, at finish. */
static axl_ssize_t
sink_buffer_write(void *ctx, const char *buf, size_t len)
{
    AxlJsonBufSink *st = (AxlJsonBufSink *)ctx;
    size_t          room;

    if (st == NULL) {
        return -1;
    }
    room = (st->used < st->size) ? st->size - st->used : 0;
    if (len > room) {
        len = room;
    }
    if (len > 0) {
        axl_memcpy(st->buf + st->used, buf, len);
        st->used += len;
    }
    return (axl_ssize_t)len;
}

void
axl_json_sink_init_buffer(AxlJsonSink *snk, AxlJsonBufSink *state,
                          char *buf, size_t size)
{
    if (snk == NULL) {
        return;
    }
    /* No state to keep `used` in, or a capacity with no buffer under it --
       both are caller bugs that would otherwise surface as a NULL dereference
       on the first write, which under UEFI is a hang rather than a report. */
    if (state == NULL || (buf == NULL && size > 0)) {
        sink_refuse(snk);
        return;
    }
    state->buf  = buf;
    state->size = size;
    state->used = 0;
    snk->write  = sink_buffer_write;
    snk->ctx    = state;
}

/* axl_fwrite, not axl_write, and the difference is the whole contract.
 *
 * A backend is free to transfer LESS than asked -- a socket or a ring buffer
 * routinely does, and axl_stream_open_custom makes that legal for anyone's
 * backend. axl_write is the raw dispatch and hands that short count straight
 * back; axl_fwrite is the layer that loops until the request is satisfied,
 * which is what "write these bytes" has to mean here. Issuing the raw call and
 * treating short as a malfunction truncated the document at the first fragment
 * a one-byte-at-a-time backend did not swallow whole.
 *
 * What remains after the loop gives up is genuinely two different things, and
 * axl_ferror() is the stream's own way of telling them apart:
 *
 *   - error set -- the backend FAILED. -1, which halts the writer.
 *   - error clear -- the backend accepted nothing and would not be drained by
 *     retrying while this call holds the CPU. That is "full", not "broken", so
 *     it is reported as a short count: the writer keeps counting and
 *     axl_json_writer_finish() reports the truncation once.
 *
 * Before custom backends existed, no built-in stream could short-transfer and
 * the distinction had no way to arise -- which is exactly why the old mapping
 * looked correct and was untestable.
 */
static axl_ssize_t
sink_stream_write(void *ctx, const char *buf, size_t len)
{
    AxlStream *s = (AxlStream *)ctx;
    size_t     n;

    if (s == NULL) {
        return -1;
    }
    n = axl_fwrite(buf, 1, len, s);
    if (n == len) {
        return (axl_ssize_t)len;
    }
    return axl_ferror(s) ? -1 : (axl_ssize_t)n;
}

void
axl_json_sink_init_stream(AxlJsonSink *snk, AxlStream *s)
{
    if (snk == NULL) {
        return;
    }
    /* A stream that cannot be written is a caller bug, and one the write path
       cannot diagnose for itself: axl_write answers -1 for a NULL write slot
       at its own guard, BEFORE anything sets the stream's error flag, so
       axl_ferror() stays false and the refusal is indistinguishable from a
       backend that merely took nothing. Asking the stream up front is both
       the honest answer -- INVALID_ARGUMENT, not IO -- and the only one
       available. */
    if (s == NULL || !axl_stream_can_write(s)) {
        sink_refuse(snk);
        return;
    }
    snk->write = sink_stream_write;
    snk->ctx   = s;
}

void
axl_json_sink_init_callback(AxlJsonSink *snk, AxlJsonWriteFn fn, void *ctx)
{
    if (snk == NULL) {
        return;
    }
    /* A NULL fn lands in the same refusal path as every other malformed sink
       -- assigning it here and letting the writer's init check catch it keeps
       ONE place that decides what an unusable sink looks like. */
    snk->write = fn;
    snk->ctx   = ctx;
}
