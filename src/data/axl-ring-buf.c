/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ring-buf.c
    AxlRingBuf — byte-oriented ring buffer with power-of-2 sizing.

    Three API layers, each building on the one below:
      Layer 1 (Bytes):    push, pop, peek, discard, regions
      Layer 2 (Messages): push_msg, pop_msg, peek_msg (variable-size)
      Layer 3 (Elements): push_elem, pop_elem, peek_elem (fixed-size)

    Monotonically increasing uint32_t indices with mask-based wrapping
    (kfifo-style). Supports reject-on-full and overwrite-on-full modes.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-ring-buf.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("data");

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static uint32_t
next_pow2(uint32_t v)
{
    if (v == 0) {
        return 1;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static bool
is_pow2(uint32_t v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

static uint32_t
ring_readable(const AxlRingBuf *rb)
{
    return rb->write_pos - rb->read_pos;
}

static uint32_t
ring_writable(AxlRingBuf *rb)
{
    return rb->size - ring_readable(rb);
}

static void
ring_copy_in(
    AxlRingBuf *rb,
    const void *data,
    uint32_t    len,
    uint32_t    pos
    )
{
    uint32_t offset = pos & rb->mask;
    uint32_t first = rb->size - offset;

    if (first >= len) {
        axl_memcpy(rb->buf + offset, data, len);
    } else {
        axl_memcpy(rb->buf + offset, data, first);
        axl_memcpy(rb->buf, (const uint8_t *)data + first, len - first);
    }
}

static void
ring_copy_out(
    const AxlRingBuf *rb,
    void       *dest,
    uint32_t    len,
    uint32_t    pos
    )
{
    uint32_t offset = pos & rb->mask;
    uint32_t first = rb->size - offset;

    if (first >= len) {
        axl_memcpy(dest, rb->buf + offset, len);
    } else {
        axl_memcpy(dest, rb->buf + offset, first);
        axl_memcpy((uint8_t *)dest + first, rb->buf, len - first);
    }
}

static void
default_buf_free(void *ptr)
{
    axl_free(ptr);
}

// ===========================================================================
// Base: Lifecycle
// ===========================================================================

int
axl_ring_buf_init(
    AxlRingBuf *rb,
    void       *buf,
    uint32_t    size,
    uint32_t    flags,
    void      (*buf_free_fn)(void *)
    )
{
    if (rb == NULL || buf == NULL || !is_pow2(size)) {
        return AXL_ERR;
    }

    rb->buf          = (uint8_t *)buf;
    rb->size         = size;
    rb->mask         = size - 1;
    rb->read_pos     = 0;
    rb->write_pos    = 0;
    rb->flags        = flags;
    rb->elem_size    = 0;
    rb->pushes_total = 0;
    rb->pushes_lost  = 0;
    rb->buf_free     = buf_free_fn;
    return AXL_OK;
}

int
axl_ring_buf_init_fixed(
    AxlRingBuf *rb,
    void       *buf,
    uint32_t    size,
    uint32_t    elem_size,
    uint32_t    flags,
    void      (*buf_free_fn)(void *)
    )
{
    if (elem_size == 0) {
        return AXL_ERR;
    }

    int rc = axl_ring_buf_init(rb, buf, size, flags, buf_free_fn);
    if (rc != 0) {
        return rc;
    }

    rb->elem_size = elem_size;
    return AXL_OK;
}

void
axl_ring_buf_deinit(AxlRingBuf *rb)
{
    if (rb == NULL) {
        return;
    }

    if (rb->buf_free != NULL && rb->buf != NULL) {
        rb->buf_free(rb->buf);
    }

    axl_memset(rb, 0, sizeof(*rb));
}

AxlRingBuf *
axl_ring_buf_new(uint32_t min_size)
{
    return axl_ring_buf_new_full(min_size, 0);
}

AxlRingBuf *
axl_ring_buf_new_full(
    uint32_t min_size,
    uint32_t flags
    )
{
    if (min_size == 0) {
        return NULL;
    }

    uint32_t size = next_pow2(min_size);
    if (size == 0) {
        return NULL;
    }

    AxlRingBuf *rb = axl_calloc(1, sizeof(AxlRingBuf));
    if (rb == NULL) {
        axl_warning(
            "axl_ring_buf_new_full: OOM allocating header (%zu bytes)",
            sizeof(AxlRingBuf)
            );
        return NULL;
    }

    uint8_t *buf = axl_malloc(size);
    if (buf == NULL) {
        axl_warning(
            "axl_ring_buf_new_full: OOM allocating %u-byte storage buffer",
            (unsigned)size
            );
        axl_free(rb);
        return NULL;
    }

    axl_ring_buf_init(rb, buf, size, flags, default_buf_free);
    return rb;
}

AxlRingBuf *
axl_ring_buf_new_fixed(
    uint32_t min_size,
    uint32_t elem_size,
    uint32_t flags
    )
{
    if (elem_size == 0) {
        return NULL;
    }

    AxlRingBuf *rb = axl_ring_buf_new_full(min_size, flags);
    if (rb == NULL) {
        return NULL;
    }

    rb->elem_size = elem_size;
    return rb;
}

AxlRingBuf *
axl_ring_buf_new_with_buffer(
    void     *buf,
    uint32_t  size,
    uint32_t  flags
    )
{
    if (buf == NULL || !is_pow2(size)) {
        return NULL;
    }

    AxlRingBuf *rb = axl_calloc(1, sizeof(AxlRingBuf));
    if (rb == NULL) {
        axl_warning(
            "axl_ring_buf_new_with_buffer: OOM allocating header (%zu bytes)",
            sizeof(AxlRingBuf)
            );
        return NULL;
    }

    axl_ring_buf_init(rb, buf, size, flags, NULL);
    return rb;
}

void
axl_ring_buf_free(AxlRingBuf *rb)
{
    if (rb == NULL) {
        return;
    }

    axl_ring_buf_deinit(rb);
    axl_free(rb);
}

// ===========================================================================
// Layer 1: Bytes
// ===========================================================================

uint32_t
axl_ring_buf_push(
    AxlRingBuf *rb,
    const void *data,
    uint32_t    len
    )
{
    if (rb == NULL || data == NULL || len == 0) {
        return 0;
    }

    uint32_t orig_len = len;
    uint32_t writable = ring_writable(rb);

    rb->pushes_total += orig_len;

    if (rb->flags & AXL_RING_BUF_OVERWRITE) {
        uint32_t input_dropped = 0;
        if (len > rb->size) {
            input_dropped = len - rb->size;
            data = (const uint8_t *)data + input_dropped;
            len = rb->size;
        }

        uint32_t displaced_old = 0;
        if (len > writable) {
            displaced_old = len - writable;
            rb->read_pos += displaced_old;
        }

        ring_copy_in(rb, data, len, rb->write_pos);
        rb->write_pos += len;
        rb->pushes_lost += (uint64_t)input_dropped + displaced_old;
        return len;
    }

    if (len > writable) {
        len = writable;
    }

    if (len == 0) {
        rb->pushes_lost += orig_len;
        return 0;
    }

    ring_copy_in(rb, data, len, rb->write_pos);
    rb->write_pos += len;
    rb->pushes_lost += (orig_len - len);
    return len;
}

uint32_t
axl_ring_buf_pop(
    AxlRingBuf *rb,
    void       *dest,
    uint32_t    len
    )
{
    if (rb == NULL || dest == NULL || len == 0) {
        return 0;
    }

    uint32_t readable = ring_readable(rb);
    if (len > readable) {
        len = readable;
    }

    if (len == 0) {
        return 0;
    }

    ring_copy_out(rb, dest, len, rb->read_pos);
    rb->read_pos += len;
    return len;
}

uint32_t
axl_ring_buf_peek(
    const AxlRingBuf *rb,
    void       *dest,
    uint32_t    len
    )
{
    if (rb == NULL || dest == NULL || len == 0) {
        return 0;
    }

    uint32_t readable = ring_readable(rb);
    if (len > readable) {
        len = readable;
    }

    if (len == 0) {
        return 0;
    }

    ring_copy_out(rb, dest, len, rb->read_pos);
    return len;
}

uint32_t
axl_ring_buf_discard(
    AxlRingBuf *rb,
    uint32_t    len
    )
{
    if (rb == NULL || len == 0) {
        return 0;
    }

    uint32_t readable = ring_readable(rb);
    if (len > readable) {
        len = readable;
    }

    rb->read_pos += len;
    return len;
}

uint32_t
axl_ring_buf_peek_regions(
    AxlRingBuf       *rb,
    AxlRingBufRegion  regions[2]
    )
{
    if (rb == NULL || regions == NULL) {
        return 0;
    }

    uint32_t readable = ring_readable(rb);
    if (readable == 0) {
        return 0;
    }

    uint32_t offset = rb->read_pos & rb->mask;
    uint32_t first = rb->size - offset;

    if (first >= readable) {
        regions[0].data = rb->buf + offset;
        regions[0].len  = readable;
        return 1;
    }

    regions[0].data = rb->buf + offset;
    regions[0].len  = first;
    regions[1].data = rb->buf;
    regions[1].len  = readable - first;
    return 2;
}

uint32_t
axl_ring_buf_push_regions(
    AxlRingBuf       *rb,
    AxlRingBufRegion  regions[2]
    )
{
    if (rb == NULL || regions == NULL) {
        return 0;
    }

    uint32_t writable = ring_writable(rb);
    if (writable == 0) {
        return 0;
    }

    uint32_t offset = rb->write_pos & rb->mask;
    uint32_t first = rb->size - offset;

    if (first >= writable) {
        regions[0].data = rb->buf + offset;
        regions[0].len  = writable;
        return 1;
    }

    regions[0].data = rb->buf + offset;
    regions[0].len  = first;
    regions[1].data = rb->buf;
    regions[1].len  = writable - first;
    return 2;
}

void
axl_ring_buf_pop_advance(
    AxlRingBuf *rb,
    uint32_t    len
    )
{
    if (rb == NULL) {
        return;
    }

    uint32_t readable = ring_readable(rb);
    if (len > readable) {
        len = readable;
    }

    rb->read_pos += len;
}

void
axl_ring_buf_push_advance(
    AxlRingBuf *rb,
    uint32_t    len
    )
{
    if (rb == NULL) {
        return;
    }

    uint32_t orig_len = len;
    uint32_t writable = ring_writable(rb);
    if (len > writable) {
        len = writable;
    }

    rb->write_pos += len;
    rb->pushes_total += orig_len;
    rb->pushes_lost  += (orig_len - len);
}

// ===========================================================================
// Layer 2: Messages
// ===========================================================================

int
axl_ring_buf_push_msg(
    AxlRingBuf *rb,
    const void *data,
    uint32_t    len
    )
{
    if (rb == NULL || (data == NULL && len > 0)) {
        return AXL_ERR;
    }

    uint32_t total = (uint32_t)sizeof(uint32_t) + len;

    if (!(rb->flags & AXL_RING_BUF_OVERWRITE)) {
        if (axl_ring_buf_get_writable(rb) < total) {
            /* Reject: count the whole message (header + payload) as
             * attempted-but-lost so the call shows up in stats. */
            rb->pushes_total += total;
            rb->pushes_lost  += total;
            return AXL_ERR;
        }
    }

    /* Successful path: the underlying push() calls track bytes. */
    axl_ring_buf_push(rb, &len, (uint32_t)sizeof(uint32_t));
    if (len > 0) {
        axl_ring_buf_push(rb, data, len);
    }

    return AXL_OK;
}

/**
 * Internal: read or peek the next message.
 * If consume is true, advances read_pos (pop). Otherwise leaves it (peek).
 */
static int
ring_buf_msg_read_internal(
    AxlRingBuf *rb,
    void       *dest,
    uint32_t    max_len,
    uint32_t   *actual_len,
    bool        consume
    )
{
    if (rb == NULL || dest == NULL) {
        return AXL_ERR;
    }

    uint32_t readable = ring_readable(rb);
    if (readable < (uint32_t)sizeof(uint32_t)) {
        return AXL_ERR;
    }

    /* Peek at length header without consuming */
    uint32_t msg_len;
    ring_copy_out(rb, &msg_len, (uint32_t)sizeof(uint32_t), rb->read_pos);

    if (readable < (uint32_t)sizeof(uint32_t) + msg_len) {
        return AXL_ERR;
    }

    if (max_len < msg_len) {
        return AXL_ERR;
    }

    if (consume) {
        rb->read_pos += (uint32_t)sizeof(uint32_t);
        if (msg_len > 0) {
            axl_ring_buf_pop(rb, dest, msg_len);
        }
    } else {
        if (msg_len > 0) {
            ring_copy_out(rb, dest, msg_len,
                          rb->read_pos + (uint32_t)sizeof(uint32_t));
        }
    }

    if (actual_len != NULL) {
        *actual_len = msg_len;
    }

    return AXL_OK;
}

int
axl_ring_buf_pop_msg(
    AxlRingBuf *rb,
    void       *dest,
    uint32_t    max_len,
    uint32_t   *actual_len
    )
{
    return ring_buf_msg_read_internal(rb, dest, max_len, actual_len, true);
}

int
axl_ring_buf_peek_msg(
    AxlRingBuf *rb,
    void       *dest,
    uint32_t    max_len,
    uint32_t   *actual_len
    )
{
    return ring_buf_msg_read_internal(rb, dest, max_len, actual_len, false);
}

uint32_t
axl_ring_buf_peek_msg_size(const AxlRingBuf *rb)
{
    if (rb == NULL) {
        return 0;
    }

    if (ring_readable(rb) < (uint32_t)sizeof(uint32_t)) {
        return 0;
    }

    uint32_t msg_len;
    ring_copy_out(rb, &msg_len, (uint32_t)sizeof(uint32_t), rb->read_pos);
    return msg_len;
}

// ===========================================================================
// Layer 3: Elements
// ===========================================================================

int
axl_ring_buf_push_elem(
    AxlRingBuf *rb,
    const void *elem
    )
{
    if (rb == NULL || elem == NULL || rb->elem_size == 0) {
        return AXL_ERR;
    }

    if (!(rb->flags & AXL_RING_BUF_OVERWRITE)) {
        if (ring_writable(rb) < rb->elem_size) {
            /* Reject: count the element as attempted-but-lost. */
            rb->pushes_total += rb->elem_size;
            rb->pushes_lost  += rb->elem_size;
            return AXL_ERR;
        }
    }

    axl_ring_buf_push(rb, elem, rb->elem_size);
    return AXL_OK;
}

int
axl_ring_buf_pop_elem(
    AxlRingBuf *rb,
    void       *elem
    )
{
    if (rb == NULL || elem == NULL || rb->elem_size == 0) {
        return AXL_ERR;
    }

    if (ring_readable(rb) < rb->elem_size) {
        return AXL_ERR;
    }

    axl_ring_buf_pop(rb, elem, rb->elem_size);
    return AXL_OK;
}

int
axl_ring_buf_peek_elem(
    const AxlRingBuf *rb,
    void       *dest
    )
{
    if (rb == NULL || dest == NULL || rb->elem_size == 0) {
        return AXL_ERR;
    }

    if (ring_readable(rb) < rb->elem_size) {
        return AXL_ERR;
    }

    axl_ring_buf_peek(rb, dest, rb->elem_size);
    return AXL_OK;
}

int
axl_ring_buf_peek_nth_elem(
    const AxlRingBuf *rb,
    uint32_t    index,
    void       *dest
    )
{
    if (rb == NULL || dest == NULL || rb->elem_size == 0) {
        return AXL_ERR;
    }

    uint32_t count = ring_readable(rb) / rb->elem_size;
    if (index >= count) {
        return AXL_ERR;
    }

    uint32_t byte_offset = rb->read_pos + index * rb->elem_size;
    ring_copy_out(rb, dest, rb->elem_size, byte_offset);
    return AXL_OK;
}

int
axl_ring_buf_set_nth_elem(
    AxlRingBuf *rb,
    uint32_t    index,
    const void *src
    )
{
    if (rb == NULL || src == NULL || rb->elem_size == 0) {
        return AXL_ERR;
    }

    uint32_t count = ring_readable(rb) / rb->elem_size;
    if (index >= count) {
        return AXL_ERR;
    }

    uint32_t byte_offset = rb->read_pos + index * rb->elem_size;
    ring_copy_in(rb, src, rb->elem_size, byte_offset);
    return AXL_OK;
}

uint32_t
axl_ring_buf_get_length(const AxlRingBuf *rb)
{
    if (rb == NULL) {
        return 0;
    }

    uint32_t readable = ring_readable(rb);
    if (rb->elem_size > 0) {
        return readable / rb->elem_size;
    }

    return readable;
}

// ===========================================================================
// Queries
// ===========================================================================

uint32_t
axl_ring_buf_get_readable(AxlRingBuf *rb)
{
    if (rb == NULL) {
        return 0;
    }

    return ring_readable(rb);
}

uint32_t
axl_ring_buf_get_writable(AxlRingBuf *rb)
{
    if (rb == NULL) {
        return 0;
    }

    return ring_writable(rb);
}

uint32_t
axl_ring_buf_get_capacity(const AxlRingBuf *rb)
{
    if (rb == NULL) {
        return 0;
    }

    return rb->size;
}

bool
axl_ring_buf_is_empty(const AxlRingBuf *rb)
{
    if (rb == NULL) {
        return true;
    }

    return rb->read_pos == rb->write_pos;
}

bool
axl_ring_buf_is_full(const AxlRingBuf *rb)
{
    if (rb == NULL) {
        return false;
    }

    return ring_readable(rb) == rb->size;
}

void
axl_ring_buf_clear(AxlRingBuf *rb)
{
    if (rb == NULL) {
        return;
    }

    rb->read_pos     = 0;
    rb->write_pos    = 0;
    rb->pushes_total = 0;
    rb->pushes_lost  = 0;
}

uint64_t
axl_ring_buf_pushes_total(const AxlRingBuf *rb)
{
    return rb == NULL ? 0 : rb->pushes_total;
}

uint64_t
axl_ring_buf_pushes_lost(const AxlRingBuf *rb)
{
    return rb == NULL ? 0 : rb->pushes_lost;
}
