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
#include "../data/axl-class-fmt.h"
#include "axl-pci-class-internal.h"
#include "axl-pci-internal.h"
#include <axl/axl-acpi.h>
#include <axl/axl-array.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("pci");

// ---------------------------------------------------------------------------
// Lazy-init of the MCFG segment table
// ---------------------------------------------------------------------------

static bool         init_done    = false;
static bool         init_failed  = false;
static AxlAcpiMcfg  cached_mcfg;

int
axl_pci_ensure_init(
    void
    )
{
    if (init_done) {
        return 0;
    }
    if (init_failed) {
        return -1;
    }
    if (axl_acpi_read_mcfg(&cached_mcfg) != AXL_OK || cached_mcfg.count == 0) {
        axl_warning("MCFG unavailable; PCI access disabled");
        init_failed = true;
        return -1;
    }
    axl_debug("MCFG: %zu segment(s)", cached_mcfg.count);
    /* Per-segment detail at debug level — captures the base address
       and bus range so platform-specific oddities (an MCFG entry
       that's declared but unmapped, narrow bus ranges, segments
       reported out of order) are visible without rebuilding. */
    for (size_t i = 0; i < cached_mcfg.count; i++) {
        const AxlAcpiMcfgEntry *e = &cached_mcfg.segments[i];
        axl_debug("MCFG[%zu]: seg=0x%04x base=0x%llx bus=0x%02x..0x%02x",
                  i, (unsigned)e->segment,
                  (unsigned long long)e->base_addr,
                  (unsigned)e->start_bus, (unsigned)e->end_bus);
    }
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
    if (out == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint8_t *p = ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *out = *p;
    return AXL_OK;
}

int
axl_pci_read_config_16(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint16_t   *out
    )
{
    if (out == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint16_t *p = (volatile uint16_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *out = *p;
    return AXL_OK;
}

int
axl_pci_read_config_32(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint32_t   *out
    )
{
    if (out == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint32_t *p = (volatile uint32_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *out = *p;
    return AXL_OK;
}

int
axl_pci_write_config_8(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint8_t     value
    )
{
    if (axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint8_t *p = ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *p = value;
    return AXL_OK;
}

int
axl_pci_write_config_16(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint16_t    value
    )
{
    if (axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint16_t *p = (volatile uint16_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *p = value;
    return AXL_OK;
}

int
axl_pci_write_config_32(
    AxlPciAddr  addr,
    uint16_t    reg,
    uint32_t    value
    )
{
    if (axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }
    volatile uint32_t *p = (volatile uint32_t *)ecam_ptr(addr, reg);
    if (p == NULL) {
        return AXL_ERR;
    }
    *p = value;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Address parse / format
// ---------------------------------------------------------------------------

/* Parse 1..max_chars hex digits into *out (uint32_t), returning the
   consumed count or -1 on no digits / overflow. Thin wrapper around
   axl_hex_parse_u64 with a 32-bit-overflow check on top. */
static int
parse_hex_field(const char *s, int max_chars, uint32_t *out)
{
    uint64_t v = 0;
    int      n = axl_hex_parse_u64(s, (size_t)max_chars, &v);
    if (n < 0 || v > 0xFFFFFFFFu) {
        return -1;
    }
    *out = (uint32_t)v;
    return n;
}

int
axl_pci_addr_parse(const char *s, AxlPciAddr *out)
{
    if (s == NULL || out == NULL) {
        return AXL_ERR;
    }

    /* Walk the input collecting (hex field, separator) pairs. Both
       accepted forms have a `.` as the final separator and `:` as
       all the others, so the only thing that varies is the number
       of `:`-separated leading fields (1 = bus only, 2 = seg+bus). */
    uint32_t    parts[4] = { 0, 0, 0, 0 };
    int         n_parts  = 0;
    const char *p        = s;

    while (n_parts < 4) {
        int n = parse_hex_field(p, 5, &parts[n_parts]);
        if (n < 0) return AXL_ERR;
        p += n;
        n_parts++;

        if (*p == '\0') break;
        if (*p != ':' && *p != '.') return AXL_ERR;

        /* Final field must be preceded by '.', all others by ':'. */
        bool is_final_sep = (*p == '.');
        p++;
        if (is_final_sep) {
            /* func is next; one more parse then must hit EOF. Reject
               an over-long address (>4 fields, e.g. "1:2:3:4.5")
               before indexing — n_parts is already 4 here, so the
               write would overflow parts[]. */
            if (n_parts >= 4) {
                return AXL_ERR;
            }
            int fn = parse_hex_field(p, 5, &parts[n_parts]);
            if (fn < 0) return AXL_ERR;
            p += fn;
            n_parts++;
            break;
        }
    }
    if (*p != '\0') return AXL_ERR;

    uint32_t seg, bus, dev, func;
    if (n_parts == 4) {
        /* seg:bus:dev.func */
        seg = parts[0]; bus = parts[1]; dev = parts[2]; func = parts[3];
    } else if (n_parts == 3) {
        /* bus:dev.func — segment defaults to 0 */
        seg = 0;        bus = parts[0]; dev = parts[1]; func = parts[2];
    } else {
        return AXL_ERR;
    }

    if (seg > 0xFFFFu || bus > 0xFFu || dev > 0x1Fu || func > 0x07u) {
        return AXL_ERR;
    }
    out->seg  = (uint16_t)seg;
    out->bus  = (uint8_t)bus;
    out->dev  = (uint8_t)dev;
    out->func = (uint8_t)func;
    return AXL_OK;
}

int
axl_pci_addr_format(AxlPciAddr addr, char *buf, size_t buflen)
{
    /* Canonical form is exactly 12 bytes ("SSSS:BB:DD.F") plus a
       NUL terminator. AXL_PCI_ADDR_STR_MAX is the recommended
       allocation; the actual minimum we'll accept is one less. */
    if (buf == NULL || buflen < 13) {
        return AXL_ERR;
    }
    /* Manual hex formatting — no axl_snprintf needed and avoids
       pulling printf into builds that don't otherwise use it. */
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    /* SSSS — 4 hex digits */
    buf[pos++] = hex[(addr.seg >> 12) & 0xF];
    buf[pos++] = hex[(addr.seg >>  8) & 0xF];
    buf[pos++] = hex[(addr.seg >>  4) & 0xF];
    buf[pos++] = hex[ addr.seg        & 0xF];
    buf[pos++] = ':';
    /* BB — 2 hex digits */
    buf[pos++] = hex[(addr.bus >> 4) & 0xF];
    buf[pos++] = hex[ addr.bus       & 0xF];
    buf[pos++] = ':';
    /* DD — 2 hex digits */
    buf[pos++] = hex[(addr.dev >> 4) & 0xF];
    buf[pos++] = hex[ addr.dev       & 0xF];
    buf[pos++] = '.';
    /* F — 1 hex digit */
    buf[pos++] = hex[addr.func & 0xF];
    buf[pos]   = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
// Common header reads (boilerplate-killer wrappers)
// ---------------------------------------------------------------------------

int
axl_pci_get_vid_did(AxlPciAddr addr, uint16_t *vid, uint16_t *did)
{
    if (vid == NULL || did == NULL) {
        return AXL_ERR;
    }
    uint16_t v;
    if (axl_pci_read_config_16(addr, 0x00, &v) != 0) {
        return AXL_ERR;
    }
    if (v == 0xFFFF) {
        /* Function absent — caller doesn't have to special-case this. */
        return AXL_ERR;
    }
    if (axl_pci_read_config_16(addr, 0x02, did) != 0) {
        return AXL_ERR;
    }
    *vid = v;
    return AXL_OK;
}

int
axl_pci_get_class_code(AxlPciAddr addr, uint32_t *class_code)
{
    if (class_code == NULL) {
        return AXL_ERR;
    }
    uint8_t prog_if, sub, base;
    if (axl_pci_read_config_8(addr, 0x09, &prog_if) != AXL_OK ||
        axl_pci_read_config_8(addr, 0x0A, &sub)     != AXL_OK ||
        axl_pci_read_config_8(addr, 0x0B, &base)    != AXL_OK)
    {
        return AXL_ERR;
    }
    *class_code = ((uint32_t)base << 16) | ((uint32_t)sub << 8) | prog_if;
    return AXL_OK;
}

int
axl_pci_get_header_type(AxlPciAddr        addr,
                        AxlPciHeaderType *type,
                        bool             *is_multi_function)
{
    /* Absent-function precheck: raw config-space reads complete
       successfully even on missing slots (the bus returns 0xFFFF
       for every byte). Mirror axl_pci_get_vid_did's posture so
       callers don't have to disambiguate "header type 0x7F multi-
       func" from "function not present." */
    uint16_t v;
    if (axl_pci_read_config_16(addr, 0x00, &v) != 0 || v == 0xFFFF) {
        return AXL_ERR;
    }
    uint8_t htype;
    if (axl_pci_read_config_8(addr, 0x0E, &htype) != AXL_OK) {
        return AXL_ERR;
    }
    if (type != NULL) {
        /* Strip the multi-function bit; pass the low 7 bits through
           as the enum value. Unknown values (anything outside the
           three named constants) round-trip as their numeric form. */
        *type = (AxlPciHeaderType)(htype & 0x7Fu);
    }
    if (is_multi_function != NULL) {
        *is_multi_function = (htype & 0x80u) != 0;
    }
    return AXL_OK;
}

int
axl_pci_get_subsystem(AxlPciAddr addr, uint16_t *svid, uint16_t *sdid)
{
    if (svid == NULL || sdid == NULL) {
        return AXL_ERR;
    }
    AxlPciHeaderType hdr;
    if (axl_pci_get_header_type(addr, &hdr, NULL) != AXL_OK) {
        return AXL_ERR;
    }
    if (hdr != AXL_PCI_HEADER_TYPE_NORMAL) {
        /* PCI-PCI bridges (Type 1) and CardBus bridges (Type 2)
           use 0x2C/0x2E for unrelated fields. */
        return AXL_ERR;
    }
    uint16_t v;
    if (axl_pci_read_config_16(addr, 0x2C, &v) != 0) {
        return AXL_ERR;
    }
    if (axl_pci_read_config_16(addr, 0x2E, sdid) != 0) {
        return AXL_ERR;
    }
    *svid = v;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// PCI class triplet → human string
// Tables sourced from the PCI Code and ID Assignment Specification.
// Linear search (tables are tiny; lookup is human-facing print).
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t      base;
    const char  *name;
} PciBaseEntry;

typedef struct {
    uint8_t      base;
    uint8_t      sub;
    const char  *name;
} PciSubEntry;

typedef struct {
    uint8_t      base;
    uint8_t      sub;
    uint8_t      prog;
    const char  *name;
} PciProgEntry;

static const PciBaseEntry pci_base_table[] = {
    { 0x00, "Unclassified" },
    { 0x01, "Mass storage controller" },
    { 0x02, "Network controller" },
    { 0x03, "Display controller" },
    { 0x04, "Multimedia controller" },
    { 0x05, "Memory controller" },
    { 0x06, "Bridge" },
    { 0x07, "Simple communication controller" },
    { 0x08, "Base system peripheral" },
    { 0x09, "Input device controller" },
    { 0x0A, "Docking station" },
    { 0x0B, "Processor" },
    { 0x0C, "Serial bus controller" },
    { 0x0D, "Wireless controller" },
    { 0x0E, "Intelligent controller" },
    { 0x0F, "Satellite communication controller" },
    { 0x10, "Encryption controller" },
    { 0x11, "Signal processing controller" },
    { 0x12, "Processing accelerator" },
    { 0x13, "Non-essential instrumentation function" },
    { 0x40, "Co-processor" },
    { 0xFF, "Unassigned class (vendor-specific)" },
};

static const PciSubEntry pci_sub_table[] = {
    /* 0x00: Unclassified */
    { 0x00, 0x00, "Non-VGA" },
    { 0x00, 0x01, "VGA-compatible" },
    { 0x00, 0x80, "Other" },
    /* 0x01: Mass storage controller */
    { 0x01, 0x00, "SCSI" },
    { 0x01, 0x01, "IDE" },
    { 0x01, 0x02, "Floppy" },
    { 0x01, 0x03, "IPI" },
    { 0x01, 0x04, "RAID" },
    { 0x01, 0x05, "ATA" },
    { 0x01, 0x06, "SATA" },
    { 0x01, 0x07, "SAS" },
    { 0x01, 0x08, "Non-volatile memory" },
    { 0x01, 0x09, "UFS" },
    { 0x01, 0x80, "Other" },
    /* 0x02: Network controller */
    { 0x02, 0x00, "Ethernet" },
    { 0x02, 0x01, "Token Ring" },
    { 0x02, 0x02, "FDDI" },
    { 0x02, 0x03, "ATM" },
    { 0x02, 0x04, "ISDN" },
    { 0x02, 0x05, "WorldFip" },
    { 0x02, 0x06, "PICMG" },
    { 0x02, 0x07, "InfiniBand" },
    { 0x02, 0x08, "Fabric" },
    { 0x02, 0x80, "Other" },
    /* 0x03: Display controller */
    { 0x03, 0x00, "VGA-compatible" },
    { 0x03, 0x01, "XGA" },
    { 0x03, 0x02, "3D" },
    { 0x03, 0x80, "Other" },
    /* 0x04: Multimedia */
    { 0x04, 0x00, "Video" },
    { 0x04, 0x01, "Audio" },
    { 0x04, 0x02, "Telephony" },
    { 0x04, 0x03, "HD Audio" },
    { 0x04, 0x80, "Other" },
    /* 0x05: Memory */
    { 0x05, 0x00, "RAM" },
    { 0x05, 0x01, "Flash" },
    { 0x05, 0x02, "CXL" },
    { 0x05, 0x80, "Other" },
    /* 0x06: Bridge */
    { 0x06, 0x00, "Host bridge" },
    { 0x06, 0x01, "ISA bridge" },
    { 0x06, 0x02, "EISA bridge" },
    { 0x06, 0x03, "MicroChannel bridge" },
    { 0x06, 0x04, "PCI-to-PCI bridge" },
    { 0x06, 0x05, "PCMCIA bridge" },
    { 0x06, 0x06, "NuBus bridge" },
    { 0x06, 0x07, "CardBus bridge" },
    { 0x06, 0x08, "RACEway bridge" },
    { 0x06, 0x09, "Semi-transparent PCI-to-PCI bridge" },
    { 0x06, 0x0A, "InfiniBand-to-PCI bridge" },
    { 0x06, 0x0B, "Advanced switching bridge" },
    { 0x06, 0x80, "Other" },
    /* 0x07: Simple comm */
    { 0x07, 0x00, "Serial" },
    { 0x07, 0x01, "Parallel" },
    { 0x07, 0x02, "Multiport serial" },
    { 0x07, 0x03, "Modem" },
    { 0x07, 0x04, "GPIB" },
    { 0x07, 0x05, "Smart card" },
    { 0x07, 0x80, "Other" },
    /* 0x08: Base system peripheral */
    { 0x08, 0x00, "PIC" },
    { 0x08, 0x01, "DMA" },
    { 0x08, 0x02, "Timer" },
    { 0x08, 0x03, "RTC" },
    { 0x08, 0x04, "PCI hot-plug" },
    { 0x08, 0x05, "SD host" },
    { 0x08, 0x06, "IOMMU" },
    { 0x08, 0x07, "RC event collector" },
    { 0x08, 0x80, "Other" },
    /* 0x09: Input */
    { 0x09, 0x00, "Keyboard" },
    { 0x09, 0x01, "Pen" },
    { 0x09, 0x02, "Mouse" },
    { 0x09, 0x03, "Scanner" },
    { 0x09, 0x04, "Gameport" },
    { 0x09, 0x80, "Other" },
    /* 0x0A: Docking */
    { 0x0A, 0x00, "Generic" },
    { 0x0A, 0x80, "Other" },
    /* 0x0B: Processor */
    { 0x0B, 0x00, "386" },
    { 0x0B, 0x01, "486" },
    { 0x0B, 0x02, "Pentium" },
    { 0x0B, 0x10, "Alpha" },
    { 0x0B, 0x20, "PowerPC" },
    { 0x0B, 0x30, "MIPS" },
    { 0x0B, 0x40, "Co-processor" },
    { 0x0B, 0x80, "Other" },
    /* 0x0C: Serial bus */
    { 0x0C, 0x00, "FireWire" },
    { 0x0C, 0x01, "ACCESS.bus" },
    { 0x0C, 0x02, "SSA" },
    { 0x0C, 0x03, "USB" },
    { 0x0C, 0x04, "Fibre Channel" },
    { 0x0C, 0x05, "SMBus" },
    { 0x0C, 0x06, "InfiniBand" },
    { 0x0C, 0x07, "IPMI" },
    { 0x0C, 0x08, "SERCOS" },
    { 0x0C, 0x09, "CANbus" },
    { 0x0C, 0x0A, "MIPI I3C" },
    { 0x0C, 0x80, "Other" },
    /* 0x0D: Wireless */
    { 0x0D, 0x00, "iRDA" },
    { 0x0D, 0x01, "Consumer IR" },
    { 0x0D, 0x10, "RF" },
    { 0x0D, 0x11, "Bluetooth" },
    { 0x0D, 0x12, "Broadband" },
    { 0x0D, 0x20, "802.11a" },
    { 0x0D, 0x21, "802.11b" },
    { 0x0D, 0x40, "Cellular" },
    { 0x0D, 0x41, "Cellular + Ethernet" },
    { 0x0D, 0x80, "Other" },
    /* 0x0E: Intelligent */
    { 0x0E, 0x00, "I2O" },
    /* 0x0F: Satellite comm */
    { 0x0F, 0x01, "TV" },
    { 0x0F, 0x02, "Audio" },
    { 0x0F, 0x03, "Voice" },
    { 0x0F, 0x04, "Data" },
    /* 0x10: Encryption */
    { 0x10, 0x00, "Network and computing" },
    { 0x10, 0x10, "Entertainment" },
    { 0x10, 0x80, "Other" },
    /* 0x11: Signal processing */
    { 0x11, 0x00, "DPIO" },
    { 0x11, 0x01, "Performance counter" },
    { 0x11, 0x10, "Communication synchronizer" },
    { 0x11, 0x20, "Signal processing management" },
    { 0x11, 0x80, "Other" },
};

/* Programming interfaces for the subclasses where they're commonly
   meaningful (USB host controller flavor, IDE/SATA/NVMe, serial UART
   variants, etc.). Subclasses without a defined prog_if don't appear
   here — axl_pci_class_string omits the prog tier in that case rather
   than printing a "<unknown>" placeholder. */
static const PciProgEntry pci_prog_table[] = {
    /* IDE 01:01 */
    { 0x01, 0x01, 0x00, "ISA-compat" },
    { 0x01, 0x01, 0x05, "PCI-native" },
    { 0x01, 0x01, 0x0A, "ISA-compat (mode-sw)" },
    { 0x01, 0x01, 0x0F, "PCI-native (mode-sw)" },
    { 0x01, 0x01, 0x80, "ISA-compat + bus-master" },
    { 0x01, 0x01, 0x85, "PCI-native + bus-master" },
    { 0x01, 0x01, 0x8A, "ISA-compat (mode-sw) + bus-master" },
    { 0x01, 0x01, 0x8F, "PCI-native (mode-sw) + bus-master" },
    /* SATA 01:06 */
    { 0x01, 0x06, 0x00, "vendor-specific" },
    { 0x01, 0x06, 0x01, "AHCI 1.0" },
    { 0x01, 0x06, 0x02, "Serial Storage Bus" },
    /* SAS 01:07 */
    { 0x01, 0x07, 0x00, "SAS" },
    { 0x01, 0x07, 0x01, "Serial Storage Bus" },
    /* NVM 01:08 */
    { 0x01, 0x08, 0x01, "NVMHCI" },
    { 0x01, 0x08, 0x02, "NVMe" },
    { 0x01, 0x08, 0x03, "NVMe Mgmt I/F" },
    /* Display VGA 03:00 */
    { 0x03, 0x00, 0x00, "standard" },
    { 0x03, 0x00, 0x01, "8514-compatible" },
    /* Bridge PCI-to-PCI 06:04 */
    { 0x06, 0x04, 0x00, "normal decode" },
    { 0x06, 0x04, 0x01, "subtractive decode" },
    /* Bridge RACEway 06:08 */
    { 0x06, 0x08, 0x00, "transparent" },
    { 0x06, 0x08, 0x01, "endpoint" },
    /* Bridge semi-transparent 06:09 */
    { 0x06, 0x09, 0x40, "primary toward host" },
    { 0x06, 0x09, 0x80, "secondary toward host" },
    /* Serial UART 07:00 */
    { 0x07, 0x00, 0x00, "8250" },
    { 0x07, 0x00, 0x01, "16450" },
    { 0x07, 0x00, 0x02, "16550" },
    { 0x07, 0x00, 0x03, "16650" },
    { 0x07, 0x00, 0x04, "16750" },
    { 0x07, 0x00, 0x05, "16850" },
    { 0x07, 0x00, 0x06, "16950" },
    /* Parallel 07:01 */
    { 0x07, 0x01, 0x00, "SPP" },
    { 0x07, 0x01, 0x01, "bidirectional" },
    { 0x07, 0x01, 0x02, "ECP 1.X" },
    { 0x07, 0x01, 0x03, "IEEE 1284" },
    { 0x07, 0x01, 0xFE, "IEEE 1284 target" },
    /* Modem 07:03 */
    { 0x07, 0x03, 0x00, "generic" },
    { 0x07, 0x03, 0x01, "Hayes 16450" },
    { 0x07, 0x03, 0x02, "Hayes 16550" },
    { 0x07, 0x03, 0x03, "Hayes 16650" },
    { 0x07, 0x03, 0x04, "Hayes 16750" },
    /* PIC 08:00 */
    { 0x08, 0x00, 0x00, "8259" },
    { 0x08, 0x00, 0x01, "ISA PIC" },
    { 0x08, 0x00, 0x02, "EISA PIC" },
    { 0x08, 0x00, 0x10, "I/O APIC" },
    { 0x08, 0x00, 0x20, "I/O x2APIC" },
    /* DMA 08:01 */
    { 0x08, 0x01, 0x00, "8237" },
    { 0x08, 0x01, 0x01, "ISA" },
    { 0x08, 0x01, 0x02, "EISA" },
    /* Timer 08:02 */
    { 0x08, 0x02, 0x00, "8254" },
    { 0x08, 0x02, 0x01, "ISA" },
    { 0x08, 0x02, 0x02, "EISA" },
    { 0x08, 0x02, 0x03, "HPET" },
    /* RTC 08:03 */
    { 0x08, 0x03, 0x00, "generic" },
    { 0x08, 0x03, 0x01, "ISA" },
    /* Keyboard 09:00 — none */
    /* USB 0C:03 */
    { 0x0C, 0x03, 0x00, "UHCI" },
    { 0x0C, 0x03, 0x10, "OHCI" },
    { 0x0C, 0x03, 0x20, "EHCI" },
    { 0x0C, 0x03, 0x30, "xHCI" },
    { 0x0C, 0x03, 0x40, "USB4" },
    { 0x0C, 0x03, 0x80, "unspecified" },
    { 0x0C, 0x03, 0xFE, "USB device (not host)" },
    /* IPMI 0C:07 */
    { 0x0C, 0x07, 0x00, "SMIC" },
    { 0x0C, 0x07, 0x01, "Keyboard Controller Style" },
    { 0x0C, 0x07, 0x02, "Block Transfer" },
};

/* Per-tier lookup: consult the optional class-name overlay first
   (loaded via axl_pci_class_load), then fall back to the compiled-in
   table. NULL when neither has an entry. */
static const char *
lookup_base(uint8_t base)
{
    const char *override = _axl_pci_class_overlay_base(base);
    if (override != NULL) {
        return override;
    }
    for (size_t i = 0; i < sizeof(pci_base_table) / sizeof(pci_base_table[0]); i++) {
        if (pci_base_table[i].base == base) {
            return pci_base_table[i].name;
        }
    }
    return NULL;
}

static const char *
lookup_sub(uint8_t base, uint8_t sub)
{
    const char *override = _axl_pci_class_overlay_sub(base, sub);
    if (override != NULL) {
        return override;
    }
    for (size_t i = 0; i < sizeof(pci_sub_table) / sizeof(pci_sub_table[0]); i++) {
        if (pci_sub_table[i].base == base && pci_sub_table[i].sub == sub) {
            return pci_sub_table[i].name;
        }
    }
    return NULL;
}

static const char *
lookup_prog(uint8_t base, uint8_t sub, uint8_t prog)
{
    const char *override = _axl_pci_class_overlay_prog(base, sub, prog);
    if (override != NULL) {
        return override;
    }
    for (size_t i = 0; i < sizeof(pci_prog_table) / sizeof(pci_prog_table[0]); i++) {
        if (pci_prog_table[i].base == base
            && pci_prog_table[i].sub == sub
            && pci_prog_table[i].prog == prog)
        {
            return pci_prog_table[i].name;
        }
    }
    return NULL;
}

int
axl_pci_class_string_fmt(
    uint32_t        class_code,
    AxlPciClassFmt  fmt,
    char           *buf,
    size_t          buflen
    )
{
    /* Resolve each tier through the overlay-then-builtin chain
       (lookup_base / _sub / _prog walk the loaded sidecar overlay
       first, fall through to compiled-in tables on miss). The
       output assembly itself — FMT_FULL / FMT_SUBCLASS / FMT_BASE
       fallbacks, omit-unknown-tier posture, numeric escape — lives
       in the shared axl-class-fmt helper. AxlPciClassFmt's enum
       values match AxlClassFmt by construction. */
    uint8_t base = (uint8_t)((class_code >> 16) & 0xFFu);
    uint8_t sub  = (uint8_t)((class_code >>  8) & 0xFFu);
    uint8_t prog = (uint8_t)( class_code        & 0xFFu);

    char numeric[20];
    axl_snprintf(numeric, sizeof(numeric), "Class %06x",
                 (unsigned)(class_code & 0xFFFFFFu));

    return axl_class_string_fmt_resolve(
        lookup_base(base),
        lookup_sub(base, sub),
        lookup_prog(base, sub, prog),
        numeric,
        (AxlClassFmt)fmt,
        buf, buflen);
}

int
axl_pci_class_string(uint32_t class_code, char *buf, size_t buflen)
{
    return axl_pci_class_string_fmt(class_code,
                                    AXL_PCI_CLASS_FMT_FULL,
                                    buf, buflen);
}

int
axl_pci_dump(
    AxlPciAddr   addr,
    uint8_t     *buf,
    size_t       bytes,
    size_t      *out_read
    )
{
    if (out_read != NULL) {
        *out_read = 0;
    }
    if (buf == NULL) {
        return AXL_ERR;
    }
    /* Cap at PCIe ECAM extent and round down to 32-bit alignment. */
    if (bytes > AXL_PCI_CONFIG_SPACE_MAX) {
        bytes = AXL_PCI_CONFIG_SPACE_MAX;
    }
    bytes &= ~(size_t)0x3u;
    if (bytes == 0) {
        return AXL_ERR;
    }

    /* Absent-function check first. ECAM reads of an unmapped address
       quietly return 0xFFFFFFFF (the bus-level "no device" sentinel)
       rather than a hard error, so the read alone can't distinguish
       absent from present. The standard absent-detection idiom: VID
       at offset 0x00 reads as 0xFFFF (i.e. low 16 bits of word 0 are
       all-ones). */
    uint32_t word0;
    if (axl_pci_read_config_32(addr, 0, &word0) != 0
        || (word0 & 0xFFFFu) == 0xFFFFu)
    {
        return AXL_ERR;
    }

    size_t   ok_bytes = 0;
    uint16_t reg;
    for (reg = 0; (size_t)reg + 4u <= bytes; reg = (uint16_t)(reg + 4)) {
        uint32_t v = (reg == 0) ? word0 : 0;
        if (reg == 0 || axl_pci_read_config_32(addr, reg, &v) == 0) {
            buf[reg + 0] = (uint8_t)(v        & 0xFFu);
            buf[reg + 1] = (uint8_t)((v >> 8) & 0xFFu);
            buf[reg + 2] = (uint8_t)((v >> 16) & 0xFFu);
            buf[reg + 3] = (uint8_t)((v >> 24) & 0xFFu);
            ok_bytes = (size_t)reg + 4u;
        } else {
            /* Defense-in-depth: after the VID check above, ECAM reads
               on the same function should not fail (axl_pci_read_config_32
               only errors on NULL-out or NULL-ecam_ptr). Keep the zero
               anyway so a future change to the read path can't leak
               stale buf bytes past out_read. */
            buf[reg + 0] = 0;
            buf[reg + 1] = 0;
            buf[reg + 2] = 0;
            buf[reg + 3] = 0;
        }
    }

    if (out_read != NULL) {
        *out_read = ok_bytes;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

/* PCI register offsets and status flags moved to
   src/pci/axl-pci-internal.h so axl-pci-cap.c can share them. */

static bool
function_present(
    AxlPciAddr  a,
    bool        include_zero_vid
    )
{
    uint16_t vid;
    if (axl_pci_read_config_16(a, PCI_VENDOR_ID_OFFSET, &vid) != 0) {
        return false;
    }
    /* 0xFFFF is the bus-level "no device" sentinel for an absent slot.
       0x0000 is also a reserved vendor ID (never assigned by PCI-SIG);
       some chipsets return all-zero config reads for disconnected
       slots/functions instead of all-ones, producing "phantom"
       0000:0000 devices. The default walk treats both as absent.
       axl_pci_next_unfiltered opts back into seeing 0x0000 slots. */
    if (vid == 0xFFFF) {
        return false;
    }
    if (vid == 0x0000 && !include_zero_vid) {
        return false;
    }
    return true;
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

static AxlPciAddr *
pci_next_impl(
    AxlPciAddr  *prev,
    bool         include_zero_vid
    )
{
    if (axl_pci_ensure_init() != 0) {
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
        if (function_present(cursor, include_zero_vid)) {
            /* On func 0, read the header-type byte and remember
               whether this is multi-function. The caller gets func 0
               first; the next call's advance honours the flag. */
            if (cursor.func == 0) {
                uint8_t htype;
                pending_skip_funcs =
                    (axl_pci_read_config_8(cursor,
                        PCI_HEADER_TYPE_OFFSET, &htype) == AXL_OK)
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

AxlPciAddr *
axl_pci_next(
    AxlPciAddr  *prev
    )
{
    /* Default: skip both 0xFFFF (absent) and 0x0000 (phantom) slots. */
    return pci_next_impl(prev, false);
}

AxlPciAddr *
axl_pci_next_unfiltered(
    AxlPciAddr  *prev
    )
{
    /* Opt-in: skip only 0xFFFF, so 0x0000 phantom slots are visible.
       For the rare consumer that must enumerate raw config space. */
    return pci_next_impl(prev, true);
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
        return AXL_ERR;
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
                return AXL_OK;
            }
            matches++;
        }
    }
    return AXL_ERR;
}

int
axl_pci_find_by_class(
    uint32_t     class_code,
    uint16_t     nth,
    AxlPciAddr  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
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
        uint32_t cur_class_code = (reg08 >> 8) & 0xFFFFFFu;
        if (class_code == 0xFFFFFFu || cur_class_code == class_code) {
            if (matches == nth) {
                *out = *p;
                return AXL_OK;
            }
            matches++;
        }
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Bridges and topology
// ---------------------------------------------------------------------------

#define PCI_HEADER_TYPE_PCI_BRIDGE  0x01u  /* low 7 bits of byte at 0x0E */
#define PCI_BRIDGE_PRIMARY_BUS      0x18u
#define PCI_BRIDGE_SECONDARY_BUS    0x19u
#define PCI_BRIDGE_SUBORDINATE_BUS  0x1Au

int
axl_pci_bridge_info(
    AxlPciAddr     addr,
    AxlPciBridge  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    uint8_t htype;
    if (axl_pci_read_config_8(addr, 0x0E, &htype) != AXL_OK) {
        return AXL_ERR;
    }
    if ((htype & 0x7Fu) != PCI_HEADER_TYPE_PCI_BRIDGE) {
        return AXL_ERR;
    }
    uint8_t pri, sec, sub;
    if (axl_pci_read_config_8(addr, PCI_BRIDGE_PRIMARY_BUS, &pri) != AXL_OK
        || axl_pci_read_config_8(addr, PCI_BRIDGE_SECONDARY_BUS, &sec) != AXL_OK
        || axl_pci_read_config_8(addr, PCI_BRIDGE_SUBORDINATE_BUS, &sub) != AXL_OK)
    {
        return AXL_ERR;
    }
    out->primary     = pri;
    out->secondary   = sec;
    out->subordinate = sub;
    return AXL_OK;
}

// ---------------------------------------------------------------------------

/* Per-function row collected by the tree walker's first pass. The
   bridge bus number is captured here so the recursive descent doesn't
   re-read config space. */
typedef struct {
    AxlPciAddr  addr;
    bool        is_bridge;
    uint8_t     secondary_bus;
} AxlPciTreeFunc;

/* Per-segment 256-bit bitmap helpers — one bit per bus number.
   Used both for "this bus is some bridge's secondary" (so it's NOT
   a root bus) and for "this bus has been visited" (cycle break). */
typedef struct {
    uint64_t  bits[4];      /* 4 * 64 = 256 bits */
} BusBitmap;

static inline void
bus_bitmap_set(
    BusBitmap  *b,
    uint8_t     bus
    )
{
    b->bits[bus >> 6] |= ((uint64_t)1 << (bus & 0x3Fu));
}

static inline bool
bus_bitmap_get(
    const BusBitmap  *b,
    uint8_t           bus
    )
{
    return (b->bits[bus >> 6] >> (bus & 0x3Fu)) & 1u;
}

/* Comparator for sorting AxlPciTreeFunc by (bus, dev, func). Bus
   sort lets the recursion locate "all functions on bus N" by scanning
   the contiguous run rather than re-filtering. */
static int
tree_func_cmp(
    const void  *a,
    const void  *b
    )
{
    const AxlPciTreeFunc *x = a;
    const AxlPciTreeFunc *y = b;
    if (x->addr.seg  != y->addr.seg)  { return (x->addr.seg  < y->addr.seg)  ? -1 : 1; }
    if (x->addr.bus  != y->addr.bus)  { return (x->addr.bus  < y->addr.bus)  ? -1 : 1; }
    if (x->addr.dev  != y->addr.dev)  { return (x->addr.dev  < y->addr.dev)  ? -1 : 1; }
    if (x->addr.func != y->addr.func) { return (x->addr.func < y->addr.func) ? -1 : 1; }
    return 0;
}

static int
tree_walk_bus(
    AxlArray      *funcs,
    uint16_t       seg,
    uint8_t        bus,
    unsigned       depth,
    BusBitmap     *visited,
    AxlPciTreeFn   fn,
    void          *ctx
    )
{
    if (depth >= AXL_PCI_TREE_MAX_DEPTH) {
        /* Defense-in-depth: real PCI trees are <8 levels. */
        return -1;
    }
    if (bus_bitmap_get(visited, bus)) {
        /* Cycle — bridge claims a bus already on the walk path. */
        return 0;
    }
    bus_bitmap_set(visited, bus);

    /* Walk every function on this bus. Funcs are sorted by
       (seg, bus, dev, func), so the run of matches is contiguous. */
    size_t n = axl_array_len(funcs);
    for (size_t i = 0; i < n; i++) {
        const AxlPciTreeFunc *f = axl_array_get(funcs, i);
        if (f->addr.seg != seg || f->addr.bus != bus) {
            continue;
        }
        int rc = fn(f->addr, depth, f->is_bridge, ctx);
        if (rc != 0) {
            return rc;
        }
        if (f->is_bridge && f->secondary_bus != bus) {
            rc = tree_walk_bus(funcs, seg, f->secondary_bus,
                               depth + 1, visited, fn, ctx);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

int
axl_pci_tree_for_each(
    AxlPciTreeFn  fn,
    void         *ctx
    )
{
    if (fn == NULL || axl_pci_ensure_init() != 0) {
        return AXL_ERR;
    }

    /* Pass 1: collect every responding function with its bridge
       bus, if any. */
    AXL_AUTOPTR(AxlArray) funcs = axl_array_new(sizeof(AxlPciTreeFunc));
    if (funcs == NULL) {
        return AXL_ERR;
    }
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        AxlPciTreeFunc f = { .addr = *p, .is_bridge = false, .secondary_bus = 0 };
        AxlPciBridge   br;
        if (axl_pci_bridge_info(*p, &br) == AXL_OK) {
            f.is_bridge     = true;
            f.secondary_bus = br.secondary;
        }
        if (axl_array_append(funcs, &f) != AXL_OK) {
            return AXL_ERR;
        }
    }
    axl_array_sort(funcs, tree_func_cmp);

    /* Pass 2: per segment, mark every bridge's secondary as a child
       bus, then start the walk from each unmarked bus that has at
       least one function. Segments are walked in MCFG order. */
    for (size_t s = 0; s < cached_mcfg.count; s++) {
        uint16_t seg = cached_mcfg.segments[s].segment;
        BusBitmap is_child = { .bits = {0} };
        BusBitmap has_funcs = { .bits = {0} };

        size_t n = axl_array_len(funcs);
        for (size_t i = 0; i < n; i++) {
            const AxlPciTreeFunc *f = axl_array_get(funcs, i);
            if (f->addr.seg != seg) {
                continue;
            }
            bus_bitmap_set(&has_funcs, f->addr.bus);
            if (f->is_bridge && f->secondary_bus != f->addr.bus) {
                bus_bitmap_set(&is_child, f->secondary_bus);
            }
        }

        BusBitmap visited = { .bits = {0} };
        for (unsigned b = 0; b < 256; b++) {
            uint8_t bus = (uint8_t)b;
            if (bus_bitmap_get(&has_funcs, bus)
                && !bus_bitmap_get(&is_child, bus))
            {
                int rc = tree_walk_bus(funcs, seg, bus, 0,
                                       &visited, fn, ctx);
                if (rc != 0) {
                    return rc;
                }
            }
        }
    }
    return AXL_OK;
}

