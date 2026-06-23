/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-compress.c
    AxlCompress one-shot codec — gzip (RFC 1952), zlib (RFC 1950), raw
    DEFLATE (RFC 1951), plus LZMA 'alone' (.lzma / EDK2 GUIDED-LZMA).

    The DEFLATE core is the vendored sdefl/sinfl single-header codec
    (deps/sdefl). AXL owns the container framing and the integrity
    fields: it computes/verifies the gzip CRC-32 and zlib Adler-32 via
    AxlDigest rather than the codec's built-in checksums, so the same
    framing path is reused by the stream filters.

    sinfl's SIMD fast paths are disabled (SINFL_NO_SIMD) so the decoder
    builds identically on x64 and aarch64 freestanding targets with no
    intrinsic-header dependency.
**/

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <axl/axl-compress.h>
#include <axl/axl-digest.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>  /* axl_memcpy */

#include "axl-compress-internal.h"

#define SDEFL_IMPLEMENTATION
#include "../../deps/sdefl/sdefl.h"

#define SINFL_NO_SIMD
#define SINFL_IMPLEMENTATION
#include "../../deps/sdefl/sinfl.h"

/* gzip header FLG bits (RFC 1952 §2.3.1). */
#define GZ_FTEXT     0x01
#define GZ_FHCRC     0x02
#define GZ_FEXTRA    0x04
#define GZ_FNAME     0x08
#define GZ_FCOMMENT  0x10
#define GZ_FRESERVED 0xE0  /* bits 5-7 must be zero */

// ---------------------------------------------------------------------------
// Little/big-endian field helpers
// ---------------------------------------------------------------------------

static void
put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t
get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// Compression
// ---------------------------------------------------------------------------

/* Compress @p in into a freshly allocated raw-DEFLATE buffer. On
   success sets @p *out / @p *out_len and returns AXL_OK. */
static int
deflate_raw(const void *in, size_t in_len, int level,
            uint8_t **out, int *out_len)
{
    /* sdefl emits zero bytes for empty input, but a 0-byte DEFLATE
       stream is ill-defined and won't decode/interop. Emit the
       canonical empty stream — one final fixed-Huffman block carrying
       only the end-of-block symbol — exactly as zlib/gzip do. */
    if (in_len == 0) {
        uint8_t *buf = axl_malloc(2);
        if (buf == NULL) {
            return AXL_ERR;
        }
        buf[0] = 0x03;
        buf[1] = 0x00;
        *out = buf;
        *out_len = 2;
        return AXL_OK;
    }

    int lvl = (level == AXL_COMPRESS_LEVEL_DEFAULT) ? SDEFL_LVL_DEF : level;
    if (lvl < SDEFL_LVL_MIN) lvl = SDEFL_LVL_MIN;
    if (lvl > SDEFL_LVL_MAX) lvl = SDEFL_LVL_MAX;

    int bound = sdefl_bound((int)in_len);
    if (bound <= 0) {   /* in_len so large the bound overflowed int */
        return AXL_ERR;
    }
    uint8_t      *buf = axl_malloc((size_t)bound);
    /* sdefl requires a zero-initialized state — its hash chains (tbl/prv)
       are read before being fully populated, so garbage here sends the
       match finder off into out-of-bounds chain walks. calloc, not malloc. */
    struct sdefl *s   = axl_calloc(1, sizeof(struct sdefl));  /* ~830 KiB */
    if (buf == NULL || s == NULL) {
        axl_free(buf);
        axl_free(s);
        return AXL_ERR;
    }
    int n = sdeflate(s, buf, in, (int)in_len, lvl);
    axl_free(s);
    if (n < 0) {
        axl_free(buf);
        return AXL_ERR;
    }
    *out = buf;
    *out_len = n;
    return AXL_OK;
}

int
axl_compress(AxlCompressFormat fmt, const void *in, size_t in_len,
             void **out, size_t *out_len, int level)
{
    if (out == NULL || out_len == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    *out_len = 0;
    if ((in == NULL && in_len != 0) || in_len > (size_t)INT_MAX) {
        return AXL_ERR;
    }

    /* LZMA uses its own codec path — not DEFLATE-based. Dispatch before
       deflate_raw() so the ~830 KiB sdefl state is never allocated for it. */
    if (fmt == AXL_COMPRESS_LZMA) {
        return axl_lzma_compress(in, in_len, out, out_len, level);
    }

    uint8_t *defl = NULL;
    int      defl_len = 0;
    if (deflate_raw(in, in_len, level, &defl, &defl_len) != AXL_OK) {
        return AXL_ERR;
    }

    size_t head = 0;
    size_t tail = 0;
    switch (fmt) {
    case AXL_COMPRESS_GZIP:        head = 10; tail = 8; break;
    case AXL_COMPRESS_ZLIB:        head = 2;  tail = 4; break;
    case AXL_COMPRESS_DEFLATE_RAW: head = 0;  tail = 0; break;
    default:
        axl_free(defl);
        return AXL_ERR;
    }

    size_t   total = head + (size_t)defl_len + tail;
    uint8_t *o     = axl_malloc(total == 0 ? 1 : total);
    if (o == NULL) {
        axl_free(defl);
        return AXL_ERR;
    }

    switch (fmt) {
    case AXL_COMPRESS_GZIP: {
        o[0] = 0x1f; o[1] = 0x8b; o[2] = 0x08; o[3] = 0x00;
        o[4] = o[5] = o[6] = o[7] = 0x00;  /* MTIME = 0 */
        o[8] = 0x00;                       /* XFL */
        o[9] = 0xff;                       /* OS = unknown */
        axl_memcpy(o + 10, defl, (size_t)defl_len);
        put_le32(o + 10 + defl_len, axl_crc32(0, in, in_len));
        put_le32(o + 14 + defl_len, (uint32_t)(in_len & 0xFFFFFFFFu));
        break;
    }
    case AXL_COMPRESS_ZLIB: {
        o[0] = 0x78;  /* CM=8, CINFO=7 (32K window) */
        o[1] = 0x01;  /* FLEVEL=0, FCHECK so (0x78<<8|0x01) % 31 == 0 */
        axl_memcpy(o + 2, defl, (size_t)defl_len);
        /* Adler-32 trailer is big-endian. */
        uint32_t a = axl_adler32(1, in, in_len);
        o[2 + defl_len]     = (uint8_t)((a >> 24) & 0xFFu);
        o[2 + defl_len + 1] = (uint8_t)((a >> 16) & 0xFFu);
        o[2 + defl_len + 2] = (uint8_t)((a >> 8) & 0xFFu);
        o[2 + defl_len + 3] = (uint8_t)(a & 0xFFu);
        break;
    }
    case AXL_COMPRESS_DEFLATE_RAW:
        axl_memcpy(o, defl, (size_t)defl_len);
        break;
    default:
        break;  /* unreachable — validated above */
    }

    axl_free(defl);
    *out = o;
    *out_len = total;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Decompression
// ---------------------------------------------------------------------------

/* sinfl's 64-bit refill always reads 8 bytes from the current bit
   pointer, so it can read up to 7 bytes past the end of the compressed
   payload. Run it over an 8-byte zero-padded copy so that over-read
   stays inside our own allocation — untrusted input may abut unmapped
   memory. Returns sinflate's result (bytes written, or its negative
   error), or INT_MIN on allocation failure. */
static int
run_sinflate(uint8_t *out, size_t cap, const uint8_t *payload, size_t plen)
{
    uint8_t *pin = axl_malloc(plen + 8);
    if (pin == NULL) {
        return INT_MIN;
    }
    if (plen != 0) {
        axl_memcpy(pin, payload, plen);
    }
    axl_memset(pin + plen, 0, 8);
    int r = sinflate(out, (int)cap, pin, (int)plen);
    axl_free(pin);
    return r;
}

/* Inflate @p payload (raw DEFLATE) when the output size is known
   exactly (gzip ISIZE). Allocates @p expected bytes (min 1) and
   requires sinflate to emit exactly that many. */
static int
inflate_exact(const uint8_t *payload, size_t plen, size_t expected,
              void **out, size_t *out_len)
{
    if (expected > AXL_COMPRESS_MAX_OUTPUT) {
        return AXL_ERR;
    }
    uint8_t *buf = axl_malloc(expected == 0 ? 1 : expected);
    if (buf == NULL) {
        return AXL_ERR;
    }
    int r = run_sinflate(buf, expected, payload, plen);
    if (r < 0 || (size_t)r != expected) {
        axl_free(buf);
        return AXL_ERR;
    }
    *out = buf;
    *out_len = expected;
    return AXL_OK;
}

/* Inflate @p payload when the output size is unknown (zlib/raw): grow
   the output buffer and retry until sinflate stops asking for more
   room, capped at AXL_COMPRESS_MAX_OUTPUT. */
static int
inflate_grow(const uint8_t *payload, size_t plen, void **out, size_t *out_len)
{
    size_t cap = (plen < 1024) ? 4096 : plen * 4;
    if (cap > AXL_COMPRESS_MAX_OUTPUT) {
        cap = AXL_COMPRESS_MAX_OUTPUT;
    }
    for (;;) {
        uint8_t *buf = axl_malloc(cap);
        if (buf == NULL) {
            return AXL_ERR;
        }
        int r = run_sinflate(buf, cap, payload, plen);
        if (r == INT_MIN) {       /* padding-copy allocation failed */
            axl_free(buf);
            return AXL_ERR;
        }
        if (r == -2) {            /* output buffer too small — grow */
            axl_free(buf);
            if (cap >= AXL_COMPRESS_MAX_OUTPUT) {
                return AXL_ERR;
            }
            cap *= 2;
            if (cap > AXL_COMPRESS_MAX_OUTPUT) {
                cap = AXL_COMPRESS_MAX_OUTPUT;
            }
            continue;
        }
        if (r < 0) {
            axl_free(buf);
            return AXL_ERR;
        }
        *out = buf;
        *out_len = (size_t)r;
        return AXL_OK;
    }
}

static int
decompress_gzip(const uint8_t *in, size_t in_len, void **out, size_t *out_len)
{
    /* Minimum: 10-byte header + 8-byte trailer. */
    if (in_len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 0x08) {
        return AXL_ERR;
    }
    uint8_t flg = in[3];
    if (flg & GZ_FRESERVED) {
        return AXL_ERR;
    }

    size_t pos = 10;
    if (flg & GZ_FEXTRA) {
        if (pos + 2 > in_len) {
            return AXL_ERR;
        }
        size_t xlen = (size_t)in[pos] | ((size_t)in[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & GZ_FNAME) {
        while (pos < in_len && in[pos] != 0) pos++;
        pos++;  /* consume the NUL */
    }
    if (flg & GZ_FCOMMENT) {
        while (pos < in_len && in[pos] != 0) pos++;
        pos++;
    }
    if (flg & GZ_FHCRC) {
        pos += 2;
    }
    /* The optional fields must leave room for a payload and the trailer. */
    if (pos + 8 > in_len || pos > in_len) {
        return AXL_ERR;
    }

    const uint8_t *payload = in + pos;
    size_t         plen    = in_len - pos - 8;
    uint32_t crc_stored = get_le32(in + in_len - 8);
    uint32_t isize      = get_le32(in + in_len - 4);

    if (inflate_exact(payload, plen, isize, out, out_len) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_crc32(0, *out, *out_len) != crc_stored) {
        axl_free(*out);
        *out = NULL;
        *out_len = 0;
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
decompress_zlib(const uint8_t *in, size_t in_len, void **out, size_t *out_len)
{
    /* Minimum: 2-byte header + 4-byte Adler-32 trailer. */
    if (in_len < 6) {
        return AXL_ERR;
    }
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0Fu) != 8) {                     /* CM must be deflate */
        return AXL_ERR;
    }
    if ((((uint32_t)cmf << 8) | flg) % 31u != 0) { /* header checksum */
        return AXL_ERR;
    }
    if (flg & 0x20u) {                            /* FDICT preset dict */
        return AXL_ERR;
    }

    const uint8_t *payload = in + 2;
    size_t         plen    = in_len - 2 - 4;
    uint32_t adler_stored  = get_be32(in + in_len - 4);

    if (inflate_grow(payload, plen, out, out_len) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_adler32(1, *out, *out_len) != adler_stored) {
        axl_free(*out);
        *out = NULL;
        *out_len = 0;
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_decompress(AxlCompressFormat fmt, const void *in, size_t in_len,
               void **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    *out_len = 0;
    /* in_len == 0 is permitted (with a non-NULL pointer): it is the raw
       DEFLATE encoding of empty input that axl_compress itself emits, so
       round-trip symmetry requires accepting it. gzip/zlib still reject
       it via their minimum-length header checks. */
    if (in == NULL || in_len > (size_t)INT_MAX) {
        return AXL_ERR;
    }

    const uint8_t *p = (const uint8_t *)in;
    switch (fmt) {
    case AXL_COMPRESS_GZIP:
        return decompress_gzip(p, in_len, out, out_len);
    case AXL_COMPRESS_ZLIB:
        return decompress_zlib(p, in_len, out, out_len);
    case AXL_COMPRESS_DEFLATE_RAW:
        return inflate_grow(p, in_len, out, out_len);
    case AXL_COMPRESS_LZMA:
        return axl_lzma_decompress(p, in_len, out, out_len);
    default:
        return AXL_ERR;
    }
}
