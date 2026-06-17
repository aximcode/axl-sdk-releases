/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ramdisk.h
    Create, enumerate, and destroy FAT RAM disks over
    EFI_RAM_DISK_PROTOCOL.

    A RAM disk is a block of allocated memory registered with the
    firmware as a virtual block device, FAT-formatted so the firmware
    binds its filesystem driver and the volume appears as an `fsN:`
    mapping. Useful for scratch space, staging files a tool produces,
    or the backing store of a virtual-media feature.

    @code
    // Make the protocol available (embedded fallback optional), then
    // create a 64 MB disk labelled "SCRATCH".
    axl_ramdisk_ensure_driver(NULL, 0, NULL);
    void *dp = NULL;
    if (axl_ramdisk_create("SCRATCH", 64, &dp) == AXL_OK) {
        // ... fsN: now resolves to the new FAT volume ...
    }
    @endcode

    EFI_RAM_DISK_PROTOCOL is optional in UEFI 2.6+ and absent on some
    firmware; `axl_ramdisk_ensure_driver` loads a RamDiskDxe driver when
    the firmware doesn't already publish it. The orchestration here is
    lifted from the `mkrd` tool so any consumer reuses it instead of
    copying the FAT formatters and the driver-ensure flow.

    Scope is create / list / destroy of the device. Reading and writing
    the disk's files is `<axl/axl-fs.h>` over the resulting `fsN:`.
**/

#ifndef AXL_RAMDISK_H
#define AXL_RAMDISK_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A registered RAM disk, as reported by `axl_ramdisk_list`.
 */
typedef struct {
    char     label[16];      ///< FAT volume label (NUL-terminated; "" if unlabeled)
    uint64_t start_addr;     ///< physical address of the backing memory
    uint64_t size_bytes;     ///< backing size in bytes
    void    *device_path;    ///< firmware-owned EFI device path (do not free); valid until this disk is destroyed
} AxlRamDisk;

/**
 * @brief Ensure EFI_RAM_DISK_PROTOCOL is available.
 *
 * Resolves the protocol in order: (1) already published by the
 * firmware, (2) a RamDiskDxe driver found on a mounted volume, (3) the
 * embedded image supplied here. Wraps
 * `axl_driver_ensure_with_embedded` for the RAM-disk GUID — the
 * non-trivial firmware/disk/embedded fallback every RAM-disk consumer
 * needs.
 *
 * The embedded image is a parameter rather than baked into the library
 * so binaries that never make a RAM disk don't carry the ~tens-of-KB
 * RamDiskDxe.efi blob. Consumers wanting the embedded fallback declare
 * the blob with `AXL_EMBED_DECLARE` / the build's blob-embedding rule
 * and pass `AXL_EMBED_DATA` / `AXL_EMBED_SIZE` (see the `mkrd` tool).
 *
 * When @p override_name is set, the disk search uses it and the
 * embedded fallback is not used (matches `axl_driver_ensure_with_embedded`).
 *
 * @return AXL_OK if the protocol is now available, AXL_ERR if every
 *     resolution step failed.
 */
int
axl_ramdisk_ensure_driver(
    const unsigned char *dxe_image,       ///< embedded RamDiskDxe.efi bytes, or NULL to skip the embedded fallback
    size_t               dxe_image_size,  ///< length of @p dxe_image (0 if NULL)
    const char          *override_name    ///< driver filename to search instead of "RamDiskDxe.efi" (NULL for the default)
);

/**
 * @brief Allocate, FAT-format, and register a RAM disk.
 *
 * Allocates @p size_mb of page-aligned memory, formats it FAT16
 * (`size_mb <= 512`) or FAT32 (larger) with @p label as the volume
 * label, and registers it via EFI_RAM_DISK_PROTOCOL. Blocks ~0.5s for
 * the firmware to bind its FAT driver, then refreshes the volume map so
 * the disk is immediately enumerable and reachable as `fsN:` (relevant
 * for a consumer creating one at boot).
 *
 * Requires the protocol — call `axl_ramdisk_ensure_driver` first (or
 * run on firmware that publishes it). @p size_mb must be in the range
 * 1..32768; values outside it return AXL_ERR. Idempotent on the label:
 * if a RAM disk with @p label already exists, returns AXL_OK without
 * allocating again and sets @p dev_path_out to the existing disk's
 * device path.
 *
 * Note the registered device path embeds the backing memory's physical
 * address (e.g. `VirtualDisk(0x17F5D000,...)`), which is wherever the
 * allocation lands — it varies run to run, so it is not stable across
 * boots or binaries. (The stable cross-boot handle is the label, which
 * is why `axl_ramdisk_destroy` keys on it.) The returned device path is
 * firmware-owned and valid until this disk is destroyed.
 *
 * @return AXL_OK on success (or an existing same-label disk), AXL_ERR
 *     if the protocol is unavailable, @p size_mb is out of range,
 *     allocation fails, or registration fails.
 */
int
axl_ramdisk_create(
    const char *label,         ///< FAT volume label (uppercased, truncated to 11 chars)
    size_t      size_mb,       ///< disk size in MB (1..32768)
    void      **dev_path_out   ///< [out] firmware-owned device path, or NULL if unwanted (do not free)
);

/**
 * @brief Destroy a RAM disk by FAT label.
 *
 * Unregisters the matching RAM disk and frees its backing memory.
 * The label match is case-insensitive and ignores trailing padding;
 * @p label is truncated to the stored 11-char FAT label before
 * comparing, so passing the same (possibly long) label given to
 * `axl_ramdisk_create` matches.
 *
 * @return AXL_OK on success, AXL_ERR if the protocol is unavailable or
 *     no RAM disk with @p label is found.
 */
int
axl_ramdisk_destroy(
    const char *label   ///< FAT volume label to destroy
);

/**
 * @brief List the registered RAM disks.
 *
 * Fills @p out with up to @p cap descriptors and writes the total to
 * @p count. (`out == NULL` / `cap == 0` reports the count only.) An
 * empty result is success with `*count == 0`.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments.
 */
int
axl_ramdisk_list(
    AxlRamDisk *out,     ///< output array (may be NULL to count only)
    size_t      cap,     ///< capacity of @p out
    size_t     *count    ///< [out] number of RAM disks found
);

/**
 * @brief Image kind for `axl_ramdisk_register_image` — the RAM-disk type
 *        GUID the firmware sees.
 *
 * An El Torito CD-ROM (a bootable `.iso`, exposed as a CD device) or a
 * raw hard-disk image (a `.img`, exposed as a block device).
 */
typedef enum {
    AXL_RAMDISK_DISK  = 0,  ///< gEfiVirtualDiskGuid — raw block image
    AXL_RAMDISK_CDROM = 1,  ///< gEfiVirtualCdGuid — El Torito CD-ROM
} AxlRamDiskKind;

/**
 * @brief Register an already-populated, page-aligned image buffer as a
 *        typed RAM disk via EFI_RAM_DISK_PROTOCOL — WITHOUT formatting.
 *
 * Unlike `axl_ramdisk_create` (which allocates and FAT-formats a blank
 * scratch disk), this registers caller-owned memory verbatim, so an
 * uploaded OS image boots as-is: the image carries its own filesystem
 * (ISO9660 for a CD-ROM, a partition / FAT image for a disk). @p kind
 * selects the firmware-recognized type GUID — CD-ROM vs raw disk. After
 * registering, the firmware's controllers are connected so it binds its
 * block / FAT / ISO9660 drivers and the device enumerates (a bootable
 * image becomes a boot option). The connect is global (equivalent to the
 * shell's `connect -r`) — heavier than a targeted connect, but it only
 * binds drivers, never unbinds, so it does not disturb controllers
 * already in use (e.g. a running server's NICs).
 *
 * Ownership: @p image is caller-owned and is NOT copied — it must be
 * page-aligned (allocate with `axl_alloc_pages`) and MUST stay valid
 * until `axl_ramdisk_unregister`. The caller frees it (with
 * `axl_free_pages`) AFTER unregistering — this call never takes
 * ownership of the memory (contrast `axl_ramdisk_create` /
 * `axl_ramdisk_destroy`, which own and free the disk they allocate).
 *
 * Requires the protocol — call `axl_ramdisk_ensure_driver` first (or run
 * on firmware that publishes it). The returned device path is
 * firmware-owned (do not free) and valid until `axl_ramdisk_unregister`.
 * Note it embeds the backing memory's physical address, so it is not
 * stable across boots; it is the handle you pass back to unregister.
 *
 * @return AXL_OK on success, or AXL_ERR if the protocol is unavailable,
 *     @p image is NULL, @p size_bytes is 0, @p kind is invalid, or
 *     registration fails.
 */
int
axl_ramdisk_register_image(
    void          *image,        ///< page-aligned image bytes (caller-owned; keep valid until unregister)
    uint64_t       size_bytes,   ///< image length in bytes
    AxlRamDiskKind kind,         ///< CD-ROM vs raw disk type GUID
    void         **dev_path_out  ///< [out] firmware-owned device path (do not free), or NULL if unwanted
);

/**
 * @brief Unregister a RAM disk by the device path returned from
 *        `axl_ramdisk_register_image`.
 *
 * Unregisters the device via EFI_RAM_DISK_PROTOCOL. Does NOT free the
 * backing memory — the caller frees its `axl_alloc_pages` buffer
 * afterward (the symmetric who-allocates-frees contract; contrast
 * `axl_ramdisk_destroy`, which owns and frees the FAT disk it created).
 *
 * @return AXL_OK on success, or AXL_ERR if @p dev_path is NULL, the
 *     protocol is unavailable, or no RAM disk matches @p dev_path.
 */
int
axl_ramdisk_unregister(
    void *dev_path   ///< device path from axl_ramdisk_register_image
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RAMDISK_H */
