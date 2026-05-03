/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-usb-class.c
    USB class triplet (class / subclass / protocol) decode tables.

    Mirrors `axl-pci.c`'s class-string decoder shape: three flat
    tables walked by linear search, with FMT_FULL / FMT_SUBCLASS /
    FMT_BASE output shapes that omit unknown tiers (rather than
    rendering `<unknown>` placeholders) and fall back to numeric
    `Class XXXXXX` when the base is wholly unknown.

    Source: USB-IF Defined Class Codes
    (https://www.usb.org/defined-class-codes), the canonical USB class
    list. This is a curated subset — the long tail (e.g. exotic
    BCD-Audio formats, every Smart-Card protocol byte) lives in
    Linux's `lsusb` / `usb.ids` but is rarely worth shipping inline.
    A future `usb-class.json5` sidecar (parallel to `pci-class.json5`)
    will let consumers extend without rebuilding.
**/

#include "../data/axl-class-fmt.h"

#include <axl/axl-str.h>
#include <axl/axl-usb.h>

// ---------------------------------------------------------------------------
// Compiled-in tables
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t      code;
    const char  *name;
} BaseEntry;

typedef struct {
    uint8_t      base;
    uint8_t      sub;
    const char  *name;
} SubEntry;

typedef struct {
    uint8_t      base;
    uint8_t      sub;
    uint8_t      prot;
    const char  *name;
} ProtEntry;

/* Base classes — USB-IF Defined Class Codes table.
   `0x00` is intentionally absent: a device descriptor's bDeviceClass
   of 0 means "look at the interface descriptors instead," which is
   not a real class to print. AxlUsb queries are interface-level, so
   bInterfaceClass=0 is genuinely undefined and falls through to the
   numeric path. */
static const BaseEntry usb_base_table[] = {
    { 0x01, "Audio" },
    { 0x02, "Communications" },
    { 0x03, "Human Interface Device" },
    { 0x05, "Physical" },
    { 0x06, "Image" },
    { 0x07, "Printer" },
    { 0x08, "Mass Storage" },
    { 0x09, "Hub" },
    { 0x0A, "CDC Data" },
    { 0x0B, "Smart Card" },
    { 0x0D, "Content Security" },
    { 0x0E, "Video" },
    { 0x0F, "Personal Healthcare" },
    { 0x10, "Audio/Video" },
    { 0x11, "Billboard" },
    { 0x12, "USB Type-C Bridge" },
    { 0x13, "Bulk Display Protocol" },
    { 0x3C, "I3C Device" },
    { 0xDC, "Diagnostic Device" },
    { 0xE0, "Wireless Controller" },
    { 0xEF, "Miscellaneous" },
    { 0xFE, "Application Specific" },
    { 0xFF, "Vendor Specific" },
};

/* Subclasses — only entries with spec-defined names. Subclasses
   without a name (e.g. Hub class 0x09 has no spec subclass split)
   don't appear; the decoder omits the tier rather than printing
   <unknown>. */
static const SubEntry usb_sub_table[] = {
    /* 0x01: Audio */
    { 0x01, 0x01, "Audio Control" },
    { 0x01, 0x02, "Audio Streaming" },
    { 0x01, 0x03, "MIDI Streaming" },
    /* 0x02: Communications (CDC Control) */
    { 0x02, 0x01, "Direct Line Control" },
    { 0x02, 0x02, "Abstract Control" },
    { 0x02, 0x03, "Telephone Control" },
    { 0x02, 0x04, "Multi-Channel Control" },
    { 0x02, 0x06, "Ethernet Networking" },
    { 0x02, 0x07, "ATM Networking" },
    { 0x02, 0x08, "Wireless Handset" },
    { 0x02, 0x09, "Device Management" },
    { 0x02, 0x0A, "Mobile Direct Line" },
    { 0x02, 0x0B, "OBEX" },
    { 0x02, 0x0C, "Ethernet Emulation" },
    { 0x02, 0x0D, "Network Control" },
    { 0x02, 0x0E, "Mobile Broadband Interface" },
    /* 0x03: HID */
    { 0x03, 0x00, "No Subclass" },
    { 0x03, 0x01, "Boot Interface" },
    /* 0x06: Image */
    { 0x06, 0x01, "Still Image Capture" },
    /* 0x07: Printer */
    { 0x07, 0x01, "Printer" },
    /* 0x08: Mass Storage */
    { 0x08, 0x01, "RBC" },
    { 0x08, 0x02, "ATAPI" },
    { 0x08, 0x03, "QIC-157" },
    { 0x08, 0x04, "UFI" },
    { 0x08, 0x05, "SFF-8070i" },
    { 0x08, 0x06, "SCSI transparent" },
    { 0x08, 0x07, "LSD FS" },
    { 0x08, 0x08, "IEEE 1667" },
    /* 0x0B: Smart Card */
    { 0x0B, 0x00, "CCID" },
    /* 0x0D: Content Security */
    { 0x0D, 0x00, "Content Security" },
    /* 0x0E: Video */
    { 0x0E, 0x01, "Video Control" },
    { 0x0E, 0x02, "Video Streaming" },
    { 0x0E, 0x03, "Video Interface Collection" },
    /* 0x10: Audio/Video */
    { 0x10, 0x01, "AVControl Interface" },
    { 0x10, 0x02, "AVData Video Streaming" },
    { 0x10, 0x03, "AVData Audio Streaming" },
    /* 0xDC: Diagnostic */
    { 0xDC, 0x01, "USB2 Compliance" },
    { 0xDC, 0x02, "Debug Target Vendor-defined" },
    { 0xDC, 0x05, "Trace" },
    { 0xDC, 0x06, "DvC Trace" },
    { 0xDC, 0x07, "DvC Dfx" },
    { 0xDC, 0x08, "DvC Trace IA" },
    /* 0xE0: Wireless Controller */
    { 0xE0, 0x01, "Bluetooth / RF" },
    { 0xE0, 0x02, "Wireless USB" },
    /* 0xEF: Miscellaneous */
    { 0xEF, 0x01, "Sync" },
    { 0xEF, 0x02, "Interface Association" },
    { 0xEF, 0x03, "Cable-Based Association" },
    { 0xEF, 0x04, "RNDIS" },
    { 0xEF, 0x05, "USB3 Vision" },
    { 0xEF, 0x06, "STEP" },
    { 0xEF, 0x07, "STEP Stream" },
    { 0xEF, 0x08, "Command Interface" },
    /* 0xFE: Application Specific */
    { 0xFE, 0x01, "DFU" },
    { 0xFE, 0x02, "IrDA Bridge" },
    { 0xFE, 0x03, "Test & Measurement" },
};

/* Protocols — same posture as the PCI prog_if table: only entries
   for (base, sub) pairs that have spec-defined protocol bytes worth
   naming. The HID Boot subclass and the Mass Storage subclasses have
   the most-used protocol bytes; everything else stays narrow. */
static const ProtEntry usb_prot_table[] = {
    /* 0x03/0x01 — HID Boot Interface */
    { 0x03, 0x01, 0x00, "None" },
    { 0x03, 0x01, 0x01, "Keyboard" },
    { 0x03, 0x01, 0x02, "Mouse" },
    /* 0x08/0x06 — Mass Storage SCSI */
    { 0x08, 0x06, 0x00, "CBI w/ command-completion interrupt" },
    { 0x08, 0x06, 0x01, "CBI w/o command-completion interrupt" },
    { 0x08, 0x06, 0x50, "Bulk-Only Transport" },
    { 0x08, 0x06, 0x62, "USB Attached SCSI" },
    /* 0x0B/0x00 — Smart Card CCID */
    { 0x0B, 0x00, 0x00, "Bulk" },
    /* 0xDC base-only diagnostic helpers — protocol decoded against
       sub=0x01 (USB2 Compliance) for completeness. */
    { 0xDC, 0x01, 0x01, "USB2 Compliance Device" },
    /* 0xE0/0x01 — Wireless RF */
    { 0xE0, 0x01, 0x01, "Bluetooth Programming Interface" },
    { 0xE0, 0x01, 0x02, "UWB Radio Control" },
    { 0xE0, 0x01, 0x03, "Remote NDIS" },
    { 0xE0, 0x01, 0x04, "Bluetooth AMP" },
};

// ---------------------------------------------------------------------------
// Linear-search lookups
// ---------------------------------------------------------------------------

static const char *
lookup_base(
    uint8_t  base
    )
{
    for (size_t i = 0;
         i < sizeof(usb_base_table) / sizeof(usb_base_table[0]); i++)
    {
        if (usb_base_table[i].code == base) {
            return usb_base_table[i].name;
        }
    }
    return NULL;
}

static const char *
lookup_sub(
    uint8_t  base,
    uint8_t  sub
    )
{
    for (size_t i = 0;
         i < sizeof(usb_sub_table) / sizeof(usb_sub_table[0]); i++)
    {
        if (usb_sub_table[i].base == base
            && usb_sub_table[i].sub == sub)
        {
            return usb_sub_table[i].name;
        }
    }
    return NULL;
}

static const char *
lookup_prot(
    uint8_t  base,
    uint8_t  sub,
    uint8_t  prot
    )
{
    for (size_t i = 0;
         i < sizeof(usb_prot_table) / sizeof(usb_prot_table[0]); i++)
    {
        if (usb_prot_table[i].base == base
            && usb_prot_table[i].sub  == sub
            && usb_prot_table[i].prot == prot)
        {
            return usb_prot_table[i].name;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_usb_class_string_fmt(
    uint8_t          class_,
    uint8_t          sub,
    uint8_t          prot,
    AxlUsbClassFmt   fmt,
    char            *buf,
    size_t           buflen
    )
{
    /* Numeric fallback: USB has no 24-bit class-code packing the way
       PCI does, so lay the three bytes out side-by-side. Output
       shape is identical to PCI's (six lowercase hex digits) — the
       shared formatter sees a fully-formed numeric string and
       doesn't care how it was assembled. */
    char numeric[24];
    axl_snprintf(numeric, sizeof(numeric), "Class %02x%02x%02x",
                 (unsigned)class_, (unsigned)sub, (unsigned)prot);

    return axl_class_string_fmt_resolve(
        lookup_base(class_),
        lookup_sub(class_, sub),
        lookup_prot(class_, sub, prot),
        numeric,
        (AxlClassFmt)fmt,
        buf, buflen);
}

int
axl_usb_class_string(
    uint8_t  class_,
    uint8_t  sub,
    uint8_t  prot,
    char    *buf,
    size_t   buflen
    )
{
    return axl_usb_class_string_fmt(class_, sub, prot,
                                    AXL_USB_CLASS_FMT_FULL,
                                    buf, buflen);
}
