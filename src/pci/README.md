PCI / PCIe configuration-space access via ECAM.

Header: `<axl/axl-pci.h>`. Lazy on first call: AxlPci consults
AxlAcpi for the MCFG table to find each segment's ECAM base
address, then computes register offsets directly. Never falls
back to legacy 0xCF8/0xCFC port pair; on platforms without MCFG
(rare on modern hardware), every `axl_pci_*` call returns -1.

Cursor-style enumeration matches `axl_smbios_find_next` and
`axl_acpi_find_next`:

```c
AxlPciAddr *p = NULL;
while ((p = axl_pci_next(p)) != NULL) {
    uint16_t vid, did;
    axl_pci_read_config_16(*p, 0x00, &vid);
    axl_pci_read_config_16(*p, 0x02, &did);
    axl_printf("%04x:%02x:%02x.%u  %04x:%04x\n",
               p->seg, p->bus, p->dev, p->func, vid, did);
}
```

`axl_pci_next` skips empty slots (vendor ID 0xFFFF) and honours
the multi-function header bit, so single-function devices'
phantom funcs 1–7 don't appear in the walk.

## Address tuple

```c
typedef struct {
    uint16_t  seg;    ///< PCI segment group
    uint8_t   bus;
    uint8_t   dev;
    uint8_t   func;
} AxlPciAddr;
```

`uint16_t seg` matches the UEFI MCFG / PCIe spec's segment-group
width — multi-segment platforms (large servers, some ARM SoCs)
are addressable directly through every `axl_pci_*` API. Single-
segment systems leave it at 0.

`axl_pci_addr_parse` and `axl_pci_addr_format` round-trip an
`AxlPciAddr` through the canonical lower-hex `SSSS:BB:DD.F` form
(same shape as `lspci`). Parse accepts both 3-component
(`bus:dev.func`, segment defaults to 0) and 4-component
(`seg:bus:dev.func`) variants, with bounded-range checks at parse
time:

```c
AxlPciAddr a;
if (axl_pci_addr_parse(argv[1], &a) != 0) { /* malformed */ }

char buf[AXL_PCI_ADDR_STR_MAX];
axl_pci_addr_format(a, buf, sizeof(buf));
axl_printf("device %s\n", buf);
```

## Common header reads

Two boilerplate-killer wrappers around the standard config-space
header offsets — they fold the "is this function absent?" and
"unpack the 24-bit class triplet" patterns into one call:

```c
uint16_t vid, did;
if (axl_pci_get_vid_did(addr, &vid, &did) == 0) {
    /* function is present (vid != 0xFFFF) and both fields read OK */
}

uint32_t class24;  /* (base << 16) | (sub << 8) | prog_if */
axl_pci_get_class24(addr, &class24);
```

`axl_pci_get_vid_did` returns -1 when the function is absent (vid
reads as `0xFFFF`), so callers don't have to special-case the
sentinel. `class24` matches the shape consumed by
`axl_pci_find_by_class`.

## Lookups

```c
AxlPciAddr nic;
if (axl_pci_find_by_vid_did(0x8086, 0x100E, 0, &nic) == 0) {
    /* Intel 82540EM e1000 */
}

AxlPciAddr usb_xhci;
/* class 0x0C 0x03 0x30 = serial bus, USB controller, xHCI prog-if */
axl_pci_find_by_class(0x0C0330, 0, &usb_xhci);
```

## Capabilities

Two cursors — legacy (chain at 0x34) and PCIe extended (chain at
0x100):

```c
uint16_t off = 0;
uint16_t id;
while (axl_pci_cap_next(addr, off, &off, &id) == 0) {
    if (id == 0x10) {
        /* PCIe Capability — PCI Express link/device control */
    }
}

off = 0;
while (axl_pci_ext_cap_next(addr, off, &off, &id) == 0) {
    if (id == 0x0002) {
        /* Virtual Channel */
    }
}
```

## VPD

Vital Product Data (PCI 3.0 §6.4) — keyword-tagged inventory data
exposed by some NICs and storage controllers. AxlPci handles the
F-bit handshake on the address register, the dword-aligned data
window, and the Read-Only / Read-Write tag walk:

```c
uint8_t buf[64];
size_t  len = 0;
if (axl_pci_vpd_read(nic, "PN", buf, sizeof(buf), &len) == 0) {
    /* `len` is the actual on-device length; buf was filled with
       up to sizeof(buf) bytes of it. */
    axl_printf("Part number: %.*s\n", (int)len, buf);
}
```

For "show me everything that's there" — vendor-specific `V0..V9` /
`Y0..Y9` keywords included — use `axl_pci_vpd_iter` and dispatch
through a callback:

```c
static int dump_cb(const char keyword[2], const uint8_t *data,
                   size_t len, void *ctx) {
    (void)ctx;
    axl_printf("  %c%c (%zu): %.*s\n",
               keyword[0], keyword[1], len, (int)len, data);
    return 0;  /* return non-zero to stop the walk early */
}

axl_pci_vpd_iter(nic, dump_cb, NULL);
```

`axl_pci_vpd_iter` and `axl_pci_vpd_read` share the same VPD
walker — one cap-list lookup, one tag walk — so calling either
reflects the same on-device state. The callback's `data` pointer
is only valid for the duration of the call; copy any bytes you
want to retain.
