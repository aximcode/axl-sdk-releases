# AxlPci consumer integration guide

One-page playbook for tools and libraries integrating the
vendor / device / subsystem / class name decoders. The full API
reference lives in `axl/axl-pci.h`; the module README covers the
schema and rationale. This document is the *integration recipe* —
what to call, when, and what to expect when the database is absent.

## Lifecycle

```c
#include <axl/axl-pci.h>

int main(int argc, char **argv) {
    /* Once at startup. NULL = autodiscover via companion path
       (next to the running .efi) then cwd. Pass an explicit path
       to override (authoritative; no fallback). */
    AxlSidecarStatus rc = axl_pci_ids_load(NULL);
    if (rc == AXL_SIDECAR_FILE_MISSING) {
        /* Deployment problem — sidecar absent. Fine to ignore;
           lookups will return NULL → numeric fallback. */
    } else if (rc == AXL_SIDECAR_PARSE_ERROR) {
        /* Authoring problem — file exists but malformed. Worth
           logging loudly so the maintainer fixes it. */
        axl_warning("pci-ids.json5 failed to parse");
    }

    /* Optional class-overlay — reads the classes[] section of
       pci-ids.json5 (same file). Compiled-in tables in axl-pci.c
       are the bootstrap default, so this is purely additive (new
       triplet names land via a `git pull` of pci-ids.json5
       instead of a rebuild). */
    (void)axl_pci_class_load(NULL);

    /* ... use the lookups ... */

    /* Optional but tidy — frees the hash tables. Process exit
       reclaims them anyway, so consumers that exit immediately
       after the last lookup can skip this. */
    axl_pci_class_free();
    axl_pci_ids_free();
    return 0;
}
```

## What happens when the database is absent

Every `*_name` lookup returns `NULL`. The composed-name helper
falls back gracefully — call it instead of rolling per-call
boilerplate:

```c
char buf[AXL_PCI_NAME_COMPOSED_MAX];
axl_pci_format_name(0x8086, 0x29C0, buf, sizeof(buf));
/* DB loaded → "Intel Corporation Q35 Host Bridge"
   DB absent → "8086:29c0"
   vendor known + device unknown → "<vendor> Device <did hex>" */
```

For the class triplet, `axl_pci_class_string_fmt` falls back to
the compiled-in tables (no overlay needed for the common cases) and
ultimately to `"Class XXXXXX"` numeric for genuinely unknown codes.

## Output convention (every consumer should match)

Use the helpers, not local string composition:

  - **Vendor + device**: `axl_pci_format_name`
  - **Class triplet**: `axl_pci_class_string_fmt(code, fmt, ...)`
    where `fmt` is `AXL_PCI_CLASS_FMT_FULL` for verbose tools, `AXL_PCI_CLASS_FMT_SUBCLASS`
    for row-oriented tools (Linux lspci shape), `AXL_PCI_CLASS_FMT_BASE` for
    coarse categorization.
  - **Subsystem**: `axl_pci_subsys_name(svid, sdid)` — caller
    composes display string (typically `"<oem-name> [svid:sdid]"`
    when name is known, just `"svid:sdid"` otherwise).
  - **Hex literals**: lowercase, 4-wide, zero-padded (matches
    Linux lspci convention; the helpers do this for you).

If two consumers render the same `(vid, did)` differently, one of
them is rolling its own composition — fix by switching to
`axl_pci_format_name`.

## Layered databases (public + private overlay)

For consumers that ship an internal SVID:SDID sheet on top of the
public set:

```c
AxlPciIds *public_db  = NULL;
AxlPciIds *private_db = NULL;
axl_pci_ids_open("pci-ids.json5",         &public_db);
axl_pci_ids_open("private-pci-ids.json5", &private_db);

/* Priority lookup — private shadows public on collision. */
const char *s = axl_pci_ids_subsys_name(private_db, svid, sdid);
if (s == NULL) s = axl_pci_ids_subsys_name(public_db, svid, sdid);

/* Same pattern for vendor_name / device_name / format_name (use
   the handle-aware variants axl_pci_ids_*_name and
   axl_pci_ids_format_name). */

axl_pci_ids_close(private_db);
axl_pci_ids_close(public_db);
```

Both handle and singleton APIs return NULL for unknown lookups, so
the priority chain doesn't need per-call NULL guards beyond the
`if (s == NULL)` step. The handle API has full parity with the
singleton — vendor, device, subsystem, format_name, plus the iter
helpers (`axl_pci_ids_foreach_*`).

## Thread safety

Single-threaded contract. The singleton (`axl_pci_ids_load` /
`axl_pci_class_load` and friends) holds module-static handles —
two threads loading concurrently can race and leak. Two threads
querying concurrently after a successful load are read-only and
safe in practice (the hash table is not mutated post-load), but
that's not a written guarantee. UEFI is single-threaded with
cooperative event dispatch, so the constraint matches the
deployment environment.

If a consumer needs hot-swap of the database (e.g. reload after a
sidecar update), serialize the `_free` + `_load` pair against
every reader.

## Where the sidecar lives in shipped artifacts

  - **Tools tarball** (`axl-sdk-tools-<arch>.tar.gz`):
    `pci-ids.json5` ships next to the .efi binaries at the tarball
    root. UEFI auto-discovery via the companion-path lookup finds
    it with no flags or paths needed — boot from the USB stick,
    run `lspci.efi`, names decode.
  - **`.deb` / `.rpm`** (axl-sdk SDK package): file installs to
    `/usr/share/axl/pci-ids.json5`. Reference content — copy or
    symlink to wherever your .efi runs from, or pass via
    `--ids-file`.
  - **Source tree** (`share/pci-ids.json5`): the curated starter
    set lives here. Edit in place when contributing additions; the
    build/install/release pipeline picks up changes on the next
    pass.

## Bulk population from canonical pci.ids

The shipped `share/pci-ids.json5` is a tiny starter set (curated
for QEMU + a few common cards). For fleet-scale OEM-rebadge
coverage, run the conversion against the canonical pci.ids:

```bash
# Schema 2 (default) — hierarchical, both vendors[] and classes[]
# in one file. Recommended for hand-edit on top.
scripts/pci-ids-to-json5.py /path/to/pci.ids > pci-ids.json5

# Curated subset (drops devices / subsystems for vendors not in
# the list; vendors themselves are always emitted):
scripts/pci-ids-to-json5.py --vendors-only 8086,1022,10de,14e4,15b3 \
    /path/to/pci.ids > pci-ids.json5

# Schema 1 (legacy) — flat vendors-only layout for back-compat
# with old generated files. Drops the classes[] section.
scripts/pci-ids-to-json5.py --schema 1 /path/to/pci.ids \
    > pci-ids-legacy.json5

# Verify the script itself:
scripts/pci-ids-to-json5.py --self-test
```

## Testing your integration

The integration test runner (`test/integration/test-axl.sh`) stages
`share/pci-ids.json5` next to test EFIs. Consumer tests can
exercise the lookup paths against the known curated entries
(e.g. `0x8086:0x29C0` decodes to `"Intel Corporation Q35 Host
Bridge"`).

For consumer-side fixtures that need controlled input, prefer the
buffer API (`axl_pci_ids_open_from_buffer` / `axl_pci_class_open_from_buffer`)
over staging additional files — JSON5 fixtures embedded in the test
source are easier to keep in sync with the assertions.
