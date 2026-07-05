/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd-ddr4.c
    DDR4 SPD codec (JEDEC JESD21-C 4.1.2.13, EE1004 hub).

    DDR4 modules carry a 512-byte SPD: the lower 256 bytes are
    accessed directly at SMBus address 0x50..0x57; the upper 256
    bytes are reached after a "Set Page Address" (SPA) transaction
    addressed at the dedicated pseudo-slaves 0x36 (lower) and 0x37
    (upper). Standards-compliant EE1004 devices ignore the data
    portion of the SPA write — only the slave address matters.
    Some platforms (and QEMU's smbus-eeprom) don't model SPA at all
    — the upper-page read either returns the lower-page bytes again
    or fails outright. We attempt SPA, fall back to "lower page
    only" silently, and decode whatever the buffer contains.

    Decoded fields (best-effort; missing data leaves the field at 0):
      capacity_bytes, speed_mts, has_ecc, registered            (lower 256)
      mfg_code_module, mfg_code_dram, mfg_*, serial, part_number (upper 256)
**/

#include "axl-spd-internal.h"

#include <axl/axl-log.h>

AXL_LOG_DOMAIN("spd-ddr4");

// ---------------------------------------------------------------------------
// Lower-page byte offsets
// ---------------------------------------------------------------------------

#define DDR4_OFF_BYTES_TOTAL      0x00
#define DDR4_OFF_KEY_BYTE         0x02   /* must be 0x0C */
#define DDR4_OFF_MODULE_TYPE      0x03   /* 1=RDIMM, 2=UDIMM, 3=SODIMM, 4=LRDIMM, ... */
#define DDR4_OFF_DENSITY_BANKS    0x04   /* low nibble = density code */
#define DDR4_OFF_SDRAM_ADDRESSING 0x05
#define DDR4_OFF_MODULE_ORG       0x0C   /* bits 5..3 = ranks-1, bits 2..0 = device width */
#define DDR4_OFF_BUS_WIDTH        0x0D   /* bits 4..3 = ECC width, bits 2..0 = primary width */
#define DDR4_OFF_TCK_MIN_MTB      0x12   /* tCK_avg_min in MTB (0.125 ns) */
#define DDR4_OFF_TCK_MIN_FTB      0x7D   /* signed FTB correction in 1 ps units */

// ---------------------------------------------------------------------------
// Upper-page (offset 256+) byte offsets — Module Manufacturing Information
// ---------------------------------------------------------------------------

#define DDR4_OFF_MOD_MFG_BANK     0x140  /* 320 */
#define DDR4_OFF_MOD_MFG_ID       0x141  /* 321 */
#define DDR4_OFF_MFG_LOCATION     0x142  /* 322 */
#define DDR4_OFF_MFG_DATE_YEAR    0x143  /* 323 BCD */
#define DDR4_OFF_MFG_DATE_WEEK    0x144  /* 324 BCD */
#define DDR4_OFF_SERIAL           0x145  /* 325..328 (4 bytes BE) */
#define DDR4_OFF_PART_NUMBER      0x149  /* 329..348 (20 ASCII chars) */
#define DDR4_OFF_PART_NUMBER_LEN  20
#define DDR4_OFF_DRAM_MFG_BANK    0x15E  /* 350 */
#define DDR4_OFF_DRAM_MFG_ID      0x15F  /* 351 */

// ---------------------------------------------------------------------------
// Density / organisation tables
// ---------------------------------------------------------------------------

/* SDRAM density per device, in bytes (byte 4 low nibble).
   Code N -> 256 Mb << N, divided by 8 bits/byte. */
static uint64_t
ddr4_sdram_bytes(
    uint8_t  density_code
    )
{
    if (density_code > 7) {
        return 0;
    }
    /* 256 Mb / 8 = 32 MB; shift left by code. */
    return (uint64_t)32 * 1024 * 1024 * ((uint64_t)1 << density_code);
}

// ---------------------------------------------------------------------------
// Read path (with optional SPA for the upper page)
// ---------------------------------------------------------------------------

/**
 * Issue an EE1004 Set Page Address transaction. The data byte is
 * arbitrary — EE1004 looks only at the slave address. Errors are
 * non-fatal; many platforms (including QEMU's smbus-eeprom) reject
 * SPA, in which case we simply stay on the lower page.
 */
static int
ddr4_set_page(
    AxlSmbus  *smbus,
    bool       upper
    )
{
    uint8_t pseudo = upper ? AXL_SPD_DDR4_SPA1 : AXL_SPD_DDR4_SPA0;
    return axl_smbus_write_byte(smbus, pseudo, 0x00, 0x00);
}

int
axl_spd_ddr4_read(
    AxlSmbus  *smbus,
    uint8_t    addr,
    uint8_t   *buf,
    size_t     cap,
    size_t    *len
    )
{
    if (smbus == NULL || buf == NULL || len == NULL || cap == 0) {
        return AXL_ERR;
    }

    /* Lower 256 bytes — always accessible. */
    ddr4_set_page(smbus, false);   /* best-effort; ignore SPA failures */

    size_t lower_len = cap < 256 ? cap : 256;
    for (size_t i = 0; i < lower_len; i++) {
        if (axl_smbus_read_byte(smbus, addr, (uint8_t)i, &buf[i]) != AXL_OK) {
            if (i == 0) {
                return AXL_ERR;
            }
            *len = i;
            return AXL_OK;
        }
    }

    /* Upper 256 bytes — gated on caller wanting them and SPA succeeding. */
    if (cap > 256) {
        if (ddr4_set_page(smbus, true) != 0) {
            axl_debug("DDR4 SPA1 NACK at 0x%02X; manufacturing block unavailable",
                      addr);
            *len = lower_len;
            return AXL_OK;
        }
        size_t upper_cap = cap - 256;
        if (upper_cap > 256) {
            upper_cap = 256;
        }
        for (size_t i = 0; i < upper_cap; i++) {
            if (axl_smbus_read_byte(smbus, addr,
                                    (uint8_t)i, &buf[256 + i]) != AXL_OK) {
                /* Restore lower-page selection so subsequent reads from
                   neighbour SPDs see the right bytes. */
                ddr4_set_page(smbus, false);
                *len = 256 + i;
                return AXL_OK;
            }
        }
        ddr4_set_page(smbus, false);
        *len = 256 + upper_cap;
        return AXL_OK;
    }

    *len = lower_len;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Decode path
// ---------------------------------------------------------------------------

int
axl_spd_ddr4_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
    )
{
    if (buf == NULL || out == NULL || len < 64) {
        return AXL_ERR;
    }
    if (buf[DDR4_OFF_KEY_BYTE] != AXL_SPD_TYPE_DDR4) {
        return AXL_ERR;
    }

    out->ddr_generation = 4;

    /* Module type → registered flag (1=RDIMM, 4=LRDIMM both registered). */
    uint8_t module_type = buf[DDR4_OFF_MODULE_TYPE] & 0x0F;
    out->registered = (module_type == 1 || module_type == 4 ||
                       module_type == 5 || module_type == 8);

    /* Capacity:
        SDRAM device bytes      = 32 MB << density_code  (code 0..7)
        Devices per module      = (bus_width / device_width) * package_ranks
        Total                   = SDRAM bytes * devices                 */
    uint64_t sdram_bytes = ddr4_sdram_bytes(buf[DDR4_OFF_DENSITY_BANKS] & 0x0F);
    uint8_t  package_ranks = (uint8_t)(((buf[DDR4_OFF_MODULE_ORG] >> 3) & 0x07) + 1);
    uint8_t  device_width  = (uint8_t)(4u  << (buf[DDR4_OFF_MODULE_ORG] & 0x07));
    uint8_t  primary_width = (uint8_t)(8u  << (buf[DDR4_OFF_BUS_WIDTH] & 0x07));
    if (sdram_bytes != 0 && device_width != 0) {
        out->capacity_bytes = sdram_bytes
                              * (primary_width / device_width)
                              * package_ranks;
    }

    /* ECC: byte 13 bits 4..3 = bus-width extension (00=none, 01=8-bit ECC). */
    uint8_t bus_ext = (buf[DDR4_OFF_BUS_WIDTH] >> 3) & 0x03;
    out->has_ecc = (bus_ext == 0x01);

    /* Speed in MT/s from tCK_avg_min (MTB = 0.125 ns + signed FTB in ps).
       Simple truncation matches JEDEC speed-grade snapping behaviour
       (e.g., tCK = 833 ps decodes to 2400, not 2401). */
    int32_t tck_ps = (int32_t)buf[DDR4_OFF_TCK_MIN_MTB] * 125
                     + (int32_t)(int8_t)buf[DDR4_OFF_TCK_MIN_FTB];
    if (tck_ps > 0) {
        out->speed_mts = (uint16_t)(2000000 / tck_ps);
    }

    /* Manufacturing info lives at offset 320+. Bail out early if the
       buffer is too short (e.g., upper-page read failed in QEMU). */
    if (len < DDR4_OFF_DRAM_MFG_ID + 1) {
        return AXL_OK;
    }

    out->mfg_code_module = axl_spd_pack_mfg_code(
            buf[DDR4_OFF_MOD_MFG_BANK], buf[DDR4_OFF_MOD_MFG_ID]);
    out->mfg_location    = buf[DDR4_OFF_MFG_LOCATION];

    uint8_t year = axl_spd_bcd_to_u8(buf[DDR4_OFF_MFG_DATE_YEAR]);
    uint8_t week = axl_spd_bcd_to_u8(buf[DDR4_OFF_MFG_DATE_WEEK]);
    if (year != 0xFF) {
        out->mfg_year = (uint16_t)(2000 + year);
    }
    if (week != 0xFF && week >= 1 && week <= 53) {
        out->mfg_week = week;
    }

    /* Serial number — 4 bytes big-endian. */
    out->serial = ((uint32_t)buf[DDR4_OFF_SERIAL]     << 24) |
                  ((uint32_t)buf[DDR4_OFF_SERIAL + 1] << 16) |
                  ((uint32_t)buf[DDR4_OFF_SERIAL + 2] << 8)  |
                  ((uint32_t)buf[DDR4_OFF_SERIAL + 3]);

    axl_spd_copy_part_number(out->part_number, sizeof(out->part_number),
                             &buf[DDR4_OFF_PART_NUMBER],
                             DDR4_OFF_PART_NUMBER_LEN);

    out->mfg_code_dram = axl_spd_pack_mfg_code(
            buf[DDR4_OFF_DRAM_MFG_BANK], buf[DDR4_OFF_DRAM_MFG_ID]);

    return AXL_OK;
}
