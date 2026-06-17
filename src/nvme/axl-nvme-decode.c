/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-nvme-decode.c
    Pure decoders for the NVMe Identify / SMART / Self-test data
    structures (little-endian, per the NVM Express base spec). No UEFI
    dependency — the device-facing readers in axl-nvme.c fetch a buffer
    via the pass-thru protocol and hand it to these. Kept dependency-free
    so the unit suite can exercise them against fixed byte buffers with no
    NVMe device.
**/

#include <axl/axl-nvme.h>

// ---------------------------------------------------------------------------
// Little-endian field readers.
// ---------------------------------------------------------------------------

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

static uint64_t
rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* NVMe stores several counters as 128-bit. We expose 64-bit and saturate
   rather than wrap when the high half is non-zero (no real drive reaches
   2^64 in any of these). */
static uint64_t
rd128_sat(const uint8_t *p)
{
    uint64_t lo = rd64(p);
    uint64_t hi = rd64(p + 8);
    return hi != 0 ? UINT64_MAX : lo;
}

/* Copy a space-padded ASCII identity field (NVMe MN/SN/FR convention) into
   @p dst (capacity @p cap), NUL-terminate, and trim trailing spaces. */
static void
trim_field(const uint8_t *src, size_t src_len, char *dst, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t n = (src_len < cap - 1) ? src_len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = (char)src[i];
    }
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

int
axl_nvme_decode_identify_controller(const uint8_t *id, size_t len,
                                    AxlNvmeController *out)
{
    if (id == NULL || out == NULL || len < 4096) {
        return AXL_ERR;
    }
    out->pci_vid   = rd16(id + 0);
    out->pci_ssvid = rd16(id + 2);
    trim_field(id + 4, 20, out->serial, sizeof(out->serial));
    trim_field(id + 24, 40, out->model, sizeof(out->model));
    trim_field(id + 64, 8, out->firmware, sizeof(out->firmware));
    out->nvme_version    = rd32(id + 80);
    out->namespace_count = rd32(id + 516);
    return AXL_OK;
}

int
axl_nvme_decode_identify_namespace(const uint8_t *id, size_t len,
                                   uint32_t nsid, AxlNvmeNamespace *out)
{
    if (id == NULL || out == NULL || len < 4096) {
        return AXL_ERR;
    }
    out->nsid        = nsid;
    out->size_blocks = rd64(id + 0);

    /* FLBAS (byte 26) low nibble selects the active LBA Format; each LBA
       Format descriptor is 4 bytes starting at byte 128, and its LBADS
       (byte +2) is log2 of the block size. */
    uint8_t flbas = id[26] & 0x0F;
    /* Max index = 128 + 15*4 + 2 = 190, provably < the enforced 4096 above. */
    uint8_t lbads = id[128 + (size_t)flbas * 4 + 2];
    out->block_size = (lbads >= 9 && lbads <= 24) ? (1u << lbads) : 0;
    out->capacity_bytes = out->size_blocks * out->block_size;
    return AXL_OK;
}

int
axl_nvme_decode_smart(const uint8_t *log, size_t len, AxlNvmeSmart *out)
{
    if (log == NULL || out == NULL || len < 512) {
        return AXL_ERR;
    }

    uint8_t cw = log[0];
    out->critical_warning     = cw;
    out->healthy              = (cw == 0);
    out->warn_spare_low       = (cw & 0x01) != 0;
    out->warn_temperature     = (cw & 0x02) != 0;
    out->warn_reliability     = (cw & 0x04) != 0;
    out->warn_read_only       = (cw & 0x08) != 0;
    out->warn_volatile_backup = (cw & 0x10) != 0;

    /* Composite Temperature is unsigned Kelvin; 0 means not reported. */
    uint16_t kelvin = rd16(log + 1);
    out->temperature_c = (kelvin == 0) ? INT32_MIN : (int32_t)kelvin - 273;

    out->available_spare           = log[3];
    out->available_spare_threshold = log[4];
    out->percent_used              = log[5];

    out->data_units_read    = rd128_sat(log + 32);
    out->data_units_written = rd128_sat(log + 48);
    out->host_read_commands = rd128_sat(log + 64);
    out->host_write_commands = rd128_sat(log + 80);
    out->power_cycles       = rd128_sat(log + 112);
    out->power_on_hours     = rd128_sat(log + 128);
    out->unsafe_shutdowns   = rd128_sat(log + 144);
    out->media_errors       = rd128_sat(log + 160);
    out->error_log_entries  = rd128_sat(log + 176);
    return AXL_OK;
}

int
axl_nvme_decode_self_test_log(const uint8_t *log, size_t len,
                              AxlNvmeSelfTestResult *out)
{
    if (log == NULL || out == NULL || len < 564) {
        return AXL_ERR;
    }

    /* Byte 0 low nibble: current operation (0 = none in progress).
       Byte 1 low 7 bits: completion percent of the in-progress op. */
    out->in_progress      = (log[0] & 0x0F) != 0;
    out->percent_complete = out->in_progress ? (log[1] & 0x7F) : 0;

    /* The newest Self-test Result Data Structure begins at byte 4; its
       status byte holds the Self-test code (bits 7:4) and Result code
       (bits 3:0). Result 0 = passed; 0xF = entry unused (no prior run). */
    uint8_t result   = log[4] & 0x0F;
    out->result_code = result;
    out->passed      = (result == 0);
    return AXL_OK;
}
