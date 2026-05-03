USB device enumeration via `EFI_USB_IO_PROTOCOL`.

Header: `<axl/axl-usb.h>`. Lazy on first call: AxlUsb walks the
firmware-installed USB I/O protocol handles, derives stable
`(bus, addr, intf)` ordinals from each handle's device path, and
caches the result. On platforms without a USB stack (rare; some
constrained BMC firmware) every `axl_usb_*` call returns -1 / NULL
cleanly.

Cursor-style enumeration matches `axl_pci_next` and
`axl_smbios_find_next`:

```c
AxlUsbAddr *u = NULL;
while ((u = axl_usb_next(u)) != NULL) {
    uint16_t vid, pid;
    if (axl_usb_get_vid_pid(*u, &vid, &pid) == 0) {
        axl_printf("Bus %03u Device %03u If %u  %04x:%04x\n",
                   u->bus, u->addr, u->intf, vid, pid);
    }
}
```

`axl_usb_next` emits one entry per `EFI_USB_IO_PROTOCOL` handle —
i.e. one per USB *interface*. A composite device with N interfaces
returns N entries that share `(bus, addr)` and differ only in
`intf`. A consumer that wants one row per physical device should
dedupe on `(bus, addr)` (mirrors what Linux `lsusb` does in its
default short form; see `tools/lsusb.c` for the reference renderer).

## Address tuple

```c
typedef struct {
    uint8_t  bus;    ///< host-controller ordinal (1-based)
    uint8_t  addr;   ///< device ordinal within bus (1-based)
    uint8_t  intf;   ///< interface number from interface descriptor
} AxlUsbAddr;
```

`bus` and `addr` are *synthesized* — UEFI doesn't expose USB topology
the way Linux does, so AxlUsb derives ordinals from each handle's
device path: `bus` numbers each unique host controller in
device-path order; `addr` numbers each unique physical device within
a bus. They're stable within a single boot but may shift across
boots if the firmware enumerates handles in a different order.
Consumers needing a persistent identity should hash the device
descriptor (vid:pid:serial) instead.

`intf` IS the real `bInterfaceNumber` from the interface descriptor.

## Per-device introspection

```c
uint16_t vid, pid;
axl_usb_get_vid_pid(addr, &vid, &pid);

uint8_t cls, sub, prot;
axl_usb_get_class(addr, &cls, &sub, &prot);  // any out param may be NULL

char buf[AXL_USB_STRING_MAX];
axl_usb_get_manufacturer(addr, buf, sizeof(buf));
axl_usb_get_product     (addr, buf, sizeof(buf));
axl_usb_get_serial      (addr, buf, sizeof(buf));
```

`axl_usb_get_vid_pid` reads the device descriptor's `idVendor` /
`idProduct` (so all interfaces of one physical device return the
same pair). `axl_usb_get_class` reads the *interface*'s class
triplet — composite devices set `bDeviceClass = 0` and drive their
identity per interface, so this is the right granularity for the
"one row per interface" walk lsusb does in `-vv` mode.

`axl_usb_get_manufacturer` / `_product` / `_serial` are convenience
wrappers around `axl_usb_get_string(addr, idx, buf, buflen)` that
read the device descriptor's `iManufacturer` / `iProduct` /
`iSerialNumber` index byte first. They return `-1` when the device
declares no string at that slot (index == 0). Each one lazily
probes the device's supported-language table on first use and
caches the first language ID per interface.

## Class triplet decode

```c
char buf[AXL_USB_CLASS_NAME_MAX];
axl_usb_class_string_fmt(0x03, 0x01, 0x02, AXL_USB_CLASS_FMT_FULL,
                         buf, sizeof(buf));
// → "Human Interface Device / Boot Interface / Mouse"
```

`AxlUsbClassFmt` selects the output shape — same posture as
`AxlPciClassFmt`:

  - `FMT_FULL` — `"<base> / <sub> / <prot>"` (default; verbose tools)
  - `FMT_SUBCLASS` — `"<sub>"` (collapses to `<base>` when sub
    unknown, then numeric — Linux lsusb shape)
  - `FMT_BASE` — `"<base>"` (collapses to numeric when unknown)

Tiers with no spec-defined name are omitted rather than rendered as
`<unknown>` placeholders; wholly-unknown class falls back to
`"Class XXXXXX"` numeric. Compiled-in tables in `src/usb/axl-usb-class.c`
cover the USB-IF Defined Class Codes
(<https://www.usb.org/defined-class-codes>) — base classes, common
subclasses (HID Boot, Mass Storage SCSI, CDC variants, ...), and the
most-used protocol bytes (HID Mouse / Keyboard, BBB, UAS). No
sidecar overlay yet (AxlPci has one — `pci-class.json5` — but the
USB compiled-in set covers what we ship today).

## Topology walk

```c
static int print_node(AxlUsbAddr a, unsigned depth, void *ctx) {
    (void)ctx;
    for (unsigned i = 0; i < depth; i++) axl_print("  ");
    axl_printf("Bus %03u Dev %03u If %u\n", a.bus, a.addr, a.intf);
    return 0;
}

axl_usb_tree_for_each(print_node, NULL);
```

`axl_usb_tree_for_each` walks every interface in `(bus, port_chain,
intf)` ascending order — guaranteeing parents arrive before
children, so a renderer can emit indentation directly from `depth`
without lookahead. `depth` is the USB *hub depth*: 0 = directly
attached to the host controller's root hub, 1 = behind one hub, etc.
`AXL_USB_TREE_MAX_DEPTH = 8` caps the recorded chain (USB spec
real-world maximum is 5 hubs / 6 USB nodes).

The walker is built from each handle's EFI device path — the chain
of consecutive `MSG_USB_DP` nodes encodes the full hub-port path —
and the existing dev-key sort guarantees parents-before-children
(parent dev_keys are byte prefixes of children's). `tools/lsusb.c`'s
`-t` mode is the reference consumer.

## Vendor / device name database

`axl_usb_ids_load(override_path)` loads a curated JSON5 sidecar
(`usb-ids.json5`). When `override_path` is non-NULL it is used
authoritatively (no fallback — explicit means explicit). When it is
NULL the loader autodiscovers in this order: companion to the
running .efi, then current working directory. axl-sdk ships a
starter set in `share/usb-ids.json5` covering common HID, NIC, hub,
and storage vendors; bulk-extract from canonical `usb.ids` via
`scripts/usb-ids-to-json5.py`.

```c
if (axl_usb_ids_load(NULL) == AXL_SIDECAR_OK) {
    const char *vendor = axl_usb_vendor_name(0x046D);
    const char *device = axl_usb_device_name(0x046D, 0xC52B);
    /* both NULL-safe; consumers fall back to numeric IDs */
}
```

`axl_usb_ids_load` returns:

  - `AXL_SIDECAR_OK` on success (idempotent on subsequent calls)
  - `AXL_SIDECAR_FILE_MISSING` if no candidate file exists
  - `AXL_SIDECAR_PARSE_ERROR` if a candidate was found but failed
    to parse

The split lets tools log differently — "no database shipped" is a
deployment problem (numeric fallback is fine), while "parse error"
is an authoring problem worth being loud about. See
`<axl/axl-sidecar.h>` for the shared status enum.

Schema 1 is the only supported layout — hierarchical from the
start (vendors with nested devices). USB has no subsystem dimension
that motivated AxlPciIds's v1 (flat) → v2 (hierarchical) split.

```json5
{
    schema: 1,
    vendors: [
        { id: 0x046D, name: 'Logitech, Inc.',
          devices: [
            { pid: 0xC52B, name: 'Unifying Receiver' },
          ],
        },
    ],
}
```

### Composed-name helper

`axl_usb_format_name(vid, pid, buf, buflen)` centralizes the
"vendor + device + numeric tail" rendering convention so every
consumer prints the same string for the same `(vid, pid)` pair:

```c
char buf[AXL_USB_NAME_COMPOSED_MAX];
axl_usb_format_name(0x046D, 0xC52B, buf, sizeof(buf));
// → "Logitech, Inc. Unifying Receiver"
```

Vendor-known + device-unknown produces `"<vendor> Device <pid hex>"`;
vendor-unknown short-circuits to `"<vid>:<pid>"` regardless of
whether a device entry happens to exist (without a verified vendor
the device hit is ambiguous provenance).

### Layered databases (handle API)

Same shape as AxlPciIds. Consumers that ship a private OEM sheet
on top of the public set load two handles and query in priority
order:

```c
AxlUsbIds *pub  = NULL;
AxlUsbIds *priv = NULL;
axl_usb_ids_open("usb-ids.json5",         &pub);
axl_usb_ids_open("private-usb-ids.json5", &priv);

const char *d = axl_usb_ids_device_name(priv, vid, pid);
if (d == NULL) d = axl_usb_ids_device_name(pub, vid, pid);

axl_usb_ids_close(priv);
axl_usb_ids_close(pub);
```

`axl_usb_ids_format_name(handle, vid, pid, buf, buflen)` is the
handle-aware equivalent of `axl_usb_format_name`. For "show me
everything in this overlay" use `axl_usb_ids_foreach_vendor` /
`_foreach_device`.

### Per-name length contracts

```
AXL_USB_VENDOR_NAME_MAX     = 128 bytes
AXL_USB_DEVICE_NAME_MAX     = 192 bytes
AXL_USB_NAME_COMPOSED_MAX   = 384 bytes
AXL_USB_CLASS_NAME_MAX      = 128 bytes
AXL_USB_STRING_MAX          = 384 bytes  (USB string descriptors;
                                          127 BMP chars * 3 UTF-8
                                          bytes + NUL; BMP only)
```

Sized to comfortably hold real `usb.ids` entries; loader silently
truncates over-cap names. Pin
`char buf[AXL_USB_NAME_COMPOSED_MAX]` on the stack and never have
to worry about formatter overflow.

## Bulk population from canonical usb.ids

The shipped `share/usb-ids.json5` is a curated starter set (~22
vendors). For fleet-scale OEM-rebadge coverage, run the conversion
against the canonical usb.ids:

```bash
# Full set:
scripts/usb-ids-to-json5.py /usr/share/hwdata/usb.ids \
    > usb-ids.json5

# Curated subset (vendor entries always emitted; only their
# devices are dropped for vendors not in the list):
scripts/usb-ids-to-json5.py --vendors-only 046d,0bda,1d6b \
    /usr/share/hwdata/usb.ids > usb-ids.json5

# Verify the script itself:
scripts/usb-ids-to-json5.py --self-test
```

The `.deb` / `.rpm` install the converter under
`/usr/share/axl/scripts/`; the line-level parser is shared with
`pci-ids-to-json5.py` via `_ids_parser.py`.
