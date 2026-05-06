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

// SPD5118 (DDR5) Mode Register layout — see JEDEC SPD5118 spec.
// The hub fronts ~128 bytes of MR space at 0x00..0x7F (vendor / device
// ID, capabilities, page-select, integrated TS sensor) and 8 pages of
// 128 bytes of EEPROM content at 0x80..0xFF after a page-select via
// MR11. We mirror Linux's `drivers/hwmon/spd5118.c` register names.
#define AXL_SPD_DDR5_MR0    0x00   ///< device-type LSB; expect 0x18
#define AXL_SPD_DDR5_MR1    0x01   ///< device-type MSB; expect 0x51
#define AXL_SPD_DDR5_MR3    0x03   ///< vendor bank
#define AXL_SPD_DDR5_MR4    0x04   ///< vendor id (JEP-106, odd parity)
#define AXL_SPD_DDR5_MR5    0x05   ///< capability — bit 1 = TS_SUPPORT
#define AXL_SPD_DDR5_MR11   0x0B   ///< I2C legacy mode: ADDR + page index

#define AXL_SPD_DDR5_DEVTYPE_LSB    0x18  ///< MR0 expected
#define AXL_SPD_DDR5_DEVTYPE_MSB    0x51  ///< MR1 expected (-> 0x5118)

// MR11 bit layout
#define AXL_SPD_DDR5_MR11_ADDR_BIT  0x08   ///< 0=1-byte (legacy), 1=2-byte
#define AXL_SPD_DDR5_MR11_PAGE_MASK 0x07   ///< low 3 bits = page index 0..7

// MR5 bit layout
#define AXL_SPD_DDR5_MR5_TS_SUPPORT 0x02

#define AXL_SPD_DDR5_PAGE_SIZE  128
#define AXL_SPD_DDR5_NUM_PAGES  8       ///< 0..7 → covers 0..1023
#define AXL_SPD_DDR5_EEPROM_BASE 0x80   ///< page content starts here

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
