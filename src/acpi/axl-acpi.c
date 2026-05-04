/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi.c
    ACPI table discovery and typed readers.

    Locates the RSDP via the firmware configuration table (preferring
    ACPI 2.0+ which exposes 64-bit XSDT pointers), validates the
    table catalog on first use, and serves up identity-mapped header
    pointers for cursor-style iteration.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-acpi.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>   /* axl_guid_cmp */

AXL_LOG_DOMAIN("acpi");

// ---------------------------------------------------------------------------
// Configuration-table GUIDs we care about
// ---------------------------------------------------------------------------

/* Stored as AxlGuid (binary-compatible with EFI_GUID) so the lookup
   uses axl_guid_cmp instead of a hand-rolled byte compare. */
static const AxlGuid ACPI_20_GUID = AXL_GUID(
    0x8868E871, 0xE4F1, 0x11D3,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81);

static const AxlGuid ACPI_10_GUID = AXL_GUID(
    0xEB9D2D30, 0x2D88, 0x11D3,
    0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D);

// ---------------------------------------------------------------------------
// On-wire ACPI structures (private)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

/* RSDP — Root System Description Pointer (ACPI spec §5.2.5). */
typedef struct {
    char     signature[8];           /* "RSD PTR " (with trailing space) */
    uint8_t  checksum;               /* sum of first 20 bytes mod 256 == 0 */
    char     oem_id[6];
    uint8_t  revision;               /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* Fields below valid only when revision >= 2: */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;      /* sum of all `length` bytes mod 256 == 0 */
    uint8_t  reserved[3];
} AcpiRsdp;

/* Common subtable header used inside MADT (and other tables). */
typedef struct {
    uint8_t type;
    uint8_t length;
} AcpiSubtableHeader;

#pragma pack(pop)

#define ACPI_HEADER_SIZE  36u

#define ACPI_MADT_TYPE_IOAPIC  1u
#define ACPI_MADT_TYPE_GICD    12u
#define ACPI_MADT_TYPE_GICR    14u

/* GICD region size is architecturally fixed by GICv3 to 64 KiB. */
#define ACPI_GICD_SIZE  0x10000u

// ---------------------------------------------------------------------------
// Cached table catalog (built lazily on first use)
// ---------------------------------------------------------------------------

#define MAX_ACPI_TABLES  64

static bool             rsdp_done    = false;   /* RSDP located + validated */
static bool             rsdp_failed  = false;
static bool             catalog_done = false;   /* catalog built */
static bool             catalog_failed = false;
static const AcpiRsdp  *cached_rsdp;
static uint8_t          cached_revision;
static AxlAcpiHeader   *catalog[MAX_ACPI_TABLES];
static size_t           catalog_count;

static const AcpiRsdp *
find_rsdp(
    void
    )
{
    /* Prefer ACPI 2.0+ — it offers 64-bit XSDT pointers, required on
       systems with tables above 4 GiB. Fall back to ACPI 1.0 only if
       2.0 isn't published. */
    const AcpiRsdp *rsdp_10 = NULL;

    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        const AxlGuid *g = (const AxlGuid *)&axl_st()->ConfigurationTable[i].VendorGuid;
        if (axl_guid_cmp(g, &ACPI_20_GUID)) {
            return axl_st()->ConfigurationTable[i].VendorTable;
        }
        if (rsdp_10 == NULL && axl_guid_cmp(g, &ACPI_10_GUID)) {
            rsdp_10 = axl_st()->ConfigurationTable[i].VendorTable;
        }
    }
    return rsdp_10;
}

static void
build_catalog_from_rsdt(
    uint32_t  rsdt_addr
    )
{
    AxlAcpiHeader *rsdt = (AxlAcpiHeader *)(uintptr_t)rsdt_addr;
    if (rsdt == NULL || rsdt->length < ACPI_HEADER_SIZE) {
        return;
    }
    size_t entry_count = (rsdt->length - ACPI_HEADER_SIZE) / sizeof(uint32_t);
    const uint32_t *entries = (const uint32_t *)((const uint8_t *)rsdt
                                                  + ACPI_HEADER_SIZE);
    for (size_t i = 0; i < entry_count && catalog_count < MAX_ACPI_TABLES; i++) {
        if (entries[i] != 0) {
            catalog[catalog_count++] = (AxlAcpiHeader *)(uintptr_t)entries[i];
        }
    }
}

static void
build_catalog_from_xsdt(
    uint64_t  xsdt_addr
    )
{
    AxlAcpiHeader *xsdt = (AxlAcpiHeader *)(uintptr_t)xsdt_addr;
    if (xsdt == NULL || xsdt->length < ACPI_HEADER_SIZE) {
        return;
    }
    size_t entry_count = (xsdt->length - ACPI_HEADER_SIZE) / sizeof(uint64_t);
    /* XSDT entries are 64-bit but may be unaligned in some firmware —
       copy bytewise to avoid faults on architectures that care. */
    const uint8_t *raw = (const uint8_t *)xsdt + ACPI_HEADER_SIZE;
    for (size_t i = 0; i < entry_count && catalog_count < MAX_ACPI_TABLES; i++) {
        uint64_t addr = 0;
        axl_memcpy(&addr, raw + i * sizeof(uint64_t), sizeof(uint64_t));
        if (addr != 0) {
            catalog[catalog_count++] = (AxlAcpiHeader *)(uintptr_t)addr;
        }
    }
}

/* Locate + validate the RSDP. Captures `cached_revision` so callers
   that only need the revision byte (axl_acpi_revision) don't have
   to wait on the catalog build to succeed. */
static int
ensure_rsdp(
    void
    )
{
    if (rsdp_done) {
        return 0;
    }
    if (rsdp_failed) {
        return -1;
    }

    cached_rsdp = find_rsdp();
    if (cached_rsdp == NULL) {
        axl_debug("RSDP not found in EFI configuration table");
        rsdp_failed = true;
        return -1;
    }

    /* Validate the RSDP "RSD PTR " signature (8 bytes, trailing space). */
    static const char rsdp_sig[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
    if (axl_memcmp(cached_rsdp->signature, rsdp_sig, 8) != 0) {
        axl_warning("RSDP signature mismatch");
        rsdp_failed = true;
        return -1;
    }

    cached_revision = cached_rsdp->revision;
    rsdp_done = true;
    return 0;
}

static int
ensure_init(
    void
    )
{
    if (catalog_done) {
        return 0;
    }
    if (catalog_failed) {
        return -1;
    }
    if (ensure_rsdp() != 0) {
        catalog_failed = true;
        return -1;
    }

    catalog_count = 0;
    if (cached_revision >= 2 && cached_rsdp->xsdt_address != 0) {
        build_catalog_from_xsdt(cached_rsdp->xsdt_address);
    } else {
        build_catalog_from_rsdt(cached_rsdp->rsdt_address);
    }

    if (catalog_count == 0) {
        axl_warning("RSDP found but RSDT/XSDT contains no entries");
        catalog_failed = true;
        return -1;
    }

    axl_debug("ACPI catalog: %zu tables (rev %u)", catalog_count, cached_revision);
    catalog_done = true;
    return 0;
}

static size_t
catalog_index_of(
    const AxlAcpiHeader  *h
    )
{
    for (size_t i = 0; i < catalog_count; i++) {
        if (catalog[i] == h) {
            return i;
        }
    }
    return catalog_count;  /* not found → past-the-end sentinel */
}

static bool
sig_match(
    const AxlAcpiHeader  *h,
    const char            sig[4]
    )
{
    return h != NULL && axl_memcmp(h->signature, sig, 4) == 0;
}

// ---------------------------------------------------------------------------
// Public API — discovery + iteration
// ---------------------------------------------------------------------------

AxlAcpiHeader *
axl_acpi_find(
    const char  sig[4]
    )
{
    return axl_acpi_find_next(sig, NULL);
}

AxlAcpiHeader *
axl_acpi_find_next(
    const char      sig[4],
    AxlAcpiHeader  *prev
    )
{
    if (sig == NULL || ensure_init() != 0) {
        return NULL;
    }
    size_t start = (prev == NULL) ? 0 : catalog_index_of(prev) + 1;
    for (size_t i = start; i < catalog_count; i++) {
        if (sig_match(catalog[i], sig)) {
            return catalog[i];
        }
    }
    return NULL;
}

AxlAcpiHeader *
axl_acpi_next(
    AxlAcpiHeader  *prev
    )
{
    if (ensure_init() != 0) {
        return NULL;
    }
    size_t start = (prev == NULL) ? 0 : catalog_index_of(prev) + 1;
    if (start >= catalog_count) {
        return NULL;
    }
    return catalog[start];
}

int
axl_acpi_revision(
    uint8_t  *rev
    )
{
    if (rev == NULL || ensure_rsdp() != 0) {
        return AXL_ERR;
    }
    *rev = cached_revision;
    return AXL_OK;
}

bool
axl_acpi_checksum_ok(
    const AxlAcpiHeader  *h
    )
{
    if (h == NULL || h->length < ACPI_HEADER_SIZE) {
        return false;
    }
    uint8_t        sum = 0;
    const uint8_t *p   = (const uint8_t *)h;
    for (uint32_t i = 0; i < h->length; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

// ---------------------------------------------------------------------------
// Typed reader — MCFG
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

#define MCFG_HEADER_AND_RESERVED  (ACPI_HEADER_SIZE + 8u)

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

// ---------------------------------------------------------------------------
// Typed reader — MADT
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
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

// ---------------------------------------------------------------------------
// Typed reader — FACP / FADT
// ---------------------------------------------------------------------------

/* Field offsets per ACPI 6.x spec §5.2.9 (Fixed ACPI Description Table).
   Reading by offset rather than mirroring the whole packed FADT struct
   keeps us robust against revision differences in the tail fields. */
#define FADT_OFF_FIRMWARE_CTRL    36u
#define FADT_OFF_DSDT             40u
#define FADT_OFF_SMI_CMD          48u
#define FADT_OFF_ACPI_ENABLE      52u
#define FADT_OFF_ACPI_DISABLE     53u
#define FADT_OFF_PM1A_EVT_BLK     56u
#define FADT_OFF_PM1B_EVT_BLK     60u
#define FADT_OFF_PM1A_CNT_BLK     64u
#define FADT_OFF_PM1B_CNT_BLK     68u
#define FADT_OFF_PM1_EVT_LEN      88u
#define FADT_OFF_PM1_CNT_LEN      89u
#define FADT_OFF_IAPC_BOOT_ARCH  109u
#define FADT_OFF_ARM_BOOT_ARCH   129u
#define FADT_OFF_X_FIRMWARE_CTRL 132u
#define FADT_OFF_X_DSDT          140u

#define FADT_MIN_LEN_LEGACY  116u  /* ACPI 1.0 minimum */
#define FADT_MIN_LEN_2_0     244u  /* ACPI 2.0 minimum (covers x_dsdt) */

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
        axl_warning("FACP checksum invalid");
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
