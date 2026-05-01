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

`uint16_t seg` future-proofs against multi-segment platforms.
Single-segment systems leave it at 0.

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
