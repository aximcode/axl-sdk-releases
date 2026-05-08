/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi-madt.c
    Typed reader for the MADT (Multiple APIC Description Table; ACPI
    spec §5.2.12). Walks the variable-length subtable list and
    populates the AxlAcpiMadt shape with I/O APIC and ARM GIC
    descriptors.

    Subtable types worth knowing live in the Local Interrupt
    Controller / I/O APIC / GIC families. We only handle I/O APIC
    (type 1) and GIC distributor / redistributor (types 12 / 14) —
    the consumers need addresses for those regions, not full
    interrupt-routing tables.
**/

#include "axl-acpi-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>   /* axl_memcpy / axl_memset */

AXL_LOG_DOMAIN("acpi");

// ---------------------------------------------------------------------------
// On-wire MADT subtable layouts (private)
// ---------------------------------------------------------------------------

#define ACPI_MADT_TYPE_IOAPIC  1u
#define ACPI_MADT_TYPE_GICD    12u
#define ACPI_MADT_TYPE_GICR    14u

/* GICD region size is architecturally fixed by GICv3 to 64 KiB. */
#define ACPI_GICD_SIZE  0x10000u

#pragma pack(push, 1)

/* Common subtable header; every MADT entry begins with these two
   bytes. Used to advance through the variable-length list. */
typedef struct {
    uint8_t type;
    uint8_t length;
} AcpiSubtableHeader;

typedef struct {
    uint8_t  type;          /* = 1 */
    uint8_t  length;        /* = 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t addr;
    uint32_t global_irq_base;
} MadtIoapic;

typedef struct {
    uint8_t  type;          /* = 12 (GICD) */
    uint8_t  length;        /* = 24 */
    uint16_t reserved;
    uint32_t gicd_id;
    uint64_t physical_base;
    /* trailing fields differ across ACPI revisions; ignored here */
} MadtGicd;

typedef struct {
    uint8_t  type;          /* = 14 (GICR) */
    uint8_t  length;        /* = 16 */
    uint16_t reserved;
    uint64_t discovery_base;
    uint32_t discovery_length;
} MadtGicr;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_acpi_read_madt(
    AxlAcpiMadt  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    AxlAcpiHeader *h = axl_acpi_find("APIC");  /* MADT signature is 'APIC' */
    if (h == NULL || h->length < ACPI_HEADER_SIZE + 8u) {
        return AXL_ERR;
    }
    if (!axl_acpi_checksum_ok(h)) {
        axl_warning("MADT checksum invalid");
        return AXL_ERR;
    }

    /* Zero output, then fill */
    axl_memset(out, 0, sizeof(*out));

    const uint8_t *body = (const uint8_t *)h + ACPI_HEADER_SIZE;
    /* MADT body header: uint32_t local_apic_addr; uint32_t flags; */
    axl_memcpy(&out->local_apic_addr, body + 0, sizeof(uint32_t));
    axl_memcpy(&out->flags,           body + 4, sizeof(uint32_t));

    const uint8_t *p   = body + 8;
    const uint8_t *end = (const uint8_t *)h + h->length;

    while (p + sizeof(AcpiSubtableHeader) <= end) {
        const AcpiSubtableHeader *sh = (const AcpiSubtableHeader *)p;
        if (sh->length < sizeof(AcpiSubtableHeader)
            || p + sh->length > end) {
            break;  /* malformed — stop */
        }

        switch (sh->type) {
        case ACPI_MADT_TYPE_IOAPIC:
            if (sh->length >= sizeof(MadtIoapic)
                && out->ioapic_count < AXL_ACPI_MADT_MAX_IOAPICS) {
                const MadtIoapic *e = (const MadtIoapic *)p;
                AxlAcpiIoapic *o = &out->ioapics[out->ioapic_count++];
                o->id              = e->ioapic_id;
                o->addr            = e->addr;
                o->global_irq_base = e->global_irq_base;
            }
            break;

        case ACPI_MADT_TYPE_GICD:
            if (sh->length >= sizeof(MadtGicd)
                && out->gic_region_count < AXL_ACPI_MADT_MAX_GIC_REGS) {
                const MadtGicd *e = (const MadtGicd *)p;
                AxlAcpiGicRegion *o = &out->gic_regions[out->gic_region_count++];
                o->subtable_type = ACPI_MADT_TYPE_GICD;
                o->addr          = e->physical_base;
                o->length        = ACPI_GICD_SIZE;
            }
            break;

        case ACPI_MADT_TYPE_GICR:
            if (sh->length >= sizeof(MadtGicr)
                && out->gic_region_count < AXL_ACPI_MADT_MAX_GIC_REGS) {
                const MadtGicr *e = (const MadtGicr *)p;
                AxlAcpiGicRegion *o = &out->gic_regions[out->gic_region_count++];
                o->subtable_type = ACPI_MADT_TYPE_GICR;
                o->addr          = e->discovery_base;
                o->length        = e->discovery_length;
            }
            break;

        default:
            /* skip — unhandled subtable type */
            break;
        }

        p += sh->length;
    }
    return AXL_OK;
}
