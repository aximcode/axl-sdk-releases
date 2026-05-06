/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-spd-ddr5.c
    DDR5 SPD codec (JEDEC JESD400-5, SPD5118 hub).

    DDR5 modules carry a 1024-byte SPD behind the SPD5118 hub. The
    hub's I²C-visible address space is divided into eight 128-byte
    pages (0..7), each addressed at offsets 0x80..0xFF after the
    target page is selected by writing the page number to MR11
    (register 0x0B). Lower-half registers (0x00..0x7F) are
    hub-specific control regs, not the SPD payload.

    The decoded view is intentionally small for v1; manufacturer
    codes (module + DRAM) are exposed raw and the part number
    is captured up to 30 ASCII chars. PMIC mfg codes, RCD codes,
    and full timing matrix are deferred to a later iteration when
    a consumer asks for them.
**/

#include "axl-spd-internal.h"

#include <axl/axl-log.h>

AXL_LOG_DOMAIN("spd-ddr5");

// ---------------------------------------------------------------------------
// Byte offsets in the linearised 1024-byte address space
// ---------------------------------------------------------------------------

#define DDR5_OFF_KEY_BYTE_0       0x02   /* must be 0x12 */
#define DDR5_OFF_KEY_BYTE_1       0x03   /* module type */
#define DDR5_OFF_DENSITY          0x04   /* low 5 bits = density code */
#define DDR5_OFF_SDRAM_IO_WIDTH   0x06   /* low 3 bits: 0=x4, 1=x8, 2=x16 */
#define DDR5_OFF_TCK_MIN_LO       0x14   /* 16-bit tCK_avg_min in ps, LE */
#define DDR5_OFF_TCK_MIN_HI       0x15
#define DDR5_OFF_BUS_WIDTH        0xEB   /* module-level bus-width byte */

#define DDR5_OFF_MOD_MFG_BANK     0x200  /* 512 */
#define DDR5_OFF_MOD_MFG_ID       0x201  /* 513 */
#define DDR5_OFF_MFG_LOCATION     0x202  /* 514 */
#define DDR5_OFF_MFG_DATE_YEAR    0x203  /* 515 BCD */
#define DDR5_OFF_MFG_DATE_WEEK    0x204  /* 516 BCD */
#define DDR5_OFF_SERIAL           0x205  /* 517..520 */
#define DDR5_OFF_PART_NUMBER      0x209  /* 521..550 (30 chars) */
#define DDR5_OFF_PART_NUMBER_LEN  30
#define DDR5_OFF_DRAM_MFG_BANK    0x228  /* 552 */
#define DDR5_OFF_DRAM_MFG_ID      0x229  /* 553 */

// ---------------------------------------------------------------------------
// Density table (DDR5)
// ---------------------------------------------------------------------------

/* SDRAM density per device, in bytes. Byte 4 bits 4..0; codes per
   JESD400-5: 0=reserved, 1=4Gb, 2=8Gb, 3=12Gb, 4=16Gb, 5=24Gb,
   6=32Gb, 7=48Gb, 8=64Gb. Stored as bits-divided-by-8. */
static uint64_t
ddr5_sdram_bytes(
    uint8_t  density_code
    )
{
    switch (density_code & 0x1F) {
        case 1:  return  4ULL  * 1024 * 1024 * 1024 / 8;  /*  4 Gb */
        case 2:  return  8ULL  * 1024 * 1024 * 1024 / 8;  /*  8 Gb */
        case 3:  return 12ULL  * 1024 * 1024 * 1024 / 8;  /* 12 Gb */
        case 4:  return 16ULL  * 1024 * 1024 * 1024 / 8;  /* 16 Gb */
        case 5:  return 24ULL  * 1024 * 1024 * 1024 / 8;  /* 24 Gb */
        case 6:  return 32ULL  * 1024 * 1024 * 1024 / 8;  /* 32 Gb */
        case 7:  return 48ULL  * 1024 * 1024 * 1024 / 8;  /* 48 Gb */
        case 8:  return 64ULL  * 1024 * 1024 * 1024 / 8;  /* 64 Gb */
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Read path
// ---------------------------------------------------------------------------

/**
 * Set MR11 to (addr_mode_bit | page) — preserves the addr-mode bit
 * the BIOS configured (bit 3 = 1 for 2-byte, 0 for 1-byte legacy
 * mode). Linux's spd5118 driver does the same: only the low 3 bits
 * are touched per page change, the addr-mode bit is read once and
 * threaded through every subsequent write. Clobbering it would flip
 * the hub into a mode our SMBus byte ops can't drive.
 */
static int
ddr5_select_page(
    AxlSmbus  *smbus,
    uint8_t    addr,
    uint8_t    addr_mode_bit,
    uint8_t    page
    )
{
    uint8_t mr11 = (uint8_t)(addr_mode_bit | (page & AXL_SPD_DDR5_MR11_PAGE_MASK));
    return axl_smbus_write_byte(smbus, addr, AXL_SPD_DDR5_MR11, mr11);
}

int
axl_spd_ddr5_read(
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

    /* Read MR11 once to capture the addr-mode bit. We must preserve
     * it across every page-select write (per JEDEC SPD5118 spec —
     * Linux's spd5118.c does the same at line 663). */
    uint8_t mr11_orig = 0;
    if (axl_smbus_read_byte(smbus, addr, AXL_SPD_DDR5_MR11, &mr11_orig)
        != AXL_OK)
    {
        axl_debug("DDR5 MR11 read failed at 0x%02X", addr);
        return AXL_ERR;
    }
    uint8_t addr_mode = mr11_orig & AXL_SPD_DDR5_MR11_ADDR_BIT;

    size_t pages = (cap + AXL_SPD_DDR5_PAGE_SIZE - 1) / AXL_SPD_DDR5_PAGE_SIZE;
    if (pages > AXL_SPD_DDR5_NUM_PAGES) {
        pages = AXL_SPD_DDR5_NUM_PAGES;
    }

    size_t  total       = 0;
    uint8_t cached_page = (uint8_t)(mr11_orig & AXL_SPD_DDR5_MR11_PAGE_MASK);
    bool    page_known  = true;
    int     rc          = 0;

    for (size_t p = 0; p < pages; p++) {
        if (!page_known || (uint8_t)p != cached_page) {
            if (ddr5_select_page(smbus, addr, addr_mode, (uint8_t)p) != 0) {
                axl_debug("DDR5 page-select to %zu failed at 0x%02X",
                          p, addr);
                rc = (total == 0) ? AXL_ERR : AXL_OK;
                goto out;
            }
            cached_page = (uint8_t)p;
            page_known  = true;
        }
        /* Read this page's 128 bytes from offsets 0x80..0xFF. */
        size_t want = cap - total;
        if (want > AXL_SPD_DDR5_PAGE_SIZE) {
            want = AXL_SPD_DDR5_PAGE_SIZE;
        }
        for (size_t i = 0; i < want; i++) {
            uint8_t off = (uint8_t)(AXL_SPD_DDR5_EEPROM_BASE + i);
            if (axl_smbus_read_byte(smbus, addr, off, &buf[total + i])
                != AXL_OK)
            {
                total += i;
                rc = (total == 0) ? AXL_ERR : AXL_OK;
                goto out;
            }
        }
        total += want;
    }

out:
    /* Restore page 0 so the device is in a predictable state for the
     * next consumer. Preserve the addr-mode bit. Best-effort.
     * Skip if we never moved off page 0 in the first place — saves a
     * redundant SMBus op. */
    if (cached_page != 0 || !page_known) {
        (void)ddr5_select_page(smbus, addr, addr_mode, 0);
    }

    if (rc == 0) {
        *len = total;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Decode path
// ---------------------------------------------------------------------------

int
axl_spd_ddr5_decode(
    const uint8_t  *buf,
    size_t          len,
    AxlSpdInfo     *out
    )
{
    if (buf == NULL || out == NULL || len < 64) {
        return AXL_ERR;
    }
    if (buf[DDR5_OFF_KEY_BYTE_0] != AXL_SPD_TYPE_DDR5) {
        return AXL_ERR;
    }

    out->ddr_generation = 5;

    /* Module type (key byte 1, low nibble): 1=RDIMM, 2=UDIMM,
       3=SODIMM, 4=LRDIMM, 5=Mini-RDIMM, 6=Mini-UDIMM,
       8=72b-SO-RDIMM, 0xA=CUDIMM, 0xB=CSODIMM. */
    uint8_t module_type = buf[DDR5_OFF_KEY_BYTE_1] & 0x0F;
    out->registered = (module_type == 1 || module_type == 4 ||
                       module_type == 5 || module_type == 8 ||
                       module_type == 0xA);

    /* Capacity = SDRAM device bytes × devices per primary bus.
       Devices = primary_bus_width / SDRAM_IO_width. SDRAM I/O width
       at byte 6 low 3 bits: 0=x4, 1=x8, 2=x16. Bus width at byte
       0xEB low 3 bits: code N → 8 << N bits (8..1024). v1 reports
       single-rank; multi-rank/sub-channel decoding lives behind a
       later flag once a consumer needs it. */
    uint64_t sdram_bytes = ddr5_sdram_bytes(buf[DDR5_OFF_DENSITY]);
    if (sdram_bytes != 0
        && len > DDR5_OFF_BUS_WIDTH
        && len > DDR5_OFF_SDRAM_IO_WIDTH)
    {
        uint8_t  bus_width_code = buf[DDR5_OFF_BUS_WIDTH] & 0x07;
        uint64_t bus_width_bits = (uint64_t)8 << bus_width_code;
        uint8_t  io_width_code  = buf[DDR5_OFF_SDRAM_IO_WIDTH] & 0x07;
        uint64_t device_width   = (uint64_t)4 << io_width_code; /* 4/8/16 */
        if (device_width != 0 && bus_width_bits >= device_width) {
            out->capacity_bytes = sdram_bytes * (bus_width_bits / device_width);
        }
    }

    /* ECC: bus-width byte bits 4:3 = bus width extension; non-zero = ECC.
       Bits 7:5 carry the channel-count encoding and must NOT be folded
       into the ECC test (a dual-channel non-ECC module flips bit 5,
       which the previous & 0x07 mask misread as ECC presence). */
    if (len > DDR5_OFF_BUS_WIDTH) {
        out->has_ecc = ((buf[DDR5_OFF_BUS_WIDTH] >> 3) & 0x03) != 0;
    }

    /* Speed (MT/s) from the 16-bit LE tCK_avg_min in ps. */
    uint16_t tck_ps = (uint16_t)buf[DDR5_OFF_TCK_MIN_LO] |
                      ((uint16_t)buf[DDR5_OFF_TCK_MIN_HI] << 8);
    if (tck_ps > 0) {
        out->speed_mts = (uint16_t)(2000000 / tck_ps);
    }

    /* Manufacturing info lives at offset 512+. */
    if (len < DDR5_OFF_DRAM_MFG_ID + 1) {
        return AXL_OK;
    }

    out->mfg_code_module = axl_spd_pack_mfg_code(
            buf[DDR5_OFF_MOD_MFG_BANK], buf[DDR5_OFF_MOD_MFG_ID]);
    out->mfg_location    = buf[DDR5_OFF_MFG_LOCATION];

    uint8_t year = axl_spd_bcd_to_u8(buf[DDR5_OFF_MFG_DATE_YEAR]);
    uint8_t week = axl_spd_bcd_to_u8(buf[DDR5_OFF_MFG_DATE_WEEK]);
    if (year != 0xFF) {
        out->mfg_year = (uint16_t)(2000 + year);
    }
    if (week != 0xFF && week >= 1 && week <= 53) {
        out->mfg_week = week;
    }

    out->serial = ((uint32_t)buf[DDR5_OFF_SERIAL]     << 24) |
                  ((uint32_t)buf[DDR5_OFF_SERIAL + 1] << 16) |
                  ((uint32_t)buf[DDR5_OFF_SERIAL + 2] << 8)  |
                  ((uint32_t)buf[DDR5_OFF_SERIAL + 3]);

    axl_spd_copy_part_number(out->part_number, sizeof(out->part_number),
                             &buf[DDR5_OFF_PART_NUMBER],
                             DDR5_OFF_PART_NUMBER_LEN);

    out->mfg_code_dram = axl_spd_pack_mfg_code(
            buf[DDR5_OFF_DRAM_MFG_BANK], buf[DDR5_OFF_DRAM_MFG_ID]);

    return AXL_OK;
}
