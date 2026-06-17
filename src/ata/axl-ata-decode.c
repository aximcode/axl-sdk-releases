/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ata-decode.c
    Pure decoders for ATA IDENTIFY DEVICE, SMART READ DATA / THRESHOLDS,
    and the self-test execution status. No UEFI dependency — the
    device-facing readers in axl-ata.c fetch the buffers via the
    pass-thru protocol and hand them here. Kept dependency-free so the
    unit suite can exercise them against fixed buffers with no device.

    ATA numeric fields are little-endian 16-bit words; ASCII identity
    fields store the two characters of each word byte-swapped.
**/

#include <axl/axl-ata.h>

#define ATA_SMART_ATTR_COUNT   30
#define ATA_SMART_ATTR_BASE    2     /* attribute table starts at byte 2 */
#define ATA_SELF_TEST_STATUS   363   /* SMART READ DATA execution-status byte */

static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read @p n (<= 8) little-endian bytes as a u64. */
static uint64_t
rdn(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* Decode an ATA identity string: each 16-bit word holds two chars
   byte-swapped (char0 in the high byte). Recover order, NUL-terminate at
   @p cap, and trim trailing spaces. */
static void
ata_string(const uint8_t *src, size_t nbytes, char *dst, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t n = (nbytes < cap - 1) ? nbytes : cap - 1;
    for (size_t i = 0; i < n; i += 2) {
        dst[i] = (char)src[i + 1];
        if (i + 1 < n) {
            dst[i + 1] = (char)src[i];
        }
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = '\0';
}

int
axl_ata_decode_identify(const uint8_t *id, size_t len, AxlAtaIdentify *out)
{
    if (id == NULL || out == NULL || len < 512) {
        return AXL_ERR;
    }

    ata_string(id + 20, 20, out->serial, sizeof(out->serial));   /* words 10..19 */
    ata_string(id + 46, 8, out->firmware, sizeof(out->firmware)); /* words 23..26 */
    ata_string(id + 54, 40, out->model, sizeof(out->model));      /* words 27..46 */

    out->smart_supported = (rd16(id + 164) & 0x0001) != 0;        /* word 82 */

    /* 48-bit LBA count (words 100..103) when word 83 bit 10 is set, else
       the 28-bit count (words 60..61). */
    if (rd16(id + 166) & (1u << 10)) {
        out->size_blocks = rdn(id + 200, 8) & 0x0000FFFFFFFFFFFFull;
    } else {
        out->size_blocks = rd32(id + 120);
    }

    /* Logical sector size: word 106 bit 14 valid + bit 15 clear + bit 12
       set means words 117..118 hold the size in 16-bit words; else 512. */
    uint16_t w106 = rd16(id + 212);
    if ((w106 & (1u << 14)) && !(w106 & (1u << 15)) && (w106 & (1u << 12))) {
        out->block_size = rd32(id + 234) * 2u;
    } else {
        out->block_size = 512;
    }
    if (out->block_size == 0) {
        out->block_size = 512;
    }
    out->capacity_bytes = out->size_blocks * out->block_size;
    return AXL_OK;
}

/* Find the threshold for attribute @p id in a SMART READ THRESHOLDS
   buffer; returns 0 (never-failing) if absent. */
static uint8_t
threshold_for(const uint8_t *thr, uint8_t id)
{
    for (int i = 0; i < ATA_SMART_ATTR_COUNT; i++) {
        size_t off = ATA_SMART_ATTR_BASE + (size_t)i * 12;
        if (thr[off] == id) {
            return thr[off + 1];
        }
    }
    return 0;
}

int
axl_ata_decode_smart(const uint8_t *data, const uint8_t *thresholds,
                     size_t len, AxlAtaSmart *out)
{
    if (data == NULL || thresholds == NULL || out == NULL || len < 512) {
        return AXL_ERR;
    }

    out->healthy             = true;
    out->temperature_c       = INT32_MIN;
    out->power_on_hours      = UINT64_MAX;
    out->power_cycles        = UINT64_MAX;
    out->reallocated_sectors = UINT64_MAX;
    out->percent_used        = 0xFF;   /* no universal ATA endurance attr */

    for (int i = 0; i < ATA_SMART_ATTR_COUNT; i++) {
        size_t off = ATA_SMART_ATTR_BASE + (size_t)i * 12;
        uint8_t id = data[off];
        if (id == 0) {
            continue;
        }
        uint16_t flags = rd16(data + off + 1);
        uint8_t  value = data[off + 3];
        uint64_t raw   = rdn(data + off + 5, 6);

        /* Health: a pre-fail attribute at/below an in-range threshold is
           "failing now" (the smartctl verdict). */
        if (flags & 0x0001) {
            uint8_t t = threshold_for(thresholds, id);
            if (t >= 1 && t <= 253 && value <= t) {
                out->healthy = false;
            }
        }

        switch (id) {
        case 5:   out->reallocated_sectors = raw; break;
        case 9:   out->power_on_hours = raw; break;
        case 12:  out->power_cycles = raw; break;
        case 194: out->temperature_c = (int32_t)(raw & 0xFF); break;
        case 190: /* airflow temperature — only if 194 was absent */
            if (out->temperature_c == INT32_MIN) {
                out->temperature_c = (int32_t)(raw & 0xFF);
            }
            break;
        default: break;
        }
    }
    return AXL_OK;
}

int
axl_ata_decode_self_test(const uint8_t *data, size_t len,
                         AxlAtaSelfTestResult *out)
{
    if (data == NULL || out == NULL || len < 512) {
        return AXL_ERR;
    }
    uint8_t b      = data[ATA_SELF_TEST_STATUS];
    uint8_t status = (uint8_t)(b >> 4);     /* execution status code */
    uint8_t remain = (uint8_t)(b & 0x0F);   /* percent remaining / 10 */

    out->in_progress      = (status == 0x0F);
    out->percent_complete = out->in_progress ? (uint8_t)(100 - remain * 10) : 0;
    out->result_code      = status;
    out->passed           = (status == 0);  /* 0 = completed ok / never run */
    return AXL_OK;
}
