/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest-sha1.c
    SHA-1 (RFC 3174) — 20-byte digest. Standalone, no dependencies.
**/

#include <axl/axl-str.h>
#include "axl-digest-internal.h"

static uint32_t
sha1_rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void
sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8)
             | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0]; b = state[1]; c = state[2];
    d = state[3]; e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rotl(b, 30); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c;
    state[3] += d; state[4] += e;
}

void
sha1_init(Sha1State *s)
{
    s->state[0] = 0x67452301;
    s->state[1] = 0xEFCDAB89;
    s->state[2] = 0x98BADCFE;
    s->state[3] = 0x10325476;
    s->state[4] = 0xC3D2E1F0;
    s->count = 0;
}

void
sha1_update(Sha1State *s, const uint8_t *data, size_t len)
{
    size_t idx = (size_t)((s->count / 8) % 64);
    s->count += (uint64_t)len * 8;

    if (idx > 0) {
        size_t fill = 64 - idx;
        if (len < fill) {
            axl_memcpy(s->buffer + idx, data, len);
            return;
        }
        axl_memcpy(s->buffer + idx, data, fill);
        sha1_transform(s->state, s->buffer);
        data += fill;
        len -= fill;
    }
    while (len >= 64) {
        sha1_transform(s->state, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) {
        axl_memcpy(s->buffer, data, len);
    }
}

void
sha1_final(Sha1State *s, uint8_t out[20])
{
    uint64_t total_bits = s->count;

    /* Pad with 0x80 + zeros */
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);

    /* Zero-fill until 56 mod 64 */
    pad = 0;
    while (((s->count / 8) % 64) != 56) {
        sha1_update(s, &pad, 1);
    }

    /* Append original length in bits (big-endian) */
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(total_bits >> (56 - i * 8));
    }
    sha1_update(s, len_be, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(s->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->state[i]);
    }
}
