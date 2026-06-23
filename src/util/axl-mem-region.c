/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-mem-region.c
    Physical-memory region map + fault-safe range access.

    The region map is built by overlaying two firmware sources:

      - the UEFI memory map (gBS->GetMemoryMap) — fine-grained types
        (RAM vs ACPI vs reserved), but it omits most MMIO;
      - the PI GCD memory-space map (gDS->GetMemorySpaceMap) — complete
        coverage of the physical address space including MMIO (PCI BARs)
        and non-existent ranges, but only coarse types.

    Both are decomposed into elementary intervals on a sorted set of
    boundaries; each interval takes the EFI type where an EFI descriptor
    covers it, else the GCD type, else UNMAPPED. Adjacent same-type
    intervals are coalesced. The result is cached; refresh rebuilds it.

    If the DXE Services table is unreachable, the EFI map is used alone
    (MMIO classification is then limited to what the EFI map reports, and
    gaps between EFI descriptors classify as UNMAPPED).
**/

#include "../backend/axl-backend.h"
#include <uefi/axl-uefi.h>

#include <axl/axl-mem-region.h>
#include <axl/axl-mem-phys.h>
#include <axl/axl-port.h>      /* axl_io_port_read/write (x86) */
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sort.h>
#include <axl/axl-atexit.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("mem-region");

// ---------------------------------------------------------------------------
// Internal interval type
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t         lo;    // inclusive start
    uint64_t         hi;    // exclusive end
    AxlMemRegionType type;
    uint64_t         attr;
} MemIv;

// ---------------------------------------------------------------------------
// State (UEFI is single-threaded / BSP — plain process-global)
// ---------------------------------------------------------------------------

static AxlMemRegion      *g_regions;
static size_t             g_region_count;
static bool               g_built;
static bool               g_cleanup_armed;
static AxlMemAccessPolicy g_policy = {
    AXL_MEM_ACCESS_ALL_MAPPED, AXL_MEM_ACCESS_ALL_MAPPED
};

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

static AxlMemRegionType
classify_efi(uint32_t type)
{
    switch (type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
        return AXL_MEM_REGION_RAM;
    case EfiACPIReclaimMemory:
    case EfiACPIMemoryNVS:
        return AXL_MEM_REGION_ACPI;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
        return AXL_MEM_REGION_MMIO;
    default:
        /* Reserved, Runtime*, Unusable, PalCode, Persistent, … — mapped
           but not general-purpose RAM. */
        return AXL_MEM_REGION_RESERVED;
    }
}

static AxlMemRegionType
classify_gcd(EFI_GCD_MEMORY_TYPE type)
{
    switch (type) {
    case EfiGcdMemoryTypeSystemMemory:
    case EfiGcdMemoryTypeMoreReliable:
        return AXL_MEM_REGION_RAM;
    case EfiGcdMemoryTypeMemoryMappedIo:
        return AXL_MEM_REGION_MMIO;
    case EfiGcdMemoryTypeNonExistent:
        return AXL_MEM_REGION_UNMAPPED;
    default:
        /* Reserved, Persistent, Unaccepted, … */
        return AXL_MEM_REGION_RESERVED;
    }
}

// ---------------------------------------------------------------------------
// Firmware-map collection
// ---------------------------------------------------------------------------

// Allocate + fill an interval array from the EFI memory map. Returns the
// interval count via *out_n (0 on failure); caller frees *out_iv.
static MemIv *
collect_efi(size_t *out_n)
{
    *out_n = 0;
    size_t map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_STATUS s = axl_bs()->GetMemoryMap(&map_size, NULL, &map_key,
                                          &desc_size, &desc_ver);
    if (s != EFI_BUFFER_TOO_SMALL || desc_size == 0) {
        return NULL;
    }
    map_size += desc_size * 8;   /* slack for growth across the two calls */
    uint8_t *map = axl_malloc(map_size);
    if (map == NULL) {
        return NULL;
    }
    s = axl_bs()->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map,
                               &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(s)) {
        axl_free(map);
        return NULL;
    }
    size_t n = map_size / desc_size;
    MemIv *iv = axl_calloc(n ? n : 1, sizeof(*iv));
    if (iv == NULL) {
        axl_free(map);
        return NULL;
    }
    size_t k = 0;
    for (size_t off = 0; off + desc_size <= map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        if (d->NumberOfPages == 0) {
            continue;
        }
        iv[k].lo   = d->PhysicalStart;
        iv[k].hi   = d->PhysicalStart + d->NumberOfPages * 4096ULL;
        iv[k].type = classify_efi(d->Type);
        iv[k].attr = d->Attribute;
        k++;
    }
    axl_free(map);
    *out_n = k;
    return iv;
}

// Locate the DXE Services table (gDS) via the EFI configuration table, or NULL.
static DXE_SERVICES *
locate_dxe_services(void)
{
    for (size_t i = 0; i < axl_st()->NumberOfTableEntries; i++) {
        if (axl_efi_guid_equal(&axl_st()->ConfigurationTable[i].VendorGuid,
                           &gEfiDxeServicesTableGuid)) {
            return axl_st()->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

// Allocate + fill an interval array from the GCD memory-space map. Returns
// NULL (and *out_n = 0) when the DXE Services table is unreachable.
static MemIv *
collect_gcd(size_t *out_n)
{
    *out_n = 0;
    DXE_SERVICES *ds = locate_dxe_services();
    if (ds == NULL || ds->GetMemorySpaceMap == NULL) {
        return NULL;
    }
    UINTN gn = 0;
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR *gmap = NULL;
    if (EFI_ERROR(ds->GetMemorySpaceMap(&gn, &gmap)) || gmap == NULL) {
        return NULL;
    }
    MemIv *iv = axl_calloc(gn ? gn : 1, sizeof(*iv));
    if (iv == NULL) {
        axl_bs()->FreePool(gmap);
        return NULL;
    }
    size_t k = 0;
    for (UINTN i = 0; i < gn; i++) {
        if (gmap[i].Length == 0) {
            continue;
        }
        iv[k].lo   = gmap[i].BaseAddress;
        iv[k].hi   = gmap[i].BaseAddress + gmap[i].Length;
        iv[k].type = classify_gcd(gmap[i].GcdMemoryType);
        iv[k].attr = gmap[i].Attributes;
        k++;
    }
    axl_bs()->FreePool(gmap);   /* firmware-allocated */
    *out_n = k;
    return iv;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// Look up the type/attr covering address @p at: EFI takes precedence over GCD,
// else UNMAPPED.
static void
classify_at(uint64_t at, const MemIv *efi, size_t ne, const MemIv *gcd,
            size_t ng, AxlMemRegionType *type, uint64_t *attr)
{
    for (size_t i = 0; i < ne; i++) {
        if (at >= efi[i].lo && at < efi[i].hi) {
            *type = efi[i].type;
            *attr = efi[i].attr;
            return;
        }
    }
    for (size_t i = 0; i < ng; i++) {
        if (at >= gcd[i].lo && at < gcd[i].hi) {
            *type = gcd[i].type;
            *attr = gcd[i].attr;
            return;
        }
    }
    *type = AXL_MEM_REGION_UNMAPPED;
    *attr = 0;
}

static void
region_cleanup(void *unused)
{
    (void)unused;
    axl_free(g_regions);
    g_regions = NULL;
    g_region_count = 0;
    g_built = false;
}

static int
build_regions(void)
{
    size_t ne = 0, ng = 0;
    MemIv *efi = collect_efi(&ne);
    if (efi == NULL) {
        return AXL_ERR;            /* the EFI map is the minimum we need */
    }
    MemIv *gcd = collect_gcd(&ng); /* may be NULL (EFI-map-only mode) */

    /* Sorted, de-duplicated boundary set from both maps. */
    size_t cap = (ne + ng) * 2;
    uint64_t *bnd = axl_malloc(cap * sizeof(*bnd));
    AxlMemRegion *out = axl_calloc((cap ? cap : 1), sizeof(*out));
    if (bnd == NULL || out == NULL) {
        axl_free(bnd);
        axl_free(out);
        axl_free(efi);
        axl_free(gcd);
        return AXL_ERR;
    }
    size_t nb = 0;
    for (size_t i = 0; i < ne; i++) { bnd[nb++] = efi[i].lo; bnd[nb++] = efi[i].hi; }
    for (size_t i = 0; i < ng; i++) { bnd[nb++] = gcd[i].lo; bnd[nb++] = gcd[i].hi; }
    axl_qsort(bnd, nb, sizeof(*bnd), cmp_u64);

    /* Each elementary interval [bnd[k], bnd[k+1]) gets one classification;
       coalesce adjacent same-type runs. */
    size_t tn = 0;
    for (size_t k = 0; k + 1 < nb; k++) {
        uint64_t lo = bnd[k], hi = bnd[k + 1];
        if (hi <= lo) {
            continue;            /* duplicate boundary */
        }
        AxlMemRegionType type;
        uint64_t attr;
        classify_at(lo, efi, ne, gcd, ng, &type, &attr);
        if (tn > 0 && out[tn - 1].type == type
            && (uint64_t)out[tn - 1].base + out[tn - 1].len == lo) {
            out[tn - 1].len += (hi - lo);
        } else {
            out[tn].base = (uintptr_t)lo;
            out[tn].len  = hi - lo;
            out[tn].type = type;
            out[tn].attr = attr;
            tn++;
        }
    }

    axl_free(bnd);
    axl_free(efi);
    axl_free(gcd);

    axl_free(g_regions);
    g_regions = out;
    g_region_count = tn;
    g_built = true;
    if (!g_cleanup_armed) {
        (void)axl_atexit(region_cleanup, NULL);
        g_cleanup_armed = true;
    }
    return AXL_OK;
}

// Ensure the cached map exists. Returns AXL_OK if usable.
static int
ensure_built(void)
{
    if (g_built) {
        return AXL_OK;
    }
    return build_regions();
}

// Index of the region containing phys, or -1.
static long
region_index_of(uint64_t phys)
{
    for (size_t i = 0; i < g_region_count; i++) {
        if (phys >= (uint64_t)g_regions[i].base
            && phys < (uint64_t)g_regions[i].base + g_regions[i].len) {
            return (long)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Public: enumeration / classification
// ---------------------------------------------------------------------------

int
axl_mem_phys_region_count(size_t *out_count)
{
    if (out_count == NULL || ensure_built() != AXL_OK) {
        return AXL_ERR;
    }
    *out_count = g_region_count;
    return AXL_OK;
}

int
axl_mem_phys_region_get(size_t index, AxlMemRegion *out)
{
    if (out == NULL || ensure_built() != AXL_OK || index >= g_region_count) {
        return AXL_ERR;
    }
    *out = g_regions[index];
    return AXL_OK;
}

int
axl_mem_phys_region_at(uintptr_t phys, AxlMemRegion *out)
{
    if (out == NULL || ensure_built() != AXL_OK) {
        return AXL_ERR;
    }
    long i = region_index_of(phys);
    if (i >= 0) {
        *out = g_regions[(size_t)i];
        return AXL_OK;
    }
    /* Outside every described region — synthesize an UNMAPPED region with
       best-effort bounds (below the first region, or above the last). */
    out->type = AXL_MEM_REGION_UNMAPPED;
    out->attr = 0;
    if (g_region_count == 0 || phys < (uint64_t)g_regions[0].base) {
        out->base = 0;
        out->len  = (g_region_count == 0) ? UINT64_MAX : (uint64_t)g_regions[0].base;
    } else {
        uint64_t top = (uint64_t)g_regions[g_region_count - 1].base
                     + g_regions[g_region_count - 1].len;
        out->base = (uintptr_t)top;
        out->len  = UINT64_MAX - top;
    }
    return AXL_OK;
}

int
axl_mem_phys_region_refresh(void)
{
    g_built = false;   /* force rebuild; build_regions frees the old map */
    return build_regions();
}

// ---------------------------------------------------------------------------
// Public: policy
// ---------------------------------------------------------------------------

int
axl_mem_phys_get_policy(AxlMemAccessPolicy *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = g_policy;
    return AXL_OK;
}

int
axl_mem_phys_set_policy(const AxlMemAccessPolicy *policy)
{
    if (policy == NULL) {
        g_policy.readable_types = AXL_MEM_ACCESS_ALL_MAPPED;
        g_policy.writable_types = AXL_MEM_ACCESS_ALL_MAPPED;
    } else {
        g_policy = *policy;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public: accessibility gate
// ---------------------------------------------------------------------------

bool
axl_mem_phys_is_accessible(uintptr_t phys, size_t len, bool want_write)
{
    if (len == 0 || ensure_built() != AXL_OK) {
        return false;
    }
    uint64_t end = (uint64_t)phys + len;
    if (end < (uint64_t)phys) {
        return false;            /* address-space overflow */
    }
    uint32_t mask = want_write ? g_policy.writable_types
                               : g_policy.readable_types;
    uint64_t cur = phys;
    while (cur < end) {
        long i = region_index_of(cur);
        if (i < 0) {
            return false;        /* a gap above the described space */
        }
        AxlMemRegion *r = &g_regions[(size_t)i];
        if (r->type == AXL_MEM_REGION_UNMAPPED) {
            return false;        /* never accessible — would fault */
        }
        if ((mask & (1u << r->type)) == 0) {
            return false;        /* denied by the current policy */
        }
        cur = (uint64_t)r->base + r->len;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public: bulk width-aware range read / write
// ---------------------------------------------------------------------------

static bool
range_args_ok(uintptr_t phys, size_t len, const void *buf, uint32_t w)
{
    if (buf == NULL || len == 0) {
        return false;
    }
    if (w != 1 && w != 2 && w != 4 && w != 8) {
        return false;
    }
    if ((phys % w) != 0 || (len % w) != 0) {
        return false;
    }
    return true;
}

int
axl_mem_phys_read_range(uintptr_t phys, size_t len, void *buf,
                        uint32_t access_width)
{
    if (!range_args_ok(phys, len, buf, access_width)
        || !axl_mem_phys_is_accessible(phys, len, false)) {
        return AXL_ERR;
    }
    uint8_t *p = buf;
    for (size_t off = 0; off < len; off += access_width) {
        union { uint8_t b[8]; uint16_t u16; uint32_t u32; uint64_t u64; } t;
        int rc;
        switch (access_width) {
        case 1:  rc = axl_mem_phys_read8 (phys + off, &t.b[0]); break;
        case 2:  rc = axl_mem_phys_read16(phys + off, &t.u16);  break;
        case 4:  rc = axl_mem_phys_read32(phys + off, &t.u32);  break;
        default: rc = axl_mem_phys_read64(phys + off, &t.u64);  break;
        }
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
        axl_memcpy(p + off, t.b, access_width);
    }
    return AXL_OK;
}

int
axl_mem_phys_write_range(uintptr_t phys, size_t len, const void *buf,
                         uint32_t access_width)
{
    if (!range_args_ok(phys, len, buf, access_width)
        || !axl_mem_phys_is_accessible(phys, len, true)) {
        return AXL_ERR;
    }
    const uint8_t *p = buf;
    for (size_t off = 0; off < len; off += access_width) {
        union { uint8_t b[8]; uint16_t u16; uint32_t u32; uint64_t u64; } t;
        axl_memcpy(t.b, p + off, access_width);
        int rc;
        switch (access_width) {
        case 1:  rc = axl_mem_phys_write8 (phys + off, t.b[0]); break;
        case 2:  rc = axl_mem_phys_write16(phys + off, t.u16);  break;
        case 4:  rc = axl_mem_phys_write32(phys + off, t.u32);  break;
        default: rc = axl_mem_phys_write64(phys + off, t.u64);  break;
        }
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

// ===========================================================================
// I/O port space — region map + access (GCD I/O-space map; x86 access)
// ===========================================================================

static AxlIoRegion *g_io_regions;
static size_t       g_io_count;
static bool         g_io_built;
static bool         g_io_cleanup_armed;

typedef struct {
    uint64_t        lo;
    uint64_t        hi;
    AxlIoRegionType type;
} IoIv;

static AxlIoRegionType
classify_gcd_io(EFI_GCD_IO_TYPE type)
{
    switch (type) {
    case EfiGcdIoTypeIo:          return AXL_IO_REGION_IO;
    case EfiGcdIoTypeNonExistent: return AXL_IO_REGION_UNMAPPED;
    default:                      return AXL_IO_REGION_RESERVED;  /* Reserved */
    }
}

static int
cmp_ioiv(const void *a, const void *b)
{
    uint64_t x = ((const IoIv *)a)->lo, y = ((const IoIv *)b)->lo;
    return (x > y) - (x < y);
}

static void
io_region_cleanup(void *unused)
{
    (void)unused;
    axl_free(g_io_regions);
    g_io_regions = NULL;
    g_io_count = 0;
    g_io_built = false;
}

static int
build_io_regions(void)
{
    DXE_SERVICES *ds = locate_dxe_services();
    if (ds == NULL || ds->GetIoSpaceMap == NULL) {
        /* No GCD I/O map (e.g. AArch64): an empty map — every port reads back
           as UNMAPPED via region_at synthesis. */
        axl_free(g_io_regions);
        g_io_regions = NULL;
        g_io_count = 0;
        g_io_built = true;
        if (!g_io_cleanup_armed) {
            (void)axl_atexit(io_region_cleanup, NULL);
            g_io_cleanup_armed = true;
        }
        return AXL_OK;
    }

    UINTN gn = 0;
    EFI_GCD_IO_SPACE_DESCRIPTOR *gmap = NULL;
    if (EFI_ERROR(ds->GetIoSpaceMap(&gn, &gmap))) {
        return AXL_ERR;
    }
    if (gmap == NULL) {
        gn = 0;   /* success with no descriptors: a legitimately empty space */
    }
    IoIv        *iv  = axl_calloc(gn ? gn : 1, sizeof(*iv));
    AxlIoRegion *out = axl_calloc(gn ? gn : 1, sizeof(*out));
    if (iv == NULL || out == NULL) {
        axl_free(iv);
        axl_free(out);
        if (gmap != NULL) {
            axl_bs()->FreePool(gmap);
        }
        return AXL_ERR;
    }
    size_t k = 0;
    for (UINTN i = 0; i < gn; i++) {
        if (gmap[i].Length == 0) {
            continue;
        }
        iv[k].lo   = gmap[i].BaseAddress;
        iv[k].hi   = gmap[i].BaseAddress + gmap[i].Length;
        iv[k].type = classify_gcd_io(gmap[i].GcdIoType);
        k++;
    }
    if (gmap != NULL) {
        axl_bs()->FreePool(gmap);
    }
    axl_qsort(iv, k, sizeof(*iv), cmp_ioiv);

    size_t tn = 0;
    for (size_t i = 0; i < k; i++) {
        if (tn > 0 && out[tn - 1].type == iv[i].type
            && (uint64_t)out[tn - 1].base + out[tn - 1].len == iv[i].lo) {
            out[tn - 1].len += (iv[i].hi - iv[i].lo);
        } else {
            out[tn].base = (uintptr_t)iv[i].lo;
            out[tn].len  = iv[i].hi - iv[i].lo;
            out[tn].type = iv[i].type;
            tn++;
        }
    }
    axl_free(iv);

    axl_free(g_io_regions);
    g_io_regions = out;
    g_io_count = tn;
    g_io_built = true;
    if (!g_io_cleanup_armed) {
        (void)axl_atexit(io_region_cleanup, NULL);
        g_io_cleanup_armed = true;
    }
    return AXL_OK;
}

static int
ensure_io_built(void)
{
    return g_io_built ? AXL_OK : build_io_regions();
}

static long
io_region_index_of(uint64_t port)
{
    for (size_t i = 0; i < g_io_count; i++) {
        if (port >= (uint64_t)g_io_regions[i].base
            && port < (uint64_t)g_io_regions[i].base + g_io_regions[i].len) {
            return (long)i;
        }
    }
    return -1;
}

int
axl_io_region_count(size_t *out_count)
{
    if (out_count == NULL || ensure_io_built() != AXL_OK) {
        return AXL_ERR;
    }
    *out_count = g_io_count;
    return AXL_OK;
}

int
axl_io_region_get(size_t index, AxlIoRegion *out)
{
    if (out == NULL || ensure_io_built() != AXL_OK || index >= g_io_count) {
        return AXL_ERR;
    }
    *out = g_io_regions[index];
    return AXL_OK;
}

int
axl_io_region_at(uintptr_t port, AxlIoRegion *out)
{
    if (out == NULL || ensure_io_built() != AXL_OK) {
        return AXL_ERR;
    }
    long i = io_region_index_of(port);
    if (i >= 0) {
        *out = g_io_regions[(size_t)i];
        return AXL_OK;
    }
    out->type = AXL_IO_REGION_UNMAPPED;
    if (g_io_count == 0 || port < (uint64_t)g_io_regions[0].base) {
        out->base = 0;
        out->len  = (g_io_count == 0) ? UINT64_MAX : (uint64_t)g_io_regions[0].base;
    } else {
        uint64_t top = (uint64_t)g_io_regions[g_io_count - 1].base
                     + g_io_regions[g_io_count - 1].len;
        out->base = (uintptr_t)top;
        out->len  = UINT64_MAX - top;
    }
    return AXL_OK;
}

int
axl_io_region_refresh(void)
{
    g_io_built = false;
    return build_io_regions();
}

bool
axl_io_is_accessible(uintptr_t port, size_t len, bool want_write)
{
    (void)want_write;   /* same policy for read and write */
    if (len == 0 || ensure_io_built() != AXL_OK) {
        return false;
    }
    uint64_t end = (uint64_t)port + len;
    if (end < (uint64_t)port) {
        return false;
    }
    uint64_t cur = port;
    while (cur < end) {
        long i = io_region_index_of(cur);
        if (i < 0 || g_io_regions[(size_t)i].type != AXL_IO_REGION_IO) {
            return false;
        }
        cur = (uint64_t)g_io_regions[(size_t)i].base + g_io_regions[(size_t)i].len;
    }
    return true;
}

#if defined(__x86_64__) || defined(__i386__)

// x86 port-space ceiling: 0x0000–0xFFFF (16-bit ports).
#define IO_PORT_SPACE_TOP  0x10000ULL

static bool
io_range_args_ok(size_t len, const void *buf, uint32_t w)
{
    if (buf == NULL || len == 0) {
        return false;
    }
    if (w != 1 && w != 2 && w != 4) {   /* no 64-bit port I/O */
        return false;
    }
    return (len % w) == 0;
}

#endif /* x86 */

int
axl_io_read_range(uintptr_t port, size_t len, void *buf, uint32_t access_width)
{
#if defined(__x86_64__) || defined(__i386__)
    if (!io_range_args_ok(len, buf, access_width)
        || (uint64_t)port + len > IO_PORT_SPACE_TOP
        || !axl_io_is_accessible(port, len, false)) {
        return AXL_ERR;
    }
    uint8_t *p = buf;
    for (size_t off = 0; off < len; off += access_width) {
        uint16_t pt = (uint16_t)(port + off);
        union { uint8_t b[4]; uint16_t u16; uint32_t u32; } t;
        switch (access_width) {
        case 1:  t.b[0] = axl_io_port_read8(pt);  break;
        case 2:  t.u16  = axl_io_port_read16(pt); break;
        default: t.u32  = axl_io_port_read32(pt); break;
        }
        axl_memcpy(p + off, t.b, access_width);
    }
    return AXL_OK;
#else
    (void)port; (void)len; (void)buf; (void)access_width;
    return AXL_ERR;   /* port I/O is x86-only */
#endif
}

int
axl_io_write_range(uintptr_t port, size_t len, const void *buf,
                   uint32_t access_width)
{
#if defined(__x86_64__) || defined(__i386__)
    if (!io_range_args_ok(len, buf, access_width)
        || (uint64_t)port + len > IO_PORT_SPACE_TOP
        || !axl_io_is_accessible(port, len, true)) {
        return AXL_ERR;
    }
    const uint8_t *p = buf;
    for (size_t off = 0; off < len; off += access_width) {
        uint16_t pt = (uint16_t)(port + off);
        union { uint8_t b[4]; uint16_t u16; uint32_t u32; } t;
        axl_memcpy(t.b, p + off, access_width);
        switch (access_width) {
        case 1:  axl_io_port_write8(pt, t.b[0]);  break;
        case 2:  axl_io_port_write16(pt, t.u16);  break;
        default: axl_io_port_write32(pt, t.u32);  break;
        }
    }
    return AXL_OK;
#else
    (void)port; (void)len; (void)buf; (void)access_width;
    return AXL_ERR;
#endif
}
