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

uint32_t class_code;  /* (base << 16) | (sub << 8) | prog_if */
axl_pci_get_class_code(addr, &class_code);
```

`axl_pci_get_vid_did` returns -1 when the function is absent (vid
reads as `0xFFFF`), so callers don't have to special-case the
sentinel. `class_code` matches the shape consumed by
`axl_pci_find_by_class`.

`axl_pci_class_string` decodes the 24-bit class triplet into a
human-readable form per the PCI Code and ID Assignment Spec:

```c
uint32_t class_code;
char     cls[80];
axl_pci_get_class_code(addr, &class_code);
axl_pci_class_string(class_code, cls, sizeof(cls));
axl_printf("Class %06X (%s)\n", class_code, cls);
// Class 0C0330 (Serial bus controller / USB / xHCI)
```

Vendor/device-name lookup is opt-in via a curated JSON5 sidecar —
see "Vendor/device name database" below. The full canonical
`pci.ids` text database (~6 MB) is intentionally out of scope as a
shipped artifact; consumers convert it to the JSON5 schema with
`scripts/pci-ids-to-json5.py` if they need the long tail.

## Config-space dump

`axl_pci_dump` reads up to 4096 bytes (the PCIe ECAM extent) of a
function's config space in 32-bit ECAM-natural chunks. Folds the
endian-pack, absent-detection (VID == 0xFFFF), and ECAM cap into one
call:

```c
uint8_t  buf[256] = { 0 };
size_t   ok       = 0;
if (axl_pci_dump(addr, buf, sizeof(buf), &ok) == 0) {
    axl_hexdump(buf, ok, 0);    // dump only the bytes that read OK
}
```

Returns `-1` on absent functions (no buffer mutation past zeroed
unread bytes); `*out_read` reports how many bytes the caller can
safely consume after a partial dump.

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
    axl_printf("  [%02x] %s\n", off, axl_pci_cap_id_str((uint8_t)id));
}

off = 0;
while (axl_pci_ext_cap_next(addr, off, &off, &id) == 0) {
    axl_printf("  [%03x] %s\n", off, axl_pci_ext_cap_id_str(id));
}
```

`axl_pci_cap_id_str` and `axl_pci_ext_cap_id_str` decode the
standard capability IDs from the PCI Local Bus Spec (PM, MSI,
MSI-X, PCIe, VPD, SATA, ...) and PCIe Base Spec (AER, VC, SR-IOV,
ATS, DPC, ...) respectively. Unknown IDs return `"<unknown>"`;
both lookups are always non-NULL.

Both walks are bounded against malformed/absent-device cap chains:
`axl_pci_cap_next` does a vendor-ID precheck on the entry call
(absent BDF → terminates immediately) and rejects back-pointers
(`next <= prev_off`) at every step. `axl_pci_ext_cap_next` enforces
the same forward-progress guard. Without these, ECAM all-1s reads
on absent BDFs would feed a synthetic header at offset `0xFC` whose
`next` byte is `0xFF`, looping forever — see commit 8b90954.

## Bridges and topology

`axl_pci_bridge_info` reads the bus-number tuple
(`primary` / `secondary` / `subordinate`) from a PCI-PCI bridge's
header — returns -1 cleanly if the function is a type-0 endpoint
or type-2 CardBus rather than a type-1 bridge:

```c
AxlPciBridge br;
if (axl_pci_bridge_info(rp, &br) == 0) {
    /* rp is a PCI-PCI bridge; rp's downstream side is bus br.secondary */
}
```

`axl_pci_tree_for_each` walks the topology in tree order (depth-first
per segment), invoking a callback for every responding function with
its depth and "is this a bridge" flag:

```c
static int print_node(AxlPciAddr a, unsigned depth, bool is_bridge, void *ctx) {
    (void)ctx;
    for (unsigned i = 0; i < depth; i++) axl_print("  ");
    char buf[AXL_PCI_ADDR_STR_MAX];
    axl_pci_addr_format(a, buf, sizeof(buf));
    axl_printf("%s%s\n", buf, is_bridge ? " (bridge)" : "");
    return 0;
}

axl_pci_tree_for_each(print_node, NULL);
```

Bridges are visited immediately before their children, so a renderer
can emit the box-drawing connector without lookahead. Cycle detection
(per-segment visited-bus bitmap) plus a recursion-depth cap
(`AXL_PCI_TREE_MAX_DEPTH = 16`) keep the walker safe against malformed
firmware or hostile bridge configurations — same defense-in-depth
posture as the cap-walk monotonic guard from commit 8b90954.

## Vendor / device / subsystem name database

`axl_pci_ids_load(override_path)` loads a curated JSON5 sidecar
(`pci-ids.json5`). When `override_path` is non-NULL it is used
authoritatively (no fallback — explicit means explicit). When it
is NULL the loader autodiscovers in this order: companion to the
running .efi, then current working directory. axl-sdk ships a
starter set in `share/pci-ids.json5` covering QEMU emulated
devices, common server NICs, NVMe, and GPUs — extend or replace
as your fleet requires.

```c
if (axl_pci_ids_load(NULL) == 0) {
    const char *vendor = axl_pci_vendor_name(0x8086);
    const char *device = axl_pci_device_name(0x8086, 0x29C0);
    const char *card   = axl_pci_subsys_name(0x1028, 0x1FCA);
    /* all NULL-safe; consumers fall back to numeric IDs */
}
```

`axl_pci_ids_load` returns an `AxlSidecarStatus` (defined in
`<axl/axl-sidecar.h>` and shared with AxlSpdIds, AxlUsbIds, and
AxlPciClassDb):

  - `AXL_SIDECAR_OK` on success (idempotent on the second call)
  - `AXL_SIDECAR_FILE_MISSING` if no candidate file exists
  - `AXL_SIDECAR_PARSE_ERROR` if a candidate was found but failed
    to parse

The split lets tools log differently — "no database shipped" is a
deployment problem (numeric fallback is fine), while "parse error"
is an authoring problem worth being loud about. Numeric values
match the legacy `0/-1/-2` ABI, so legacy callers using
`if (rc != 0)` still compile and run; new code uses the named
constants.

Two schema versions supported:

  - **Schema 2** (default for new files) — hierarchical: devices
    nest under their parent vendor, subsystems nest under their
    parent device. Locality of related rows is the win when
    maintaining thousands of entries by hand. The loader also
    accepts a top-level `subsystems[]` block for orphan entries
    the maintainer doesn't know which device to nest under.

  - **Schema 1** (legacy) — flat: three independent top-level
    arrays (`vendors[]`, `devices[]`, `subsystems[]`), each entry
    self-contained. Cheap to parse and easy to generate; awkward
    to hand-maintain at scale.

Both populate the same internal hash tables — lookups are global
on the respective key regardless of which form the file used. The
loader pivots on the `schema` field; an unrecognized schema number
returns `AXL_SIDECAR_PARSE_ERROR` rather than silently misparsing.

Subsystem entries identify the OEM card built around a piece of
silicon; the `(svid, sdid)` pair lives at config-space offsets
0x2C / 0x2E on header-type-0 functions. For the long tail,
`scripts/pci-ids-to-json5.py` converts canonical `pci.ids` text to
this schema (default schema 2; `--schema 1` opts into the flat
layout if you need it; `--vendors-only` filters to a curated
subset).

### Composed-name helper

`axl_pci_format_name(vid, did, buf, buflen)` centralizes the
"vendor + device + numeric tail" rendering convention so every
consumer prints the same string for the same `(vid, did)` pair:

```c
char buf[AXL_PCI_NAME_COMPOSED_MAX];
axl_pci_format_name(0x8086, 0x29C0, buf, sizeof(buf));
// → "Intel Corporation Q35 Host Bridge"
```

Vendor-known + device-unknown produces `"<vendor> Device <DID hex>"`;
vendor-unknown short-circuits to `"<VID>:<DID>"` regardless of
whether the device entry happens to exist.

### Layered databases (handle API)

For consumers that want a "public + private" overlay (private DB
shadows public on `(svid, sdid)` collisions), the handle API lets
you load and query multiple databases:

```c
AxlPciIds *pub  = NULL;
AxlPciIds *priv = NULL;
axl_pci_ids_open("pci-ids.json5",         &pub);
axl_pci_ids_open("private-pci-ids.json5", &priv);

const char *s = axl_pci_ids_subsys_name(priv, svid, sdid);
if (s == NULL) s = axl_pci_ids_subsys_name(pub, svid, sdid);

axl_pci_ids_close(priv);
axl_pci_ids_close(pub);
```

`axl_pci_ids_format_name(handle, vid, did, buf, buflen)` is the
handle-aware equivalent of `axl_pci_format_name`.

For "show me everything in this overlay" (debug dumps, validators,
text exports), use the iterator API — `axl_pci_ids_foreach_vendor`
/ `_device` / `_subsys` walks the loaded entries and propagates a
non-zero callback return as an early stop.

### Per-name length contracts

```
AXL_PCI_VENDOR_NAME_MAX     = 128 bytes
AXL_PCI_DEVICE_NAME_MAX     = 192 bytes
AXL_PCI_SUBSYS_NAME_MAX     = 192 bytes
AXL_PCI_CLASS_NAME_MAX      = 128 bytes
AXL_PCI_NAME_COMPOSED_MAX   = 384 bytes
```

Sized to comfortably hold real `pci.ids` entries; loader silently
truncates over-cap names. Pin `char buf[AXL_PCI_NAME_COMPOSED_MAX]`
on the stack and never have to worry about formatter overflow.

## Class-name overlay (optional sidecar)

For decoding the class triplet itself, the compiled-in tables in
`src/pci/axl-pci.c` are the bootstrap default. A JSON5 sidecar
(`pci-class.json5`) layered on top lets new triplets (CXL Memory
Expanders, future PCIe class assignments, ...) get human names
without rebuilding every consumer:

```c
axl_pci_class_load(NULL);  /* opt-in opportunistic load */
char buf[AXL_PCI_CLASS_NAME_MAX];
axl_pci_class_string_fmt(0x060000, AXL_PCI_CLASS_FMT_FULL,
                         buf, sizeof(buf));
/* overlay consulted first per-tier; compiled-in falls through */
```

Same `-1`/`-2` distinction and override-authoritative semantics as
`axl_pci_ids_load`. Schema in `share/pci-class.json5` — each entry
pins any subset of `(base, sub, prog)`.

`AxlPciClassFmt` selects the output shape:
  - `FMT_FULL` — `"Bridge / Host bridge / <prog>"` (default)
  - `FMT_SUBCLASS` — `"Host bridge"` (Linux lspci shape; collapses
    to base when sub unknown, then numeric)
  - `FMT_BASE` — `"Bridge"` (collapses to numeric when unknown)

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
