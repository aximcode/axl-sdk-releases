/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi-fadt.c
    Typed reader for the Fixed ACPI Description Table (FADT, signature
    "FACP" per ACPI spec §5.2.9). Reads fields by spec offset rather
    than mirroring the whole packed FADT struct, which keeps us robust
    against revision differences in the tail fields.
**/

#include "axl-acpi-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>   /* axl_memcpy / axl_memset */

AXL_LOG_DOMAIN("acpi");

/* FADT field offsets (FADT_OFF_*) + minimum-length constants live in
   axl-acpi-internal.h — shared with the table catalog in axl-acpi.c. */

// ---------------------------------------------------------------------------
// Unaligned little-endian read helpers
// ---------------------------------------------------------------------------

/* axl_memcpy avoids unaligned-access faults on architectures that
   trap them (some aarch64 configurations) — the ACPI tables are
   packed and field alignment within the body isn't guaranteed. */

static uint8_t
read_u8(
    const uint8_t  *base,
    uint32_t        offset
    )
{
    return base[offset];
}

static uint16_t
read_u16(
    const uint8_t  *base,
    uint32_t        offset
    )
{
    uint16_t v;
    axl_memcpy(&v, base + offset, sizeof(v));
    return v;
}

static uint32_t
read_u32(
    const uint8_t  *base,
    uint32_t        offset
    )
{
    uint32_t v;
    axl_memcpy(&v, base + offset, sizeof(v));
    return v;
}

static uint64_t
read_u64(
    const uint8_t  *base,
    uint32_t        offset
    )
{
    uint64_t v;
    axl_memcpy(&v, base + offset, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_acpi_read_facp(
    AxlAcpiFacp  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    AxlAcpiHeader *h = axl_acpi_find("FACP");
    if (h == NULL || h->length < FADT_MIN_LEN_LEGACY) {
        return AXL_ERR;
    }
    if (!axl_acpi_checksum_ok(h)) {
        axl_debug("FACP checksum invalid");
        return AXL_ERR;
    }

    axl_memset(out, 0, sizeof(*out));
    const uint8_t *p = (const uint8_t *)h;

    out->firmware_ctrl  = read_u32(p, FADT_OFF_FIRMWARE_CTRL);
    out->dsdt           = read_u32(p, FADT_OFF_DSDT);
    out->smi_cmd        = read_u32(p, FADT_OFF_SMI_CMD);
    out->acpi_enable    = read_u8 (p, FADT_OFF_ACPI_ENABLE);
    out->acpi_disable   = read_u8 (p, FADT_OFF_ACPI_DISABLE);
    /* PM1 event/control blocks: legacy fields are 32-bit ports; truncate
       to 16-bit which is all the I/O port space allows. */
    out->pm1a_evt_blk   = (uint16_t)read_u32(p, FADT_OFF_PM1A_EVT_BLK);
    out->pm1b_evt_blk   = (uint16_t)read_u32(p, FADT_OFF_PM1B_EVT_BLK);
    out->pm1a_cnt_blk   = (uint16_t)read_u32(p, FADT_OFF_PM1A_CNT_BLK);
    out->pm1b_cnt_blk   = (uint16_t)read_u32(p, FADT_OFF_PM1B_CNT_BLK);
    out->pm1_evt_len    = read_u8 (p, FADT_OFF_PM1_EVT_LEN);
    out->pm1_cnt_len    = read_u8 (p, FADT_OFF_PM1_CNT_LEN);

    if (h->length >= FADT_OFF_IAPC_BOOT_ARCH + sizeof(uint16_t)) {
        out->iapc_boot_arch = read_u16(p, FADT_OFF_IAPC_BOOT_ARCH);
    }
    if (h->length >= FADT_OFF_ARM_BOOT_ARCH + sizeof(uint16_t)) {
        out->arm_boot_arch = read_u16(p, FADT_OFF_ARM_BOOT_ARCH);
    }

    /* ACPI 2.0+ extended pointers (only valid if the FADT is large
       enough to contain them). */
    if (h->length >= FADT_MIN_LEN_2_0) {
        out->x_firmware_ctrl = read_u64(p, FADT_OFF_X_FIRMWARE_CTRL);
        out->x_dsdt          = read_u64(p, FADT_OFF_X_DSDT);
    }
    return AXL_OK;
}
