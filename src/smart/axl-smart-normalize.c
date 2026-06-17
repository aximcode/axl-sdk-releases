/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smart-normalize.c
    Pure per-transport normalizers — map an NVMe / ATA / SCSI decoded
    identity+health pair onto the uniform AxlSmartHealth record. No device,
    no firmware: pure functions over the already-decoded structs, and the
    hardware-free unit-test seam for AxlSmart. The device-facing
    axl_smart_health (axl-smart.c) reads the transport's structs and calls
    one of these.
**/

#include <axl/axl-smart.h>
#include <axl/axl-str.h>   /* axl_memset / axl_strlcpy / axl_snprintf */

// ---------------------------------------------------------------------------
// Normalizers
// ---------------------------------------------------------------------------

/* Map the NVMe SMART warn_* booleans onto the normalized warning bits. */
static uint32_t
nvme_warning_flags(const AxlNvmeSmart *s)
{
    uint32_t f = 0;
    if (s->warn_spare_low)       f |= AXL_SMART_WARN_SPARE;
    if (s->warn_temperature)     f |= AXL_SMART_WARN_TEMPERATURE;
    if (s->warn_reliability)     f |= AXL_SMART_WARN_RELIABILITY;
    if (s->warn_read_only)       f |= AXL_SMART_WARN_READ_ONLY;
    if (s->warn_volatile_backup) f |= AXL_SMART_WARN_BACKUP;
    return f;
}

int
axl_smart_from_nvme(
    const AxlNvmeController *id,
    const AxlNvmeSmart      *s,
    AxlSmartHealth          *out
    )
{
    if (id == NULL || s == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));
    out->transport = AXL_STORAGE_NVME;
    axl_strlcpy(out->model,    id->model,    sizeof(out->model));
    axl_strlcpy(out->serial,   id->serial,   sizeof(out->serial));
    axl_strlcpy(out->firmware, id->firmware, sizeof(out->firmware));

    out->healthy            = s->healthy;
    out->warning_flags      = nvme_warning_flags(s);
    out->temperature_c      = s->temperature_c;
    out->power_on_hours     = s->power_on_hours;
    out->power_cycles       = s->power_cycles;
    out->percent_used       = s->percent_used;
    out->media_errors       = s->media_errors;
    out->data_units_read    = s->data_units_read;
    out->data_units_written = s->data_units_written;
    return AXL_OK;
}

int
axl_smart_from_ata(
    const AxlAtaIdentify *id,
    const AxlAtaSmart    *s,
    AxlSmartHealth       *out
    )
{
    if (id == NULL || s == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));
    out->transport = AXL_STORAGE_ATA;
    axl_strlcpy(out->model,    id->model,    sizeof(out->model));
    axl_strlcpy(out->serial,   id->serial,   sizeof(out->serial));
    axl_strlcpy(out->firmware, id->firmware, sizeof(out->firmware));

    out->healthy        = s->healthy;
    /* ATA exposes only the overall verdict — no per-condition decode. */
    out->warning_flags  = s->healthy ? 0u : AXL_SMART_WARN_RELIABILITY;
    out->temperature_c  = s->temperature_c;
    out->power_on_hours = s->power_on_hours;
    out->power_cycles   = s->power_cycles;
    out->percent_used   = s->percent_used;              /* 0xFF for ATA */
    out->media_errors   = s->reallocated_sectors;
    out->data_units_read    = UINT64_MAX;               /* no ATA equivalent */
    out->data_units_written = UINT64_MAX;
    return AXL_OK;
}

int
axl_smart_from_scsi(
    const AxlScsiInquiry *inq,
    const AxlScsiHealth  *h,
    AxlSmartHealth       *out
    )
{
    if (inq == NULL || h == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));
    out->transport = AXL_STORAGE_SCSI;
    /* model = "vendor product" (both already space-trimmed); drop the join
       space when the vendor field is empty. */
    if (inq->vendor[0] != '\0') {
        axl_snprintf(out->model, sizeof(out->model), "%s %s",
                     inq->vendor, inq->product);
    } else {
        axl_strlcpy(out->model, inq->product, sizeof(out->model));
    }
    axl_strlcpy(out->serial,   inq->serial,   sizeof(out->serial));
    axl_strlcpy(out->firmware, inq->revision, sizeof(out->firmware));

    out->healthy        = h->healthy;
    out->warning_flags  = h->healthy ? 0u : AXL_SMART_WARN_RELIABILITY;
    out->temperature_c  = h->temperature_c;
    /* Phase-3 SCSI health does not supply these. */
    out->power_on_hours     = UINT64_MAX;
    out->power_cycles       = UINT64_MAX;
    out->percent_used       = 0xFF;
    out->media_errors       = UINT64_MAX;
    out->data_units_read    = UINT64_MAX;
    out->data_units_written = UINT64_MAX;
    return AXL_OK;
}
