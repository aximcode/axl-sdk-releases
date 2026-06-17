/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-scsi-decode.c
    Pure raw-buffer decoders for SCSI parameter data — INQUIRY (standard +
    VPD page 0x80 serial), READ CAPACITY (16), and LOG SENSE (Informational
    Exceptions page 0x2F + Temperature page 0x0D). No device, no firmware:
    each is a pure function over an on-the-wire (big-endian) byte buffer and
    is the hardware-free unit-test seam for AxlScsi. The device-facing
    readers in axl-scsi.c pass-thru the command and delegate here.
**/

#include <axl/axl-scsi.h>
#include <axl/axl-str.h>   /* axl_memset / axl_memcpy / axl_strlcpy */

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

/* Copy a fixed-width, space-padded SCSI text field (vendor/product/...) into
   @p out, trimming trailing spaces and NUL-terminating. @p out_size must be
   at least @p field_len + 1. */
static void
scsi_trim_field(
    char          *out,
    const uint8_t *src,
    size_t         field_len
    )
{
    size_t n = field_len;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\0')) {
        n--;
    }
    axl_memcpy(out, src, n);
    out[n] = '\0';
}

/* Big-endian readers (SCSI parameter data is big-endian). */
static uint32_t
be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static uint64_t
be64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | b[i];
    }
    return v;
}

/// Temperature byte value meaning "not available".
#define SCSI_TEMP_NA  0xFF

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

int
axl_scsi_decode_inquiry(
    const uint8_t  *data,
    size_t          len,
    AxlScsiInquiry *out
    )
{
    if (data == NULL || out == NULL || len < 36) {
        return AXL_ERR;
    }
    out->device_type = (uint8_t)(data[0] & 0x1F);
    out->removable   = (data[1] & 0x80) != 0;
    scsi_trim_field(out->vendor,   data + 8,  8);
    scsi_trim_field(out->product,  data + 16, 16);
    scsi_trim_field(out->revision, data + 32, 4);
    return AXL_OK;
}

int
axl_scsi_decode_serial(
    const uint8_t *vpd80,
    size_t         len,
    char          *out,
    size_t         out_size
    )
{
    if (vpd80 == NULL || out == NULL || out_size == 0 || len < 4) {
        return AXL_ERR;
    }
    out[0] = '\0';

    /* The page-length byte counts the serial that follows the 4-byte header;
       clamp to what the buffer actually holds. */
    size_t plen = vpd80[3];
    if (plen > len - 4) {
        plen = len - 4;
    }
    while (plen > 0 && (vpd80[4 + plen - 1] == ' ' || vpd80[4 + plen - 1] == '\0')) {
        plen--;
    }
    size_t cap = out_size - 1;
    size_t n   = (plen < cap) ? plen : cap;
    axl_memcpy(out, vpd80 + 4, n);
    out[n] = '\0';
    return AXL_OK;
}

int
axl_scsi_decode_capacity(
    const uint8_t   *data,
    size_t           len,
    AxlScsiCapacity *out
    )
{
    if (data == NULL || out == NULL || len < 16) {
        return AXL_ERR;
    }
    uint64_t last_lba = be64(data);          /* returned logical block address */
    out->size_blocks    = last_lba + 1;      /* count = last LBA + 1 */
    out->block_size     = be32(data + 8);    /* logical block length */
    out->capacity_bytes = out->size_blocks * (uint64_t)out->block_size;
    return AXL_OK;
}

int
axl_scsi_decode_health(
    const uint8_t *data,
    size_t         len,
    AxlScsiHealth *out
    )
{
    /* Need the 4-byte log header + the IE parameter's 4-byte header + the
       ASC/ASCQ (offsets 8, 9). */
    if (data == NULL || out == NULL || len < 10) {
        return AXL_ERR;
    }
    if ((data[0] & 0x3F) != 0x2F) {   /* not the Informational Exceptions page */
        return AXL_ERR;
    }

    out->asc           = data[8];
    out->ascq          = data[9];
    out->healthy       = (out->asc == 0);
    out->temperature_c = INT32_MIN;

    /* The IE parameter optionally carries a temperature byte after ASC/ASCQ
       (parameter length, at data[7], >= 3). */
    if (data[7] >= 3 && len >= 11 && data[10] != SCSI_TEMP_NA) {
        out->temperature_c = (int32_t)data[10];
    }
    return AXL_OK;
}

int
axl_scsi_decode_temperature(
    const uint8_t *data,
    size_t         len,
    int32_t       *out
    )
{
    /* 4-byte log header + 4-byte parameter header + reserved + temperature
       (offset 9). */
    if (data == NULL || out == NULL || len < 10) {
        return AXL_ERR;
    }
    if ((data[0] & 0x3F) != 0x0D) {   /* not the Temperature page */
        return AXL_ERR;
    }
    *out = (data[9] == SCSI_TEMP_NA) ? INT32_MIN : (int32_t)data[9];
    return AXL_OK;
}
