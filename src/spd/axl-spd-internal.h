/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd-internal.h
    Private declarations shared across the AxlSpd codecs. Not exposed
    to consumers — they use <axl/axl-spd.h>.
**/

#ifndef AXL_SPD_INTERNAL_H
#define AXL_SPD_INTERNAL_H

#include <axl/axl-spd.h>
#include <axl/axl-smbus.h>

// JEDEC memory-type byte at SPD offset 2 (key byte / DRAM type).
#define AXL_SPD_TYPE_DDR3   0x0B
#define AXL_SPD_TYPE_DDR4   0x0C
#define AXL_SPD_TYPE_DDR5   0x12

// EE1004 (DDR4) Set Page Address pseudo-slaves. Writes here switch
// the page seen by reads from 0x50..0x57 (no other devices may sit
// at these addresses).
#define AXL_SPD_DDR4_SPA0   0x36   ///< select lower 256 bytes
#define AXL_SPD_DDR4_SPA1   0x37   ///< select upper 256 bytes

// SPD5118 (DDR5) MR11 register selects the read page for the upper
// 128-byte window of the 1024-byte address space.
#define AXL_SPD_DDR5_MR11   0x0B
#define AXL_SPD_DDR5_PAGE_SIZE  128
#define AXL_SPD_DDR5_NUM_PAGES  8       ///< 0..7 → covers 0..1023

// ---------------------------------------------------------------------------
// DDR4 codec (axl-spd-ddr4.c)
// ---------------------------------------------------------------------------

/**
 * Read up to @a cap raw SPD bytes from the DDR4 EEPROM at @a addr.
 * Tries SPA1 to access the upper 256 bytes when @a cap > 256.
 */
int
axl_spd_ddr4_read(
    AxlSmbus  *smbus,
    uint8_t    addr,
    uint8_t   *buf,
    size_t     cap,
    size_t    *len
    );

/**
 * Decode a captured DDR4 SPD buffer. @a out is fully populated;
 * fields the buffer can't supply remain at zero/empty.
 */
int
axl_spd_ddr4_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
    );

// ---------------------------------------------------------------------------
// DDR5 codec (axl-spd-ddr5.c)
// ---------------------------------------------------------------------------

/**
 * Read up to @a cap raw SPD bytes from the DDR5 SPD5118 hub at
 * @a addr. Internally page-selects via MR11.
 */
int
axl_spd_ddr5_read(
    AxlSmbus  *smbus,
    uint8_t    addr,
    uint8_t   *buf,
    size_t     cap,
    size_t    *len
    );

/**
 * Decode a captured DDR5 SPD buffer.
 */
int
axl_spd_ddr5_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
    );

// ---------------------------------------------------------------------------
// Shared decode helpers (axl-spd.c)
// ---------------------------------------------------------------------------

/**
 * Pack a JEP-106 (bank, id) pair from on-the-wire bytes into the
 * 16-bit code AxlSpdInfo exposes. Preserves the raw id byte (parity
 * bit included) so consumer lookup tables can key by the same byte
 * the SPD physically stores. Returns 0 if the id byte is 0x00
 * (unprogrammed) or 0xFF (erased) — neither is a valid JEP-106 position.
 */
uint16_t
axl_spd_pack_mfg_code(
    uint8_t  bank,
    uint8_t  id_byte
    );

/**
 * Trim trailing 0x20 / 0xFF padding from an in-place ASCII string
 * captured out of an SPD buffer; ensures NUL termination. Caller
 * provides @a dst with capacity @a dst_cap (must be > @a src_len).
 */
void
axl_spd_copy_part_number(
    char           *dst,
    size_t          dst_cap,
    const uint8_t  *src,
    size_t          src_len
    );

/**
 * Decode a 2-digit JEDEC BCD value (e.g., manufacture year/week).
 * Returns 0xFF if the input isn't valid BCD.
 */
uint8_t
axl_spd_bcd_to_u8(
    uint8_t  bcd
    );

#endif /* AXL_SPD_INTERNAL_H */
