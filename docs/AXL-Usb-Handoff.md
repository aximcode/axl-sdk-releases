# AxlUsb + lsusb Handoff

Session bring-up notes for the next contributor (or Claude session)
picking up the AxlUsb library + `lsusb.efi` tool work. Phase B3 R+5
follow-on; sequenced after the PCI tooling that landed in v0.10.0.

## Where things stand (2026-05-02)

- **v0.10.0 just shipped** (commit `de729a8`, tag `v0.10.0`,
  release at https://github.com/aximcode/axl-sdk-releases/releases/tag/v0.10.0)
  with the PCI tooling complete: lspci + axl-pci-ids + axl-pci-class
  + JSON5 sidecars + the conversion script. Tests at 2309/2309
  both arches. Tip of `main` is on the v0.10.0 commit.
- **AxlUsb + lsusb is unstarted**. ROADMAP entries already exist
  at `docs/ROADMAP.md` under Phase B3 R+5.
- **The PCI work is the template.** Read it first; the AxlUsb
  surface should follow the same patterns (handle API + singleton
  wrapper, JSON5 sidecar, length contracts, load -1/-2 split,
  iter API, composed-name helper, format-shape enum, CONSUMERS.md).

## The PCI pattern — read these first

The next session should walk through these in order before writing
any code; they document the public surface, schema, loader shape,
test pattern, and integration playbook the AxlUsb work should
mirror:

1. `include/axl/axl-pci.h` — public API surface (handle types,
   length contracts, lookups, format flags, CONSUMERS pointers).
2. `src/pci/axl-pci-ids.c` — handle implementation, schema dispatch
   (v1 flat / v2 hierarchical), parse helpers, singleton shim.
3. `src/pci/axl-pci-class.c` — overlay sidecar pattern (compiled-in
   tables + optional JSON5 overlay).
4. `share/pci-ids.json5` + `share/pci-class.json5` — JSON5 sidecar
   shape (schema field is REQUIRED).
5. `scripts/pci-ids-to-json5.py` — bulk conversion tool with
   `--self-test`, `--schema`, `--vendors-only`, `--emit-class FILE`.
6. `src/pci/CONSUMERS.md` — one-page integration guide. AxlUsb
   should ship its own equivalent.
7. `tools/lspci.c` — tool over the platform API, including the
   `--bridges`-style runner staging considerations.
8. `test/unit/axl-test-platform.c` — search for `test_pci_ids_*`
   and `test_pci_class_*` to see how each capability is pinned.

## What AxlUsb needs

Per the ROADMAP entry:

> USB device enumeration via `EFI_USB_IO_PROTOCOL` handles. Standard
> descriptor walk (device → config → interface → endpoint),
> class/subclass/protocol decode tables, optional string descriptor
> reads (manufacturer / product / serial). Header: `axl/axl-usb.h`.
> Source: `src/usb/`. Sequenced after lspci so the "tool over
> existing platform API" pattern is validated on the simpler PCI
> surface first.

### Suggested public API shape (mirror PCI)

```c
// include/axl/axl-usb.h

// Length contracts
#define AXL_USB_VENDOR_NAME_MAX     128u
#define AXL_USB_DEVICE_NAME_MAX     192u
#define AXL_USB_CLASS_NAME_MAX      128u
#define AXL_USB_NAME_COMPOSED_MAX   384u
// String descriptor caps (manufacturer / product / serial)
#define AXL_USB_STRING_MAX          384u

// Enumeration
typedef struct { uint8_t bus; uint8_t addr; } AxlUsbAddr;
AxlUsbAddr *axl_usb_next(AxlUsbAddr *prev);  // cursor-style; mirrors axl_pci_next

// Per-device introspection
int axl_usb_get_vid_pid(AxlUsbAddr addr, uint16_t *vid, uint16_t *pid);
int axl_usb_get_class(AxlUsbAddr addr, uint8_t *class, uint8_t *sub, uint8_t *prot);
int axl_usb_get_string(AxlUsbAddr addr, uint8_t string_index,
                       char *buf, size_t buflen);
// Common shortcuts that compose the get_string + descriptor reads:
int axl_usb_get_manufacturer(AxlUsbAddr addr, char *buf, size_t buflen);
int axl_usb_get_product(AxlUsbAddr addr, char *buf, size_t buflen);
int axl_usb_get_serial(AxlUsbAddr addr, char *buf, size_t buflen);

// Class triplet decode (parallel to axl_pci_class_string_fmt)
typedef enum {
    AXL_USB_CLASS_FMT_FULL     = 0,  // "HID / Keyboard / Boot Protocol"
    AXL_USB_CLASS_FMT_SUBCLASS = 1,  // "Keyboard"
    AXL_USB_CLASS_FMT_BASE     = 2,  // "HID"
} AxlUsbClassFmt;
int axl_usb_class_string_fmt(uint8_t class, uint8_t sub, uint8_t prot,
                             AxlUsbClassFmt fmt, char *buf, size_t buflen);

// Vendor / device / class name database (JSON5 sidecar, mirrors AxlPciIds)
typedef struct AxlUsbIds AxlUsbIds;
int  axl_usb_ids_open(const char *path, AxlUsbIds **out);
int  axl_usb_ids_open_from_buffer(const char *json5, size_t len,
                                  AxlUsbIds **out);
void axl_usb_ids_close(AxlUsbIds *ids);
const char *axl_usb_ids_vendor_name(const AxlUsbIds *ids, uint16_t vid);
const char *axl_usb_ids_device_name(const AxlUsbIds *ids,
                                    uint16_t vid, uint16_t pid);
int  axl_usb_ids_format_name(const AxlUsbIds *ids,
                             uint16_t vid, uint16_t pid,
                             char *buf, size_t buflen);
// Iter
typedef int (*AxlUsbIdsVendorFn)(uint16_t vid, const char *name, void *ctx);
typedef int (*AxlUsbIdsDeviceFn)(uint16_t vid, uint16_t pid,
                                 const char *name, void *ctx);
int axl_usb_ids_foreach_vendor(const AxlUsbIds *ids,
                               AxlUsbIdsVendorFn fn, void *ctx);
int axl_usb_ids_foreach_device(const AxlUsbIds *ids,
                               AxlUsbIdsDeviceFn fn, void *ctx);

// Singleton (thin shim over an internal handle)
int  axl_usb_ids_load(const char *override_path);  // 0/-1/-2 like PCI
void axl_usb_ids_free(void);
const char *axl_usb_vendor_name(uint16_t vid);
const char *axl_usb_device_name(uint16_t vid, uint16_t pid);
int  axl_usb_format_name(uint16_t vid, uint16_t pid,
                         char *buf, size_t buflen);

// Optional class-name overlay (mirrors axl_pci_class_load)
int  axl_usb_class_load(const char *override_path);
void axl_usb_class_free(void);
```

### Key differences from PCI (don't blindly copy)

- **PID, not DID**. The USB convention is `vid:pid`, not `vid:did`.
  Use `pid` field names throughout. Schema entries use `pid` not
  `did`.
- **String descriptors live on the device, not in a database.**
  `axl_usb_get_manufacturer` etc. read the device's own string
  descriptors via control transfers; they're orthogonal to the
  `usb-ids.json5` lookup. lsusb shows BOTH (database name + device
  string) when verbose.
- **Class hierarchy is different**. USB has fewer base classes
  (~25 vs PCI's ~40). The compiled-in tables in `src/usb/axl-usb.c`
  will be smaller than `src/pci/axl-pci.c`'s.
- **No subsystem analog**. USB has no `(svid, sdid)` equivalent —
  device is identified by (vid, pid) alone. Skip the subsystem
  schema array.
- **Bus topology is different**. USB topology is a tree of hubs;
  consider whether `axl_usb_tree_for_each` makes sense for hub
  walks (probably yes, mirroring `axl_pci_tree_for_each`).
- **EFI_USB_IO_PROTOCOL is per-interface**, not per-device.
  Multiple `EFI_USB_IO_PROTOCOL` handles can exist for one
  physical device (one per interface). Decide early how
  `AxlUsbAddr` distinguishes interfaces — likely
  `{ bus, dev, intf }` rather than `{ bus, dev }` alone, OR
  collapse to per-device with the per-interface info exposed
  via a separate `axl_usb_for_each_interface` walker. Look at
  what Linux lsusb does and follow.

## Schema for usb-ids.json5

Mirror `pci-ids.json5` schema 2 hierarchical:

```js
{
    schema: 1,
    vendors: [
        { id: 0x046D, name: 'Logitech, Inc.',
          devices: [
            { pid: 0xC52B,
              name: 'Unifying Receiver' },
            { pid: 0xC077,
              name: 'M105 Optical Mouse' },
          ],
        },
    ],
}
```

Schema 1 because USB has no subsystem dimension that motivated
PCI's v1→v2 migration. Skip the v1/v2 split entirely; just ship
hierarchical from the start.

`scripts/usb-ids-to-json5.py` should mirror `pci-ids-to-json5.py` —
parse canonical `usb.ids` from https://www.linux-usb.org/usb.ids
(separate database from pci.ids). Same `--vendors-only` filter,
`--self-test` mode, optional `--emit-class FILE` for the class-name
overlay if you also do an `axl_usb_class_load` companion.

## Test infrastructure

- **QEMU has USB**. The runner already enables `-device qemu-xhci`
  implicitly via the q35 / virt machine config — there's typically
  a USB controller and (depending on build) some emulated devices.
  Test runner doesn't currently inject specific USB devices the
  way it does for PCI bridges (`pcie-root-port`); you may need to
  extend `test/integration/common-test.sh` similarly. Reasonable
  starter: `-device usb-mouse` or `-device usb-storage,drive=…`.
- `test/unit/axl-test-platform.c` is the right home for AxlUsb
  unit tests (parallel to the `test_pci_*` family there).
- Stage `share/usb-ids.json5` in `test/integration/test-axl.sh`
  the same way `pci-ids.json5` is staged today.
- Auto-stage in `scripts/run-qemu.sh` for interactive smoke tests.

## Tool: `tools/lsusb.c`

Mirror `tools/lspci.c`:

```
lsusb [-t] [-s BBB:DDD] [-d V[:P]] [-n] [-v[v[v]]]
      [--ids-file PATH] [--debug]
```

Default short form (Linux lsusb shape):
```
Bus 001 Device 003: ID 046d:c52b Logitech, Inc. Unifying Receiver
```

`-t` for tree (hub topology). `-v` for full descriptor dump
(device → config → interface → endpoint).

Same `--debug` / `-v` divergence as lspci (Linux lsusb's
`-v` is detail-level, not log-verbosity — match it).

## Workflow rules to follow (auto-loaded via memory)

These are codified in `axl-sdk/CLAUDE.md` and live in cross-session
memory; the new session inherits them automatically:

- **Test-first development** for all new public API. Watch RED
  before implementing GREEN. Exact-string assertions for output
  tests (`axl_strcmp`, NOT `axl_strstr`).
- **Don't hesitate to change axl-sdk APIs** when they create
  friction during implementation — the singleton-fallback issue
  in pci-ids was a textbook case.
- **No downstream consumer names** in code/docs/data files. Use
  generic terms ("OEM", "downstream consumer", "diagnostic tool")
  rather than naming a specific consumer or its tool.
- **Independent code review** before each commit (general-purpose
  agent pass between "all green" and `git commit`). Catches the
  bugs tests miss.
- **Balanced SKIP counts** for cross-arch ratchet stability.

## Pitfalls / things to know going in

- The Makefile uses `ar rcs` for libaxl.a — after any structural
  change to a public-header struct, run `make clean && make tests`
  before trusting the ratchet. (See `axl-sdk/CLAUDE.md`'s "Build"
  section for the v0.7.3 anecdote.)
- Adding a new C file to a module requires updating `Makefile`'s
  source list — the `axl-pci-class.c` Phase E shipped initially
  with the symbols undefined because I forgot the Makefile entry
  (caught by the test runner crashing).
- `axl_file_info(path, NULL)` rejects NULL `info` — pass a stack-
  allocated `AxlFileInfo` even if you don't need the metadata.
  (See commit `2d30e91`'s memory note.)
- `axl_resolve_data_file` falls back through override → companion
  → cwd. For axl-sdk's load functions, **explicit override should
  be authoritative** (no fallback) so `-1` distinguishes "missing"
  from `-2` "parse error" cleanly. See `axl_pci_ids_load`'s docstring.

## Suggested first commits

1. **Read** the PCI files listed above (~30 min).
2. **Draft** `include/axl/axl-usb.h` — header + docstrings only,
   no impl. Get the API surface right first.
3. **Skeleton** `src/usb/axl-usb.c` with `EFI_USB_IO_PROTOCOL`
   enumeration via `LocateHandleBuffer`. First milestone:
   `axl_usb_next()` returns at least one device on a QEMU machine
   with a usb-mouse or usb-kbd attached.
4. **TDD-first**: write the failing tests for `axl_usb_next` +
   `axl_usb_get_vid_pid` in `test/unit/axl-test-platform.c`,
   confirm RED, then implement.
5. Iterate from there: descriptor walks, string reads, class
   decode, ids loader, lsusb tool, sidecar shipping, release.

Don't try to land the whole thing in one commit chain — the PCI
work was 23 commits over many sessions. Phase A might be
"enumeration + vid_pid + a couple unit tests." Phase B might be
class decode + descriptor reads. Etc.

## Anchors for the new session

- **Last release**: v0.10.0 (commit `de729a8`)
- **Tip of main**: `de729a8 release: v0.10.0`
- **Tests**: 2309/2309 both arches at the tip
- **ROADMAP entries**: see `docs/ROADMAP.md`'s Phase B3 R+5 block
  (look for the `[ ] **AxlUsb**` and `[ ] **tools/lsusb.c**` items)
- **This handoff**: `docs/AXL-Usb-Handoff.md`
- **Auto-memory**: `~/.claude/projects/-home-mgosha-projects-aximcode-axl-sdk/memory/`
  — the workflow rules and previous-session learnings load
  automatically.

Good luck. Read the PCI work first; the patterns it established
are load-bearing for the AxlUsb design.
