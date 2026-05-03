# AxlUsb consumer integration guide

One-page playbook for tools and libraries integrating the USB
enumeration + descriptor reads + vendor/device-name decoders. The
full API reference lives in `axl/axl-usb.h`; the module README
covers the schema and rationale. This document is the *integration
recipe* — what to call, when, and what to expect when the database
or the USB stack itself is absent.

## Lifecycle

```c
#include <axl/axl-usb.h>

int main(int argc, char **argv) {
    /* Once at startup. NULL = autodiscover via companion path
       (next to the running .efi) then cwd. Pass an explicit path
       to override (authoritative; no fallback). */
    AxlSidecarStatus rc = axl_usb_ids_load(NULL);
    if (rc == AXL_SIDECAR_FILE_MISSING) {
        /* Deployment problem — sidecar absent. Fine to ignore;
           lookups will return NULL → numeric fallback. */
    } else if (rc == AXL_SIDECAR_PARSE_ERROR) {
        /* Authoring problem — file exists but malformed. Worth
           logging loudly so the maintainer fixes it. */
        axl_warning("usb-ids.json5 failed to parse");
    }

    /* ... use the lookups ... */

    /* Optional but tidy — frees the hash tables. axl_usb_ids_load
       registers an atexit hook so process exit reclaims them
       automatically; consumers that exit immediately after the
       last lookup can skip this. */
    axl_usb_ids_free();
    return 0;
}
```

## What happens when the database is absent

Every `*_name` lookup returns `NULL`. The composed-name helper
falls back gracefully — call it instead of rolling per-call
boilerplate:

```c
char buf[AXL_USB_NAME_COMPOSED_MAX];
axl_usb_format_name(0x046D, 0xC52B, buf, sizeof(buf));
/* DB loaded → "Logitech, Inc. Unifying Receiver"
   DB absent → "046d:c52b"
   vendor known + device unknown → "<vendor> Device <pid hex>" */
```

For the class triplet, `axl_usb_class_string_fmt` falls back to
the compiled-in tables (no overlay is loaded — USB ships with the
USB-IF Defined Class Codes baked in). Wholly-unknown class triplets
render as `"Class XXXXXX"` numeric.

## What happens when no USB stack is available

`axl_usb_next` returns `NULL` on the first call (no
`EFI_USB_IO_PROTOCOL` handles installed). Real-hardware platforms
without USB controllers, or QEMU machines built without
`-device qemu-xhci`, hit this path cleanly. Walking with an outer
SKIP guard is the standard pattern:

```c
AxlUsbAddr *u = axl_usb_next(NULL);
if (u == NULL) {
    axl_print("(no USB devices)\n");
    return 0;
}
```

## Output convention (every consumer should match)

Use the helpers, not local string composition:

  - **Vendor + device**: `axl_usb_format_name`
  - **Class triplet**: `axl_usb_class_string_fmt(cls, sub, prot, fmt, ...)`
    where `fmt` is `AXL_USB_CLASS_FMT_FULL` for verbose tools,
    `AXL_USB_CLASS_FMT_SUBCLASS` for row-oriented tools (Linux lsusb
    shape), `AXL_USB_CLASS_FMT_BASE` for coarse categorization.
  - **Hex literals**: lowercase, 4-wide, zero-padded (matches
    Linux lsusb's `-d` filter convention).

If two consumers render the same `(vid, pid)` differently, one of
them is rolling its own composition — fix by switching to
`axl_usb_format_name`.

## Per-device vs per-interface walks

`axl_usb_next` emits one entry per `EFI_USB_IO_PROTOCOL` handle —
i.e. one per *interface*. Three usage patterns:

```c
/* (1) Show every interface (Linux `lsusb -vv` shape). */
AxlUsbAddr *u = NULL;
while ((u = axl_usb_next(u)) != NULL) {
    /* render row */
}

/* (2) Dedupe to one row per device (default `lsusb` shape). Keep
   prev_(bus, addr); skip when both match. The cursor's sort
   guarantees consecutive same-device interfaces are adjacent. */
AxlUsbAddr  *u = NULL;
uint8_t      prev_bus = 0, prev_addr = 0;
bool         have_prev = false;
while ((u = axl_usb_next(u)) != NULL) {
    if (have_prev && u->bus == prev_bus && u->addr == prev_addr) {
        continue;
    }
    /* render row for this device */
    prev_bus = u->bus; prev_addr = u->addr; have_prev = true;
}

/* (3) Tree view (real hub-port chain). */
axl_usb_tree_for_each(my_render_cb, &ctx);
```

`axl_usb_get_vid_pid` returns the same `(vid, pid)` pair for every
interface of a single physical device (it's a device-descriptor
field, not an interface-descriptor field), so dedupe by
`(bus, addr)` is safe.

## Layered databases (public + private overlay)

For consumers that ship an internal `(vid, pid)` sheet on top of
the public set:

```c
AxlUsbIds *public_db  = NULL;
AxlUsbIds *private_db = NULL;
axl_usb_ids_open("usb-ids.json5",         &public_db);
axl_usb_ids_open("private-usb-ids.json5", &private_db);

/* Priority lookup — private shadows public on collision. */
const char *d = axl_usb_ids_device_name(private_db, vid, pid);
if (d == NULL) d = axl_usb_ids_device_name(public_db, vid, pid);

/* Same pattern for vendor_name / format_name. The handle API has
   full parity with the singleton — open / close / vendor_name /
   device_name / foreach_* / format_name. */

axl_usb_ids_close(private_db);
axl_usb_ids_close(public_db);
```

## Thread safety

Single-threaded contract — same posture as AxlPciIds. The singleton
holds a module-static handle; two threads loading concurrently can
race and leak. UEFI is single-threaded with cooperative event
dispatch, so the constraint matches the deployment environment.

If a consumer needs hot-swap of the database (e.g. reload after a
sidecar update), serialize the `_free` + `_load` pair against
every reader.

## Where the sidecars live in shipped artifacts

  - **Tools tarball** (`axl-sdk-tools-<arch>.tar.gz`): `usb-ids.json5`
    ships next to `lsusb.efi` at the tarball root. UEFI auto-
    discovery via the companion-path lookup finds it with no flags
    or paths needed — boot from the USB stick, run `lsusb.efi`,
    names decode.
  - **`.deb` / `.rpm`** (axl-sdk SDK package): installs to
    `/usr/share/axl/usb-ids.json5`. Reference content — copy or
    symlink to wherever your `.efi` runs from, or pass via
    `--ids-file`.
  - **Source tree** (`share/usb-ids.json5`): the curated starter
    set lives here. Edit in place when contributing additions; the
    build / install / release pipeline picks up changes on the
    next pass.

## Bulk population from canonical usb.ids

The shipped `share/usb-ids.json5` is a small curated starter
(~22 vendors). For fleet-scale OEM-rebadge coverage, run the
conversion against the canonical usb.ids:

```bash
# Full set:
scripts/usb-ids-to-json5.py /usr/share/hwdata/usb.ids \
    > usb-ids.json5

# Curated subset (drops devices for vendors not in the list;
# vendors themselves are always emitted):
scripts/usb-ids-to-json5.py --vendors-only 046d,0bda,1d6b \
    /usr/share/hwdata/usb.ids > usb-ids.json5

# Verify the script itself:
scripts/usb-ids-to-json5.py --self-test
```

The `.deb` / `.rpm` install the converter under
`/usr/share/axl/scripts/`. The line-level parser is shared with
`pci-ids-to-json5.py` via `_ids_parser.py` — pci.ids and usb.ids
use the same plain-text format.

## Testing your integration

The integration test runner (`test/integration/test-axl.sh`) stages
`share/usb-ids.json5` next to test EFIs and wires QEMU with
`qemu-xhci` + `usb-mouse` + `usb-hub` + `usb-tablet` so the
populated branch of every test runs in CI. Consumer tests can
exercise the lookup paths against the known curated entries (e.g.
`0x0627 → "Adomax Technology Co., Ltd"`).

For consumer-side fixtures that need controlled input, prefer the
buffer API (`axl_usb_ids_open_from_buffer`) over staging additional
files — JSON5 fixtures embedded in the test source are easier to
keep in sync with the assertions.
