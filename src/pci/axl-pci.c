/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci.c
    PCI/PCIe configuration-space access via ECAM.

    On first use, locates MCFG via AxlAcpi to obtain the per-segment
    ECAM base addresses. All access is through identity-mapped MMIO
    — UEFI's flat physical memory map makes the math direct.

    Single-threaded; not reentrant. The MCFG cache, the enumeration
    cursor, and the segment-index pointer are all module-level
    statics. UEFI runs at a single TPL with cooperative event
    dispatch, so concurrent callers aren't a concern.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-pci.h>
#include <axl/axl-acpi.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("pci");

// ---------------------------------------------------------------------------
// Lazy-init of the MCFG segment table
// ---------------------------------------------------------------------------

static bool         init_done    = false;
static bool         init_failed  = false;
static AxlAcpiMcfg  cached_mcfg;

static int
ensure_init(
    void
    )
{
    if (init_done) {
        return 0;
    }
    if (init_failed) {
        return -1;
    }
    if (axl_acpi_read_mcfg(&cached_mcfg) != 0 || cached_mcfg.count == 0) {
        axl_warning("MCFG unavailable; PCI access disabled");
        init_failed = true;
        return -1;
    }
    axl_debug("MCFG: %zu segment(s)", cached_mcfg.count);
    init_done = true;
    return 0;
}

// ---------------------------------------------------------------------------
// ECAM address resolution
// ---------------------------------------------------------------------------

static volatile uint8_t *
ecam_ptr(
    AxlPciAddr  a,
    uint16_t    reg
    )
{
    if (a.dev > 31 || a.func > 7 || reg > 4095) {
        return NULL;
    }
    for (size_t i = 0; i < cached_mcfg.count; i++) {
        const AxlAcpiMcfgEntry *e = &cached_mcfg.segments[i];
        if (a.seg == e->segment
            && a.bus >= e->start_bus
            && a.bus <= e->end_bus) {
            uintptr_t base = (uintptr_t)e->base_addr;
            uintptr_t off = ((uintptr_t)(a.bus - e->start_bus) << 20)
                          | ((uintptr_t)a.dev  << 15)
                          | ((uintptr_t)a.func << 12)
                          | (uintptr_t)reg;
            return (volatile uint8_t *)(base + off);
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API — read / write
// ---------------------------------------------------------------------------

int
axl_pci_read_config_8(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint8_t    *out
    )
{
    if (out == NULL || ensure_init() != 0) {
        return -1;
    }
    volatile uint8_t *p = ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *out = *p;
    return 0;
}

int
axl_pci_read_config_16(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint16_t   *out
    )
{
    if (out == NULL || ensure_init() != 0) {
        return -1;
    }
    volatile uint16_t *p = (volatile uint16_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *out = *p;
    return 0;
}

int
axl_pci_read_config_32(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint32_t   *out
    )
{
    if (out == NULL || ensure_init() != 0) {
        return -1;
    }
    volatile uint32_t *p = (volatile uint32_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *out = *p;
    return 0;
}

int
axl_pci_write_config_8(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint8_t     value
    )
{
    if (ensure_init() != 0) {
        return -1;
    }
    volatile uint8_t *p = ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *p = value;
    return 0;
}

int
axl_pci_write_config_16(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint16_t    value
    )
{
    if (ensure_init() != 0) {
        return -1;
    }
    volatile uint16_t *p = (volatile uint16_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *p = value;
    return 0;
}

int
axl_pci_write_config_32(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint32_t    value
    )
{
    if (ensure_init() != 0) {
        return -1;
    }
    volatile uint32_t *p = (volatile uint32_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return -1;
    }
    *p = value;
    return 0;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

#define PCI_VENDOR_ID_OFFSET   0x00
#define PCI_DEVICE_ID_OFFSET   0x02
#define PCI_HEADER_TYPE_OFFSET 0x0E
#define PCI_STATUS_OFFSET      0x06
#define PCI_CAP_PTR_OFFSET     0x34
#define PCI_CLASSCODE_OFFSET   0x09  /* 3 bytes: progif, subclass, baseclass */

#define PCI_HEADER_MULTIFUNC   0x80
#define PCI_STATUS_CAP_LIST    0x10

#define PCIE_FIRST_EXT_CAP     0x100u
#define PCIE_EXT_CAP_END       0xFFFFu  /* cap_id when no caps present */

static bool
function_present(
    AxlPciAddr  a
    )
{
    uint16_t vid;
    if (axl_pci_read_config_16(a, PCI_VENDOR_ID_OFFSET, &vid) != 0) {
        return false;
    }
    return vid != 0xFFFF;
}

/* Static cursor returned to callers. Reused across calls. The
   segment index is cached alongside so we never re-scan the MCFG
   table to find where the cursor lives, and `pending_skip_funcs`
   carries the multi-function bit decision from one return to the
   next call's advance step. */
static AxlPciAddr  cursor;
static bool        cursor_valid;
static size_t      cursor_seg_idx;
static bool        pending_skip_funcs;

/* Skip cursor.func from 1 → 8 so the next advance lands on the
   next dev. Used when func 0 of a single-function device was just
   returned. */
static void
skip_to_next_dev(
    void
    )
{
    cursor.func = 0;
    cursor.dev++;
}

AxlPciAddr *
axl_pci_next(
    AxlPciAddr  *prev
    )
{
    if (ensure_init() != 0) {
        return NULL;
    }

    /* prev is treated as a one-bit "continue or restart" signal —
       the only valid non-NULL value is the previous return value
       (which is &cursor). Any other pointer or NULL restarts the
       walk; the caller never owns cursor's storage. */
    if (prev == NULL || prev != &cursor || !cursor_valid) {
        cursor_seg_idx     = 0;
        pending_skip_funcs = false;
        const AxlAcpiMcfgEntry *e = &cached_mcfg.segments[cursor_seg_idx];
        cursor.seg  = e->segment;
        cursor.bus  = e->start_bus;
        cursor.dev  = 0;
        cursor.func = 0;
        cursor_valid = true;
    } else {
        /* Advance past the previous match. If the previous return
           was func 0 of a single-function device, jump straight to
           the next dev rather than crawling funcs 1..7. */
        if (pending_skip_funcs) {
            pending_skip_funcs = false;
            skip_to_next_dev();
        } else {
            cursor.func++;
        }

        /* Normalize overflow: func > 7 → next dev; dev > 31 → next
           bus; bus past end → next segment. */
        if (cursor.func > 7) {
            cursor.func = 0;
            cursor.dev++;
        }
        while (1) {
            if (cursor.dev <= 31) {
                break;
            }
            cursor.dev = 0;
            const AxlAcpiMcfgEntry *e =
                &cached_mcfg.segments[cursor_seg_idx];
            if (cursor.bus < e->end_bus) {
                cursor.bus++;
                break;
            }
            cursor_seg_idx++;
            if (cursor_seg_idx >= cached_mcfg.count) {
                cursor_valid = false;
                return NULL;
            }
            const AxlAcpiMcfgEntry *ne =
                &cached_mcfg.segments[cursor_seg_idx];
            cursor.seg = ne->segment;
            cursor.bus = ne->start_bus;
            /* dev/func already 0 from the start of this loop */
        }
    }

    /* Walk forward until a present function is found. */
    for (;;) {
        if (function_present(cursor)) {
            /* On func 0, read the header-type byte and remember
               whether this is multi-function. The caller gets func 0
               first; the next call's advance honours the flag. */
            if (cursor.func == 0) {
                uint8_t htype;
                pending_skip_funcs =
                    (axl_pci_read_config_8(cursor,
                        PCI_HEADER_TYPE_OFFSET, &htype) == 0)
                    && ((htype & PCI_HEADER_MULTIFUNC) == 0);
            } else {
                pending_skip_funcs = false;
            }
            return &cursor;
        }

        /* Slot empty. If we were on func 0, the whole dev is absent;
           skip to next dev. Otherwise advance one func. */
        if (cursor.func == 0) {
            cursor.dev++;
        } else {
            cursor.func++;
            if (cursor.func <= 7) {
                continue;
            }
            cursor.func = 0;
            cursor.dev++;
        }

        while (cursor.dev > 31) {
            cursor.dev = 0;
            const AxlAcpiMcfgEntry *e =
                &cached_mcfg.segments[cursor_seg_idx];
            if (cursor.bus < e->end_bus) {
                cursor.bus++;
                break;
            }
            cursor_seg_idx++;
            if (cursor_seg_idx >= cached_mcfg.count) {
                cursor_valid = false;
                return NULL;
            }
            const AxlAcpiMcfgEntry *ne =
                &cached_mcfg.segments[cursor_seg_idx];
            cursor.seg = ne->segment;
            cursor.bus = ne->start_bus;
            cursor.func = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// find-by-* helpers
// ---------------------------------------------------------------------------

int
axl_pci_find_by_vid_did(
    uint16_t     vid,
    uint16_t     did,
    uint16_t     nth,
    AxlPciAddr  *out
    )
{
    if (out == NULL) {
        return -1;
    }
    AxlPciAddr *p = NULL;
    uint16_t    matches = 0;
    while ((p = axl_pci_next(p)) != NULL) {
        uint16_t cur_vid;
        uint16_t cur_did;
        if (axl_pci_read_config_16(*p, PCI_VENDOR_ID_OFFSET, &cur_vid) != 0
            || axl_pci_read_config_16(*p, PCI_DEVICE_ID_OFFSET, &cur_did) != 0) {
            continue;
        }
        if (cur_vid == vid && cur_did == did) {
            if (matches == nth) {
                *out = *p;
                return 0;
            }
            matches++;
        }
    }
    return -1;
}

int
axl_pci_find_by_class(
    uint32_t     class24,
    uint16_t     nth,
    AxlPciAddr  *out
    )
{
    if (out == NULL) {
        return -1;
    }
    AxlPciAddr *p = NULL;
    uint16_t    matches = 0;
    while ((p = axl_pci_next(p)) != NULL) {
        /* Bytes at 0x09..0x0B: prog_if, subclass, base_class. Read
           the 32-bit dword starting at 0x08 (revision_id is 0x08)
           and shift; equivalently a 24-bit triple from 0x09. */
        uint32_t reg08;
        if (axl_pci_read_config_32(*p, 0x08, &reg08) != 0) {
            continue;
        }
        uint32_t cur_class24 = (reg08 >> 8) & 0xFFFFFFu;
        if (class24 == 0xFFFFFFu || cur_class24 == class24) {
            if (matches == nth) {
                *out = *p;
                return 0;
            }
            matches++;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Capability walk (legacy)
// ---------------------------------------------------------------------------

int
axl_pci_cap_next(
    AxlPciAddr  addr,
    uint16_t    prev_off,
    uint16_t   *out_off,
    uint16_t   *out_id
    )
{
    if (out_off == NULL || out_id == NULL || ensure_init() != 0) {
        return -1;
    }

    uint16_t next;
    if (prev_off == 0) {
        uint16_t status;
        if (axl_pci_read_config_16(addr, PCI_STATUS_OFFSET, &status) != 0) {
            return -1;
        }
        if ((status & PCI_STATUS_CAP_LIST) == 0) {
            return -1;
        }
        uint8_t cap_ptr;
        if (axl_pci_read_config_8(addr, PCI_CAP_PTR_OFFSET, &cap_ptr) != 0) {
            return -1;
        }
        next = (uint16_t)(cap_ptr & 0xFC);
    } else {
        uint8_t np;
        if (axl_pci_read_config_8(addr, (uint16_t)(prev_off + 1), &np) != 0) {
            return -1;
        }
        next = (uint16_t)(np & 0xFC);
    }

    /* End of chain on 0; per PCI spec valid cap pointers live in
       0x40..0xFC, so anything outside that range terminates the
       walk too — guards against malformed chains that would
       otherwise loop (e.g. a cap whose `next` points back into the
       header). */
    if (next < 0x40 || next > 0xFC) {
        return -1;
    }
    uint8_t cap_id;
    if (axl_pci_read_config_8(addr, next, &cap_id) != 0) {
        return -1;
    }
    *out_off = next;
    *out_id  = cap_id;
    return 0;
}

// ---------------------------------------------------------------------------
// Extended capability walk (PCIe)
// ---------------------------------------------------------------------------

int
axl_pci_ext_cap_next(
    AxlPciAddr  addr,
    uint16_t    prev_off,
    uint16_t   *out_off,
    uint16_t   *out_id
    )
{
    if (out_off == NULL || out_id == NULL || ensure_init() != 0) {
        return -1;
    }

    uint16_t off = (prev_off == 0) ? PCIE_FIRST_EXT_CAP : 0;

    if (prev_off != 0) {
        uint32_t hdr;
        if (axl_pci_read_config_32(addr, prev_off, &hdr) != 0) {
            return -1;
        }
        off = (uint16_t)((hdr >> 20) & 0xFFFu);
    }
    if (off == 0) {
        return -1;
    }

    uint32_t hdr;
    if (axl_pci_read_config_32(addr, off, &hdr) != 0) {
        return -1;
    }
    uint16_t cap_id = (uint16_t)(hdr & 0xFFFFu);
    if (cap_id == PCIE_EXT_CAP_END || cap_id == 0) {
        return -1;
    }
    *out_off = off;
    *out_id  = cap_id;
    return 0;
}

// ---------------------------------------------------------------------------
// VPD reader
// ---------------------------------------------------------------------------

#define PCI_CAP_ID_VPD     0x03
#define VPD_ADDR_OFFSET    0x02   /* relative to cap base */
#define VPD_DATA_OFFSET    0x04
#define VPD_F_BIT          0x8000u

#define VPD_TAG_ID_STRING  0x82
#define VPD_TAG_RO         0x90
#define VPD_TAG_RW         0x91
#define VPD_TAG_END        0x78

static int
find_vpd_cap(
    AxlPciAddr  addr,
    uint16_t   *out_cap_off
    )
{
    AxlPciAddr a = addr;
    uint16_t   off = 0;
    uint16_t   id;
    while (axl_pci_cap_next(a, off, &off, &id) == 0) {
        if (id == PCI_CAP_ID_VPD) {
            *out_cap_off = off;
            return 0;
        }
    }
    return -1;
}

/* Trigger a read of the 32-bit VPD word starting at @vpd_addr.
   Returns 0 on success with the data in @out, -1 on timeout. */
static int
vpd_read32(
    AxlPciAddr  addr,
    uint16_t    cap_off,
    uint16_t    vpd_addr,
    uint32_t   *out
    )
{
    uint16_t addr_reg = (uint16_t)(cap_off + VPD_ADDR_OFFSET);
    uint16_t data_reg = (uint16_t)(cap_off + VPD_DATA_OFFSET);

    /* Write address with F=0 to start a read. */
    if (axl_pci_write_config_16(addr, addr_reg, vpd_addr) != 0) {
        return -1;
    }
    /* Spin on F bit. PCI 3.0 says completion is "essentially
       instantaneous"; the budget here is generous for slow VPD
       implementations. */
    for (int i = 0; i < 10000; i++) {
        uint16_t status;
        if (axl_pci_read_config_16(addr, addr_reg, &status) != 0) {
            return -1;
        }
        if (status & VPD_F_BIT) {
            return axl_pci_read_config_32(addr, data_reg, out);
        }
    }
    return -1;
}

static int
vpd_read_bytes(
    AxlPciAddr  addr,
    uint16_t    cap_off,
    uint16_t    start,
    uint8_t    *buf,
    size_t      len
    )
{
    uint16_t off = start;
    size_t   wrote = 0;

    /* Align reads to 32-bit boundaries — VPD data window is dword-only. */
    while (wrote < len) {
        uint16_t aligned = (uint16_t)(off & ~0x3u);
        uint32_t word;
        if (vpd_read32(addr, cap_off, aligned, &word) != 0) {
            return -1;
        }
        const uint8_t *bytes = (const uint8_t *)&word;
        for (size_t b = (off - aligned); b < 4 && wrote < len; b++) {
            buf[wrote++] = bytes[b];
            off++;
        }
    }
    return 0;
}

int
axl_pci_vpd_read(
    AxlPciAddr   addr,
    const char   keyword[2],
    uint8_t     *buf,
    size_t       buflen,
    size_t      *out_len
    )
{
    if (keyword == NULL || out_len == NULL || ensure_init() != 0) {
        return -1;
    }
    *out_len = 0;

    uint16_t cap_off;
    if (find_vpd_cap(addr, &cap_off) != 0) {
        return -1;
    }

    /* Walk VPD resource tags starting at offset 0. The on-device
       VPD area is bounded but malformed firmware can fail to emit
       the End tag; the bytes-consumed budget guarantees we exit
       even then. 32 KiB is well above any real VPD size. */
    const uint16_t  budget    = 0x8000;
    uint16_t        off       = 0;
    uint16_t        consumed  = 0;

    while (consumed < budget) {
        uint8_t tag;
        if (vpd_read_bytes(addr, cap_off, off, &tag, 1) != 0) {
            return -1;
        }
        off++;
        consumed++;

        if (tag == VPD_TAG_END) {
            return -1;
        }

        /* Large resource tags (high bit set) carry a 16-bit length
           in the next two bytes; small tags carry their length in
           the bottom 3 bits and a 4-bit name in bits 6:3. The
           comparison at line below uses large-tag constants
           (VPD_TAG_RO=0x90, VPD_TAG_RW=0x91), so small-tag entries
           are correctly ignored — RO/RW are large-tag-only. */
        uint16_t len;
        if (tag & 0x80) {
            uint8_t lenbuf[2];
            if (vpd_read_bytes(addr, cap_off, off, lenbuf, 2) != 0) {
                return -1;
            }
            off += 2;
            consumed = (uint16_t)(consumed + 2);
            len = (uint16_t)(lenbuf[0] | ((uint16_t)lenbuf[1] << 8));
        } else {
            len = (uint16_t)(tag & 0x07);
            tag &= 0xF8;
        }

        if (tag != VPD_TAG_RO && tag != VPD_TAG_RW) {
            /* Skip ID String and any unknown tags */
            off = (uint16_t)(off + len);
            consumed = (uint16_t)(consumed + len);
            continue;
        }

        /* RO/RW resource: walk keyword entries within. */
        uint16_t inner_end = (uint16_t)(off + len);
        while (off < inner_end) {
            uint8_t ent[3];
            if (vpd_read_bytes(addr, cap_off, off, ent, 3) != 0) {
                return -1;
            }
            uint16_t kdata = (uint16_t)(off + 3);
            uint8_t  klen  = ent[2];
            uint16_t next  = (uint16_t)(kdata + klen);

            /* A malformed entry whose length spills past the
               resource's end is treated as fatal — there's no
               clean re-sync to the next tag. */
            if (next > inner_end) {
                return -1;
            }

            if (axl_memcmp(ent, keyword, 2) == 0) {
                *out_len = klen;
                size_t to_copy = klen < buflen ? klen : buflen;
                if (to_copy > 0
                    && vpd_read_bytes(addr, cap_off, kdata, buf, to_copy) != 0) {
                    return -1;
                }
                return 0;
            }
            off = next;
        }
        consumed = (uint16_t)(consumed + len);
    }
    return -1;
}
