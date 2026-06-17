/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi.c
    ACPI table discovery and typed readers.

    Locates the RSDP via the firmware configuration table (preferring
    ACPI 2.0+ which exposes 64-bit XSDT pointers), validates the
    table catalog on first use, and serves up identity-mapped header
    pointers for cursor-style iteration.
**/

#include "axl-acpi-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
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

#pragma pack(pop)

/* ACPI_HEADER_SIZE lives in axl-acpi-internal.h — shared with the
   per-table typed readers. MADT-specific subtable layouts and
   macros (AcpiSubtableHeader, ACPI_MADT_TYPE_*, ACPI_GICD_SIZE)
   moved to axl-acpi-madt.c. */

// ---------------------------------------------------------------------------
// Cached table catalog (built lazily on first use)
// ---------------------------------------------------------------------------

/* Headroom over the ~40-60 tables a populated server publishes, with
   room for the FADT's FACS + DSDT children folded in below. */
#define MAX_ACPI_TABLES  128

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

/* Append a table to the catalog, skipping NULL, duplicates, and
   overflow. The single append site for every source — the XSDT/RSDT
   walk and the FADT's children — so dedup and the cap are handled
   once, and a too-small catalog warns rather than silently dropping. */
static void
catalog_append(
    AxlAcpiHeader  *h
    )
{
    if (h == NULL) {
        return;
    }
    if (catalog_count >= MAX_ACPI_TABLES) {
        static bool warned;
        if (!warned) {
            axl_warning("ACPI catalog full at %d entries; some tables omitted",
                        MAX_ACPI_TABLES);
            warned = true;
        }
        return;
    }
    for (size_t i = 0; i < catalog_count; i++) {
        if (catalog[i] == h) {
            return;   /* already catalogued */
        }
    }
    catalog[catalog_count++] = h;
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
    for (size_t i = 0; i < entry_count; i++) {
        catalog_append((AxlAcpiHeader *)(uintptr_t)entries[i]);
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
    for (size_t i = 0; i < entry_count; i++) {
        uint64_t addr = 0;
        axl_memcpy(&addr, raw + i * sizeof(uint64_t), sizeof(uint64_t));
        catalog_append((AxlAcpiHeader *)(uintptr_t)addr);
    }
}

/* FACS and DSDT are not XSDT/RSDT entries — they hang off the FADT
   (FirmwareCtrl/X_FirmwareCtrl, Dsdt/X_Dsdt). Fold them into the catalog
   so a single walk yields the complete table set, and place them FIRST
   — FACS then DSDT, ahead of the XSDT/RSDT tables — matching
   EFI_ACPI_SDT_PROTOCOL.GetAcpiTable / acpidump /
   /sys/firmware/acpi/tables, so a consumer diffing against that oracle
   needs no reordering. Prefer the 64-bit extended pointers (ACPI 2.0+),
   falling back to the legacy 32-bit fields. Note FACS is not a standard
   SDT — only its signature + length are meaningful (axl_acpi_checksum_ok
   does not apply to it). */
static void
front_load_fadt_children(
    void
    )
{
    AxlAcpiHeader *fadt = NULL;
    for (size_t i = 0; i < catalog_count; i++) {
        if (catalog[i] != NULL
            && axl_memcmp(catalog[i]->signature, "FACP", 4) == 0) {
            fadt = catalog[i];
            break;
        }
    }
    if (fadt == NULL || fadt->length < FADT_OFF_DSDT + sizeof(uint32_t)) {
        return;
    }
    const uint8_t *p = (const uint8_t *)fadt;

    /* DSDT — prefer X_Dsdt (ACPI 2.0+), fall back to the 32-bit Dsdt. */
    uint64_t dsdt = 0;
    if (fadt->length >= FADT_OFF_X_DSDT + sizeof(uint64_t)) {
        axl_memcpy(&dsdt, p + FADT_OFF_X_DSDT, sizeof(dsdt));
    }
    if (dsdt == 0) {
        uint32_t dsdt32 = 0;
        axl_memcpy(&dsdt32, p + FADT_OFF_DSDT, sizeof(dsdt32));
        dsdt = dsdt32;
    }

    /* FACS — prefer X_FirmwareCtrl, fall back to FirmwareCtrl. Absent
       (0) on platforms without a FACS (typical on ARM). */
    uint64_t facs = 0;
    if (fadt->length >= FADT_OFF_X_FIRMWARE_CTRL + sizeof(uint64_t)) {
        axl_memcpy(&facs, p + FADT_OFF_X_FIRMWARE_CTRL, sizeof(facs));
    }
    if (facs == 0) {
        uint32_t facs32 = 0;
        axl_memcpy(&facs32, p + FADT_OFF_FIRMWARE_CTRL, sizeof(facs32));
        facs = facs32;
    }

    /* Front-load order: FACS, then DSDT (oracle order). */
    AxlAcpiHeader *children[2];
    size_t         nchildren = 0;
    if (facs != 0) {
        children[nchildren++] = (AxlAcpiHeader *)(uintptr_t)facs;
    }
    if (dsdt != 0) {
        children[nchildren++] = (AxlAcpiHeader *)(uintptr_t)dsdt;
    }
    if (nchildren == 0) {
        return;
    }

    /* Rebuild the catalog: children first, then the existing tables in
       XSDT/RSDT order, de-duplicated (a child the XSDT also named — not
       expected — is listed once, at the front). */
    AxlAcpiHeader *reordered[MAX_ACPI_TABLES];
    size_t         n = 0;
    for (size_t i = 0; i < nchildren; i++) {
        reordered[n++] = children[i];
    }
    for (size_t i = 0; i < catalog_count && n < MAX_ACPI_TABLES; i++) {
        bool is_child = false;
        for (size_t j = 0; j < nchildren; j++) {
            if (catalog[i] == children[j]) {
                is_child = true;
                break;
            }
        }
        if (!is_child) {
            reordered[n++] = catalog[i];
        }
    }
    axl_memcpy(catalog, reordered, n * sizeof(catalog[0]));
    catalog_count = n;
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

    /* Fold in the FADT's FACS + DSDT children (front of the catalog),
       which the XSDT/RSDT does not list, so the catalog is the complete
       table set in the oracle's order. */
    front_load_fadt_children();

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

/* Typed-table readers (MCFG, MADT, FACP) live in their own
   sibling .c files: axl-acpi-mcfg.c, axl-acpi-madt.c,
   axl-acpi-fadt.c. They consume axl_acpi_find +
   axl_acpi_checksum_ok via <axl/axl-acpi.h>; this file owns the
   discovery / iteration primitives only. */
