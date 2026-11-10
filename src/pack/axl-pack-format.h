/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pack-format.h
    Packed-image format: a 40-byte header followed by a raw LZMA stream
    holding the inner PE image. Freestanding on purpose: this module and its
    .c depend on nothing but <stdint.h>, <stddef.h> and the vendored LZMA SDK,
    so the same source compiles into libaxl, a raw-UEFI stub and a host
    packer. INTERNAL header (not under include/axl).

    On-disk layout, little-endian, serialised byte-wise (never cast over):

      off  size  field
        0     8  magic "AXLPACK1" (no NUL)
        8     2  version
       10     2  machine (PE Machine of the inner image: 0x8664 / 0xAA64)
       12     1  filter (AxlPackFilter)
       13     5  lzma_props (lc/lp/pb byte + LE32 dictionary size)
       18     2  reserved16 (0)
       20     4  raw_size    (inner image bytes)
       24     4  packed_size (LZMA stream bytes following the header)
       28     4  raw_crc32   (CRC-32/IEEE of the inner image)
       32     4  hdr_crc32   (CRC-32 of bytes [0, 32))
       36     4  reserved (0)

    The header CRC covers [0, 32) only: it cannot cover itself, and the
    reserved word at 36 lies OUTSIDE the span. That word is still checked --
    any non-zero value is AXL_PACK_E_HEADER -- so a future version that gives
    it a meaning must also bump the version.
**/

#ifndef AXL_PACK_FORMAT_H
#define AXL_PACK_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define AXL_PACK_MAGIC      "AXLPACK1"
#define AXL_PACK_VERSION    1u
#define AXL_PACK_MAX_RAW    (64u << 20)
#define AXL_PACK_HDR_SIZE   40u
/** PE section holding a wrapped image's payload (8 chars: fills the name). */
#define AXL_PACK_SECTION    ".axlpack"

/* PE/COFF Machine values the format records (PE/COFF spec, "Machine Types").
   The ONE spelling of them: the stub, the in-process loaders and the host
   packer all use these. AXL_PACK_MACHINE_NATIVE is the machine of the code
   being compiled (undefined on any other architecture). Only firmware-side
   code may use it: the host packer takes the machine from its input. */
#define AXL_PACK_MACHINE_X64    0x8664u
#define AXL_PACK_MACHINE_AA64   0xAA64u
#if defined(__x86_64__)
#  define AXL_PACK_MACHINE_NATIVE  AXL_PACK_MACHINE_X64
#elif defined(__aarch64__)
#  define AXL_PACK_MACHINE_NATIVE  AXL_PACK_MACHINE_AA64
#endif

/** Pre-compression filter applied to the inner image (stored as u8). */
typedef enum {
    AXL_PACK_FILTER_NONE  = 0,
    AXL_PACK_FILTER_X86   = 1,
    AXL_PACK_FILTER_ARM64 = 2,
} AxlPackFilter;

/** Parsed header, host byte order. Filled by axl_pack_parse(). */
typedef struct {
    char     magic[8];
    uint16_t version;
    uint16_t machine;
    uint8_t  filter;
    uint8_t  lzma_props[5];
    uint16_t reserved16;
    uint32_t raw_size;
    uint32_t packed_size;
    uint32_t raw_crc32;
    uint32_t hdr_crc32;
    uint32_t reserved;
} AxlPackHeader;

/** Result of every pack operation. */
typedef enum {
    AXL_PACK_OK = 0,
    AXL_PACK_E_NOT_PACKED,     ///< magic absent: caller treats input as raw
    AXL_PACK_E_VERSION,
    AXL_PACK_E_HEADER,         ///< hdr_crc32 mismatch, reserved != 0, bad filter
    AXL_PACK_E_MACHINE,        ///< machine != the caller's expected machine
    AXL_PACK_E_TOO_BIG,        ///< raw_size == 0 or > AXL_PACK_MAX_RAW
    AXL_PACK_E_TRUNCATED,      ///< len < HDR_SIZE + packed_size
    AXL_PACK_E_CORRUPT,        ///< LZMA error, short/long output, raw_crc32 mismatch
    AXL_PACK_E_NO_MEMORY,
} AxlPackStatus;

/** Caller-supplied allocator; the module owns no heap of its own. */
typedef struct {
    void *(*alloc)(void *ctx, size_t n);
    void  (*free)(void *ctx, void *p);
    void  *ctx;
} AxlPackAlloc;

/**
 * @brief Validate a packed image's header without decoding.
 *
 * Checks magic, version, header CRC, reserved fields, filter, size range and
 * that the whole stream is present, in that order. The machine is NOT checked
 * here: only the caller knows which one it expects.
 *
 * @param buf     Candidate image bytes.
 * @param len     Length of @p buf.
 * @param out_hdr Receives the parsed header on AXL_PACK_OK; may be NULL.
 * @return AXL_PACK_OK, or the first failing check (NOT_PACKED means "treat as raw").
 */
AxlPackStatus
axl_pack_parse(
    const void    *buf,
    size_t         len,
    AxlPackHeader *out_hdr);

/**
 * @brief Validate, decode and CRC-check a packed image.
 *
 * Allocates exactly raw_size bytes through @p a and decodes with
 * LzmaDecode(dest_len = raw_size). A machine mismatch is refused before any
 * allocation, as are all header errors.
 *
 * @param buf            Packed image.
 * @param len            Length of @p buf.
 * @param expect_machine PE Machine the inner image must carry.
 * @param a              Allocator for the output buffer and decoder state.
 * @param out_raw        Receives the inner image (free with @p a->free).
 * @param out_raw_len    Receives its length.
 * @return AXL_PACK_OK or the failing status; outputs are NULL/0 on failure.
 */
AxlPackStatus
axl_pack_unpack(
    const void         *buf,
    size_t              len,
    uint16_t            expect_machine,
    const AxlPackAlloc *a,
    void              **out_raw,
    size_t             *out_raw_len);

/**
 * @brief Compress an image into a packed one (level 9, end mark written).
 *
 * The only function that reaches LzmaEnc, so a decode-only image drops the
 * encoder under --gc-sections.
 *
 * @param raw      Inner image.
 * @param raw_len  Length (1 .. AXL_PACK_MAX_RAW).
 * @param machine  PE Machine to record.
 * @param filter   Branch pre-filter, applied to a copy (the caller's buffer is
 *                 never modified); an unknown value returns AXL_PACK_E_HEADER.
 * @param a        Allocator for the output and encoder state.
 * @param out_buf  Receives the packed image (free with @p a->free).
 * @param out_len  Receives its length.
 * @return AXL_PACK_OK or the failing status; outputs are NULL/0 on failure.
 */
AxlPackStatus
axl_pack_build(
    const void         *raw,
    size_t              raw_len,
    uint16_t            machine,
    AxlPackFilter       filter,
    const AxlPackAlloc *a,
    void              **out_buf,
    size_t             *out_len);

/**
 * @brief Locate a wrapped image's payload: the PE section AXL_PACK_SECTION.
 *
 * A wrapped .efi is the stub linked with the packed image in its own section,
 * so a FILE is recognised by its section table, never by scanning for the
 * magic (a raw image may legitimately embed a packed blob in .rodata). Every
 * offset the file supplies -- e_lfanew, the COFF header, SizeOfOptionalHeader,
 * NumberOfSections, the section's PointerToRawData/SizeOfRawData -- is checked
 * against @p len before it is used.
 *
 * Two different shortfalls, kept apart because only one of them says
 * anything about the input:
 *   - the PE headers or section table end past @p len: nothing is known, so
 *     the answer is NOT_PACKED with *@p out_headers_short = 1. On a COMPLETE
 *     buffer that is simply a malformed (or non-) PE, not a packed image; on
 *     a prefix of a file it means "read more to decide".
 *   - AXL_PACK_SECTION is named but its bytes end past @p len: TRUNCATED.
 *     The input IS a wrapped image; on a complete buffer, a cut-off one.
 *
 * The payload length is min(VirtualSize, SizeOfRawData) (VirtualSize 0 means
 * SizeOfRawData), so file-alignment padding is excluded.
 *
 * @param pe                Image bytes from offset 0.
 * @param len               Bytes available at @p pe.
 * @param out_off           Receives the payload's offset on AXL_PACK_OK; may be NULL.
 * @param out_len           Receives the payload's length on AXL_PACK_OK; may be NULL.
 * @param out_headers_short Receives 1 when the answer is NOT_PACKED only because
 *                          the headers end past @p len, else 0; may be NULL.
 * @return AXL_PACK_OK when found and wholly inside @p len;
 *         AXL_PACK_E_TRUNCATED when the section is named but its bytes end
 *         past @p len; AXL_PACK_E_NOT_PACKED otherwise (no "MZ", no
 *         "PE\0\0" signature, no AXL_PACK_SECTION, or headers cut short).
 */
AxlPackStatus
axl_pack_find_payload(
    const void *pe,
    size_t      len,
    size_t     *out_off,
    size_t     *out_len,
    int        *out_headers_short);

/** Fixed, human-readable text for a status ("unknown pack status" otherwise). */
const char *
axl_pack_status_str(
    AxlPackStatus s);

/** CRC-32 (IEEE 802.3, reflected, as used by zlib) of @p len bytes. */
uint32_t
axl_pack_crc32(
    const void *buf,
    size_t      len);

#endif /* AXL_PACK_FORMAT_H */
