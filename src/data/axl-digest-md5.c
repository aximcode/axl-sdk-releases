/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-digest-md5.c
    MD5 (RFC 1321) — 16-byte digest. Standalone, no dependencies.
**/

#include <axl/axl-str.h>
#include "axl-digest-internal.h"

#define MD5_F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~(z))))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~(z))))
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_STEP(f, a, b, c, d, x, t, s) do { \
    (a) += f((b), (c), (d)) + (x) + (t);      \
    (a) = MD5_ROTL((a), (s));                   \
    (a) += (b);                                 \
} while (0)

static void
md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t m[16];

    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)block[i * 4]
             | ((uint32_t)block[i * 4 + 1] << 8)
             | ((uint32_t)block[i * 4 + 2] << 16)
             | ((uint32_t)block[i * 4 + 3] << 24);
    }

    MD5_STEP(MD5_F, a,b,c,d, m[ 0], 0xd76aa478,  7);
    MD5_STEP(MD5_F, d,a,b,c, m[ 1], 0xe8c7b756, 12);
    MD5_STEP(MD5_F, c,d,a,b, m[ 2], 0x242070db, 17);
    MD5_STEP(MD5_F, b,c,d,a, m[ 3], 0xc1bdceee, 22);
    MD5_STEP(MD5_F, a,b,c,d, m[ 4], 0xf57c0faf,  7);
    MD5_STEP(MD5_F, d,a,b,c, m[ 5], 0x4787c62a, 12);
    MD5_STEP(MD5_F, c,d,a,b, m[ 6], 0xa8304613, 17);
    MD5_STEP(MD5_F, b,c,d,a, m[ 7], 0xfd469501, 22);
    MD5_STEP(MD5_F, a,b,c,d, m[ 8], 0x698098d8,  7);
    MD5_STEP(MD5_F, d,a,b,c, m[ 9], 0x8b44f7af, 12);
    MD5_STEP(MD5_F, c,d,a,b, m[10], 0xffff5bb1, 17);
    MD5_STEP(MD5_F, b,c,d,a, m[11], 0x895cd7be, 22);
    MD5_STEP(MD5_F, a,b,c,d, m[12], 0x6b901122,  7);
    MD5_STEP(MD5_F, d,a,b,c, m[13], 0xfd987193, 12);
    MD5_STEP(MD5_F, c,d,a,b, m[14], 0xa679438e, 17);
    MD5_STEP(MD5_F, b,c,d,a, m[15], 0x49b40821, 22);

    MD5_STEP(MD5_G, a,b,c,d, m[ 1], 0xf61e2562,  5);
    MD5_STEP(MD5_G, d,a,b,c, m[ 6], 0xc040b340,  9);
    MD5_STEP(MD5_G, c,d,a,b, m[11], 0x265e5a51, 14);
    MD5_STEP(MD5_G, b,c,d,a, m[ 0], 0xe9b6c7aa, 20);
    MD5_STEP(MD5_G, a,b,c,d, m[ 5], 0xd62f105d,  5);
    MD5_STEP(MD5_G, d,a,b,c, m[10], 0x02441453,  9);
    MD5_STEP(MD5_G, c,d,a,b, m[15], 0xd8a1e681, 14);
    MD5_STEP(MD5_G, b,c,d,a, m[ 4], 0xe7d3fbc8, 20);
    MD5_STEP(MD5_G, a,b,c,d, m[ 9], 0x21e1cde6,  5);
    MD5_STEP(MD5_G, d,a,b,c, m[14], 0xc33707d6,  9);
    MD5_STEP(MD5_G, c,d,a,b, m[ 3], 0xf4d50d87, 14);
    MD5_STEP(MD5_G, b,c,d,a, m[ 8], 0x455a14ed, 20);
    MD5_STEP(MD5_G, a,b,c,d, m[13], 0xa9e3e905,  5);
    MD5_STEP(MD5_G, d,a,b,c, m[ 2], 0xfcefa3f8,  9);
    MD5_STEP(MD5_G, c,d,a,b, m[ 7], 0x676f02d9, 14);
    MD5_STEP(MD5_G, b,c,d,a, m[12], 0x8d2a4c8a, 20);

    MD5_STEP(MD5_H, a,b,c,d, m[ 5], 0xfffa3942,  4);
    MD5_STEP(MD5_H, d,a,b,c, m[ 8], 0x8771f681, 11);
    MD5_STEP(MD5_H, c,d,a,b, m[11], 0x6d9d6122, 16);
    MD5_STEP(MD5_H, b,c,d,a, m[14], 0xfde5380c, 23);
    MD5_STEP(MD5_H, a,b,c,d, m[ 1], 0xa4beea44,  4);
    MD5_STEP(MD5_H, d,a,b,c, m[ 4], 0x4bdecfa9, 11);
    MD5_STEP(MD5_H, c,d,a,b, m[ 7], 0xf6bb4b60, 16);
    MD5_STEP(MD5_H, b,c,d,a, m[10], 0xbebfbc70, 23);
    MD5_STEP(MD5_H, a,b,c,d, m[13], 0x289b7ec6,  4);
    MD5_STEP(MD5_H, d,a,b,c, m[ 0], 0xeaa127fa, 11);
    MD5_STEP(MD5_H, c,d,a,b, m[ 3], 0xd4ef3085, 16);
    MD5_STEP(MD5_H, b,c,d,a, m[ 6], 0x04881d05, 23);
    MD5_STEP(MD5_H, a,b,c,d, m[ 9], 0xd9d4d039,  4);
    MD5_STEP(MD5_H, d,a,b,c, m[12], 0xe6db99e5, 11);
    MD5_STEP(MD5_H, c,d,a,b, m[15], 0x1fa27cf8, 16);
    MD5_STEP(MD5_H, b,c,d,a, m[ 2], 0xc4ac5665, 23);

    MD5_STEP(MD5_I, a,b,c,d, m[ 0], 0xf4292244,  6);
    MD5_STEP(MD5_I, d,a,b,c, m[ 7], 0x432aff97, 10);
    MD5_STEP(MD5_I, c,d,a,b, m[14], 0xab9423a7, 15);
    MD5_STEP(MD5_I, b,c,d,a, m[ 5], 0xfc93a039, 21);
    MD5_STEP(MD5_I, a,b,c,d, m[12], 0x655b59c3,  6);
    MD5_STEP(MD5_I, d,a,b,c, m[ 3], 0x8f0ccc92, 10);
    MD5_STEP(MD5_I, c,d,a,b, m[10], 0xffeff47d, 15);
    MD5_STEP(MD5_I, b,c,d,a, m[ 1], 0x85845dd1, 21);
    MD5_STEP(MD5_I, a,b,c,d, m[ 8], 0x6fa87e4f,  6);
    MD5_STEP(MD5_I, d,a,b,c, m[15], 0xfe2ce6e0, 10);
    MD5_STEP(MD5_I, c,d,a,b, m[ 6], 0xa3014314, 15);
    MD5_STEP(MD5_I, b,c,d,a, m[13], 0x4e0811a1, 21);
    MD5_STEP(MD5_I, a,b,c,d, m[ 4], 0xf7537e82,  6);
    MD5_STEP(MD5_I, d,a,b,c, m[11], 0xbd3af235, 10);
    MD5_STEP(MD5_I, c,d,a,b, m[ 2], 0x2ad7d2bb, 15);
    MD5_STEP(MD5_I, b,c,d,a, m[ 9], 0xeb86d391, 21);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

void
md5_init(Md5State *s)
{
    s->state[0] = 0x67452301;
    s->state[1] = 0xefcdab89;
    s->state[2] = 0x98badcfe;
    s->state[3] = 0x10325476;
    s->count = 0;
}

void
md5_update(Md5State *s, const uint8_t *data, size_t len)
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
        md5_transform(s->state, s->buffer);
        data += fill;
        len -= fill;
    }
    while (len >= 64) {
        md5_transform(s->state, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) {
        axl_memcpy(s->buffer, data, len);
    }
}

void
md5_final(Md5State *s, uint8_t out[16])
{
    uint64_t total_bits = s->count;

    uint8_t pad = 0x80;
    md5_update(s, &pad, 1);

    pad = 0;
    while (((s->count / 8) % 64) != 56) {
        md5_update(s, &pad, 1);
    }

    /* Append original length in bits (little-endian) */
    uint8_t len_le[8];
    for (int i = 0; i < 8; i++) {
        len_le[i] = (uint8_t)(total_bits >> (i * 8));
    }
    md5_update(s, len_le, 8);

    for (int i = 0; i < 4; i++) {
        out[i * 4]     = (uint8_t)(s->state[i]);
        out[i * 4 + 1] = (uint8_t)(s->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(s->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(s->state[i] >> 24);
    }
}
