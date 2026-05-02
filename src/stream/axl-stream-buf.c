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
#include "axl-stream-internal.h"
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
            axl_warning("buffer grow failed");
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
            axl_warning("buffer grow failed");
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

AxlStream *
axl_bufopen(void)
{
    AxlStream *s;
    BufCtx *b;

    b = axl_new(BufCtx);
    if (b == NULL) {
        axl_warning("allocation failed");
        return NULL;
    }

    b->data = (char *)axl_malloc(BUF_INITIAL);
    if (b->data == NULL) {
        axl_warning("buffer allocation failed");
        axl_free(b);
        return NULL;
    }

    b->len      = 0;
    b->alloc    = BUF_INITIAL;
    b->read_pos = 0;

    s = axl_stream_new();
    if (s == NULL) {
        axl_warning("allocation failed");
        axl_free(b->data);
        axl_free(b);
        return NULL;
    }

    s->ctx    = b;
    s->read   = buf_read;
    s->write  = buf_write;
    s->pread  = buf_pread;
    s->pwrite = buf_pwrite;
    s->seek   = buf_seek;
    s->tell   = buf_tell;
    s->flush  = NULL;
    s->close  = buf_close;

    return s;
}

// ---------------------------------------------------------------------------
// Buffer-specific accessors
// ---------------------------------------------------------------------------

const void *
axl_bufdata(AxlStream *s, size_t *size)
{
    BufCtx *b;

    if (s == NULL || s->ctx == NULL) {
        return NULL;
    }

    b = (BufCtx *)s->ctx;
    if (size != NULL) {
        *size = b->len;
    }
    return b->data;
}

void *
axl_bufsteal(AxlStream *s, size_t *size)
{
    BufCtx *b;
    char *data;

    if (s == NULL || s->ctx == NULL) {
        return NULL;
    }

    b = (BufCtx *)s->ctx;
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
