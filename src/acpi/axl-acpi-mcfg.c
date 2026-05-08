/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi-mcfg.c
    Typed reader for the MCFG table (PCI Express Memory-Mapped
    Configuration space description). Walks the segment-group entries
    that follow the standard ACPI header and copies them into the
    public AxlAcpiMcfg shape.
**/

#include "axl-acpi-internal.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("acpi");

// ---------------------------------------------------------------------------
// On-wire MCFG entry layout (private)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct {
    uint64_t base_addr;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} McfgEntry;
#pragma pack(pop)

/* MCFG body starts after the standard 36-byte ACPI header plus an
   8-byte reserved field (per the PCI Firmware Specification §4.1.2). */
#define MCFG_HEADER_AND_RESERVED  (ACPI_HEADER_SIZE + 8u)

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_acpi_read_mcfg(
    AxlAcpiMcfg  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    AxlAcpiHeader *h = axl_acpi_find("MCFG");
    if (h == NULL || h->length < MCFG_HEADER_AND_RESERVED) {
        return AXL_ERR;
    }
    if (!axl_acpi_checksum_ok(h)) {
        axl_warning("MCFG checksum invalid");
        return AXL_ERR;
    }

    out->count = 0;
    const McfgEntry *entries = (const McfgEntry *)((const uint8_t *)h
                                                    + MCFG_HEADER_AND_RESERVED);
    size_t total = (h->length - MCFG_HEADER_AND_RESERVED) / sizeof(McfgEntry);

    for (size_t i = 0; i < total && out->count < AXL_ACPI_MCFG_MAX_SEGMENTS; i++) {
        out->segments[out->count].base_addr = entries[i].base_addr;
        out->segments[out->count].segment   = entries[i].segment;
        out->segments[out->count].start_bus = entries[i].start_bus;
        out->segments[out->count].end_bus   = entries[i].end_bus;
        out->count++;
    }
    return AXL_OK;
}
