/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-edid.h
    VESA E-EDID base-block parser.

    Decodes the 128-byte EDID base block a display publishes (via
    `EFI_EDID_DISCOVERED_PROTOCOL` under UEFI, or any other transport
    that yields the raw bytes) into a flat AxlEdidInfo: monitor
    identity, EDID version, physical size, and the panel's native
    timing.

    This module is a **pure decoder** — it takes a caller-supplied byte
    buffer and never touches firmware, so it unit-tests against canned
    blobs with zero hardware. The display-side plumbing that obtains the
    bytes lives in AxlGfx (`axl_gfx_get_edid`); keeping the parse
    separate is what makes the decode logic testable.

    Scope: the EDID 1.x/2.x **base block** (the first 128 bytes).
    Extension blocks (CEA-861, DisplayID) are counted via
    `extension_count` but not decoded — they carry HDMI/audio/extra
    timing data outside what AXL's display layer needs today.
**/

#ifndef AXL_EDID_H
#define AXL_EDID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Size of the EDID base block. `axl_edid_parse` requires at least
/// this many bytes; extension blocks (if any) follow it.
#define AXL_EDID_BLOCK_SIZE  128u

/// Capacity (including NUL) of the monitor name / serial strings, which
/// EDID descriptor blocks carry as up to 13 ASCII bytes.
#define AXL_EDID_STRING_MAX  14u

/**
 * @brief Decoded fields from an EDID base block.
 *
 * Numeric fields are 0 when the EDID does not specify them (e.g. a
 * display with no Detailed Timing Descriptor leaves @ref native_width
 * / @ref native_height at 0). String fields are empty (`[0] == '\0'`)
 * when the corresponding descriptor block is absent.
 */
typedef struct {
    char     manufacturer[4];        ///< PNP vendor ID (3 letters + NUL), e.g. "DEL". Decoded faithfully from the packed 5-bit fields — a non-conformant block that still passes the checksum could yield a non-letter (a 0 field decodes to '@').
    uint16_t product_code;           ///< manufacturer product code (bytes 10-11)
    uint32_t serial_number;          ///< numeric serial (bytes 12-15); 0 if unset
    uint8_t  manufacture_week;       ///< week of manufacture (1-54), 0 if unspecified, 0xFF = model year
    uint16_t manufacture_year;       ///< full year (1990 + raw byte 17), 0 if unspecified
    uint8_t  version;                ///< EDID structure version (byte 18)
    uint8_t  revision;               ///< EDID structure revision (byte 19)
    bool     digital;                ///< true if a digital input (byte 20 bit 7), false if analog
    uint16_t native_width;           ///< native horizontal resolution from Detailed Timing #1, px (0 if none)
    uint16_t native_height;          ///< native vertical resolution from Detailed Timing #1, px (0 if none)
    uint32_t native_pixel_clock_khz; ///< Detailed Timing #1 pixel clock in kHz (0 if none)
    uint16_t image_width_mm;         ///< Detailed Timing #1 horizontal image size, mm (0 if unknown)
    uint16_t image_height_mm;        ///< Detailed Timing #1 vertical image size, mm (0 if unknown)
    uint8_t  extension_count;        ///< number of 128-byte extension blocks following (byte 126)
    char     monitor_name[AXL_EDID_STRING_MAX];    ///< Monitor Name descriptor (0xFC), trimmed; "" if absent
    char     monitor_serial[AXL_EDID_STRING_MAX];  ///< Monitor Serial descriptor (0xFF), trimmed; "" if absent
} AxlEdidInfo;

/**
 * @brief Parse an EDID base block into @p out.
 *
 * Validates the 8-byte EDID header signature
 * (`00 FF FF FF FF FF FF 00`) and the base-block checksum (all 128
 * bytes sum to 0 mod 256) before decoding — a buffer that fails either
 * check is rejected so callers get a clean "this is a real EDID"
 * signal rather than garbage decoded from random bytes.
 *
 * Only the base block is read; @p len may be larger (extension blocks
 * present) but must be at least @ref AXL_EDID_BLOCK_SIZE. The number of
 * extension blocks is reported via @ref AxlEdidInfo::extension_count
 * but their contents are not parsed.
 *
 * The native-timing fields (@ref AxlEdidInfo::native_width etc.) come
 * from Detailed Timing Descriptor #1 (the preferred timing per the EDID
 * spec). If the first descriptor slot holds a display descriptor
 * instead of a timing (pixel clock == 0), those fields are left 0.
 *
 * @return AXL_OK on a valid base block (@p out fully populated),
 *     AXL_ERR if @p edid or @p out is NULL, @p len <
 *     @ref AXL_EDID_BLOCK_SIZE, the header signature is wrong, or the
 *     checksum fails. @p out is left untouched on error.
 */
int
axl_edid_parse(
    const uint8_t  *edid,   ///< raw EDID bytes (base block + optional extensions)
    size_t          len,    ///< length of @p edid in bytes (>= AXL_EDID_BLOCK_SIZE)
    AxlEdidInfo    *out     ///< [out] decoded fields (untouched on error)
);

/**
 * @brief Compute display DPI from a parsed EDID.
 *
 * DPI is derived from the native pixel resolution and the physical
 * image size: `dpi = round(pixels * 25.4 / mm)`, computed
 * independently per axis. Uses the Detailed Timing #1 resolution and
 * image-size-in-mm fields, which describe the same rectangle, so the
 * ratio is meaningful.
 *
 * Either out parameter may be NULL if the caller wants only one axis.
 *
 * @return AXL_OK with the requested axes filled, or AXL_ERR if @p info
 *     is NULL or the EDID lacks a usable native resolution or image
 *     size (any of native_width / native_height / image_width_mm /
 *     image_height_mm is 0) — in which case the out parameters are
 *     untouched.
 */
int
axl_edid_dpi(
    const AxlEdidInfo  *info,    ///< parsed EDID
    uint32_t           *dpi_x,   ///< [out, optional] horizontal DPI
    uint32_t           *dpi_y    ///< [out, optional] vertical DPI
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_EDID_H */
