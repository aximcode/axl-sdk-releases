/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-cap.c
    PCI capability walking — legacy capability-list at offset 0x34
    and the PCIe extended capability list at offset 0x100 — plus the
    capability-ID name lookup tables and the VPD reader (which walks
    the legacy cap list to find the VPD capability and then reads
    via its address/data window).

    Split out of axl-pci.c per docs/Style-Cleanup-Plan.md Pass C.
    Self-contained: owns the pci_cap_table / pci_ext_cap_table name
    tables and the VPD-protocol macros. Consumes axl_pci_read* /
    write* via the public <axl/axl-pci.h>.
**/

#include "axl-pci-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("pci");

// ---------------------------------------------------------------------------
// Capability ID -> name (legacy + PCIe extended)
// Tables sourced from the PCI Local Bus Spec and PCIe Base Spec
// capability-ID assignments. Linear search — tables are small and
// the lookup is human-facing print only.
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t      id;
    const char  *name;
} PciCapEntry;

typedef struct {
    uint16_t     id;
    const char  *name;
} PciExtCapEntry;

static const PciCapEntry pci_cap_table[] = {
    { 0x01, "Power Management" },
    { 0x02, "AGP" },
    { 0x03, "Vital Product Data" },
    { 0x04, "Slot Identification" },
    { 0x05, "MSI" },
    { 0x06, "CompactPCI Hot Swap" },
    { 0x07, "PCI-X" },
    { 0x08, "HyperTransport" },
    { 0x09, "Vendor-Specific" },
    { 0x0A, "Debug Port" },
    { 0x0B, "CompactPCI Central Resource Control" },
    { 0x0C, "PCI Hot-Plug" },
    { 0x0D, "PCI Bridge Subsystem Vendor ID" },
    { 0x0E, "AGP 8x" },
    { 0x0F, "Secure Device" },
    { 0x10, "PCI Express" },
    { 0x11, "MSI-X" },
    { 0x12, "SATA Data/Index Configuration" },
    { 0x13, "Advanced Features" },
    { 0x14, "Enhanced Allocation" },
    { 0x15, "Flattening Portal Bridge" },
};

static const PciExtCapEntry pci_ext_cap_table[] = {
    { 0x0001, "Advanced Error Reporting" },
    { 0x0002, "Virtual Channel" },
    { 0x0003, "Device Serial Number" },
    { 0x0004, "Power Budgeting" },
    { 0x0005, "Root Complex Link Declaration" },
    { 0x0006, "Root Complex Internal Link Control" },
    { 0x0007, "Root Complex Event Collector Endpoint Association" },
    { 0x0008, "Multi-Function Virtual Channel" },
    { 0x0009, "Virtual Channel (MFVC)" },
    { 0x000A, "Root Complex Register Block" },
    { 0x000B, "Vendor-Specific Extended" },
    { 0x000C, "Configuration Access Correlation" },
    { 0x000D, "Access Control Services" },
    { 0x000E, "Alternative Routing-ID Interpretation" },
    { 0x000F, "Address Translation Services" },
    { 0x0010, "Single Root I/O Virtualization" },
    { 0x0011, "Multi Root I/O Virtualization" },
    { 0x0012, "Multicast" },
    { 0x0013, "Page Request Interface" },
    { 0x0014, "Reserved for AMD" },
    { 0x0015, "Resizable BAR" },
    { 0x0016, "Dynamic Power Allocation" },
    { 0x0017, "TPH Requester" },
    { 0x0018, "Latency Tolerance Reporting" },
    { 0x0019, "Secondary PCI Express" },
    { 0x001A, "Protocol Multiplexing" },
    { 0x001B, "Process Address Space ID" },
    { 0x001C, "LN Requester" },
    { 0x001D, "Downstream Port Containment" },
    { 0x001E, "L1 PM Substates" },
    { 0x001F, "Precision Time Measurement" },
    { 0x0020, "PCI Express over M-PHY" },
    { 0x0021, "FRS Queueing" },
    { 0x0022, "Readiness Time Reporting" },
    { 0x0023, "Designated Vendor-Specific" },
    { 0x0024, "VF Resizable BAR" },
    { 0x0025, "Data Link Feature" },
    { 0x0026, "Physical Layer 16.0 GT/s" },
    { 0x0027, "Lane Margining at the Receiver" },
    { 0x0028, "Hierarchy ID" },
    { 0x0029, "Native PCIe Enclosure Management" },
    { 0x002A, "Physical Layer 32.0 GT/s" },
    { 0x002B, "Alternate Protocol" },
    { 0x002C, "System Firmware Intermediary" },
    { 0x002D, "Shadow Functions" },
    { 0x002E, "Data Object Exchange" },
    { 0x002F, "Device 3" },
    { 0x0030, "Integrity and Data Encryption" },
};

const char *
axl_pci_cap_id_str(uint8_t cap_id)
{
    for (size_t i = 0; i < sizeof(pci_cap_table) / sizeof(pci_cap_table[0]); i++) {
        if (pci_cap_table[i].id == cap_id) {
            return pci_cap_table[i].name;
        }
    }
    return "<unknown>";
}

const char *
axl_pci_ext_cap_id_str(uint16_t cap_id)
{
    for (size_t i = 0; i < sizeof(pci_ext_cap_table) / sizeof(pci_ext_cap_table[0]); i++) {
        if (pci_ext_cap_table[i].id == cap_id) {
            return pci_ext_cap_table[i].name;
        }
    }
    return "<unknown>";
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
    if (out_off == NULL || out_id == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }

    uint16_t next;
    if (prev_off == 0) {
        /* Absent-function check at walk entry. ECAM reads to a
           nonexistent BDF return all-1s; without this guard the
           status register reads as 0xFFFF (cap-list bit set), the
           cap pointer reads as 0xFC, and the cap header at 0xFC
           reads as 0xFFFF whose `next` byte is 0xFF — the iter
           then re-reads the same offset forever. Surfaced on aa64
           QEMU virt where 0:1f.0 doesn't exist. */
        uint16_t vid;
        if (axl_pci_read_config_16(addr, PCI_VENDOR_ID_OFFSET, &vid) != 0
            || vid == 0xFFFF)
        {
            return AXL_ERR;
        }
        uint16_t status;
        if (axl_pci_read_config_16(addr, PCI_STATUS_OFFSET, &status) != 0) {
            return AXL_ERR;
        }
        if ((status & PCI_STATUS_CAP_LIST) == 0) {
            return AXL_ERR;
        }
        uint8_t cap_ptr;
        if (axl_pci_read_config_8(addr, PCI_CAP_PTR_OFFSET, &cap_ptr) != AXL_OK) {
            return AXL_ERR;
        }
        next = (uint16_t)(cap_ptr & 0xFC);
    } else {
        uint8_t np;
        if (axl_pci_read_config_8(addr, (uint16_t)(prev_off + 1), &np) != AXL_OK) {
            return AXL_ERR;
        }
        next = (uint16_t)(np & 0xFC);
    }

    /* End of chain on 0; per PCI spec valid cap pointers live in
       0x40..0xFC, so anything outside that range terminates the
       walk too — guards against malformed chains that would
       otherwise loop (e.g. a cap whose `next` points back into the
       header). */
    if (next < 0x40 || next > 0xFC) {
        return AXL_ERR;
    }
    /* Forward-progress guard. Spec doesn't formally require monotonic
       cap offsets, but no real device chains backwards; a back-pointer
       (next <= prev_off) is malformed and would loop the iter. Also
       catches the all-1s self-loop (next == prev_off == 0xFC) on a
       device that becomes absent mid-walk. */
    if (next <= prev_off) {
        return AXL_ERR;
    }
    uint8_t cap_id;
    if (axl_pci_read_config_8(addr, next, &cap_id) != AXL_OK) {
        return AXL_ERR;
    }
    *out_off = next;
    *out_id  = cap_id;
    return AXL_OK;
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
    if (out_off == NULL || out_id == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }

    uint16_t off = (prev_off == 0) ? PCIE_FIRST_EXT_CAP : 0;

    if (prev_off != 0) {
        uint32_t hdr;
        if (axl_pci_read_config_32(addr, prev_off, &hdr) != 0) {
            return AXL_ERR;
        }
        off = (uint16_t)((hdr >> 20) & 0xFFFu);
    }
    if (off == 0) {
        return AXL_ERR;
    }
    /* Forward-progress guard, mirror of the legacy-cap walk. A next
       offset that doesn't move forward is malformed and would loop;
       valid ext-cap offsets live in 0x100..0xFFC. The absent-device
       all-1s case still terminates via the cap_id == PCIE_EXT_CAP_END
       check below, but this catches mid-walk cycles too. */
    if (prev_off != 0 && off <= prev_off) {
        return AXL_ERR;
    }

    uint32_t hdr;
    if (axl_pci_read_config_32(addr, off, &hdr) != 0) {
        return AXL_ERR;
    }
    uint16_t cap_id = (uint16_t)(hdr & 0xFFFFu);
    if (cap_id == PCIE_EXT_CAP_END || cap_id == 0) {
        return AXL_ERR;
    }
    *out_off = off;
    *out_id  = cap_id;
    return AXL_OK;
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
    while (axl_pci_cap_next(a, off, &off, &id) == AXL_OK) {
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

/* Inner VPD walker — produces every keyword and dispatches to a
   callback. Used by both axl_pci_vpd_iter (the public iter) and
   axl_pci_vpd_read (which uses a "stop on match" callback).

   Returns 0 if the walk completed without the callback stopping it,
   the callback's first non-zero return if it stopped early, or
   -1 on bus error / malformed VPD. */
typedef int (*VpdWalkCb)(const char keyword[2],
                         const uint8_t *data, size_t len, void *ctx);

static int
vpd_walk(AxlPciAddr addr, VpdWalkCb cb, void *ctx)
{
    if (cb == NULL || axl_pci_ensure_init() != 0) {
        return -1;
    }

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
            return 0;
        }

        /* Large resource tags (high bit set) carry a 16-bit length
           in the next two bytes; small tags carry their length in
           the bottom 3 bits and a 4-bit name in bits 6:3. The
           comparison below uses large-tag constants
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

            /* VPD keyword data length is 1 byte, so klen <= 255 —
               this stack buffer is always sufficient. */
            uint8_t kbuf[256];
            if (klen > 0 &&
                vpd_read_bytes(addr, cap_off, kdata, kbuf, klen) != 0)
            {
                return -1;
            }
            int rc = cb((const char *)ent, kbuf, klen, ctx);
            if (rc != 0) {
                return rc;
            }
            off = next;
        }
        consumed = (uint16_t)(consumed + len);
    }
    /* Budget exhausted without seeing END — treat as malformed. */
    return -1;
}

/* Context + callback for axl_pci_vpd_read's "find one keyword" mode. */
typedef struct {
    const char *want;       /* 2-char keyword the caller asked for */
    uint8_t    *buf;        /* destination */
    size_t      buflen;     /* destination capacity */
    size_t     *out_len;    /* receives actual on-device length */
    bool        found;
} VpdFindOneCtx;

static int
vpd_find_one_cb(const char keyword[2],
                const uint8_t *data, size_t len, void *vctx)
{
    VpdFindOneCtx *c = (VpdFindOneCtx *)vctx;
    if (axl_memcmp(keyword, c->want, 2) != 0) {
        return 0;
    }
    *c->out_len = len;
    size_t to_copy = (len < c->buflen) ? len : c->buflen;
    if (to_copy > 0) {
        axl_memcpy(c->buf, data, to_copy);
    }
    c->found = true;
    return 1;  /* stop walk */
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
    if (keyword == NULL || out_len == NULL) {
        return AXL_ERR;
    }
    *out_len = 0;

    VpdFindOneCtx ctx = {
        .want    = keyword,
        .buf     = buf,
        .buflen  = buflen,
        .out_len = out_len,
        .found   = false,
    };
    int rc = vpd_walk(addr, vpd_find_one_cb, &ctx);
    if (rc < 0) {
        /* bus error or malformed VPD */
        return AXL_ERR;
    }
    return ctx.found ? AXL_OK : AXL_ERR;
}

int
axl_pci_vpd_iter(
    AxlPciAddr   addr,
    int        (*cb)(const char keyword[2],
                     const uint8_t *data, size_t len, void *ctx),
    void        *ctx
    )
{
    if (cb == NULL) {
        return AXL_ERR;
    }
    return vpd_walk(addr, cb, ctx);
}
