/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-pack.c
    Unit tests for the packed-image format (src/pack/axl-pack-format.c).
    Pure buffer tests; no QEMU firmware, no UEFI calls.
**/

#include <axl.h>
#include "../../src/pack/axl-pack-format.h"
#include "axl-test.h"

/* ---------------------------------------------------------------------------
 * Allocator fixture: counts calls, can be told to fail every allocation.
 * ---------------------------------------------------------------------------
 */

typedef struct {
    int  allocs;
    bool fail;
} PackAllocState;

static void *
pack_test_alloc(void *ctx, size_t n)
{
    PackAllocState *st = ctx;
    st->allocs++;
    if (st->fail) {
        return NULL;
    }
    return axl_malloc(n);
}

static void
pack_test_free(void *ctx, void *p)
{
    (void)ctx;
    axl_free(p);
}

static PackAllocState g_state;

static AxlPackAlloc g_alloc = { pack_test_alloc, pack_test_free, &g_state };

#define MACHINE_X64  AXL_PACK_MACHINE_X64
#define MACHINE_AA64 AXL_PACK_MACHINE_AA64

/* libaxl decodes with THIS architecture's branch converter only (the other
   one is never linked); the encoder still produces both. */
#if defined(__x86_64__)
#define FILTER_NATIVE   AXL_PACK_FILTER_X86
#define FILTER_FOREIGN  AXL_PACK_FILTER_ARM64
#define MACHINE_FOREIGN MACHINE_AA64
#else
#define FILTER_NATIVE   AXL_PACK_FILTER_ARM64
#define FILTER_FOREIGN  AXL_PACK_FILTER_X86
#define MACHINE_FOREIGN MACHINE_X64
#endif

/* Header field offsets (R1 layout). */
#define OFF_VERSION   8u
#define OFF_FILTER    12u
#define OFF_RAW_SIZE  20u
#define OFF_PACKED_SIZE 24u
#define OFF_RAW_CRC   28u
#define OFF_HDR_CRC   32u

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t
get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Recompute the header CRC after a deliberate field edit. */
static void
fix_hdr_crc(uint8_t *packed)
{
    put32(packed + OFF_HDR_CRC, axl_pack_crc32(packed, OFF_HDR_CRC));
}

static uint8_t *
make_pattern(size_t n)
{
    uint8_t *p = axl_malloc(n);
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)("axl-pack"[i % 8] + (i / 64) % 7);
    }
    return p;
}

static uint8_t *
make_random(size_t n)
{
    uint8_t *p = axl_malloc(n);
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = (uint8_t)(s >> 24);
    }
    return p;
}

/* Build a packed image of 70000 random bytes for the error-path tests. */
static uint8_t *
build_sample(size_t *out_len)
{
    uint8_t *raw = make_random(70000);
    void *packed = NULL;
    AxlPackStatus st = axl_pack_build(raw, 70000, MACHINE_X64,
                                      AXL_PACK_FILTER_NONE, &g_alloc,
                                      &packed, out_len);
    axl_free(raw);
    return st == AXL_PACK_OK ? packed : NULL;
}

/* Unpack and assert the status; frees any output. */
static AxlPackStatus
unpack_status(const void *buf, size_t len, uint16_t machine)
{
    void  *out = NULL;
    size_t out_len = 0;
    AxlPackStatus st = axl_pack_unpack(buf, len, machine, &g_alloc,
                                       &out, &out_len);
    if (st == AXL_PACK_OK) {
        axl_free(out);
    }
    return st;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 */

static void
round_trip_one(const uint8_t *raw, size_t n, const char *name)
{
    void  *packed = NULL;
    size_t packed_len = 0;
    AxlPackStatus st = axl_pack_build(raw, n, MACHINE_X64,
                                      AXL_PACK_FILTER_NONE, &g_alloc,
                                      &packed, &packed_len);
    test_check(st == AXL_PACK_OK, name);
    if (st != AXL_PACK_OK) {
        return;
    }

    AxlPackHeader h;
    test_check(axl_pack_parse(packed, packed_len, &h) == AXL_PACK_OK,
               "round_trip: parse ok");
    test_check(h.raw_size == n && h.machine == MACHINE_X64
               && h.version == AXL_PACK_VERSION
               && h.packed_size == packed_len - AXL_PACK_HDR_SIZE,
               "round_trip: parsed fields");

    void  *out = NULL;
    size_t out_len = 0;
    st = axl_pack_unpack(packed, packed_len, MACHINE_X64, &g_alloc,
                         &out, &out_len);
    test_check(st == AXL_PACK_OK, "round_trip: unpack ok");
    test_check(out_len == n, "round_trip: length");
    test_check(out != NULL && axl_memcmp(out, raw, n) == 0,
               "round_trip: bytes");
    axl_free(out);
    axl_free(packed);
}

static void
test_pack_round_trip(void)
{
    uint8_t one = 0x5A;
    round_trip_one(&one, 1, "round_trip: 1 byte build");
    uint8_t *pat = make_pattern(4096);
    round_trip_one(pat, 4096, "round_trip: 4096 pattern build");
    axl_free(pat);
    uint8_t *rnd = make_random(70000);
    round_trip_one(rnd, 70000, "round_trip: 70000 random build");
    axl_free(rnd);
}

static void
test_pack_not_packed(void)
{
    uint8_t pe[64] = { 'M', 'Z' };
    test_check(axl_pack_parse(pe, sizeof pe, NULL) == AXL_PACK_E_NOT_PACKED,
               "not_packed: parse");
    test_check(unpack_status(pe, sizeof pe, MACHINE_X64)
               == AXL_PACK_E_NOT_PACKED, "not_packed: unpack");
    test_check(axl_pack_parse(pe, 3, NULL) == AXL_PACK_E_NOT_PACKED,
               "not_packed: shorter than the magic");
}

static void
test_pack_header_errors(void)
{
    size_t len = 0;
    uint8_t *p = build_sample(&len);
    test_check(p != NULL, "header_errors: sample built");
    if (p == NULL) {
        return;
    }

    /* bad_version: version 2 with a valid header CRC. */
    put16(p + OFF_VERSION, 2);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_VERSION,
               "bad_version");
    put16(p + OFF_VERSION, 1);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_OK,
               "bad_version: control (restored header is good)");

    /* hdr_crc: flip a header byte without fixing the CRC. */
    p[10] ^= 0x01;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_HEADER,
               "hdr_crc");
    p[10] ^= 0x01;

    /* reserved fields and the filter byte, each with a valid CRC. */
    p[18] = 1;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_E_HEADER,
               "header: reserved16 != 0");
    p[18] = 0;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_OK,
               "header: control (reserved16 restored)");
    p[36] = 1;
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_E_HEADER,
               "header: reserved != 0");
    p[36] = 0;
    p[12] = 9;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_E_HEADER,
               "header: unknown filter");
    p[OFF_FILTER] = FILTER_NATIVE;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_OK,
               "header: native filter byte parses");
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_CORRUPT,
               "header: native filter on an unfiltered stream fails the raw CRC");
    p[OFF_FILTER] = FILTER_FOREIGN;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_OK,
               "header: foreign filter byte parses");
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_HEADER,
               "header: foreign filter is refused by the native-only decoder");
    p[12] = 0;
    fix_hdr_crc(p);
    test_check(axl_pack_parse(p, len, NULL) == AXL_PACK_OK,
               "header: control (restored header parses)");
    axl_free(p);
}

static void
test_pack_wrong_machine(void)
{
    size_t len = 0;
    uint8_t *p = build_sample(&len);
    test_check(p != NULL, "wrong_machine: sample built");
    if (p == NULL) {
        return;
    }
    test_check(unpack_status(p, len, MACHINE_AA64) == AXL_PACK_E_MACHINE,
               "wrong_machine");
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_OK,
               "wrong_machine: control (right machine)");
    axl_free(p);
}

static void
test_pack_sizes(void)
{
    size_t len = 0;
    uint8_t *p = build_sample(&len);
    test_check(p != NULL, "sizes: sample built");
    if (p == NULL) {
        return;
    }
    uint32_t raw_size = get32(p + OFF_RAW_SIZE);

    /* too_big: refused before any allocation. */
    put32(p + OFF_RAW_SIZE, AXL_PACK_MAX_RAW + 1u);
    fix_hdr_crc(p);
    g_state.allocs = 0;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_TOO_BIG,
               "too_big");
    test_check(g_state.allocs == 0, "too_big: allocator never called");

    /* zero_size. */
    put32(p + OFF_RAW_SIZE, 0);
    fix_hdr_crc(p);
    g_state.allocs = 0;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_TOO_BIG,
               "zero_size");
    test_check(g_state.allocs == 0, "zero_size: allocator never called");

    /* size_lies_low / size_lies_high. */
    put32(p + OFF_RAW_SIZE, raw_size - 1);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_CORRUPT,
               "size_lies_low");
    put32(p + OFF_RAW_SIZE, raw_size + 1);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_CORRUPT,
               "size_lies_high");
    put32(p + OFF_RAW_SIZE, raw_size);
    fix_hdr_crc(p);

    /* crc_mismatch. */
    put32(p + OFF_RAW_CRC, get32(p + OFF_RAW_CRC) ^ 1u);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_CORRUPT,
               "crc_mismatch");
    put32(p + OFF_RAW_CRC, get32(p + OFF_RAW_CRC) ^ 1u);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_OK,
               "sizes: control (restored image unpacks)");

    /* packed_size 0xFFFFFFFF with a valid header CRC: the extent check must
       not wrap (HDR_SIZE + packed_size overflows 32 bits). */
    uint32_t packed_size = get32(p + OFF_PACKED_SIZE);
    put32(p + OFF_PACKED_SIZE, 0xFFFFFFFFu);
    fix_hdr_crc(p);
    g_state.allocs = 0;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_TRUNCATED,
               "packed_size 0xFFFFFFFF");
    test_check(g_state.allocs == 0,
               "packed_size 0xFFFFFFFF: allocator never called");
    put32(p + OFF_PACKED_SIZE, packed_size);
    fix_hdr_crc(p);
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_OK,
               "packed_size: control (restored image unpacks)");

    /* truncated: drop the last byte. */
    test_check(unpack_status(p, len - 1, MACHINE_X64) == AXL_PACK_E_TRUNCATED,
               "truncated");
    test_check(axl_pack_parse(p, AXL_PACK_HDR_SIZE - 1, NULL)
               == AXL_PACK_E_TRUNCATED, "truncated: shorter than a header");

    /* corrupt_stream: flip a byte in the middle of the LZMA stream. */
    p[AXL_PACK_HDR_SIZE + (len - AXL_PACK_HDR_SIZE) / 2] ^= 0xFF;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_CORRUPT,
               "corrupt_stream");
    axl_free(p);
}

static void
test_pack_oom(void)
{
    size_t len = 0;
    uint8_t *p = build_sample(&len);
    test_check(p != NULL, "oom: sample built");
    if (p == NULL) {
        return;
    }
    g_state.fail = true;
    test_check(unpack_status(p, len, MACHINE_X64) == AXL_PACK_E_NO_MEMORY,
               "oom: unpack");
    void  *out = NULL;
    size_t out_len = 0;
    uint8_t raw[16] = { 1 };
    test_check(axl_pack_build(raw, sizeof raw, MACHINE_X64,
                              AXL_PACK_FILTER_NONE, &g_alloc, &out, &out_len)
               == AXL_PACK_E_NO_MEMORY, "oom: build");
    test_check(out == NULL, "oom: build leaves no output");
    g_state.fail = false;
    axl_free(p);
}

static void
test_pack_build_refusals(void)
{
    void  *out = NULL;
    size_t out_len = 0;
    uint8_t raw[16] = { 1 };
    test_check(axl_pack_build(raw, 0, MACHINE_X64, AXL_PACK_FILTER_NONE,
                              &g_alloc, &out, &out_len) == AXL_PACK_E_TOO_BIG,
               "build: empty input");
    test_check(axl_pack_build(raw, sizeof raw, MACHINE_X64,
                              (AxlPackFilter)9, &g_alloc, &out, &out_len)
               == AXL_PACK_E_HEADER, "build: unknown filter");
    test_check(out == NULL, "build: unknown filter leaves no output");
}

/* ---------------------------------------------------------------------------
 * Branch filters
 * ---------------------------------------------------------------------------
 */

#define FILTER_BUF  32768u
#define FILTER_STRIDE 16u

/* Call/branch targets repeat across a small set, so the ABSOLUTE addresses
   the filter produces repeat while the RELATIVE ones the compiler emitted
   (target - pc) never do. */
static const uint32_t g_targets[4] = { 0x2000u, 0x3400u, 0x5A80u, 0x7C10u };

/* x86: every 16 bytes, E8 rel32 followed by 11 constant filler bytes. */
static uint8_t *
make_x86_calls(size_t n)
{
    uint8_t *p = axl_malloc(n);
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(0x90 + (i % 5));
    }
    for (size_t off = 0; off + FILTER_STRIDE <= n; off += FILTER_STRIDE) {
        uint32_t tgt = g_targets[(off / FILTER_STRIDE * 7) % 4];
        put32(p + off + 1, tgt - (uint32_t)(off + 5));
        p[off] = 0xE8;
    }
    return p;
}

/* ARM64: every 16 bytes, BL imm26 followed by three NOPs. */
static uint8_t *
make_arm64_calls(size_t n)
{
    uint8_t *p = axl_malloc(n);
    for (size_t off = 0; off + FILTER_STRIDE <= n; off += FILTER_STRIDE) {
        uint32_t tgt = g_targets[(off / FILTER_STRIDE * 7) % 4];
        uint32_t imm = ((tgt - (uint32_t)off) >> 2) & 0x03FFFFFFu;
        put32(p + off, 0x94000000u | imm);
        put32(p + off + 4, 0xD503201Fu);
        put32(p + off + 8, 0xD503201Fu);
        put32(p + off + 12, 0xD503201Fu);
    }
    return p;
}

/* Build with @p filter and with NONE; the filtered stream must round trip,
   be smaller than the unfiltered one, and the caller's buffer must come out
   of the build byte-identical (the filter works on a copy). */
static void
filter_case(uint8_t *raw, size_t n, AxlPackFilter filter, uint16_t machine,
            const char *name)
{
    uint8_t *orig = axl_malloc(n);
    axl_memcpy(orig, raw, n);

    void  *plain = NULL, *filt = NULL;
    size_t plain_len = 0, filt_len = 0;
    test_check(axl_pack_build(raw, n, machine, AXL_PACK_FILTER_NONE, &g_alloc,
                              &plain, &plain_len) == AXL_PACK_OK,
               "filter: unfiltered build");
    AxlPackStatus st = axl_pack_build(raw, n, machine, filter, &g_alloc,
                                      &filt, &filt_len);
    test_check(st == AXL_PACK_OK, name);
    test_check(axl_memcmp(raw, orig, n) == 0,
               "filter: build did not mutate the caller's buffer");
    if (st == AXL_PACK_OK) {
        AxlPackHeader h;
        test_check(axl_pack_parse(filt, filt_len, &h) == AXL_PACK_OK
                   && h.filter == (uint8_t)filter,
                   "filter: header records the filter");
        test_check(filt_len < plain_len,
                   "filter: filtered stream is smaller than unfiltered");
        void  *out = NULL;
        size_t out_len = 0;
        st = axl_pack_unpack(filt, filt_len, machine, &g_alloc, &out,
                             &out_len);
        test_check(st == AXL_PACK_OK && out_len == n, "filter: unpack ok");
        test_check(out != NULL && axl_memcmp(out, orig, n) == 0,
                   "filter: round trip restores the original bytes");
        axl_free(out);
    }
    axl_free(filt);
    axl_free(plain);
    axl_free(orig);
}

/* The native filter round trips here; the foreign one is built (the encoder
   keeps both) and refused by the decoder before it allocates anything. The
   foreign converter's round trip is the host packer's to prove
   (test-efi-pack-host.sh, both machines through axl-efi-pack --verify). */
static uint8_t *
make_native_calls(size_t n)
{
    return FILTER_NATIVE == AXL_PACK_FILTER_X86 ? make_x86_calls(n)
                                                : make_arm64_calls(n);
}

static uint8_t *
make_foreign_calls(size_t n)
{
    return FILTER_FOREIGN == AXL_PACK_FILTER_X86 ? make_x86_calls(n)
                                                 : make_arm64_calls(n);
}

static void
test_pack_filters(void)
{
    uint8_t *nat = make_native_calls(FILTER_BUF);
    filter_case(nat, FILTER_BUF, FILTER_NATIVE, AXL_PACK_MACHINE_NATIVE,
                "filter: native build");
    axl_free(nat);

    /* A size that is not a multiple of the instruction width. */
    uint8_t *natb = make_native_calls(FILTER_BUF + 3);
    filter_case(natb, FILTER_BUF + 3, FILTER_NATIVE, AXL_PACK_MACHINE_NATIVE,
                "filter: native build, ragged tail");
    axl_free(natb);

    uint8_t *frn = make_foreign_calls(FILTER_BUF);
    void    *pk = NULL;
    size_t   pk_len = 0;
    test_check(axl_pack_build(frn, FILTER_BUF, MACHINE_FOREIGN, FILTER_FOREIGN,
                              &g_alloc, &pk, &pk_len) == AXL_PACK_OK,
               "filter: foreign build (the encoder keeps both filters)");
    if (pk != NULL) {
        g_state.allocs = 0;
        test_check(unpack_status(pk, pk_len, MACHINE_FOREIGN)
                   == AXL_PACK_E_HEADER,
                   "filter: foreign filter refused by the native decoder");
        test_check(g_state.allocs == 0,
                   "filter: foreign refusal allocates nothing");
    }
    axl_free(pk);
    axl_free(frn);
}

static void
test_pack_status_str(void)
{
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_OK), "ok") == 0,
               "status_str: ok");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_NOT_PACKED),
                          "not a packed image") == 0, "status_str: not_packed");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_VERSION),
                          "packed image version is not supported") == 0,
               "status_str: version");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_HEADER),
                          "packed image header is invalid") == 0,
               "status_str: header");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_MACHINE),
                          "packed image is for a different architecture") == 0,
               "status_str: machine");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_TOO_BIG),
                          "packed image size is out of range") == 0,
               "status_str: too_big");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_TRUNCATED),
                          "packed image is truncated") == 0,
               "status_str: truncated");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_CORRUPT),
                          "packed image is corrupt") == 0,
               "status_str: corrupt");
    test_check(axl_strcmp(axl_pack_status_str(AXL_PACK_E_NO_MEMORY),
                          "out of memory unpacking image") == 0,
               "status_str: no_memory");
    test_check(axl_strcmp(axl_pack_status_str((AxlPackStatus)99),
                          "unknown pack status") == 0, "status_str: unknown");
}

static void
test_pack_crc32_known(void)
{
    test_check(axl_pack_crc32("123456789", 9) == 0xCBF43926u, "crc32_known");
    test_check(axl_pack_crc32("", 0) == 0u, "crc32_known: empty");
}

/* ---------------------------------------------------------------------------
 * axl_pack_find_payload: a wrapped FILE is recognised by its section table.
 * ---------------------------------------------------------------------------
 */

#define PE_LFANEW     0x40u
#define PE_COFF       (PE_LFANEW + 4u)
#define PE_OPT_SIZE   240u
#define PE_TABLE      (PE_COFF + 20u + PE_OPT_SIZE)   /* 328 */
#define PE_SEC_SIZE   40u
#define PE_PAYLOAD    512u

/* MZ + e_lfanew + "PE\0\0" + COFF header (nsec sections) + a zeroed PE32+
   optional header. Section entries go at PE_TABLE. */
static void
pe_put_headers(
    uint8_t  *b,
    uint16_t  nsec)
{
    b[0] = 'M';
    b[1] = 'Z';
    put32(b + 0x3C, PE_LFANEW);
    b[PE_LFANEW]     = 'P';
    b[PE_LFANEW + 1] = 'E';
    put16(b + PE_COFF, MACHINE_X64);
    put16(b + PE_COFF + 2, nsec);
    put16(b + PE_COFF + 16, PE_OPT_SIZE);
}

static void
pe_put_section(
    uint8_t    *b,
    unsigned    i,
    const char *name,
    uint32_t    vsize,
    uint32_t    raw_ptr,
    uint32_t    raw_size)
{
    uint8_t *s = b + PE_TABLE + i * PE_SEC_SIZE;
    for (unsigned k = 0; k < 8 && name[k] != '\0'; k++) {
        s[k] = (uint8_t)name[k];
    }
    put32(s + 8, vsize);
    put32(s + 16, raw_size);
    put32(s + 20, raw_ptr);
}

/* A wrapped-image lookalike: .text, then .axlpack holding a real packed
   payload at PE_PAYLOAD, SizeOfRawData rounded up to 512 as a linker would. */
static uint8_t *
pe_build_wrapped(
    size_t *out_len,
    size_t *out_payload_len)
{
    size_t   packed_len = 0;
    uint8_t *packed = build_sample(&packed_len);
    if (packed == NULL) {
        return NULL;
    }
    uint32_t raw_size = (uint32_t)((packed_len + 511u) & ~(size_t)511u);
    size_t   len = PE_PAYLOAD + raw_size;
    uint8_t *b = axl_calloc(1, len);
    pe_put_headers(b, 2);
    pe_put_section(b, 0, ".text", 16, PE_TABLE + 2 * PE_SEC_SIZE, 16);
    pe_put_section(b, 1, AXL_PACK_SECTION, (uint32_t)packed_len, PE_PAYLOAD,
                   raw_size);
    axl_memcpy(b + PE_PAYLOAD, packed, packed_len);
    axl_free(packed);
    *out_len = len;
    *out_payload_len = packed_len;
    return b;
}

/* axl_pack_find_payload's status plus its headers-short flag, in one value:
   FIND_OK, FIND_CUT (TRUNCATED: section named, bytes cut off), FIND_RAW
   (NOT_PACKED, headers complete) or FIND_SHORT (NOT_PACKED, headers cut). */
typedef enum { FIND_OK, FIND_CUT, FIND_RAW, FIND_SHORT, FIND_OTHER } FindOutcome;

static FindOutcome
find_outcome(
    const uint8_t *b,
    size_t         len)
{
    int short_hdr = -1;
    AxlPackStatus st = axl_pack_find_payload(b, len, NULL, NULL, &short_hdr);
    if (st == AXL_PACK_OK && short_hdr == 0) {
        return FIND_OK;
    }
    if (st == AXL_PACK_E_TRUNCATED && short_hdr == 0) {
        return FIND_CUT;
    }
    if (st == AXL_PACK_E_NOT_PACKED && short_hdr == 0) {
        return FIND_RAW;
    }
    if (st == AXL_PACK_E_NOT_PACKED && short_hdr == 1) {
        return FIND_SHORT;
    }
    return FIND_OTHER;
}

static void
test_pack_find_found(void)
{
    size_t   len = 0;
    size_t   plen = 0;
    uint8_t *b = pe_build_wrapped(&len, &plen);
    test_check(b != NULL, "find: fixture built");
    if (b == NULL) {
        return;
    }

    size_t off = 0;
    size_t got = 0;
    test_check(axl_pack_find_payload(b, len, &off, &got, NULL) == AXL_PACK_OK,
               "find: found");
    test_check(off == PE_PAYLOAD, "find: offset is PointerToRawData");
    test_check(got == plen, "find: length is VirtualSize, not the padded raw size");
    test_check(axl_pack_parse(b + off, got, NULL) == AXL_PACK_OK,
               "find: payload parses");
    test_check(unpack_status(b + off, got, MACHINE_X64) == AXL_PACK_OK,
               "find: payload unpacks");
    test_check(find_outcome(b, len) == FIND_OK, "find: OK clears headers_short");

    /* VirtualSize 0: the raw size stands. */
    put32(b + PE_TABLE + PE_SEC_SIZE + 8, 0);
    test_check(axl_pack_find_payload(b, len, &off, &got, NULL) == AXL_PACK_OK
               && got == len - PE_PAYLOAD, "find: VirtualSize 0 uses SizeOfRawData");
    axl_free(b);
}

/* A prefix of a file: a named section whose bytes are not in it yet is CUT
   (it is a wrapped image); headers that run past it are SHORT (read more). */
static void
test_pack_find_prefix(void)
{
    size_t   len = 0;
    size_t   plen = 0;
    uint8_t *b = pe_build_wrapped(&len, &plen);
    if (b == NULL) {
        test_check(false, "find_prefix: fixture built");
        return;
    }
    size_t table_end = PE_TABLE + 2 * PE_SEC_SIZE;
    test_check(find_outcome(b, table_end) == FIND_CUT,
               "find_prefix: named, bytes beyond -> TRUNCATED");
    test_check(find_outcome(b, len - 1) == FIND_CUT,
               "find_prefix: payload cut by one byte -> TRUNCATED");
    test_check(find_outcome(b, table_end - 1) == FIND_SHORT,
               "find_prefix: table cut short -> NOT_PACKED, headers short");
    test_check(find_outcome(b, 0x3F) == FIND_SHORT,
               "find_prefix: e_lfanew not present -> NOT_PACKED, headers short");

    /* A raw image: the same headers without the section. A header-sized
       prefix is enough to say so, and it is not "short". */
    axl_memset(b + PE_TABLE + PE_SEC_SIZE, 0, PE_SEC_SIZE);
    put16(b + PE_COFF + 2, 1);
    test_check(find_outcome(b, PE_TABLE + PE_SEC_SIZE) == FIND_RAW,
               "find_prefix: raw image from its headers");
    axl_free(b);
}

static void
test_pack_find_not_packed(void)
{
    uint8_t b[1024] = { 0 };
    test_check(find_outcome(b, sizeof b) == FIND_RAW, "find_not_packed: no MZ");
    pe_put_headers(b, 1);
    b[PE_LFANEW + 1] = 'X';
    pe_put_section(b, 0, AXL_PACK_SECTION, 16, PE_PAYLOAD, 16);
    test_check(find_outcome(b, sizeof b) == FIND_RAW,
               "find_not_packed: no PE signature");
    b[PE_LFANEW + 1] = 'E';
    test_check(find_outcome(b, sizeof b) == FIND_OK,
               "find_not_packed: control, the same image is found");

    /* The name must match all eight bytes. */
    b[PE_TABLE + 7] = 'K';
    test_check(find_outcome(b, sizeof b) == FIND_RAW, "find_not_packed: .axlpacK");
    b[PE_TABLE + 7] = 0;
    test_check(find_outcome(b, sizeof b) == FIND_RAW, "find_not_packed: .axlpac");

    /* The packed magic in another section is not a payload. */
    b[PE_TABLE + 7] = 'k';
    b[PE_TABLE + 1] = 'r';
    axl_memcpy(b + PE_PAYLOAD, AXL_PACK_MAGIC, 8);
    test_check(find_outcome(b, sizeof b) == FIND_RAW,
               "find_not_packed: magic in another section");
}

/* Every file-supplied offset, made hostile one at a time, against a buffer
   that IS long enough for the honest image. A hostile HEADER offset is a
   malformed PE, not a packed one (NOT_PACKED, headers short); a hostile
   range for the NAMED section is a cut-off wrapped image (TRUNCATED). The
   call must not read outside the buffer to reach either. */
static void
test_pack_find_hostile(void)
{
    uint8_t b[1024];
    struct {
        const char *name;
        uint32_t    off;
        uint32_t    val;
        int         width;
        FindOutcome want;
    } cases[] = {
        { "find_hostile: e_lfanew past the end",         0x3C,          0xFFFFFFF0u, 32, FIND_SHORT },
        { "find_hostile: e_lfanew leaves no COFF",       0x3C,          1024u - 8u,  32, FIND_SHORT },
        { "find_hostile: NumberOfSections 0xFFFF",       PE_COFF + 2,   0xFFFFu,     16, FIND_SHORT },
        { "find_hostile: SizeOfOptionalHeader 0xFFFF",   PE_COFF + 16,  0xFFFFu,     16, FIND_SHORT },
        { "find_hostile: PointerToRawData wraps 32 bit", PE_TABLE + 20, 0xFFFFFFF0u, 32, FIND_CUT },
        { "find_hostile: SizeOfRawData 0xFFFFFFFF",      PE_TABLE + 16, 0xFFFFFFFFu, 32, FIND_CUT },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        axl_memset(b, 0, sizeof b);
        pe_put_headers(b, 1);
        pe_put_section(b, 0, AXL_PACK_SECTION, 16, PE_PAYLOAD, 16);
        if (cases[i].width == 16) {
            put16(b + cases[i].off, (uint16_t)cases[i].val);
        } else {
            put32(b + cases[i].off, cases[i].val);
        }
        test_check(find_outcome(b, sizeof b) == cases[i].want, cases[i].name);
    }
}

static int
test_pack_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_print_header("AxlPack");

    test_pack_crc32_known();
    test_pack_status_str();
    test_pack_round_trip();
    test_pack_not_packed();
    test_pack_header_errors();
    test_pack_wrong_machine();
    test_pack_sizes();
    test_pack_oom();
    test_pack_build_refusals();
    test_pack_filters();
    test_pack_find_found();
    test_pack_find_prefix();
    test_pack_find_not_packed();
    test_pack_find_hostile();
    return test_print_results();
}

AXL_APP(test_pack_main)
