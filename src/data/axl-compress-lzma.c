/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-compress-lzma.c
    LZMA "alone" (.lzma) one-shot encode/decode for AxlCompress, backed by
    the vendored public-domain LZMA SDK (deps/lzma). The 13-byte alone
    header (5 props + 8 uncompressed-size LE64) is what EDK2's
    GUIDED-LZMA section and Python's lzma.FORMAT_ALONE use. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-compress.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include "../../deps/lzma/LzmaDec.h"
#include "../../deps/lzma/LzmaEnc.h"
#include "axl-compress-internal.h"

/* LZMA "alone" format header layout:
     bytes  0..4   LzmaProps (1 props byte + 4-byte LE dict size)
     bytes  5..12  uncompressed size, little-endian uint64_t
                   0xFFFFFFFFFFFFFFFF = sentinel (stream carries end mark;
                   not yet supported — EDK2/OVMF always writes a real size) */
#define LZMA_ALONE_HEADER_SIZE  13
#define LZMA_ALONE_SENTINEL     UINT64_C(0xFFFFFFFFFFFFFFFF)

/* Bridge the SDK's ISzAlloc to axl_malloc/axl_free. */
static void *
lzma_alloc(ISzAllocPtr p, size_t size)
{
    (void)p;
    return axl_malloc(size);
}

static void
lzma_free(ISzAllocPtr p, void *addr)
{
    (void)p;
    axl_free(addr);
}

static const ISzAlloc g_lzma_alloc = { lzma_alloc, lzma_free };

/* ---------------------------------------------------------------------------
 * Header parsing
 * --------------------------------------------------------------------------- */

/** Parse the 13-byte LZMA-alone header.
 *
 * @param in      Start of the input buffer.
 * @param in_len  Length of the input buffer.
 * @param unc_out On success, receives the uncompressed size (real, not sentinel).
 * @return 0 on success, -1 on any rejection (too short, sentinel, over cap). */
static int
parse_alone_header(const uint8_t *in, size_t in_len, size_t *unc_out)
{
    if (in_len < LZMA_ALONE_HEADER_SIZE)
        return -1;

    /* bytes 5..12: uncompressed size, little-endian uint64. */
    uint64_t unc =
        (uint64_t)in[5]        | ((uint64_t)in[6]  << 8)
      | ((uint64_t)in[7]  << 16) | ((uint64_t)in[8]  << 24)
      | ((uint64_t)in[9]  << 32) | ((uint64_t)in[10] << 40)
      | ((uint64_t)in[11] << 48) | ((uint64_t)in[12] << 56);

    /* Sentinel 0xFFFF…: stream has an end mark; streaming-grow needed.
       Not yet supported — EDK2/OVMF always writes a real size.
       Reject until a streaming path is added (Task 0.2+). */
    if (unc == LZMA_ALONE_SENTINEL)
        return -1;

    /* Sanity cap: reject a bomb before allocating. */
    if (unc > AXL_COMPRESS_MAX_OUTPUT)
        return -1;

    *unc_out = (size_t)unc;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Public entry points
 * --------------------------------------------------------------------------- */

int
axl_lzma_decompress(const void *in, size_t in_len,
                    void **out, size_t *out_len)
{
    const uint8_t *p = in;

    size_t unc = 0;
    if (parse_alone_header(p, in_len, &unc) != 0) {
        *out     = NULL;
        *out_len = 0;
        return AXL_ERR;
    }

    /* Allocate at least 1 byte (avoids NULL for zero-length decompression;
       mirrors inflate_exact in axl-compress.c). */
    size_t alloc_len = unc > 0 ? unc : 1;
    uint8_t *buf = axl_malloc(alloc_len);
    if (buf == NULL) {
        *out     = NULL;
        *out_len = 0;
        return AXL_ERR;
    }

    /* Payload starts after the 13-byte header; props are the first 5 bytes.
       SizeT is 64-bit on X64/AARCH64 (the only targets), so a <=512 MiB
       unc cannot truncate; a 32-bit IA32 target would need a range guard. */
    const Byte *payload    = p + LZMA_ALONE_HEADER_SIZE;
    SizeT       src_len    = (SizeT)(in_len - LZMA_ALONE_HEADER_SIZE);
    SizeT       dest_len   = (SizeT)unc;
    ELzmaStatus status;

    SRes res = LzmaDecode(buf, &dest_len, payload, &src_len,
                          p, LZMA_PROPS_SIZE,   /* propData, propSize (first 5 bytes) */
                          LZMA_FINISH_END,
                          &status, &g_lzma_alloc);

    /* LzmaDecode can return SZ_OK with LZMA_STATUS_NOT_FINISHED on a
       truncated or malformed stream (mirroring EDK2's LzmaDecode wrapper).
       Require a clean finish: either an explicit end mark or a
       maybe-finished state (no end mark but output fully consumed). */
    bool finished = (status == LZMA_STATUS_FINISHED_WITH_MARK ||
                     status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK);
    if (res != SZ_OK || dest_len != unc || !finished) {
        axl_free(buf);
        *out     = NULL;
        *out_len = 0;
        return AXL_ERR;
    }

    *out     = buf;
    *out_len = unc;
    return AXL_OK;
}

int
axl_lzma_compress(const void *in, size_t in_len,
                  void **out, size_t *out_len, int level)
{
    *out     = NULL;
    *out_len = 0;

    /* Clamp level to [0, 9]; map AXL_COMPRESS_LEVEL_DEFAULT (-1) to 5. */
    if (level < 0)
        level = 5;
    if (level > 9)
        level = 9;

    /* Guard: reject inputs that would overflow the payload_bound arithmetic
       below; also matches the cap axl_compress() applies before calling us. */
    if (in_len > (size_t)INT_MAX) {
        *out = NULL;
        *out_len = 0;
        return AXL_ERR;
    }

    /* Worst-case payload bound: LZMA SDK recommends in_len + in_len/2 + 256. */
    size_t payload_bound = in_len + in_len / 2 + 256;
    uint8_t *payload = axl_malloc(payload_bound);
    if (payload == NULL)
        return AXL_ERR;

    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.level = level;
    LzmaEncProps_Normalize(&props);

    Byte     props_encoded[LZMA_PROPS_SIZE];
    SizeT    props_size  = LZMA_PROPS_SIZE;
    SizeT    payload_len = (SizeT)payload_bound;

    SRes res = LzmaEncode(
        payload, &payload_len,
        (const Byte *)in, (SizeT)in_len,
        &props, props_encoded, &props_size,
        1 /* writeEndMark */,
        NULL /* progress */,
        &g_lzma_alloc, &g_lzma_alloc);

    if (res != SZ_OK || props_size != LZMA_PROPS_SIZE) {
        axl_free(payload);
        return AXL_ERR;
    }

    /* Assemble the 13-byte LZMA-alone header + payload into a single buffer. */
    size_t total = LZMA_ALONE_HEADER_SIZE + (size_t)payload_len;
    uint8_t *buf = axl_malloc(total);
    if (buf == NULL) {
        axl_free(payload);
        return AXL_ERR;
    }

    /* bytes 0..4: props */
    axl_memcpy(buf, props_encoded, LZMA_PROPS_SIZE);

    /* bytes 5..12: uncompressed size, little-endian uint64 (real size, NOT sentinel) */
    uint64_t unc = (uint64_t)in_len;
    buf[5]  = (uint8_t)(unc);
    buf[6]  = (uint8_t)(unc >> 8);
    buf[7]  = (uint8_t)(unc >> 16);
    buf[8]  = (uint8_t)(unc >> 24);
    buf[9]  = (uint8_t)(unc >> 32);
    buf[10] = (uint8_t)(unc >> 40);
    buf[11] = (uint8_t)(unc >> 48);
    buf[12] = (uint8_t)(unc >> 56);

    /* payload */
    axl_memcpy(buf + LZMA_ALONE_HEADER_SIZE, payload, (size_t)payload_len);
    axl_free(payload);

    *out     = buf;
    *out_len = total;
    return AXL_OK;
}
