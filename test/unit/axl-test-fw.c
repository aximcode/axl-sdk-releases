/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-fw.c
    Unit tests for AxlFw — raw firmware image parser.
    Pure buffer tests; no QEMU firmware, no UEFI calls.
**/

#include <axl.h>
#include <axl/axl-fw.h>
#include <axl/axl-compress.h>
#include "axl-test.h"

/* ---------------------------------------------------------------------------
 * Synthetic fixture: a minimal single-FV image with one FFS DRIVER file
 * containing one PE32 section.
 *
 * Byte layout (all values little-endian):
 *
 * [0..71]    EFI_FIRMWARE_VOLUME_HEADER (HeaderLength=0x48, 72 bytes)
 *   [0..15]    ZeroVector              (all 0x00)
 *   [16..31]   FileSystemGuid          (all 0x00, arbitrary)
 *   [32..39]   FvLength                (LE64 = 112 = 0x70)
 *   [40..43]   Signature               "_FVH"
 *   [44..47]   Attributes              (0x00000000)
 *   [48..49]   HeaderLength            (LE16 = 72 = 0x48)
 *   [50..51]   Checksum                (0x0000, not validated by parser)
 *   [52..53]   ExtHeaderOffset         (LE16 = 0, no ext header)
 *   [54]       Reserved                (0x00)
 *   [55]       Revision                (0x02)
 *   [56..63]   BlockMap[0]             NumBlocks=LE32(1), Length=LE32(112)
 *   [64..71]   BlockMap[1] (terminator) NumBlocks=LE32(0), Length=LE32(0)
 *
 * [72..111]  EFI_FFS_FILE_HEADER (24-byte header + 16-byte section = 40 bytes)
 *   [72..87]   Name (file GUID, bytes_le):
 *              11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 00
 *   [88..89]   IntegrityCheck  (0x00 0x00)
 *   [90]       Type            = 0x07 (EFI_FV_FILETYPE_DRIVER)
 *   [91]       Attributes      = 0x00
 *   [92..94]   Size (LE24)     = 40 = 0x28, i.e. 0x28 0x00 0x00
 *   [95]       State           = 0xF8 (valid)
 *
 *   File data (16 bytes):
 *   [96..111]  EFI_COMMON_SECTION_HEADER + body
 *     [96..98]   Size (LE24)   = 16 = 0x10, i.e. 0x10 0x00 0x00
 *     [99]       Type          = 0x10 (EFI_SECTION_PE32)
 *     [100..111] Body: "MZ\x90\x00testbody" (12 bytes)
 *
 * Total image length: 112 bytes
 *
 * Key offsets (within the image buffer, used to pin axl_fw_node_offset):
 *   FIXTURE_VOLUME_OFFSET = 0   (FV header starts at byte 0 of image)
 *   FIXTURE_FILE_OFFSET   = 72  (FFS file starts at byte 72 of image)
 * ---------------------------------------------------------------------------
 */

#define FIXTURE_VOLUME_OFFSET  0u
#define FIXTURE_FILE_OFFSET   72u

/*
 * GUID stored bytes_le: 11 22 33 44  55 66  77 88  99 AA  BB CC DD EE FF 00
 * AxlGuid: Data1=0x44332211, Data2=0x6655, Data3=0x8877,
 *          Data4={0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00}
 */
static const AxlGuid fixture_file_guid = {
    0x44332211u, 0x6655u, 0x8877u,
    { 0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu, 0x00u }
};

static const unsigned char fixture_fv[] = {
    /* [0..15] ZeroVector */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [16..31] FileSystemGuid (zeros, arbitrary) */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [32..39] FvLength = 112 = 0x70 (LE64) */
    0x70, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [40..43] Signature "_FVH" */
    0x5F, 0x46, 0x56, 0x48,
    /* [44..47] Attributes = 0 */
    0x00, 0x00, 0x00, 0x00,
    /* [48..49] HeaderLength = 72 = 0x48 (LE16) */
    0x48, 0x00,
    /* [50..51] Checksum = 0 (not validated by parser) */
    0x00, 0x00,
    /* [52..53] ExtHeaderOffset = 0 (LE16, no ext header) */
    0x00, 0x00,
    /* [54] Reserved */
    0x00,
    /* [55] Revision = 2 */
    0x02,
    /* [56..63] BlockMap[0]: NumBlocks=1, Length=112=0x70 */
    0x01, 0x00, 0x00, 0x00,  0x70, 0x00, 0x00, 0x00,
    /* [64..71] BlockMap[1] terminator */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    /* [72..87] FFS Name GUID bytes_le */
    0x11, 0x22, 0x33, 0x44,  0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    /* [88..89] IntegrityCheck */
    0x00, 0x00,
    /* [90] Type = 0x07 (DRIVER) */
    0x07,
    /* [91] Attributes = 0x00 */
    0x00,
    /* [92..94] Size24 = 40 = 0x28 (LE24) */
    0x28, 0x00, 0x00,
    /* [95] State = 0xF8 */
    0xF8,

    /* [96..98] Section Size24 = 16 = 0x10 (LE24) */
    0x10, 0x00, 0x00,
    /* [99] Section Type = 0x10 (PE32) */
    0x10,
    /* [100..111] Body: "MZ\x90\x00testbody" (12 bytes) */
    0x4D, 0x5A, 0x90, 0x00,  0x74, 0x65, 0x73, 0x74,
    0x62, 0x6F, 0x64, 0x79
};

static const unsigned int fixture_fv_len = sizeof(fixture_fv); /* 112 */

/* ---------------------------------------------------------------------------
 * Regression fixture (integer-overflow guard): a valid FV with ONE real FFS
 * file followed by ERASED FREE SPACE whose Attributes byte is 0xFF.
 *
 * Real OVMF/AAVMF firmware ends each FV's used region with flash-erased
 * (all-0xFF) free space. When the FFS iterator lands on that region it reads
 * Attributes=0xFF — which spuriously sets FFS_ATTRIB_LARGE_FILE (0x01) — so
 * the "0xFFFFFF size24 == end of files" break is skipped, and the 8-byte
 * LARGE_FILE size field (also all 0xFF) is read as 0xFFFFFFFFFFFFFFFF.
 * The naive bounds check `pos + file_size > fv_len` then OVERFLOWS size_t and
 * wraps below fv_len, defeating the break, and `pos = align(pos + file_size)`
 * fails to advance — an INFINITE LOOP. (The Python reference dodges this only
 * because its ints are arbitrary-precision.) The fix compares against the
 * remaining span (`file_size > fv_len - pos`). This fixture pins it: parsing
 * must TERMINATE and expose exactly the one real file.
 *
 * Layout:
 * [0..71]    FV header, FvLength = 152 = 0x98 (so 40 bytes of trailing free
 *            space exist — enough for the iterator to read a 24-byte header
 *            plus the 8-byte LARGE_FILE size at the erased region).
 * [72..111]  One DRIVER file (24-byte header + 16-byte PE32 section), Size24=40.
 * [112..151] Erased free space: 40 bytes of 0xFF.
 * ---------------------------------------------------------------------------
 */
static const AxlGuid fixture_erased_file_guid = {
    0x44332211u, 0x6655u, 0x8877u,
    { 0x99u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu, 0x00u }
};

static const unsigned char fixture_erased_tail_fv[] = {
    /* [0..15] ZeroVector */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [16..31] FileSystemGuid */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [32..39] FvLength = 152 = 0x98 (LE64) */
    0x98, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [40..43] Signature "_FVH" */
    0x5F, 0x46, 0x56, 0x48,
    /* [44..47] Attributes = 0 */
    0x00, 0x00, 0x00, 0x00,
    /* [48..49] HeaderLength = 72 = 0x48 (LE16) */
    0x48, 0x00,
    /* [50..51] Checksum */
    0x00, 0x00,
    /* [52..53] ExtHeaderOffset = 0 */
    0x00, 0x00,
    /* [54] Reserved */
    0x00,
    /* [55] Revision = 2 */
    0x02,
    /* [56..63] BlockMap[0]: NumBlocks=1, Length=152=0x98 */
    0x01, 0x00, 0x00, 0x00,  0x98, 0x00, 0x00, 0x00,
    /* [64..71] BlockMap[1] terminator */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    /* [72..87] FFS Name GUID bytes_le */
    0x11, 0x22, 0x33, 0x44,  0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    /* [88..89] IntegrityCheck */
    0x00, 0x00,
    /* [90] Type = 0x07 (DRIVER) */
    0x07,
    /* [91] Attributes = 0x00 */
    0x00,
    /* [92..94] Size24 = 40 = 0x28 (LE24) */
    0x28, 0x00, 0x00,
    /* [95] State = 0xF8 */
    0xF8,
    /* [96..98] Section Size24 = 16 = 0x10 (LE24) */
    0x10, 0x00, 0x00,
    /* [99] Section Type = 0x10 (PE32) */
    0x10,
    /* [100..111] Body: "MZ\x90\x00testbody" (12 bytes) */
    0x4D, 0x5A, 0x90, 0x00,  0x74, 0x65, 0x73, 0x74,
    0x62, 0x6F, 0x64, 0x79,

    /* [112..151] Erased free space: 40 bytes of 0xFF. The FFS iterator next
       lands here; Attributes (0xFF) sets LARGE_FILE, the LARGE_FILE size
       reads as 0xFFFFFFFFFFFFFFFF — the overflow trap. */
    0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,  0xFF, 0xFF, 0xFF, 0xFF
};

static const unsigned int fixture_erased_tail_fv_len =
    sizeof(fixture_erased_tail_fv); /* 152 */

/* ---------------------------------------------------------------------------
 * Encapsulation fixture: a minimal FV image with ONE FFS DRIVER file that
 * contains TWO sections:
 *   (A) SECTION_COMPRESSION (0x01, CompressionType=0 / uncompressed)
 *       wrapping a single PE32 section (the "compressed-PE" placeholder)
 *   (B) SECTION_FIRMWARE_VOLUME_IMAGE (0x17)
 *       whose body is a self-contained inner FV containing one FFS file
 *
 * Inner PE32 body used inside COMPRESSION: "CP\x00\x00\x00\x00\x00\x00" (8 B)
 * Inner PE32 body used inside the nested FV: "IN\x00\x00\x00\x00\x00\x00" (8 B)
 *
 * Byte layout (all values little-endian):
 *
 * ── OUTER FV (232 bytes = 0xE8) ─────────────────────────────────────────────
 * [0..71]    EFI_FIRMWARE_VOLUME_HEADER (HeaderLength=0x48, 72 bytes)
 *   [0..15]    ZeroVector              (all 0x00)
 *   [16..31]   FileSystemGuid          (all 0x00)
 *   [32..39]   FvLength  = 232 = 0xE8 (LE64)
 *   [40..43]   Signature "_FVH"
 *   [44..47]   Attributes = 0x00000000
 *   [48..49]   HeaderLength = 72 = 0x48 (LE16)
 *   [50..51]   Checksum = 0x0000
 *   [52..53]   ExtHeaderOffset = 0x0000
 *   [54]       Reserved = 0x00
 *   [55]       Revision = 0x02
 *   [56..63]   BlockMap[0]: NumBlocks=1, Length=232=0xE8
 *   [64..71]   BlockMap[1]: terminator
 *
 * [72..231]  EFI_FFS_FILE_HEADER (24 bytes) + file body (136 bytes) = 160 bytes
 *   [72..87]   Name GUID bytes_le:
 *              AA BB CC DD 55 66 77 88 99 AA BB CC DD EE FF 00
 *   [88..89]   IntegrityCheck = 0x00 0x00
 *   [90]       Type = 0x07 (DRIVER)
 *   [91]       Attributes = 0x00
 *   [92..94]   Size24 = 160 = 0xA0 (LE24)
 *   [95]       State = 0xF8
 *
 *   ── SECTION A: COMPRESSION (0x01) at file-body offset 0 (image off 96) ──
 *   [96..98]   Size24 = 21 = 0x15 (LE24)
 *   [99]       Type = 0x01 (COMPRESSION)
 *   [100..103] UncompressedLength = 12 = 0x0C (LE32)
 *   [104]      CompressionType = 0x00 (none)
 *   [105..116] Inner PE32 section (12 bytes):
 *     [105..107]  Size24 = 12 = 0x0C (LE24)
 *     [108]       Type = 0x10 (PE32)
 *     [109..116]  Body: "CP\x00\x00\x00\x00\x00\x00" (8 bytes)
 *   [117..119] Padding to 4-byte align (3 bytes, 0x00)
 *
 *   ── SECTION B: FIRMWARE_VOLUME_IMAGE (0x17) at file-body off 24 (img 120) ─
 *   [120..122] Size24 = 112 = 0x70 (LE24)
 *   [123]      Type = 0x17 (FV_IMAGE)
 *   [124..231] Inner FV (108 bytes):
 *
 *   ── INNER FV (108 bytes, self-contained) ─────────────────────────────────
 *     [124..195]  EFI_FIRMWARE_VOLUME_HEADER (72 bytes):
 *       [124..139]   ZeroVector
 *       [140..155]   FileSystemGuid (zeros)
 *       [156..163]   FvLength = 108 = 0x6C (LE64)
 *       [164..167]   Signature "_FVH"
 *       [168..171]   Attributes = 0x00000000
 *       [172..173]   HeaderLength = 72 = 0x48 (LE16)
 *       [174..175]   Checksum = 0x0000
 *       [176..177]   ExtHeaderOffset = 0x0000
 *       [178]        Reserved = 0x00
 *       [179]        Revision = 0x02
 *       [180..187]   BlockMap[0]: NumBlocks=1, Length=108=0x6C
 *       [188..195]   BlockMap[1]: terminator
 *     [196..231]  Inner FFS file (36 bytes):
 *       [196..211]   Name GUID bytes_le:
 *                    BB CC DD EE 55 66 77 88 99 AA BB CC DD EE FF 00
 *       [212..213]   IntegrityCheck = 0x00 0x00
 *       [214]        Type = 0x07 (DRIVER)
 *       [215]        Attributes = 0x00
 *       [216..218]   Size24 = 36 = 0x24 (LE24)
 *       [219]        State = 0xF8
 *       Inner PE32 section (12 bytes) at inner-FV offset 96:
 *       [220..222]   Size24 = 12 = 0x0C (LE24)
 *       [223]        Type = 0x10 (PE32)
 *       [224..231]   Body: "IN\x00\x00\x00\x00\x00\x00" (8 bytes)
 *
 * Total image length: 232 bytes
 *
 * Key offsets for SECTION A:
 *   ENCAP_VOLUME_OFFSET    = 0    (outer FV header in image)
 *   ENCAP_FILE_OFFSET      = 72   (outer FFS file in image)
 *   ENCAP_COMP_SEC_OFFSET  = 0    (COMPRESSION section in file-body stream)
 *   ENCAP_FVI_SEC_OFFSET   = 24   (FV_IMAGE section in file-body stream)
 * ---------------------------------------------------------------------------
 */

#define ENCAP_VOLUME_OFFSET    0u
#define ENCAP_FILE_OFFSET     72u

static const unsigned char fixture_encap[] = {
    /* ── OUTER FV HEADER (72 bytes) ────────────────────────────── */
    /* [0..15] ZeroVector */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [16..31] FileSystemGuid (zeros) */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [32..39] FvLength = 232 = 0xE8 (LE64) */
    0xE8, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [40..43] Signature "_FVH" */
    0x5F, 0x46, 0x56, 0x48,
    /* [44..47] Attributes = 0 */
    0x00, 0x00, 0x00, 0x00,
    /* [48..49] HeaderLength = 72 = 0x48 (LE16) */
    0x48, 0x00,
    /* [50..51] Checksum = 0 */
    0x00, 0x00,
    /* [52..53] ExtHeaderOffset = 0 (LE16) */
    0x00, 0x00,
    /* [54] Reserved */
    0x00,
    /* [55] Revision = 2 */
    0x02,
    /* [56..63] BlockMap[0]: NumBlocks=1, Length=232=0xE8 */
    0x01, 0x00, 0x00, 0x00,  0xE8, 0x00, 0x00, 0x00,
    /* [64..71] BlockMap[1] terminator */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    /* ── OUTER FFS FILE HEADER (24 bytes) at image offset 72 ────── */
    /* [72..87] Name GUID bytes_le: AA BB CC DD 55 66 77 88 99 AA BB CC DD EE FF 00 */
    0xAA, 0xBB, 0xCC, 0xDD,  0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    /* [88..89] IntegrityCheck = 0 */
    0x00, 0x00,
    /* [90] Type = 0x07 (DRIVER) */
    0x07,
    /* [91] Attributes = 0 */
    0x00,
    /* [92..94] Size24 = 160 = 0xA0 (LE24) */
    0xA0, 0x00, 0x00,
    /* [95] State = 0xF8 */
    0xF8,

    /* ── FILE BODY (136 bytes) starting at image offset 96 ────────── */

    /* ── SECTION A: COMPRESSION (0x01) at file-body offset 0, image offset 96 */
    /* [96..98] Size24 = 21 = 0x15 (LE24) */
    0x15, 0x00, 0x00,
    /* [99] Type = 0x01 (COMPRESSION) */
    0x01,
    /* [100..103] UncompressedLength = 12 = 0x0C (LE32) */
    0x0C, 0x00, 0x00, 0x00,
    /* [104] CompressionType = 0 (none / uncompressed) */
    0x00,
    /* [105..116] Inner PE32 section (12 bytes): */
    /*   [105..107] Size24 = 12 = 0x0C (LE24) */
    0x0C, 0x00, 0x00,
    /*   [108] Type = 0x10 (PE32) */
    0x10,
    /*   [109..116] Body "CP\x00\x00\x00\x00\x00\x00" (8 bytes) */
    0x43, 0x50, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [117..119] Padding to 4-byte align (3 bytes) */
    0x00, 0x00, 0x00,

    /* ── SECTION B: FIRMWARE_VOLUME_IMAGE (0x17) at file-body offset 24 ── */
    /* image offset 120 */
    /* [120..122] Size24 = 112 = 0x70 (LE24) */
    0x70, 0x00, 0x00,
    /* [123] Type = 0x17 (FV_IMAGE) */
    0x17,

    /* ── INNER FV HEADER (72 bytes) starting at image offset 124 ──────── */
    /* [124..139] ZeroVector */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [140..155] FileSystemGuid (zeros) */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [156..163] FvLength = 108 = 0x6C (LE64) */
    0x6C, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [164..167] Signature "_FVH" */
    0x5F, 0x46, 0x56, 0x48,
    /* [168..171] Attributes = 0 */
    0x00, 0x00, 0x00, 0x00,
    /* [172..173] HeaderLength = 72 = 0x48 (LE16) */
    0x48, 0x00,
    /* [174..175] Checksum = 0 */
    0x00, 0x00,
    /* [176..177] ExtHeaderOffset = 0 */
    0x00, 0x00,
    /* [178] Reserved */
    0x00,
    /* [179] Revision = 2 */
    0x02,
    /* [180..187] BlockMap[0]: NumBlocks=1, Length=108=0x6C */
    0x01, 0x00, 0x00, 0x00,  0x6C, 0x00, 0x00, 0x00,
    /* [188..195] BlockMap[1] terminator */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    /* ── INNER FFS FILE HEADER (24 bytes) at inner-FV offset 72 ─────── */
    /* image offset 196 */
    /* [196..211] Name GUID bytes_le: BB CC DD EE 55 66 77 88 99 AA BB CC DD EE FF 00 */
    0xBB, 0xCC, 0xDD, 0xEE,  0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC,  0xDD, 0xEE, 0xFF, 0x00,
    /* [212..213] IntegrityCheck = 0 */
    0x00, 0x00,
    /* [214] Type = 0x07 (DRIVER) */
    0x07,
    /* [215] Attributes = 0 */
    0x00,
    /* [216..218] Size24 = 36 = 0x24 (LE24) */
    0x24, 0x00, 0x00,
    /* [219] State = 0xF8 */
    0xF8,
    /* ── INNER FILE BODY: one PE32 section (12 bytes) ────────────────── */
    /* image offset 220, inner-FV offset 96 */
    /* [220..222] Size24 = 12 = 0x0C (LE24) */
    0x0C, 0x00, 0x00,
    /* [223] Type = 0x10 (PE32) */
    0x10,
    /* [224..231] Body "IN\x00\x00\x00\x00\x00\x00" (8 bytes) */
    0x49, 0x4E, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00
};

static const unsigned int fixture_encap_len = sizeof(fixture_encap); /* 232 */

/* ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 */

static void
test_fw_open_close(void)
{
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    test_check(img != NULL, "fw: open synthetic FV");
    axl_fw_close(img);

    /* NULL-safe close */
    axl_fw_close(NULL);
    test_survived("fw: close(NULL) does not crash");

    /* NULL image: axl_fw_root returns NULL */
    test_check(axl_fw_root(NULL) == NULL, "fw: root(NULL) == NULL");
}

static void
test_fw_uncompressed_tree(void)
{
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    test_check(img != NULL, "fw: open synthetic FV (tree)");
    if (img == NULL)
        return;

    /* Root node */
    AxlFwNode *root = axl_fw_root(img);
    test_check(root != NULL, "fw: root node exists");
    test_check(root != NULL && axl_fw_node_kind(root) == AXL_FW_NODE_IMAGE,
               "fw: root kind == IMAGE");

    /* Volume node under root */
    AxlFwNode *vol = axl_fw_node_first_child(root);
    test_check(vol != NULL, "fw: root has a volume child");
    test_check(vol != NULL && axl_fw_node_kind(vol) == AXL_FW_NODE_VOLUME,
               "fw: root's first child is a VOLUME");

    /* No second volume in this fixture */
    test_check(axl_fw_node_next_sibling(vol) == NULL,
               "fw: volume has no sibling");

    /* File node under volume */
    AxlFwNode *file = axl_fw_node_first_child(vol);
    test_check(file != NULL && axl_fw_node_kind(file) == AXL_FW_NODE_FILE,
               "fw: volume has a file child");
    test_check(file != NULL && axl_fw_node_type(file) == 0x07,
               "fw: file type == 0x07 (DRIVER)");
    test_check(file != NULL && axl_fw_node_next_sibling(file) == NULL,
               "fw: file has no sibling");

    /* Section under file */
    AxlFwNode *sec = axl_fw_node_first_child(file);
    test_check(sec != NULL && axl_fw_node_kind(sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(sec) == 0x10,
               "fw: file has PE32 section");
    test_check(sec != NULL && axl_fw_node_next_sibling(sec) == NULL,
               "fw: section has no sibling");

    /* Section body: exact "MZ\x90\x00testbody" */
    const void *body = NULL;
    size_t blen = 0;
    test_check(axl_fw_node_data(sec, &body, &blen) && blen >= 2
               && axl_memcmp(body, "MZ", 2) == 0,
               "fw: section body is the PE32");
    test_check(blen == 12
               && axl_memcmp(body, "MZ\x90\x00testbody", 12) == 0,
               "fw: section body exact (12 bytes)");

    axl_fw_close(img);
}

static void
test_fw_guid(void)
{
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    if (img == NULL) {
        test_check(false, "fw: open for GUID test");
        return;
    }

    AxlFwNode *vol  = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file = axl_fw_node_first_child(vol);

    /* File node should have a GUID */
    AxlGuid got;
    test_check(axl_fw_node_guid(file, &got), "fw: file has a GUID");
    test_check(axl_memcmp(&got, &fixture_file_guid, sizeof(AxlGuid)) == 0,
               "fw: file GUID matches fixture bytes_le");

    /* VOLUME node has no GUID */
    test_check(!axl_fw_node_guid(vol, &got), "fw: volume has no GUID");

    /* NULL node */
    test_check(!axl_fw_node_guid(NULL, &got), "fw: guid(NULL) == false");

    axl_fw_close(img);
}

static void
test_fw_offset(void)
{
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    if (img == NULL) {
        test_check(false, "fw: open for offset test");
        return;
    }

    AxlFwNode *vol  = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file = axl_fw_node_first_child(vol);

    /* Volume is at image byte 0 */
    size_t voff = (size_t)-1;
    test_check(axl_fw_node_offset(vol, &voff), "fw: volume offset returns true");
    test_check(voff == FIXTURE_VOLUME_OFFSET, "fw: volume offset == 0");

    /* FFS file is at image byte 72 */
    size_t foff = (size_t)-1;
    test_check(axl_fw_node_offset(file, &foff), "fw: file offset returns true");
    test_check(foff == FIXTURE_FILE_OFFSET, "fw: file offset == 72");

    /* NULL node */
    size_t noff = (size_t)-1;
    test_check(!axl_fw_node_offset(NULL, &noff), "fw: offset(NULL) == false");

    axl_fw_close(img);
}

static void
test_fw_find(void)
{
    AxlFwImage *img = axl_fw_open(fixture_fv, fixture_fv_len);
    if (img == NULL) {
        test_check(false, "fw: open for find test");
        return;
    }

    /* Find by GUID, kind=FILE */
    AxlFwNode *found = axl_fw_find(img, &fixture_file_guid, AXL_FW_NODE_FILE);
    test_check(found != NULL, "fw: find FILE by GUID");
    test_check(found != NULL && axl_fw_node_kind(found) == AXL_FW_NODE_FILE,
               "fw: found node is FILE");

    /* Find with kind=IMAGE (any GUID-bearing node) */
    AxlFwNode *any = axl_fw_find(img, &fixture_file_guid, AXL_FW_NODE_IMAGE);
    test_check(any != NULL, "fw: find any-kind by GUID");
    test_check(any == found, "fw: find any-kind returns same node");

    /* NULL image */
    test_check(axl_fw_find(NULL, &fixture_file_guid, AXL_FW_NODE_FILE) == NULL,
               "fw: find(NULL, ...) == NULL");

    axl_fw_close(img);
}

static void
test_fw_null_tolerance(void)
{
    /* All accessors on NULL return the empty result per header @note */
    test_check(axl_fw_node_kind(NULL) == AXL_FW_NODE_IMAGE,
               "fw: kind(NULL) == AXL_FW_NODE_IMAGE");
    test_check(axl_fw_node_type(NULL) == 0,
               "fw: type(NULL) == 0");
    test_check(axl_fw_node_first_child(NULL) == NULL,
               "fw: first_child(NULL) == NULL");
    test_check(axl_fw_node_next_sibling(NULL) == NULL,
               "fw: next_sibling(NULL) == NULL");

    AxlGuid g;
    axl_memset(&g, 0, sizeof(g));
    test_check(!axl_fw_node_guid(NULL, &g), "fw: guid(NULL) == false");

    const void *p = NULL;
    size_t l = 0;
    test_check(!axl_fw_node_data(NULL, &p, &l), "fw: data(NULL) == false");

    size_t off = 0;
    test_check(!axl_fw_node_offset(NULL, &off), "fw: offset(NULL) == false");
}

static void
test_fw_compression_none(void)
{
    AxlFwImage *img = axl_fw_open(fixture_encap, fixture_encap_len);
    test_check(img != NULL, "fw-encap: open COMPRESSION-none fixture");
    if (img == NULL)
        return;

    AxlFwNode *root = axl_fw_root(img);
    AxlFwNode *vol  = axl_fw_node_first_child(root);
    test_check(vol != NULL && axl_fw_node_kind(vol) == AXL_FW_NODE_VOLUME,
               "fw-encap: outer volume present");
    if (vol == NULL) {
        axl_fw_close(img);
        return;
    }

    AxlFwNode *file = axl_fw_node_first_child(vol);
    test_check(file != NULL && axl_fw_node_kind(file) == AXL_FW_NODE_FILE,
               "fw-encap: outer file present");
    if (file == NULL) {
        axl_fw_close(img);
        return;
    }

    /* First section is the COMPRESSION (0x01) section */
    AxlFwNode *comp_sec = axl_fw_node_first_child(file);
    test_check(comp_sec != NULL
               && axl_fw_node_kind(comp_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(comp_sec) == 0x01,
               "fw-encap: first section is COMPRESSION (0x01)");
    if (comp_sec == NULL) {
        axl_fw_close(img);
        return;
    }

    /* COMPRESSION section must have a PE32 child (recursed) */
    AxlFwNode *pe32 = axl_fw_node_first_child(comp_sec);
    test_check(pe32 != NULL
               && axl_fw_node_kind(pe32) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(pe32) == 0x10,
               "fw-encap: COMPRESSION child is PE32 (0x10)");
    test_check(pe32 != NULL && axl_fw_node_next_sibling(pe32) == NULL,
               "fw-encap: COMPRESSION has exactly one child");

    /* PE32 child body must be exactly "CP\x00\x00\x00\x00\x00\x00" */
    const void *body = NULL;
    size_t blen = 0;
    test_check(pe32 != NULL
               && axl_fw_node_data(pe32, &body, &blen)
               && blen == 8
               && axl_memcmp(body, "CP\x00\x00\x00\x00\x00\x00", 8) == 0,
               "fw-encap: COMPRESSION child PE32 body exact (8 bytes)");

    axl_fw_close(img);
}

static void
test_fw_fv_image(void)
{
    AxlFwImage *img = axl_fw_open(fixture_encap, fixture_encap_len);
    test_check(img != NULL, "fw-fv-image: open FV_IMAGE fixture");
    if (img == NULL)
        return;

    AxlFwNode *root = axl_fw_root(img);
    AxlFwNode *vol  = axl_fw_node_first_child(root);
    AxlFwNode *file = (vol != NULL) ? axl_fw_node_first_child(vol) : NULL;
    if (file == NULL) {
        test_check(false, "fw-fv-image: outer file present");
        axl_fw_close(img);
        return;
    }

    /* Second section in the file is the FV_IMAGE (0x17) section */
    AxlFwNode *comp_sec = axl_fw_node_first_child(file);
    AxlFwNode *fvi_sec  = (comp_sec != NULL)
                          ? axl_fw_node_next_sibling(comp_sec)
                          : NULL;
    test_check(fvi_sec != NULL
               && axl_fw_node_kind(fvi_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(fvi_sec) == 0x17,
               "fw-fv-image: second section is FV_IMAGE (0x17)");
    if (fvi_sec == NULL) {
        axl_fw_close(img);
        return;
    }

    /* FV_IMAGE section must have a VOLUME child (recursed) */
    AxlFwNode *inner_vol = axl_fw_node_first_child(fvi_sec);
    test_check(inner_vol != NULL
               && axl_fw_node_kind(inner_vol) == AXL_FW_NODE_VOLUME,
               "fw-fv-image: FV_IMAGE child is a VOLUME");
    test_check(inner_vol != NULL && axl_fw_node_next_sibling(inner_vol) == NULL,
               "fw-fv-image: FV_IMAGE has exactly one child");

    /* VOLUME child must have a FILE child */
    AxlFwNode *inner_file = (inner_vol != NULL)
                            ? axl_fw_node_first_child(inner_vol)
                            : NULL;
    test_check(inner_file != NULL
               && axl_fw_node_kind(inner_file) == AXL_FW_NODE_FILE,
               "fw-fv-image: nested VOLUME has a FILE child");

    /* That FILE must have a PE32 section child */
    AxlFwNode *inner_sec = (inner_file != NULL)
                           ? axl_fw_node_first_child(inner_file)
                           : NULL;
    test_check(inner_sec != NULL
               && axl_fw_node_kind(inner_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(inner_sec) == 0x10,
               "fw-fv-image: nested FILE has PE32 section (0x10)");

    /* PE32 body must be exactly "IN\x00\x00\x00\x00\x00\x00" */
    const void *body = NULL;
    size_t blen = 0;
    test_check(inner_sec != NULL
               && axl_fw_node_data(inner_sec, &body, &blen)
               && blen == 8
               && axl_memcmp(body, "IN\x00\x00\x00\x00\x00\x00", 8) == 0,
               "fw-fv-image: nested PE32 body exact (8 bytes)");

    axl_fw_close(img);
}

/* ---------------------------------------------------------------------------
 * Malformed-COMPRESSION fixture: a COMPRESSION section with declared
 * size == 8 (too small to hold the 4-byte UncompressedLength +
 * 1-byte CompressionType = 9-byte minimum).  The parser must treat it
 * as a leaf (no child added, no crash / OOB read).
 *
 * Byte layout (all values little-endian):
 *
 * ── FV HEADER (72 bytes) ────────────────────────────────────────────────
 * [0..71]    EFI_FIRMWARE_VOLUME_HEADER (same as fixture_fv)
 *   [32..39]   FvLength = 104 = 0x68 (LE64)
 *   [56..63]   BlockMap[0]: NumBlocks=1, Length=104=0x68
 *
 * ── FFS FILE (32 bytes = 24-byte header + 8-byte section) ───────────────
 * [72..87]   Name GUID (all 0xAB)
 * [88..89]   IntegrityCheck = 0
 * [90]       Type = 0x07 (DRIVER)
 * [91]       Attributes = 0
 * [92..94]   Size24 = 32 = 0x20 (LE24)
 * [95]       State = 0xF8
 *
 * ── SECTION: COMPRESSION with size == 8 (too small) ────────────────────
 * [96..98]   Size24 = 8 = 0x08 (LE24)  ← only 4 bytes of body; < 9 minimum
 * [99]       Type = 0x01 (COMPRESSION)
 * [100..103] 4 bytes filler (would be partial UncompressedLength)
 *
 * Total image length: 104 bytes
 * ---------------------------------------------------------------------------
 */
static const unsigned char fixture_small_comp[] = {
    /* [0..15] ZeroVector */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [16..31] FileSystemGuid (zeros) */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [32..39] FvLength = 104 = 0x68 (LE64) */
    0x68, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    /* [40..43] Signature "_FVH" */
    0x5F, 0x46, 0x56, 0x48,
    /* [44..47] Attributes = 0 */
    0x00, 0x00, 0x00, 0x00,
    /* [48..49] HeaderLength = 72 = 0x48 (LE16) */
    0x48, 0x00,
    /* [50..51] Checksum = 0 */
    0x00, 0x00,
    /* [52..53] ExtHeaderOffset = 0 (LE16) */
    0x00, 0x00,
    /* [54] Reserved */
    0x00,
    /* [55] Revision = 2 */
    0x02,
    /* [56..63] BlockMap[0]: NumBlocks=1, Length=104=0x68 */
    0x01, 0x00, 0x00, 0x00,  0x68, 0x00, 0x00, 0x00,
    /* [64..71] BlockMap[1] terminator */
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,

    /* [72..87] Name GUID: all 0xAB */
    0xAB, 0xAB, 0xAB, 0xAB,  0xAB, 0xAB, 0xAB, 0xAB,
    0xAB, 0xAB, 0xAB, 0xAB,  0xAB, 0xAB, 0xAB, 0xAB,
    /* [88..89] IntegrityCheck = 0 */
    0x00, 0x00,
    /* [90] Type = 0x07 (DRIVER) */
    0x07,
    /* [91] Attributes = 0 */
    0x00,
    /* [92..94] Size24 = 32 = 0x20 (LE24) */
    0x20, 0x00, 0x00,
    /* [95] State = 0xF8 */
    0xF8,

    /* [96..98] Section Size24 = 8 = 0x08 (LE24) — too small for COMPRESSION */
    0x08, 0x00, 0x00,
    /* [99] Type = 0x01 (COMPRESSION) */
    0x01,
    /* [100..103] 4 filler bytes (partial UncompressedLength field) */
    0x0C, 0x00, 0x00, 0x00
};

static const unsigned int fixture_small_comp_len = sizeof(fixture_small_comp); /* 104 */

static void
test_fw_compression_too_small(void)
{
    /* A COMPRESSION section with declared size == 8 (< 9-byte minimum to
     * hold UncompressedLength + CompressionType) must be treated as a leaf:
     * it must have NO child and must not crash or read out-of-bounds. */
    AxlFwImage *img = axl_fw_open(fixture_small_comp, fixture_small_comp_len);
    test_check(img != NULL, "fw-comp-small: open malformed COMPRESSION fixture");
    if (img == NULL)
        return;

    AxlFwNode *vol  = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file = (vol != NULL) ? axl_fw_node_first_child(vol) : NULL;
    AxlFwNode *sec  = (file != NULL) ? axl_fw_node_first_child(file) : NULL;

    test_check(sec != NULL
               && axl_fw_node_kind(sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(sec) == 0x01,
               "fw-comp-small: COMPRESSION section parsed as leaf");
    test_check(sec != NULL && axl_fw_node_first_child(sec) == NULL,
               "fw-comp-small: malformed COMPRESSION has no child");

    axl_fw_close(img);
}

/* ---------------------------------------------------------------------------
 * LZMA fixture — built at runtime.
 *
 * In real EDK2 firmware a GUIDED-LZMA section decompresses to a section
 * STREAM (not a bare FV).  That stream contains one or more FV_IMAGE
 * sections (0x17), each of which is a self-contained FV.  We mirror that
 * layout here so fw_parse_sections correctly recurses.
 *
 * What gets compressed (the "inner section stream", 116 bytes):
 *   [0..2]   Size24 = 116 = 0x74 (LE24)
 *   [3]      Type = 0x17 (FV_IMAGE)
 *   [4..115] inner FV (112 bytes):
 *              FV header (72 bytes, FvLength=112)
 *              FFS DRIVER file (40 bytes, size=0x28):
 *                Name GUID bytes_le: CC DD EE FF 11 22 33 44 55 66 77 88 99 AA BB 00
 *                PE32 section (16 bytes): body "LZ\x00\x00testlzma"
 *
 * Outer FV layout (72 + 24 + gd_sec_size bytes):
 *   [0..71]   EFI_FIRMWARE_VOLUME_HEADER
 *   [72..95]  FFS DRIVER file header (24 bytes):
 *               Name GUID: DD EE FF 00 22 33 44 55 66 77 88 99 AA BB CC DD
 *               Size24 = 24 + gd_sec_size
 *   [96..]    GUID_DEFINED section (gd_sec_size = 24 + lzlen bytes):
 *               [0..2]   Size24 = gd_sec_size
 *               [3]      Type = 0x02 (GUID_DEFINED)
 *               [4..19]  LZMA codec GUID bytes_le (EE4E5898-3914-4259-…)
 *               [20..21] DataOffset = 24 (LE16)
 *               [22..23] Attributes = 0x0001 PROCESSING_REQUIRED (LE16)
 *               [24..]   LZMA payload
 *
 * Parse tree:
 *   IMAGE → VOLUME → FILE(outer) → SECTION(0x02,GUID_DEFINED)
 *     → SECTION(0x17,FV_IMAGE) → VOLUME(inner) → FILE(inner)
 *
 * Inner file GUID (bytes_le: CC DD EE FF 11 22 33 44 55 66 77 88 99 AA BB 00):
 *   AxlGuid: Data1=0xFFEEDDCC, Data2=0x2211, Data3=0x4433,
 *            Data4={0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0x00}
 * ---------------------------------------------------------------------------
 */

/* Inner file GUID (bytes_le: CC DD EE FF 11 22 33 44 55 66 77 88 99 AA BB 00) */
static const AxlGuid lzma_inner_file_guid = {
    0xFFEEDDCCu, 0x2211u, 0x4433u,
    { 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xAAu, 0xBBu, 0x00u }
};

/* Absent GUID used to verify NULL return from axl_fw_find */
static const AxlGuid lzma_absent_guid = {
    0x12345678u, 0xABCDu, 0xEF01u,
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u }
};

/* Build the 116-byte inner section stream to be LZMA-compressed.
 * Layout: one FV_IMAGE section (0x17, size=116) wrapping a 112-byte FV
 * which contains the inner FFS DRIVER file with lzma_inner_file_guid.
 * @p buf must be caller-supplied and zeroed (116 bytes). */
static void
build_lzma_inner_sec_stream(uint8_t *buf)
{
    /* FV_IMAGE section header (4 bytes) */
    buf[0] = 0x74; /* Size24 low byte = 116 */
    /* buf[1], buf[2] = 0x00 (high bytes of 116) — already zeroed */
    buf[3] = 0x17; /* Type = FV_IMAGE */

    /* Inner FV body starting at buf+4 */
    uint8_t *fv = buf + 4;
    /* FV header (72 bytes): FvLength=112 */
    fv[32] = 0x70; /* FvLength LE64 low byte */
    fv[40] = 0x5F; fv[41] = 0x46; fv[42] = 0x56; fv[43] = 0x48; /* "_FVH" */
    fv[48] = 0x48; /* HeaderLength = 72 */
    fv[55] = 0x02; /* Revision = 2 */
    fv[56] = 0x01; /* BlockMap[0]: NumBlocks=1 */
    fv[60] = 0x70; /* BlockMap[0]: Length=112 */

    /* FFS DRIVER file at inner-FV offset 72 (fv+72) */
    uint8_t *ff = fv + 72;
    /* Name GUID bytes_le: CC DD EE FF  11 22  33 44  55 66  77 88 99 AA BB 00 */
    ff[0]  = 0xCC; ff[1]  = 0xDD; ff[2]  = 0xEE; ff[3]  = 0xFF;
    ff[4]  = 0x11; ff[5]  = 0x22; ff[6]  = 0x33; ff[7]  = 0x44;
    ff[8]  = 0x55; ff[9]  = 0x66; ff[10] = 0x77; ff[11] = 0x88;
    ff[12] = 0x99; ff[13] = 0xAA; ff[14] = 0xBB; ff[15] = 0x00;
    ff[18] = 0x07; /* Type = DRIVER */
    ff[20] = 0x28; /* Size24 = 40 = 0x28 */
    ff[23] = 0xF8; /* State */

    /* PE32 section at file-body offset 0 (ff+24) */
    uint8_t *pe = ff + 24;
    pe[0] = 0x10; /* Size24 = 16 */
    pe[3] = 0x10; /* Type = PE32 */
    /* Body: "LZ\x00\x00testlzma" (12 bytes) at pe+4 */
    pe[4] = 0x4C; pe[5] = 0x5A; /* "LZ" */
    axl_memcpy(pe + 8, "testlzma", 8);
}

static void
test_fw_guided_lzma(void)
{
    /* Build the 116-byte inner section stream (FV_IMAGE section wrapping inner FV) */
    uint8_t inner_sec_stream[116];
    axl_memset(inner_sec_stream, 0, sizeof(inner_sec_stream));
    build_lzma_inner_sec_stream(inner_sec_stream);

    /* Compress the section stream with LZMA */
    void  *lz    = NULL;
    size_t lzlen = 0;
    int rc = axl_compress(AXL_COMPRESS_LZMA, inner_sec_stream,
                          sizeof(inner_sec_stream),
                          AXL_COMPRESS_LEVEL_DEFAULT, &lz, &lzlen);
    if (rc != AXL_OK || !lz) {
        test_check(false, "fw-lzma: axl_compress succeeded");
        return;
    }
    test_check(lzlen > 0, "fw-lzma: compressed payload is non-empty");

    /* GUID_DEFINED section total size = 24 (fixed header fields) + lzlen payload */
    size_t gd_sec_size = 24u + lzlen;
    size_t file_size   = 24u + gd_sec_size; /* FFS header + section body */
    size_t fv_size     = 72u + file_size;   /* FV header + file */

    uint8_t *outer = axl_calloc(1, fv_size);
    if (!outer) {
        test_check(false, "fw-lzma: alloc outer FV buffer");
        axl_free(lz);
        return;
    }

    /* Outer FV header */
    outer[32] = (uint8_t)(fv_size & 0xFF);
    outer[33] = (uint8_t)((fv_size >> 8) & 0xFF);
    outer[40] = 0x5F; outer[41] = 0x46; outer[42] = 0x56; outer[43] = 0x48; /* "_FVH" */
    outer[48] = 0x48; /* HeaderLength = 72 */
    outer[55] = 0x02; /* Revision = 2 */
    outer[56] = 0x01; /* BlockMap[0]: NumBlocks=1 */
    outer[60] = (uint8_t)(fv_size & 0xFF);
    outer[61] = (uint8_t)((fv_size >> 8) & 0xFF);

    /* FFS DRIVER file header at outer-FV offset 72 */
    /* Name GUID bytes_le: DD EE FF 00  22 33  44 55  66 77  88 99 AA BB CC DD */
    outer[72]  = 0xDD; outer[73]  = 0xEE; outer[74]  = 0xFF; outer[75]  = 0x00;
    outer[76]  = 0x22; outer[77]  = 0x33; outer[78]  = 0x44; outer[79]  = 0x55;
    outer[80]  = 0x66; outer[81]  = 0x77; outer[82]  = 0x88; outer[83]  = 0x99;
    outer[84]  = 0xAA; outer[85]  = 0xBB; outer[86]  = 0xCC; outer[87]  = 0xDD;
    outer[90] = 0x07; /* Type = DRIVER */
    outer[92] = (uint8_t)(file_size & 0xFF);
    outer[93] = (uint8_t)((file_size >> 8) & 0xFF);
    outer[95] = 0xF8; /* State */

    /* GUID_DEFINED section at file-body offset 0 (outer image offset 96) */
    uint8_t *gd = outer + 96;
    /* Size24 = gd_sec_size */
    gd[0] = (uint8_t)(gd_sec_size & 0xFF);
    gd[1] = (uint8_t)((gd_sec_size >> 8) & 0xFF);
    gd[2] = (uint8_t)((gd_sec_size >> 16) & 0xFF);
    /* Type = 0x02 (GUID_DEFINED) */
    gd[3] = 0x02;
    /* LZMA codec GUID bytes_le: EE4E5898-3914-4259-9D6E-DC7BD79403CF
     *   Data1=0xEE4E5898 → LE bytes: 98 58 4E EE
     *   Data2=0x3914     → LE bytes: 14 39
     *   Data3=0x4259     → LE bytes: 59 42
     *   Data4[0..7]:       9D 6E DC 7B D7 94 03 CF */
    gd[4]  = 0x98; gd[5]  = 0x58; gd[6]  = 0x4E; gd[7]  = 0xEE;
    gd[8]  = 0x14; gd[9]  = 0x39;
    gd[10] = 0x59; gd[11] = 0x42;
    gd[12] = 0x9D; gd[13] = 0x6E; gd[14] = 0xDC; gd[15] = 0x7B;
    gd[16] = 0xD7; gd[17] = 0x94; gd[18] = 0x03; gd[19] = 0xCF;
    /* DataOffset = 24 (LE16): payload starts at byte 24 of the section */
    gd[20] = 24;
    gd[21] = 0;
    /* Attributes = 0x0001 PROCESSING_REQUIRED (LE16) */
    gd[22] = 0x01;
    gd[23] = 0x00;
    /* LZMA-compressed payload */
    axl_memcpy(gd + 24, lz, lzlen);

    /* lz is no longer needed — the parser makes its own decompressed copy */
    axl_free(lz);

    /* Parse the outer image */
    AxlFwImage *img = axl_fw_open(outer, fv_size);
    test_check(img != NULL, "fw-lzma: axl_fw_open outer FV");
    if (!img) {
        axl_free(outer);
        return;
    }

    /* Walk: IMAGE → VOLUME → FILE(outer) → SECTION(0x02,GUID_DEFINED)
     *         → SECTION(0x17,FV_IMAGE) → VOLUME(inner) → FILE(inner) */
    AxlFwNode *root   = axl_fw_root(img);
    AxlFwNode *vol    = axl_fw_node_first_child(root);
    test_check(vol != NULL && axl_fw_node_kind(vol) == AXL_FW_NODE_VOLUME,
               "fw-lzma: outer volume present");
    AxlFwNode *file   = vol  ? axl_fw_node_first_child(vol)  : NULL;
    test_check(file != NULL && axl_fw_node_kind(file) == AXL_FW_NODE_FILE,
               "fw-lzma: outer file present");
    AxlFwNode *gd_sec = file ? axl_fw_node_first_child(file) : NULL;
    test_check(gd_sec != NULL
               && axl_fw_node_kind(gd_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(gd_sec) == 0x02,
               "fw-lzma: GUID_DEFINED section (0x02) present");

    /* After LZMA decode, the GD section's first child is the FV_IMAGE section */
    AxlFwNode *fvi_sec = gd_sec ? axl_fw_node_first_child(gd_sec) : NULL;
    test_check(fvi_sec != NULL
               && axl_fw_node_kind(fvi_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(fvi_sec) == 0x17,
               "fw-lzma: inner FV_IMAGE section present (LZMA decoded)");

    AxlFwNode *inner_vol = fvi_sec ? axl_fw_node_first_child(fvi_sec) : NULL;
    test_check(inner_vol != NULL && axl_fw_node_kind(inner_vol) == AXL_FW_NODE_VOLUME,
               "fw-lzma: inner VOLUME present");

    AxlFwNode *inner_file = inner_vol ? axl_fw_node_first_child(inner_vol) : NULL;
    test_check(inner_file != NULL && axl_fw_node_kind(inner_file) == AXL_FW_NODE_FILE,
               "fw-lzma: inner FILE present");

    /* axl_fw_find must locate the inner file by its GUID across the LZMA boundary */
    AxlFwNode *hit = axl_fw_find(img, &lzma_inner_file_guid, AXL_FW_NODE_FILE);
    test_check(hit != NULL, "fw-lzma: find inner file by GUID across LZMA boundary");
    test_check(hit == inner_file, "fw-lzma: find returns the correct inner FILE node");

    /* Find with kind=IMAGE (any) must also match */
    AxlFwNode *any = axl_fw_find(img, &lzma_inner_file_guid, AXL_FW_NODE_IMAGE);
    test_check(any != NULL && any == hit,
               "fw-lzma: find any-kind matches same node");

    /* Absent GUID must return NULL */
    test_check(axl_fw_find(img, &lzma_absent_guid, AXL_FW_NODE_FILE) == NULL,
               "fw-lzma: find absent GUID returns NULL");

    axl_fw_close(img);
    axl_free(outer);
}

/* ---------------------------------------------------------------------------
 * Fix 1 negative-path tests: GUIDED decode-failure fallback to raw stream.
 *
 * These three tests pin the restructured GUID_DEFINED branch from Fix 1:
 *   (a) LZMA codec + Attributes=0 + corrupt payload → raw-section fallback
 *   (b) Unknown codec + Attributes=0           → raw-section fallback
 *   (c) Unknown codec + Attributes=0x01         → opaque leaf (no children)
 *
 * Shared raw payload embedded into each GUID_DEFINED section:
 *   A single PE32 section stream (16 bytes):
 *     [0..2]  Size24 = 16 = 0x10 (LE24)
 *     [3]     Type   = 0x10 (PE32)
 *     [4..15] Body: "MZ\x90\x00rawfb\x00\x00" (12 bytes)
 *   The payload is pure garbage as an LZMA stream (axl_decompress rejects it)
 *   but is a valid section stream when interpreted raw.
 *
 * GUID_DEFINED section layout (total = 4 + 20 = 24-byte fixed hdr + 16 payload
 *   = 40 bytes):
 *   [0..2]  Size24  = 40 (LE24)
 *   [3]     Type    = 0x02 (GUID_DEFINED)
 *   [4..19] Codec GUID (bytes_le, varies per test)
 *   [20..21] DataOffset = 24 (LE16)  — payload begins at byte 24 of section
 *   [22..23] Attributes (LE16, varies per test)
 *   [24..39] raw PE32 section stream (16 bytes)
 *
 * Outer FV wraps a single FFS DRIVER file containing that one section.
 * FV size = 72 (FV hdr) + 64 (FFS hdr 24 + section 40) = 136 bytes.
 * ---------------------------------------------------------------------------
 */

/*
 * LZMA codec GUID bytes_le: EE4E5898-3914-4259-9D6E-DC7BD79403CF
 *   Data1=0xEE4E5898 → LE bytes: 98 58 4E EE
 *   Data2=0x3914     → LE bytes: 14 39
 *   Data3=0x4259     → LE bytes: 59 42
 *   Data4[0..7]:       9D 6E DC 7B D7 94 03 CF
 */
#define GD_FV_SIZE   136u   /* 72 + 24 + 40 */
#define GD_FILE_SIZE  64u   /* 24 (FFS hdr) + 40 (GD section) */
#define GD_SEC_SIZE   40u   /* 24 (GD fixed hdr) + 16 (raw PE32 payload) */

/* Build an outer FV (GD_FV_SIZE bytes) holding one GUID_DEFINED section.
 * @p codec_guid_le : 16 bytes of the codec GUID in bytes_le layout
 * @p attrs         : Attributes LE16 value (0x00 or 0x01)
 * The GUID_DEFINED payload (bytes [24..39] of the section) is the 16-byte
 * raw PE32 section stream described above.  Against the LZMA codec this
 * will fail axl_decompress (garbage LZMA). */
static void
build_gd_fixture(uint8_t buf[GD_FV_SIZE],
                 const uint8_t codec_guid_le[16],
                 uint16_t attrs)
{
    axl_memset(buf, 0, GD_FV_SIZE);

    /* FV header */
    buf[32] = (uint8_t)(GD_FV_SIZE & 0xFFu); /* FvLength LE64 low byte */
    buf[40] = 0x5F; buf[41] = 0x46; buf[42] = 0x56; buf[43] = 0x48; /* "_FVH" */
    buf[48] = 0x48; /* HeaderLength = 72 */
    buf[55] = 0x02; /* Revision = 2 */
    buf[56] = 0x01; /* BlockMap[0]: NumBlocks=1 */
    buf[60] = (uint8_t)(GD_FV_SIZE & 0xFFu); /* BlockMap[0]: Length */

    /* FFS file header at offset 72 */
    /* Name GUID: arbitrary non-erased non-PAD pattern */
    for (size_t i = 0; i < 16u; i++)
        buf[72u + i] = (uint8_t)(0x11u + i);
    buf[90] = 0x07; /* Type = DRIVER */
    buf[92] = (uint8_t)(GD_FILE_SIZE & 0xFFu); /* Size24 low */
    buf[95] = 0xF8; /* State */

    /* GUID_DEFINED section at file-body offset 0 (image offset 96) */
    uint8_t *gd = buf + 96u;
    gd[0] = (uint8_t)(GD_SEC_SIZE & 0xFFu); /* Size24 = 40 */
    gd[3] = 0x02;                            /* Type = GUID_DEFINED */
    axl_memcpy(gd + 4u, codec_guid_le, 16u); /* Codec GUID */
    gd[20] = 24u;                            /* DataOffset = 24 */
    gd[21] = 0x00;
    gd[22] = (uint8_t)(attrs & 0xFFu);       /* Attributes low */
    gd[23] = (uint8_t)((attrs >> 8) & 0xFFu);

    /* Raw PE32 section stream as payload (bytes [24..39] of section,
     * i.e. buf[120..135]):
     *   Size24=16 (0x10 0x00 0x00), Type=0x10 (PE32),
     *   Body: "MZ\x90\x00rawfb\x00\x00" */
    uint8_t *pl = gd + 24u;
    pl[0] = 0x10u; /* Size24 = 16 */
    pl[3] = 0x10u; /* Type   = PE32 */
    pl[4] = 0x4Du; pl[5] = 0x5Au; pl[6] = 0x90u; pl[7] = 0x00u; /* "MZ\x90\x00" */
    pl[8] = 0x72u; pl[9] = 0x61u; pl[10] = 0x77u; pl[11] = 0x66u; /* "rawf" */
    pl[12] = 0x62u; pl[13] = 0x00u; pl[14] = 0x00u; pl[15] = 0x00u; /* "b\x00\x00\x00" */
}

/* LZMA codec GUID bytes_le */
static const uint8_t gd_lzma_guid_le[16] = {
    0x98u, 0x58u, 0x4Eu, 0xEEu, /* Data1 = 0xEE4E5898 LE */
    0x14u, 0x39u,                /* Data2 = 0x3914 LE */
    0x59u, 0x42u,                /* Data3 = 0x4259 LE */
    0x9Du, 0x6Eu, 0xDCu, 0x7Bu, 0xD7u, 0x94u, 0x03u, 0xCFu /* Data4 */
};

/* Unknown codec GUID bytes_le (arbitrary value, not LZMA) */
static const uint8_t gd_unknown_guid_le[16] = {
    0xDEu, 0xADu, 0xBEu, 0xEFu,
    0x00u, 0x00u,
    0x00u, 0x00u,
    0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu, 0x00u, 0x11u
};

/* (a) LZMA codec + Attributes=0 + corrupt payload → raw-section fallback.
 *
 * This test is written FIRST, BEFORE Fix 1.  Against the original code the
 * LZMA-failed branch leaves an opaque leaf → no PE32 child → FAILS.
 * After Fix 1 the fallback recurses on the raw payload → PE32 child found → PASSES.
 */
static void
test_fw_guided_lzma_fail_raw_fallback(void)
{
    uint8_t buf[GD_FV_SIZE];
    /* Attributes=0: PROCESSING_REQUIRED clear → raw fallback must trigger */
    build_gd_fixture(buf, gd_lzma_guid_le, 0x0000u);

    AxlFwImage *img = axl_fw_open(buf, GD_FV_SIZE);
    test_check(img != NULL,
               "fw-gd-lzma-fail: open GD-LZMA fixture with corrupt payload");
    if (img == NULL)
        return;

    AxlFwNode *vol    = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file   = vol  ? axl_fw_node_first_child(vol)  : NULL;
    AxlFwNode *gd_sec = file ? axl_fw_node_first_child(file) : NULL;

    /* GUID_DEFINED node must exist with type 0x02 */
    test_check(gd_sec != NULL
               && axl_fw_node_kind(gd_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(gd_sec) == 0x02,
               "fw-gd-lzma-fail: GUID_DEFINED section (0x02) present");

    /* After raw fallback, gd_sec must have a PE32 child (type 0x10) */
    AxlFwNode *pe32 = gd_sec ? axl_fw_node_first_child(gd_sec) : NULL;
    test_check(pe32 != NULL
               && axl_fw_node_kind(pe32) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(pe32) == 0x10,
               "fw-gd-lzma-fail: raw fallback exposes PE32 child (fix 1 regression gate)");

    /* PE32 child body must start with "MZ" */
    const void *body = NULL;
    size_t blen = 0u;
    test_check(pe32 != NULL
               && axl_fw_node_data(pe32, &body, &blen)
               && blen == 12u
               && axl_memcmp(body, "MZ", 2u) == 0,
               "fw-gd-lzma-fail: PE32 body starts with MZ (exact 12 bytes)");

    axl_fw_close(img);
}

/* (b) Unknown codec + Attributes=0 → raw-section fallback (children parsed). */
static void
test_fw_guided_unknown_codec_raw(void)
{
    uint8_t buf[GD_FV_SIZE];
    build_gd_fixture(buf, gd_unknown_guid_le, 0x0000u);

    AxlFwImage *img = axl_fw_open(buf, GD_FV_SIZE);
    test_check(img != NULL,
               "fw-gd-unknown-raw: open unknown-codec fixture");
    if (img == NULL)
        return;

    AxlFwNode *vol    = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file   = vol  ? axl_fw_node_first_child(vol)  : NULL;
    AxlFwNode *gd_sec = file ? axl_fw_node_first_child(file) : NULL;

    test_check(gd_sec != NULL
               && axl_fw_node_kind(gd_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(gd_sec) == 0x02,
               "fw-gd-unknown-raw: GUID_DEFINED section present");

    /* Unknown codec, Attributes=0: must recurse on raw payload → PE32 child */
    AxlFwNode *pe32 = gd_sec ? axl_fw_node_first_child(gd_sec) : NULL;
    test_check(pe32 != NULL
               && axl_fw_node_kind(pe32) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(pe32) == 0x10,
               "fw-gd-unknown-raw: unknown codec raw fallback gives PE32 child");

    const void *body = NULL;
    size_t blen = 0u;
    test_check(pe32 != NULL
               && axl_fw_node_data(pe32, &body, &blen)
               && blen == 12u
               && axl_memcmp(body, "MZ", 2u) == 0,
               "fw-gd-unknown-raw: PE32 body exact (MZ prefix, 12 bytes)");

    axl_fw_close(img);
}

/* (c) Unknown codec + Attributes=0x01 (PROCESSING_REQUIRED) → opaque leaf. */
static void
test_fw_guided_unknown_codec_opaque(void)
{
    uint8_t buf[GD_FV_SIZE];
    /* Attributes=0x01: PROCESSING_REQUIRED set → must NOT recurse → no children */
    build_gd_fixture(buf, gd_unknown_guid_le, 0x0001u);

    AxlFwImage *img = axl_fw_open(buf, GD_FV_SIZE);
    test_check(img != NULL,
               "fw-gd-unknown-opaque: open unknown-codec+PR fixture");
    if (img == NULL)
        return;

    AxlFwNode *vol    = axl_fw_node_first_child(axl_fw_root(img));
    AxlFwNode *file   = vol  ? axl_fw_node_first_child(vol)  : NULL;
    AxlFwNode *gd_sec = file ? axl_fw_node_first_child(file) : NULL;

    /* Node must exist */
    test_check(gd_sec != NULL
               && axl_fw_node_kind(gd_sec) == AXL_FW_NODE_SECTION
               && axl_fw_node_type(gd_sec) == 0x02,
               "fw-gd-unknown-opaque: GUID_DEFINED section present");

    /* PROCESSING_REQUIRED=1 → opaque leaf, zero children */
    test_check(gd_sec != NULL
               && axl_fw_node_first_child(gd_sec) == NULL,
               "fw-gd-unknown-opaque: PROCESSING_REQUIRED gives opaque leaf (no children)");

    axl_fw_close(img);
}

/* ---------------------------------------------------------------------------
 * Regression: erased-free-space (all-0xFF) FFS tail must not infinite-loop.
 * Pre-fix this fixture hung axl_fw_open forever (size_t overflow in the FFS
 * bounds check); post-fix parsing terminates and exposes the one real file.
 * ---------------------------------------------------------------------------
 */
static void
test_fw_erased_tail_terminates(void)
{
    /* If the overflow guard regressed, axl_fw_open never returns and the
       whole binary stalls under the QEMU timeout — that IS the failure
       signal. On the fixed parser it returns promptly. */
    AxlFwImage *img = axl_fw_open(fixture_erased_tail_fv,
                                  fixture_erased_tail_fv_len);
    test_check(img != NULL, "fw-erased-tail: open FV with erased free space");

    /* Exactly one VOLUME, exactly one real FILE — the erased tail must NOT
       have produced a spurious file node. */
    AxlFwNode *vol = axl_fw_node_first_child(axl_fw_root(img));
    test_check(vol != NULL
               && axl_fw_node_kind(vol) == AXL_FW_NODE_VOLUME
               && axl_fw_node_next_sibling(vol) == NULL,
               "fw-erased-tail: exactly one VOLUME");

    AxlFwNode *file = vol ? axl_fw_node_first_child(vol) : NULL;
    test_check(file != NULL
               && axl_fw_node_kind(file) == AXL_FW_NODE_FILE
               && axl_fw_node_next_sibling(file) == NULL,
               "fw-erased-tail: exactly one FILE (erased tail ignored)");

    /* And the real file is still findable by GUID. */
    AxlFwNode *hit = axl_fw_find(img, &fixture_erased_file_guid,
                                 AXL_FW_NODE_FILE);
    test_check(hit != NULL && hit == file,
               "fw-erased-tail: real file found by GUID");

    axl_fw_close(img);
}

/* C++ RAII autoptr — AXL_AUTOPTR(AxlFwImage) must call axl_fw_close at scope
   exit. Live-allocation count returns to baseline once the parsed image (and
   its whole node tree) is torn down. */
static void
test_autoptr_fw(void)
{
    axl_fw_close(axl_fw_open(fixture_fv, fixture_fv_len));   /* prime */

    AxlMemStats before, after;
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(AxlFwImage) img = axl_fw_open(fixture_fv, fixture_fv_len);
        test_check(img != NULL, "autoptr: fw open");
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "autoptr: fw image closed at scope exit");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

static int
test_fw_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlFw");

    test_fw_open_close();
    test_fw_uncompressed_tree();
    test_fw_guid();
    test_fw_offset();
    test_fw_find();
    test_fw_null_tolerance();
    test_fw_compression_none();
    test_fw_fv_image();
    test_fw_compression_too_small();
    test_fw_guided_lzma();
    test_fw_guided_lzma_fail_raw_fallback();
    test_fw_guided_unknown_codec_raw();
    test_fw_guided_unknown_codec_opaque();
    test_fw_erased_tail_terminates();
    test_autoptr_fw();

    return test_print_results();
}

AXL_APP(test_fw_main)
