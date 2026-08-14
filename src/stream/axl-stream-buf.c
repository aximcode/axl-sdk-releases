/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-stream-buf.c
    In-memory buffer stream backend.
**/

#include <stddef.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>
AXL_LOG_DOMAIN("io");

#define BUF_INITIAL  256

typedef struct {
    char    *data;
    size_t  len;       /* bytes written */
    size_t  alloc;     /* allocated capacity */
    size_t  read_pos;  /* current read position */
} BufCtx;

// ---------------------------------------------------------------------------
// Buffer vtable
// ---------------------------------------------------------------------------

static axl_ssize_t
buf_write(void *ctx, const void *data, size_t count)
{
    BufCtx *b = (BufCtx *)ctx;
    size_t needed = b->len + count;

    if (needed > b->alloc) {
        size_t new_alloc = b->alloc;
        while (new_alloc < needed) {
            new_alloc *= 2;
        }
        char *new_data = (char *)axl_realloc(b->data, new_alloc);
        if (new_data == NULL) {
            axl_debug("buffer grow failed");
            return -1;
        }
        b->data  = new_data;
        b->alloc = new_alloc;
    }

    axl_memcpy(b->data + b->len, data, count);
    b->len += count;
    return (axl_ssize_t)count;
}

static axl_ssize_t
buf_read(void *ctx, void *data, size_t count)
{
    BufCtx *b = (BufCtx *)ctx;
    size_t avail;

    if (b->read_pos >= b->len) {
        return 0; /* EOF */
    }
    avail = b->len - b->read_pos;
    if (count > avail) {
        count = avail;
    }

    axl_memcpy(data, b->data + b->read_pos, count);
    b->read_pos += count;
    return (axl_ssize_t)count;
}

static axl_ssize_t
buf_pread(void *ctx, void *data, size_t count, size_t offset)
{
    BufCtx *b = (BufCtx *)ctx;

    if (offset >= b->len) {
        return 0;
    }
    if (count > b->len - offset) {
        count = b->len - offset;
    }

    axl_memcpy(data, b->data + offset, count);
    return (axl_ssize_t)count;
}

static axl_ssize_t
buf_pwrite(void *ctx, const void *data, size_t count, size_t offset)
{
    BufCtx *b = (BufCtx *)ctx;
    size_t end = offset + count;

    if (end > b->alloc) {
        size_t new_alloc = b->alloc;
        while (new_alloc < end) {
            new_alloc *= 2;
        }
        char *new_data = (char *)axl_realloc(b->data, new_alloc);
        if (new_data == NULL) {
            axl_debug("buffer grow failed");
            return -1;
        }
        b->data  = new_data;
        b->alloc = new_alloc;
    }

    axl_memcpy(b->data + offset, data, count);
    if (end > b->len) {
        b->len = end;
    }
    return (axl_ssize_t)count;
}

static int
buf_seek(void *ctx, int64_t offset, int whence)
{
    BufCtx *b = (BufCtx *)ctx;
    int64_t new_pos;

    if (whence == AXL_SEEK_SET) {
        new_pos = offset;
    } else if (whence == AXL_SEEK_CUR) {
        new_pos = (int64_t)b->read_pos + offset;
    } else if (whence == AXL_SEEK_END) {
        new_pos = (int64_t)b->len + offset;
    } else {
        return -1;
    }

    if (new_pos < 0) {
        return -1;
    }
    b->read_pos = (size_t)new_pos;
    return 0;
}

static int64_t
buf_tell(void *ctx)
{
    BufCtx *b = (BufCtx *)ctx;
    return (int64_t)b->read_pos;
}

static void
buf_close(void *ctx)
{
    BufCtx *b = (BufCtx *)ctx;
    axl_free(b->data);
    axl_free(b);
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/* The buffer backend's operations, filled in ONE place.
   The constructor opens with these and the accessors below name them again to
   recover the context, so they must be the same set -- a helper rather than
   two literal blocks is what guarantees that, and it keeps both sides driven
   by AXL_STREAM_OPS_INIT so a future slot cannot be forgotten on one side. */
static AxlStreamOps
buf_ops(void)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    /* `flush` stays as AXL_STREAM_OPS_INIT left it: an in-memory buffer holds
       nothing onward-bound, and a NULL flush slot is contractually AXL_OK. */
    ops.read   = buf_read;
    ops.write  = buf_write;
    ops.pread  = buf_pread;
    ops.pwrite = buf_pwrite;
    ops.seek   = buf_seek;
    ops.tell   = buf_tell;
    ops.close  = buf_close;
    return ops;
}

AxlStream *
axl_bufopen(void)
{
    AxlStreamOps  ops = buf_ops();
    AxlStream    *s;
    BufCtx       *b;

    b = axl_new(BufCtx);
    if (b == NULL) {
        axl_debug("allocation failed");
        return NULL;
    }

    b->data = (char *)axl_malloc(BUF_INITIAL);
    if (b->data == NULL) {
        axl_debug("buffer allocation failed");
        axl_free(b);
        return NULL;
    }

    b->len      = 0;
    b->alloc    = BUF_INITIAL;
    b->read_pos = 0;

    s = axl_stream_open_custom(b, &ops, "buffer");
    if (s == NULL) {
        /* A refused open never calls `close`, so releasing the context is
           still ours to do -- the same contract a consumer gets. */
        axl_debug("allocation failed");
        axl_free(b->data);
        axl_free(b);
        return NULL;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Buffer-specific accessors
//
// Both are keyed on the STREAM and have to recover the context from it. That
// used to require axl-stream-internal.h, which made this pair the one thing
// the public custom-backend contract could not express -- a consumer could
// build the backend but not the accessors. axl_stream_ctx closed that, and
// this file is the dogfooding proof: it names no private header and reaches
// its context exactly the way a consumer's would.
//
// A NULL check cannot stand in for the ops check -- every stream has a ctx --
// so without it a file stream's FileCtx got read as a BufCtx: `len` came out
// of whatever field landed at that offset, `data` was the firmware file
// handle, and axl_bufsteal then zeroed a 32-byte BufCtx over a 16-byte
// FileCtx, a write past the end of the allocation. All of it silent.
// ---------------------------------------------------------------------------

/* Recover the BufCtx, or NULL if @a s is not one of ours.
   The buffer backend's operations are all file-static, so no stream built
   anywhere else can present them and no consumer can even name them -- the
   same unspoofability the old direct vtable comparison had, now expressed
   through the public getter. */
static BufCtx *
buf_ctx(AxlStream *s)
{
    AxlStreamOps ops = buf_ops();

    return (BufCtx *)axl_stream_ctx(s, &ops);
}

const void *
axl_bufdata(AxlStream *s, size_t *size)
{
    BufCtx *b = buf_ctx(s);

    if (b == NULL) {
        return NULL;
    }
    if (size != NULL) {
        *size = b->len;
    }
    return b->data;
}

void *
axl_bufsteal(AxlStream *s, size_t *size)
{
    BufCtx *b = buf_ctx(s);
    char   *data;

    if (b == NULL) {
        return NULL;
    }

    data = b->data;
    if (size != NULL) {
        *size = b->len;
    }

    b->data     = NULL;
    b->len      = 0;
    b->alloc    = 0;
    b->read_pos = 0;

    return data;
}
