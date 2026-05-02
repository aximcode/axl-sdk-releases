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
#include "axl-pci-class-internal.h"
#include <axl/axl-pci.h>
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
// Address parse / format
// ---------------------------------------------------------------------------

/* Parse 1..max_chars hex digits into *out, returning the consumed
   count or -1 on no digits / overflow. *out is set even on success
   of a single digit. */
static int
parse_hex_field(const char *s, int max_chars, uint32_t *out)
{
    uint32_t v = 0;
    int      i = 0;
    while (i < max_chars) {
        int d = axl_hex_nibble(s[i]);
        if (d < 0) break;
        v = (v << 4) | (uint32_t)d;
        i++;
    }
    if (i == 0) return -1;
    *out = v;
    return i;
}

int
axl_pci_addr_parse(const char *s, AxlPciAddr *out)
{
    if (s == NULL || out == NULL) {
        return -1;
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
        if (n < 0) return -1;
        p += n;
        n_parts++;

        if (*p == '\0') break;
        if (*p != ':' && *p != '.') return -1;

        /* Final field must be preceded by '.', all others by ':'. */
        bool is_final_sep = (*p == '.');
        p++;
        if (is_final_sep) {
            /* func is next; one more parse then must hit EOF. */
            int fn = parse_hex_field(p, 5, &parts[n_parts]);
            if (fn < 0) return -1;
            p += fn;
            n_parts++;
            break;
        }
    }
    if (*p != '\0') return -1;

    uint32_t seg, bus, dev, func;
    if (n_parts == 4) {
        /* seg:bus:dev.func */
        seg = parts[0]; bus = parts[1]; dev = parts[2]; func = parts[3];
    } else if (n_parts == 3) {
        /* bus:dev.func — segment defaults to 0 */
        seg = 0;        bus = parts[0]; dev = parts[1]; func = parts[2];
    } else {
        return -1;
    }

    if (seg > 0xFFFFu || bus > 0xFFu || dev > 0x1Fu || func > 0x07u) {
        return -1;
    }
    out->seg  = (uint16_t)seg;
    out->bus  = (uint8_t)bus;
    out->dev  = (uint8_t)dev;
    out->func = (uint8_t)func;
    return 0;
}

int
axl_pci_addr_format(AxlPciAddr addr, char *buf, size_t buflen)
{
    /* Canonical form is exactly 12 bytes ("SSSS:BB:DD.F") plus a
       NUL terminator. AXL_PCI_ADDR_STR_MAX is the recommended
       allocation; the actual minimum we'll accept is one less. */
    if (buf == NULL || buflen < 13) {
        return -1;
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
        return -1;
    }
    uint16_t v;
    if (axl_pci_read_config_16(addr, 0x00, &v) != 0) {
        return -1;
    }
    if (v == 0xFFFF) {
        /* Function absent — caller doesn't have to special-case this. */
        return -1;
    }
    if (axl_pci_read_config_16(addr, 0x02, did) != 0) {
        return -1;
    }
    *vid = v;
    return 0;
}

int
axl_pci_get_class_code(AxlPciAddr addr, uint32_t *class_code)
{
    if (class_code == NULL) {
        return -1;
    }
    uint8_t prog_if, sub, base;
    if (axl_pci_read_config_8(addr, 0x09, &prog_if) != 0 ||
        axl_pci_read_config_8(addr, 0x0A, &sub)     != 0 ||
        axl_pci_read_config_8(addr, 0x0B, &base)    != 0)
    {
        return -1;
    }
    *class_code = ((uint32_t)base << 16) | ((uint32_t)sub << 8) | prog_if;
    return 0;
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
    if (buf == NULL || buflen == 0) {
        return -1;
    }
    uint8_t base = (uint8_t)((class_code >> 16) & 0xFFu);
    uint8_t sub  = (uint8_t)((class_code >>  8) & 0xFFu);
    uint8_t prog = (uint8_t)( class_code        & 0xFFu);

    /* Omit empty tiers rather than emit "<unknown>" placeholders —
       many spec subclasses have no defined prog_if (Host bridge,
       ISA bridge, SMBus, ...) and printing "<unknown>" for those
       is just noise. When the base class itself is unrecognized,
       fall back to numeric "Class XXXXXX" form. Both behaviors
       mirror Linux lspci. */
    const char *base_str = lookup_base(base);
    const char *sub_str  = lookup_sub(base, sub);
    const char *prog_str = lookup_prog(base, sub, prog);

    /* Common numeric fallback for "wholly unknown" cases — used by
       every fmt mode when the requested tier (and the next-coarser
       fallback) has no entry. */
    char numeric[20];
    axl_snprintf(numeric, sizeof(numeric), "Class %06x",
                 (unsigned)(class_code & 0xFFFFFFu));

    switch (fmt) {
    case AXL_PCI_CLASS_FMT_BASE:
        if (base_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", numeric);
        }
        return axl_snprintf(buf, buflen, "%s", base_str);

    case AXL_PCI_CLASS_FMT_SUBCLASS:
        /* Prefer subclass; coarsen to base; then numeric. */
        if (sub_str != NULL) {
            return axl_snprintf(buf, buflen, "%s", sub_str);
        }
        if (base_str != NULL) {
            return axl_snprintf(buf, buflen, "%s", base_str);
        }
        return axl_snprintf(buf, buflen, "%s", numeric);

    case AXL_PCI_CLASS_FMT_FULL:
        if (base_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", numeric);
        }
        if (sub_str == NULL) {
            return axl_snprintf(buf, buflen, "%s", base_str);
        }
        if (prog_str == NULL) {
            return axl_snprintf(buf, buflen, "%s / %s",
                                base_str, sub_str);
        }
        return axl_snprintf(buf, buflen, "%s / %s / %s",
                            base_str, sub_str, prog_str);
    }
    /* Unknown fmt enum value. */
    return -1;
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
        return -1;
    }
    /* Cap at PCIe ECAM extent and round down to 32-bit alignment. */
    if (bytes > AXL_PCI_CONFIG_SPACE_MAX) {
        bytes = AXL_PCI_CONFIG_SPACE_MAX;
    }
    bytes &= ~(size_t)0x3u;
    if (bytes == 0) {
        return -1;
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
        return -1;
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
    uint32_t     class_code,
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
        uint32_t cur_class_code = (reg08 >> 8) & 0xFFFFFFu;
        if (class_code == 0xFFFFFFu || cur_class_code == class_code) {
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
        return -1;
    }
    uint8_t htype;
    if (axl_pci_read_config_8(addr, 0x0E, &htype) != 0) {
        return -1;
    }
    if ((htype & 0x7Fu) != PCI_HEADER_TYPE_PCI_BRIDGE) {
        return -1;
    }
    uint8_t pri, sec, sub;
    if (axl_pci_read_config_8(addr, PCI_BRIDGE_PRIMARY_BUS, &pri) != 0
        || axl_pci_read_config_8(addr, PCI_BRIDGE_SECONDARY_BUS, &sec) != 0
        || axl_pci_read_config_8(addr, PCI_BRIDGE_SUBORDINATE_BUS, &sub) != 0)
    {
        return -1;
    }
    out->primary     = pri;
    out->secondary   = sec;
    out->subordinate = sub;
    return 0;
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
    if (fn == NULL || ensure_init() != 0) {
        return -1;
    }

    /* Pass 1: collect every responding function with its bridge
       bus, if any. */
    AXL_AUTOPTR(AxlArray) funcs = axl_array_new(sizeof(AxlPciTreeFunc));
    if (funcs == NULL) {
        return -1;
    }
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        AxlPciTreeFunc f = { .addr = *p, .is_bridge = false, .secondary_bus = 0 };
        AxlPciBridge   br;
        if (axl_pci_bridge_info(*p, &br) == 0) {
            f.is_bridge     = true;
            f.secondary_bus = br.secondary;
        }
        if (axl_array_append(funcs, &f) != 0) {
            return -1;
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
    return 0;
}

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
    if (out_off == NULL || out_id == NULL || ensure_init() != 0) {
        return -1;
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
            return -1;
        }
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
    /* Forward-progress guard. Spec doesn't formally require monotonic
       cap offsets, but no real device chains backwards; a back-pointer
       (next <= prev_off) is malformed and would loop the iter. Also
       catches the all-1s self-loop (next == prev_off == 0xFC) on a
       device that becomes absent mid-walk. */
    if (next <= prev_off) {
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
    /* Forward-progress guard, mirror of the legacy-cap walk. A next
       offset that doesn't move forward is malformed and would loop;
       valid ext-cap offsets live in 0x100..0xFFC. The absent-device
       all-1s case still terminates via the cap_id == PCIE_EXT_CAP_END
       check below, but this catches mid-walk cycles too. */
    if (prev_off != 0 && off <= prev_off) {
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
    if (cb == NULL || ensure_init() != 0) {
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
        return -1;
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
        return -1;
    }
    return ctx.found ? 0 : -1;
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
        return -1;
    }
    return vpd_walk(addr, cb, ctx);
}
