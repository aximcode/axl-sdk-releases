FAT RAM disks over `EFI_RAM_DISK_PROTOCOL`.

Header: `<axl/axl-ramdisk.h>`. A RAM disk is a block of allocated
memory registered with the firmware as a virtual block device,
FAT-formatted so the firmware binds its filesystem driver and the
volume appears as an `fsN:` mapping — scratch space, file staging, or
the backing store of a virtual-media feature.

This module is the create/list/destroy orchestration lifted out of the
`mkrd` tool (FAT16/FAT32 formatters, page allocation, `Register` /
`Unregister`, volume enumeration, the duplicate-label check) so any
consumer reuses it instead of copying ~250 lines from a tool. `mkrd` is
now a thin CLI over it.

```c
// Ensure the protocol is available (firmware -> disk -> embedded
// fallback), then create a 64 MB disk labelled "SCRATCH".
axl_ramdisk_ensure_driver(NULL, 0, NULL);
void *dp = NULL;
if (axl_ramdisk_create("SCRATCH", 64, &dp) == AXL_OK) {
    // ... fsN: now resolves to the new FAT volume ...
}
```

### Scriptable use from a non-interactive `.nsh`

A read-only boot medium (a locked USB key / floppy image) can't hold a
scratch file, so a `startup.nsh` makes a RAM disk and writes there. The
snag is that a freshly created disk isn't in the shell's map, and the
shell only assigns `fsN` names at startup or on an explicit `map -r`.
`mkrd <LABEL>` solves this by **assigning the shell map names itself**
(`EFI_SHELL_PROTOCOL.SetMap`), so the volume is a normal-looking `fsN:` in
**one call, with no `map -r`**:

```
mkrd SCRATCH             # shows in `map` as `FS<n>: Alias(s):SCRATCH:`
FS<n>:  (or)  SCRATCH:   # switch to it — bare `<name>:` verb, NOT `cd`
echo ... > last.nsh      # write to the writable RAM disk
```

A bare `map` then lists it exactly like a firmware volume:

```
FS3: Alias(s):SCRATCH:
    VirtualDisk(0x...,...)
```

**Two names, not one.** `mkrd` makes **two** `SetMap` calls on the disk's
device path, and order matters (the shell shows the *first-inserted* name as
the primary):

1. the **next-free `FS<n>`** first — becomes the primary name a bare `map`
   displays (matching the shell's own `fsN` convention), then
2. the **volume `<LABEL>`** second — lands in the `Alias(s):` column.

Both `FS<n>:` and `<LABEL>:` are usable paths. The label is the stable,
scriptable handle; the fsN is the native display name. There is **no
`-a/--alias` option and no `%<LABEL>%` env var** — the label *is* the alias.

Why this works where a plain create doesn't, and its limits:

- **The disk shows in `map` with no `map -r`.** `map` enumerates every
  SimpleFileSystem/BlockIo handle and displays one whose name matches the
  shell's `FS#`/`BLK#`/`HD*`/`CD*`/`F*` filter — which the `FS<n>` primary
  does. (A plain `SetMap` of just the label is *hidden*: a bare label matches
  none of those patterns. That was the behavior before this two-name scheme.)
- **`SetMap` targets the shell's *global* map**, so names set by `mkrd` (a
  child image) are visible to the launching script immediately — unlike
  `EFI_SHELL_PROTOCOL.Execute("map -r")`, which runs a *nested* shell that
  can't rebuild the parent's map.
- **The `FS<n>` is the lowest free slot at create time** — a best-effort
  guess at the shell's own numbering. It never reuses a taken slot (so the
  boot volume's `fs0` is untouched). When the shell is unreachable
  (`get_shell() == NULL`), `SetMap` fails cleanly and the disk is still
  created (mount via `map -r`); mkrd reports which half broke.
- **The label is set as the alias only when it's a clean map token** — not
  when it matches the reserved `fs<digits>` namespace, is too long, or has
  characters outside `[A-Za-z0-9_-]`. A rejected label just isn't an alias;
  the disk is reachable as `FS<n>:` only, with a note.
- **Idempotent.** A re-run of a still-mapped disk reuses its existing fsN
  handle — no drift, no duplicate — so a defensive `startup.nsh` can call it
  every boot.
- **Switch with the bare `<name>:`, not `cd`.** In the UEFI Shell a bare
  `fsN:` selects the filesystem; `cd` only moves within one.
- **A later `map -r` reclaims the names.** It rebuilds the shell map from
  scratch, drops the `SetMap`'d names, and the disk re-emerges as a real
  firmware-assigned `fsN` (a different number) + consist name. The one-call
  workflow never runs `map -r`, so it's unaffected — but don't assume the
  `FS<n>` / `<LABEL>` names survive one.

(`axl_volume_set_map` / `axl_volume_map_taken` / `axl_volume_map_alias` /
`axl_volume_unmap` in `<axl/axl-fs.h>` expose the mapping, the name-in-use
check, the "what is this device mapped as" read-back, and mapping removal for
other callers; `axl_ramdisk_find` returns a disk's device path by label.
`mkrd -d` uses these to drop **both** of a destroyed disk's now-dangling shell
names — the `fsN` primary and the label alias — so a later `<name>:` can't
dereference the freed device path.)

`EFI_RAM_DISK_PROTOCOL` is optional in UEFI 2.6+ and absent on some
firmware. `axl_ramdisk_ensure_driver` resolves it in order: already
published → a `RamDiskDxe.efi` found on a mounted volume → an embedded
RamDiskDxe image **supplied by the caller**. The embedded image is a
parameter (not baked into `libaxl.a`) so only binaries that want the
fallback carry the ~tens-of-KB blob; declare it with `AXL_EMBED_DECLARE`
+ the build's blob-embedding rule and pass `AXL_EMBED_DATA` /
`AXL_EMBED_SIZE` (see `tools/mkrd.c`). A vendored RamDiskDxe.efi per arch
lives in `third_party/edk2/`.

The registered device path embeds the backing memory's physical address
(`VirtualDisk(0x...,...)`), which is wherever `AllocatePages` lands and
varies run to run — it is not stable across boots or binaries. The
stable cross-boot handle is the FAT label, which is why
`axl_ramdisk_destroy` keys on it.

### Registering a pre-populated image (virtual media)

`axl_ramdisk_register_image` registers an **already-populated**,
page-aligned buffer **without formatting** — for mounting an uploaded
`.iso` / `.img` as a virtual block device (the iDRAC "Virtual Media"
shape). `AxlRamDiskKind` picks the firmware-recognized type GUID:
`AXL_RAMDISK_CDROM` (`gEfiVirtualCdGuid`, an El Torito CD-ROM for a
bootable ISO) or `AXL_RAMDISK_DISK` (`gEfiVirtualDiskGuid`, a raw disk
image). It connects controllers so the firmware binds its block / FAT /
ISO9660 drivers and the device enumerates.

```c
// Mount an uploaded ISO as a bootable virtual CD-ROM.
uint64_t pages = (size + 4095) / 4096, phys = 0;
axl_alloc_pages(pages, &phys);
void *img = (void *)(uintptr_t)phys;
// ... read the .iso bytes into img (e.g. via axl_file_view) ...
void *dp = NULL;
axl_ramdisk_register_image(img, size, AXL_RAMDISK_CDROM, &dp);
// ... device now enumerates / is bootable ...
axl_ramdisk_unregister(dp);
axl_free_pages(phys, pages);   // caller owns the buffer
```

Ownership differs from `create`/`destroy`: the image buffer is
**caller-owned and not copied**, so it must stay valid until
`axl_ramdisk_unregister`, and the caller frees it (`axl_free_pages`)
afterward — `unregister` only detaches the device.

Scope is create / list / destroy / register-image of the device; reading
and writing its files is `<axl/axl-fs.h>` over the resulting `fsN:`.
