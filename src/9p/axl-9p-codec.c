/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-codec.c
    9P2000.L little-endian wire codec: write/read cursors + message framing.
**/

#include "axl-9p-internal.h"
#include <axl/axl-str.h>     /* axl_strlen, axl_memcpy */

void
axl_9p_w_init(Axl9pWriter *w, uint8_t *buf, size_t cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false;
}

static void
w_bytes(Axl9pWriter *w, const uint8_t *p, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    axl_memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void
axl_9p_w_u8(Axl9pWriter *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

void
axl_9p_w_u16(Axl9pWriter *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    w_bytes(w, b, 2);
}

void
axl_9p_w_u32(Axl9pWriter *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    w_bytes(w, b, 4);
}

void
axl_9p_w_u64(Axl9pWriter *w, uint64_t v)
{
    axl_9p_w_u32(w, (uint32_t)v);
    axl_9p_w_u32(w, (uint32_t)(v >> 32));
}

void
axl_9p_w_str(Axl9pWriter *w, const char *s)
{
    size_t n = axl_strlen(s);
    if (n > 0xFFFFu) {
        w->overflow = true;
        return;
    }
    axl_9p_w_u16(w, (uint16_t)n);
    w_bytes(w, (const uint8_t *)s, n);
}

void
axl_9p_w_bytes(Axl9pWriter *w, const void *data, size_t len)
{
    w_bytes(w, (const uint8_t *)data, len);
}

void
axl_9p_w_patch_u16(Axl9pWriter *w, size_t pos, uint16_t v)
{
    if (pos > w->len || w->len - pos < 2) {
        w->overflow = true;
        return;
    }
    w->buf[pos]     = (uint8_t)v;
    w->buf[pos + 1] = (uint8_t)(v >> 8);
}

void
axl_9p_w_patch_u32(Axl9pWriter *w, size_t pos, uint32_t v)
{
    if (pos > w->len || w->len - pos < 4) {
        w->overflow = true;
        return;
    }
    w->buf[pos]     = (uint8_t)v;
    w->buf[pos + 1] = (uint8_t)(v >> 8);
    w->buf[pos + 2] = (uint8_t)(v >> 16);
    w->buf[pos + 3] = (uint8_t)(v >> 24);
}

uint8_t *
axl_9p_w_reserve(Axl9pWriter *w, size_t n)
{
    if (n > w->cap - w->len) {
        /* Phrased as a subtraction on the REMAINING capacity rather than as
           `w->len + n > w->cap`: n is derived from a wire-supplied count in
           the Rread path, and the addition could wrap for a large enough one
           and hand back a pointer past the end. w->len <= w->cap always
           holds (nothing here ever advances the cursor past it), so the
           subtraction cannot underflow. */
        w->overflow = true;
        return NULL;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

bool
axl_9p_negotiate_msize(uint32_t client_msize, uint32_t server_cap, uint32_t *out)
{
    if (client_msize < AXL_9P_MIN_MSIZE || server_cap < AXL_9P_MIN_MSIZE) {
        return false;
    }
    *out = (client_msize < server_cap) ? client_msize : server_cap;
    return true;
}

void
axl_9p_r_init(Axl9pReader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf; r->len = len; r->pos = 0; r->error = false;
}

static bool
r_need(Axl9pReader *r, size_t n)
{
    if (r->pos + n > r->len) {
        r->error = true;
        return false;
    }
    return true;
}

uint8_t
axl_9p_r_u8(Axl9pReader *r)
{
    if (!r_need(r, 1)) return 0;
    return r->buf[r->pos++];
}

uint16_t
axl_9p_r_u16(Axl9pReader *r)
{
    if (!r_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->buf[r->pos] | (r->buf[r->pos + 1] << 8));
    r->pos += 2;
    return v;
}

uint32_t
axl_9p_r_u32(Axl9pReader *r)
{
    if (!r_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->buf[r->pos]
               | ((uint32_t)r->buf[r->pos + 1] << 8)
               | ((uint32_t)r->buf[r->pos + 2] << 16)
               | ((uint32_t)r->buf[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

uint64_t
axl_9p_r_u64(Axl9pReader *r)
{
    uint64_t lo = axl_9p_r_u32(r);
    uint64_t hi = axl_9p_r_u32(r);
    return lo | (hi << 32);
}

size_t
axl_9p_r_str(Axl9pReader *r, char *out, size_t cap)
{
    uint16_t n = axl_9p_r_u16(r);
    if (!r_need(r, n)) {
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    size_t max  = (cap > 0) ? cap - 1 : 0;
    size_t copy = (n < max) ? n : max;
    if (cap > 0) {
        axl_memcpy(out, r->buf + r->pos, copy);
        out[copy] = '\0';
    }
    r->pos += n;
    return n;
}

void
axl_9p_msg_begin(Axl9pWriter *w, uint8_t *buf, size_t cap,
                 uint8_t type, uint16_t tag)
{
    axl_9p_w_init(w, buf, cap);
    axl_9p_w_u32(w, 0);        /* size placeholder — patched by _finish */
    axl_9p_w_u8(w, type);
    axl_9p_w_u16(w, tag);
}

size_t
axl_9p_msg_finish(Axl9pWriter *w)
{
    uint32_t sz = (uint32_t)w->len;
    if (w->cap >= 4) {
        w->buf[0] = (uint8_t)sz;         w->buf[1] = (uint8_t)(sz >> 8);
        w->buf[2] = (uint8_t)(sz >> 16); w->buf[3] = (uint8_t)(sz >> 24);
    }
    return w->len;
}

bool
axl_9p_msg_header(Axl9pReader *r, uint32_t *size,
                  uint8_t *type, uint16_t *tag)
{
    *size = axl_9p_r_u32(r);
    *type = axl_9p_r_u8(r);
    *tag  = axl_9p_r_u16(r);
    return !r->error;
}
