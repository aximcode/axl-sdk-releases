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
snag is that a freshly created disk isn't in the shell's map, and its
auto-assigned `fsN` is **not deterministic** — a script can't hard-code
`fs5:`. `mkrd <LABEL>` solves both by **assigning the shell map name
itself** (`EFI_SHELL_PROTOCOL.SetMap`), so the volume is usable in **one
call, with no `map -r`**:

```
mkrd RAMDISK        # creates + maps as RAMDISK: (the label), sets %RAMDISK%
%RAMDISK%:          # switch to it — the bare `<name>:` verb, NOT `cd`
echo ... > last.nsh # write to the writable RAM disk

mkrd SCRATCH -a RD  # or override the alias: maps as RD: (fails if RD: is taken)
RD:                 # use it directly
```

**Label vs alias.** The positional `<LABEL>` is the FAT volume label *and* the
shell var `%<LABEL>%`; the map **alias** (what you type to switch to it) defaults
to the label — so `mkrd RD` gives you `RD:` with no `-a`. `-a <alias>` overrides
the alias.

Why this works where a plain create doesn't:

- **`SetMap` targets the shell's *global* map**, so a name set by `mkrd`
  (a child image) is visible to the launching script immediately — unlike
  `EFI_SHELL_PROTOCOL.Execute("map -r")`, which runs a *nested* shell that
  can't rebuild the parent's map. This is the same trick the old EFI
  Toolkit `MKRAMDISK` used by taking the map name as an argument.
- **The alias defaults to the label**, falling back to the lowest free `fsN`
  when the label matches the reserved `fs<digits>` namespace (so `mkrd fs2`
  never shadows or clobbers a real `fs2` / the boot volume), is already in use
  by another volume (never clobbers it), or is too long / has characters
  outside `[A-Za-z0-9_-]`. `-a <alias>` overrides and fails if `<name>` is taken.
  `%<LABEL>%` is set to the chosen alias either way.
- **Idempotent on the label *and* its map.** A same-session re-run reuses the
  disk's existing alias **in any form** (its label, an `fsN`, or a custom `-a`
  name) — it does **not** drift to a new slot or leave a duplicate alias — and
  republishes `%<LABEL>%` to it, so a defensive `startup.nsh` can call it every
  boot. A `-a <alias>` on an already-mapped disk keeps the existing alias and
  reports that `<name>` was not added.
- **Switch with the bare `<name>:` / `%<LABEL>%:`, not `cd`.** In the UEFI
  Shell a bare `fsN:` selects the filesystem; `cd` only moves within one.
- Idempotent on the label — a `startup.nsh` may re-run `mkrd` each boot.
- **Don't mix with a later `map -r`.** A subsequent `map -r` rebuilds the
  shell map and may drop or renumber the `SetMap`'d name. The one-call
  workflow above never runs `map -r`, so it's unaffected; just don't assume
  the assigned name survives a later `map -r`.

(`axl_volume_set_map` / `axl_volume_map_taken` / `axl_volume_map_alias` /
`axl_volume_unmap` in `<axl/axl-fs.h>` expose the mapping, the name-in-use
check, the "what is this device mapped as" read-back, and mapping removal for
other callers; `axl_ramdisk_find` returns a disk's device path by label.
`mkrd -d` uses these to drop a destroyed disk's now-dangling shell alias so a
later `<alias>:` can't dereference the freed device path.)

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
