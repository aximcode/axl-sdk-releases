/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pack-format.c
    Packed-image format over the vendored LZMA SDK. Freestanding: no AXL
    headers, no libc (own tiny copy helpers), no heap except through the
    caller's AxlPackAlloc. LzmaEnc is referenced ONLY from axl_pack_build, so
    --gc-sections drops the encoder from a decode-only image. **/

#include <stddef.h>
#include <stdint.h>
#include "axl-pack-format.h"
#include "../../deps/lzma/LzmaDec.h"
#include "../../deps/lzma/LzmaEnc.h"
#include "../../deps/lzma/Bra.h"

/* Which branch converters the DECODER carries. Firmware code (libaxl's
   in-process loaders and the raw-UEFI stub, which link this one object)
   decodes only its own architecture's payloads: the machine check refuses
   anything else before the filter matters, so the other converter is dead
   weight, and with it unreferenced --gc-sections drops it from every image
   that links the loaders. The ENCODER always keeps both, so libaxl and the
   host packer can build either architecture's payload. The host packer is
   the one decoder that must read both (axl-efi-pack --verify), and asks for
   them with AXL_PACK_ALL_DECODERS. A payload naming an absent filter is
   refused by axl_pack_unpack before anything is allocated or decoded. */
#if defined(AXL_PACK_ALL_DECODERS)
#  define PACK_DEC_X86    1
#  define PACK_DEC_ARM64  1
#elif defined(__x86_64__)
#  define PACK_DEC_X86    1
#  define PACK_DEC_ARM64  0
#elif defined(__aarch64__)
#  define PACK_DEC_X86    0
#  define PACK_DEC_ARM64  1
#else
#  error "axl-pack-format.c: unsupported architecture (a host build defines AXL_PACK_ALL_DECODERS)"
#endif

#define HDR_CRC_SPAN     32u
#define PACK_LZMA_LEVEL  9

/* Per-filter LZMA literal/position parameters, measured over the shipped
   tools of each architecture (src/pack/README.md). No per-image search. */
#define LZMA_LC_NONE   3
#define LZMA_LP_NONE   0
#define LZMA_PB_NONE   2
#define LZMA_LC_X86    3
#define LZMA_LP_X86    0
#define LZMA_PB_X86    2
#define LZMA_LC_ARM64  0
#define LZMA_LP_ARM64  2
#define LZMA_PB_ARM64  2

/* Header byte offsets. */
#define OFF_MAGIC        0u
#define OFF_VERSION      8u
#define OFF_MACHINE      10u
#define OFF_FILTER       12u
#define OFF_PROPS        13u
#define OFF_RESERVED16   18u
#define OFF_RAW_SIZE     20u
#define OFF_PACKED_SIZE  24u
#define OFF_RAW_CRC      28u
#define OFF_HDR_CRC      32u
#define OFF_RESERVED     36u

/* PE/COFF fields axl_pack_find_payload reads (PE/COFF spec, sections 3-4). */
#define PE_DOS_LFANEW       0x3Cu
#define PE_SIG_SIZE         4u
#define PE_COFF_SIZE        20u
#define PE_COFF_NSECTIONS   2u
#define PE_COFF_OPT_SIZE    16u
#define PE_SEC_SIZE         40u
#define PE_SEC_NAME_SIZE    8u
#define PE_SEC_VSIZE        8u
#define PE_SEC_RAW_SIZE     16u
#define PE_SEC_RAW_PTR      20u

typedef struct {
    ISzAlloc            iface;      /* must be first: callbacks cast back */
    const AxlPackAlloc *a;
} PackLzmaAlloc;

static void    *pack_lzma_alloc(ISzAllocPtr p, size_t size);
static void     pack_lzma_free(ISzAllocPtr p, void *addr);
static void     pack_copy(void *dst, const void *src, size_t n);
static int      pack_equal(const void *a, const void *b, size_t n);
static uint16_t rd16(const uint8_t *p);
static uint32_t rd32(const uint8_t *p);
static void     wr16(uint8_t *p, uint16_t v);
static void     wr32(uint8_t *p, uint32_t v);
static void     pack_filter_enc(uint8_t *p, size_t n, uint8_t filter);
static int      pack_filter_decodable(uint8_t filter);
static void     pack_filter_dec(uint8_t *p, size_t n, uint8_t filter);
static void     pack_params(uint8_t filter, CLzmaEncProps *props);
static AxlPackStatus pack_check_header(const uint8_t *b, size_t len,
                                       AxlPackHeader *h);
static AxlPackStatus pack_check_extent(const AxlPackHeader *h, size_t len);

/* One whole-buffer call at pc 0 with fresh state: encoder and decoder are
   exact inverses over the same buffer. The X86 converter holds back the last
   4 bytes (lookahead); the same bytes are held back on both sides. */
static void
pack_filter_enc(uint8_t *p, size_t n, uint8_t filter)
{
    if (filter == AXL_PACK_FILTER_X86) {
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Enc(p, n, 0, &state);
    } else if (filter == AXL_PACK_FILTER_ARM64) {
        z7_BranchConv_ARM64_Enc(p, n, 0);
    }
}

static int
pack_filter_decodable(uint8_t filter)
{
    return filter == AXL_PACK_FILTER_NONE
        || (PACK_DEC_X86 && filter == AXL_PACK_FILTER_X86)
        || (PACK_DEC_ARM64 && filter == AXL_PACK_FILTER_ARM64);
}

static void
pack_filter_dec(uint8_t *p, size_t n, uint8_t filter)
{
#if PACK_DEC_X86
    if (filter == AXL_PACK_FILTER_X86) {
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Dec(p, n, 0, &state);
    }
#endif
#if PACK_DEC_ARM64
    if (filter == AXL_PACK_FILTER_ARM64) {
        z7_BranchConv_ARM64_Dec(p, n, 0);
    }
#endif
}

static void
pack_params(uint8_t filter, CLzmaEncProps *props)
{
    switch (filter) {
    case AXL_PACK_FILTER_X86:
        props->lc = LZMA_LC_X86;
        props->lp = LZMA_LP_X86;
        props->pb = LZMA_PB_X86;
        break;
    case AXL_PACK_FILTER_ARM64:
        props->lc = LZMA_LC_ARM64;
        props->lp = LZMA_LP_ARM64;
        props->pb = LZMA_PB_ARM64;
        break;
    default:
        props->lc = LZMA_LC_NONE;
        props->lp = LZMA_LP_NONE;
        props->pb = LZMA_PB_NONE;
        break;
    }
}

static void *
pack_lzma_alloc(ISzAllocPtr p, size_t size)
{
    const PackLzmaAlloc *w = (const PackLzmaAlloc *)p;
    return w->a->alloc(w->a->ctx, size);
}

static void
pack_lzma_free(ISzAllocPtr p, void *addr)
{
    const PackLzmaAlloc *w = (const PackLzmaAlloc *)p;
    if (addr != NULL) {
        w->a->free(w->a->ctx, addr);
    }
}

static void
pack_copy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static int
pack_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void
wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t
axl_pack_crc32(
    const void *buf,
    size_t      len)
{
    /* Nibble-at-a-time: a 64-byte table keeps the stub small and is ~2x the
       bitwise loop. Same polynomial (0xEDB88320) as zlib. */
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = buf;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ tab[crc & 0xFu];
        crc = (crc >> 4) ^ tab[crc & 0xFu];
    }
    return ~crc;
}

const char *
axl_pack_status_str(
    AxlPackStatus s)
{
    switch (s) {
    case AXL_PACK_OK:           return "ok";
    case AXL_PACK_E_NOT_PACKED: return "not a packed image";
    case AXL_PACK_E_VERSION:    return "packed image version is not supported";
    case AXL_PACK_E_HEADER:     return "packed image header is invalid";
    case AXL_PACK_E_MACHINE:    return "packed image is for a different architecture";
    case AXL_PACK_E_TOO_BIG:    return "packed image size is out of range";
    case AXL_PACK_E_TRUNCATED:  return "packed image is truncated";
    case AXL_PACK_E_CORRUPT:    return "packed image is corrupt";
    case AXL_PACK_E_NO_MEMORY:  return "out of memory unpacking image";
    }
    return "unknown pack status";
}

/* Magic, version, header CRC, reserved fields, filter: everything that needs
   only the header bytes. Fills @p h on success. */
static AxlPackStatus
pack_check_header(const uint8_t *b, size_t len, AxlPackHeader *h)
{
    if (len < 8 || !pack_equal(b + OFF_MAGIC, AXL_PACK_MAGIC, 8)) {
        return AXL_PACK_E_NOT_PACKED;
    }
    if (len < AXL_PACK_HDR_SIZE) {
        return AXL_PACK_E_TRUNCATED;
    }
    if (rd16(b + OFF_VERSION) != AXL_PACK_VERSION) {
        return AXL_PACK_E_VERSION;
    }
    if (rd32(b + OFF_HDR_CRC) != axl_pack_crc32(b, HDR_CRC_SPAN)
        || rd16(b + OFF_RESERVED16) != 0 || rd32(b + OFF_RESERVED) != 0
        || b[OFF_FILTER] > AXL_PACK_FILTER_ARM64) {
        return AXL_PACK_E_HEADER;
    }
    pack_copy(h->magic, b + OFF_MAGIC, 8);
    h->version     = rd16(b + OFF_VERSION);
    h->machine     = rd16(b + OFF_MACHINE);
    h->filter      = b[OFF_FILTER];
    pack_copy(h->lzma_props, b + OFF_PROPS, 5);
    h->reserved16  = rd16(b + OFF_RESERVED16);
    h->raw_size    = rd32(b + OFF_RAW_SIZE);
    h->packed_size = rd32(b + OFF_PACKED_SIZE);
    h->raw_crc32   = rd32(b + OFF_RAW_CRC);
    h->hdr_crc32   = rd32(b + OFF_HDR_CRC);
    h->reserved    = rd32(b + OFF_RESERVED);
    return AXL_PACK_OK;
}

/* Size range, then that the whole stream is present. Order is part of the
   contract: an out-of-range size is refused before it is believed. */
static AxlPackStatus
pack_check_extent(const AxlPackHeader *h, size_t len)
{
    if (h->raw_size == 0 || h->raw_size > AXL_PACK_MAX_RAW) {
        return AXL_PACK_E_TOO_BIG;
    }
    if (h->packed_size > len - AXL_PACK_HDR_SIZE) {
        return AXL_PACK_E_TRUNCATED;
    }
    return AXL_PACK_OK;
}

AxlPackStatus
axl_pack_parse(
    const void    *buf,
    size_t         len,
    AxlPackHeader *out_hdr)
{
    AxlPackHeader h;
    AxlPackStatus st = pack_check_header(buf, len, &h);
    if (st == AXL_PACK_OK) {
        st = pack_check_extent(&h, len);
    }
    if (st == AXL_PACK_OK && out_hdr != NULL) {
        *out_hdr = h;
    }
    return st;
}

AxlPackStatus
axl_pack_unpack(
    const void         *buf,
    size_t              len,
    uint16_t            expect_machine,
    const AxlPackAlloc *a,
    void              **out_raw,
    size_t             *out_raw_len)
{
    *out_raw     = NULL;
    *out_raw_len = 0;

    AxlPackHeader h;
    AxlPackStatus st = pack_check_header(buf, len, &h);
    if (st != AXL_PACK_OK) {
        return st;
    }
    if (h.machine != expect_machine) {
        return AXL_PACK_E_MACHINE;
    }
    if (!pack_filter_decodable(h.filter)) {
        /* A filter this decoder was built without (another
           architecture's). Refused before any allocation or decoding. */
        return AXL_PACK_E_HEADER;
    }
    st = pack_check_extent(&h, len);
    if (st != AXL_PACK_OK) {
        return st;
    }

    uint8_t *raw = a->alloc(a->ctx, h.raw_size);
    if (raw == NULL) {
        return AXL_PACK_E_NO_MEMORY;
    }

    PackLzmaAlloc w = { { pack_lzma_alloc, pack_lzma_free }, a };
    SizeT       dest_len = h.raw_size;
    SizeT       src_len  = h.packed_size;
    ELzmaStatus lst;
    SRes res = LzmaDecode(raw, &dest_len,
                          (const Byte *)buf + AXL_PACK_HDR_SIZE, &src_len,
                          h.lzma_props, LZMA_PROPS_SIZE,
                          LZMA_FINISH_END, &lst, &w.iface);
    int finished = lst == LZMA_STATUS_FINISHED_WITH_MARK
                || lst == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK;
    if (res == SZ_ERROR_MEM) {
        a->free(a->ctx, raw);
        return AXL_PACK_E_NO_MEMORY;
    }
    if (res == SZ_OK && finished && dest_len == h.raw_size) {
        /* Undo the filter before the CRC: it covers the ORIGINAL image. */
        pack_filter_dec(raw, h.raw_size, h.filter);
    }
    if (res != SZ_OK || !finished || dest_len != h.raw_size
        || axl_pack_crc32(raw, h.raw_size) != h.raw_crc32) {
        a->free(a->ctx, raw);
        return AXL_PACK_E_CORRUPT;
    }

    *out_raw     = raw;
    *out_raw_len = h.raw_size;
    return AXL_PACK_OK;
}

AxlPackStatus
axl_pack_build(
    const void         *raw,
    size_t              raw_len,
    uint16_t            machine,
    AxlPackFilter       filter,
    const AxlPackAlloc *a,
    void              **out_buf,
    size_t             *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    if (raw_len == 0 || raw_len > AXL_PACK_MAX_RAW) {
        return AXL_PACK_E_TOO_BIG;
    }
    if ((unsigned)filter > (unsigned)AXL_PACK_FILTER_ARM64) {
        return AXL_PACK_E_HEADER;
    }

    /* LZMA SDK worst-case bound. */
    size_t bound = raw_len + raw_len / 2 + 256;
    uint8_t *buf = a->alloc(a->ctx, AXL_PACK_HDR_SIZE + bound);
    if (buf == NULL) {
        return AXL_PACK_E_NO_MEMORY;
    }

    /* The filter runs on a copy: the caller's buffer is never mutated. */
    const uint8_t *src = raw;
    uint8_t       *work = NULL;
    if (filter != AXL_PACK_FILTER_NONE) {
        work = a->alloc(a->ctx, raw_len);
        if (work == NULL) {
            a->free(a->ctx, buf);
            return AXL_PACK_E_NO_MEMORY;
        }
        pack_copy(work, raw, raw_len);
        pack_filter_enc(work, raw_len, (uint8_t)filter);
        src = work;
    }

    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.level      = PACK_LZMA_LEVEL;
    pack_params(filter, &props);
    props.reduceSize = raw_len;   /* shrinks the dictionary for small inputs */
    LzmaEncProps_Normalize(&props);

    PackLzmaAlloc w = { { pack_lzma_alloc, pack_lzma_free }, a };
    Byte  enc_props[LZMA_PROPS_SIZE];
    SizeT enc_props_size = LZMA_PROPS_SIZE;
    SizeT stream_len     = bound;
    SRes  res = LzmaEncode(buf + AXL_PACK_HDR_SIZE, &stream_len,
                           (const Byte *)src, raw_len, &props,
                           enc_props, &enc_props_size,
                           1 /* writeEndMark */, NULL, &w.iface, &w.iface);
    if (work != NULL) {
        a->free(a->ctx, work);
    }
    if (res != SZ_OK || enc_props_size != LZMA_PROPS_SIZE) {
        a->free(a->ctx, buf);
        return res == SZ_ERROR_MEM ? AXL_PACK_E_NO_MEMORY : AXL_PACK_E_CORRUPT;
    }

    pack_copy(buf + OFF_MAGIC, AXL_PACK_MAGIC, 8);
    wr16(buf + OFF_VERSION, (uint16_t)AXL_PACK_VERSION);
    wr16(buf + OFF_MACHINE, machine);
    buf[OFF_FILTER] = (uint8_t)filter;
    pack_copy(buf + OFF_PROPS, enc_props, 5);
    wr16(buf + OFF_RESERVED16, 0);
    wr32(buf + OFF_RAW_SIZE, (uint32_t)raw_len);
    wr32(buf + OFF_PACKED_SIZE, (uint32_t)stream_len);
    wr32(buf + OFF_RAW_CRC, axl_pack_crc32(raw, raw_len));
    wr32(buf + OFF_HDR_CRC, axl_pack_crc32(buf, HDR_CRC_SPAN));
    wr32(buf + OFF_RESERVED, 0);

    *out_buf = buf;
    *out_len = AXL_PACK_HDR_SIZE + stream_len;
    return AXL_PACK_OK;
}

AxlPackStatus
axl_pack_find_payload(
    const void *pe,
    size_t      len,
    size_t     *out_off,
    size_t     *out_len,
    int        *out_headers_short)
{
    const uint8_t *b = pe;
    int            dummy;
    int           *headers_short = (out_headers_short != NULL)
                                       ? out_headers_short : &dummy;
    *headers_short = 0;

    if (len < 2 || b[0] != 'M' || b[1] != 'Z') {
        return AXL_PACK_E_NOT_PACKED;
    }
    /* Every file-supplied quantity below is at most 32 bits, so sums of a
       few of them cannot overflow 64; each is compared with len before the
       bytes it names are read. A header that ends past len decides nothing:
       NOT_PACKED, flagged so a caller holding only a prefix can read more. */
    if (len < PE_DOS_LFANEW + 4u) {
        *headers_short = 1;
        return AXL_PACK_E_NOT_PACKED;
    }
    uint64_t lfanew = rd32(b + PE_DOS_LFANEW);
    if (lfanew + PE_SIG_SIZE + PE_COFF_SIZE > len) {
        *headers_short = 1;
        return AXL_PACK_E_NOT_PACKED;
    }
    if (!pack_equal(b + lfanew, "PE\0\0", PE_SIG_SIZE)) {
        return AXL_PACK_E_NOT_PACKED;
    }
    const uint8_t *coff  = b + lfanew + PE_SIG_SIZE;
    uint64_t       nsec  = rd16(coff + PE_COFF_NSECTIONS);
    uint64_t       table = lfanew + PE_SIG_SIZE + PE_COFF_SIZE
                         + rd16(coff + PE_COFF_OPT_SIZE);
    if (table + nsec * PE_SEC_SIZE > len) {
        *headers_short = 1;
        return AXL_PACK_E_NOT_PACKED;
    }
    for (uint64_t i = 0; i < nsec; i++) {
        const uint8_t *s = b + table + i * PE_SEC_SIZE;
        if (!pack_equal(s, AXL_PACK_SECTION, PE_SEC_NAME_SIZE)) {
            continue;
        }
        uint64_t vsize = rd32(s + PE_SEC_VSIZE);
        uint64_t raw   = rd32(s + PE_SEC_RAW_SIZE);
        uint64_t ptr   = rd32(s + PE_SEC_RAW_PTR);
        if (ptr + raw > len) {
            return AXL_PACK_E_TRUNCATED;   /* named: this IS a wrapped image */
        }
        if (out_off != NULL) {
            *out_off = (size_t)ptr;
        }
        if (out_len != NULL) {
            /* SizeOfRawData is padded to FileAlignment; VirtualSize is the
               linked size (0 in some producers' output: then raw stands). */
            *out_len = (size_t)(vsize != 0 && vsize < raw ? vsize : raw);
        }
        return AXL_PACK_OK;
    }
    return AXL_PACK_E_NOT_PACKED;
}
