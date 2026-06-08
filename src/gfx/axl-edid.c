/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-edid.c
    VESA E-EDID base-block parser — see <axl/axl-edid.h>.

    Pure byte decoder: no firmware, no allocation. Field offsets follow
    VESA E-EDID Release A (the 128-byte base block); the four 18-byte
    descriptor slots at offsets 54/72/90/108 are each either a Detailed
    Timing Descriptor (non-zero pixel clock in bytes 0-1) or a display
    descriptor (bytes 0-1 == 0, type tag in byte 3).
**/

#include <axl/axl-edid.h>

/* A descriptor slot's 18 bytes start here. */
#define DTD0_OFFSET   54
#define DESC_STRIDE   18
#define NUM_DESCS     4

int
axl_edid_parse(
    const uint8_t  *edid,
    size_t          len,
    AxlEdidInfo    *out
    )
{
    if (edid == NULL || out == NULL || len < AXL_EDID_BLOCK_SIZE) {
        return AXL_ERR;
    }

    /* Header signature: 00 FF FF FF FF FF FF 00. */
    static const uint8_t magic[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    for (int i = 0; i < 8; i++) {
        if (edid[i] != magic[i]) { return AXL_ERR; }
    }

    /* Base-block checksum: all 128 bytes sum to 0 mod 256. */
    unsigned sum = 0;
    for (int i = 0; i < (int)AXL_EDID_BLOCK_SIZE; i++) { sum += edid[i]; }
    if ((sum & 0xFFu) != 0) { return AXL_ERR; }

    AxlEdidInfo info = {0};

    /* Manufacturer ID: bytes 8-9 big-endian, three 5-bit letters with
       'A' encoded as 1 (bits 14:10 / 9:5 / 4:0). */
    uint16_t m = (uint16_t)(((uint16_t)edid[8] << 8) | edid[9]);
    info.manufacturer[0] = (char)('A' + ((m >> 10) & 0x1F) - 1);
    info.manufacturer[1] = (char)('A' + ((m >> 5)  & 0x1F) - 1);
    info.manufacturer[2] = (char)('A' + (m         & 0x1F) - 1);
    info.manufacturer[3] = '\0';

    info.product_code  = (uint16_t)(edid[10] | ((uint16_t)edid[11] << 8));
    info.serial_number = (uint32_t)edid[12]
                       | ((uint32_t)edid[13] << 8)
                       | ((uint32_t)edid[14] << 16)
                       | ((uint32_t)edid[15] << 24);
    info.manufacture_week = edid[16];
    info.manufacture_year = (uint16_t)(1990 + edid[17]);
    info.version          = edid[18];
    info.revision         = edid[19];
    info.digital          = (edid[20] & 0x80) != 0;
    info.extension_count  = edid[126];

    /* Walk the four descriptor slots. Slot 0, when it is a timing
       descriptor, is the preferred (native) timing. */
    for (int slot = 0; slot < NUM_DESCS; slot++) {
        const uint8_t *d = edid + DTD0_OFFSET + slot * DESC_STRIDE;
        bool is_timing = (d[0] != 0 || d[1] != 0);

        if (is_timing) {
            if (slot == 0) {
                info.native_pixel_clock_khz =
                    (uint32_t)(d[0] | ((uint16_t)d[1] << 8)) * 10u;
                /* Active/size fields: low 8 bits + high nibble packed in
                   an upper byte (see VESA DTD layout). */
                info.native_width =
                    (uint16_t)(d[2] | (((uint16_t)(d[4] & 0xF0)) << 4));
                info.native_height =
                    (uint16_t)(d[5] | (((uint16_t)(d[7] & 0xF0)) << 4));
                info.image_width_mm =
                    (uint16_t)(d[12] | (((uint16_t)(d[14] & 0xF0)) << 4));
                info.image_height_mm =
                    (uint16_t)(d[13] | (((uint16_t)(d[14] & 0x0F)) << 8));
            }
        } else {
            /* Display descriptor: type tag at byte 3, ASCII text at
               bytes 5..17, 0x0A-terminated and space-padded. */
            uint8_t tag = d[3];
            if (tag == 0xFC || tag == 0xFF) {
                char *dst = (tag == 0xFC) ? info.monitor_name
                                          : info.monitor_serial;
                int j = 0;
                for (int i = 5; i < DESC_STRIDE
                                && j < (int)AXL_EDID_STRING_MAX - 1; i++) {
                    if (d[i] == 0x0A) { break; }
                    dst[j++] = (char)d[i];
                }
                while (j > 0 && dst[j - 1] == ' ') { j--; }
                dst[j] = '\0';
            }
        }
    }

    *out = info;
    return AXL_OK;
}

int
axl_edid_dpi(
    const AxlEdidInfo  *info,
    uint32_t           *dpi_x,
    uint32_t           *dpi_y
    )
{
    if (info == NULL
        || info->native_width == 0 || info->native_height == 0
        || info->image_width_mm == 0 || info->image_height_mm == 0) {
        return AXL_ERR;
    }

    /* dpi = round(pixels * 25.4 / mm), as integer math:
       (px * 254 + mm * 5) / (mm * 10). */
    if (dpi_x != NULL) {
        *dpi_x = ((uint32_t)info->native_width * 254u
                  + (uint32_t)info->image_width_mm * 5u)
                 / ((uint32_t)info->image_width_mm * 10u);
    }
    if (dpi_y != NULL) {
        *dpi_y = ((uint32_t)info->native_height * 254u
                  + (uint32_t)info->image_height_mm * 5u)
                 / ((uint32_t)info->image_height_mm * 10u);
    }
    return AXL_OK;
}
