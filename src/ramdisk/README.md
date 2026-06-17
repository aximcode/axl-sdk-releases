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
