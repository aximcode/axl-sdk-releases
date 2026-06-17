/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-block.h
    Block-device enumeration and media descriptors.

    Enumerates the handles that publish the firmware's block-I/O
    protocol (raw block devices: disks, partitions, CD-ROMs, RAM
    disks) and exposes each device's media descriptor as a typed
    struct. This is the low-level counterpart to `<axl/axl-fs.h>`:
    AxlFs gives path-based file access over mounted volumes, while
    AxlBlock enumerates the underlying block devices and reports
    their geometry and media state.

    Cursor-style iteration matches the other platform readers
    (AxlPci, AxlUsb, AxlAcpi). `axl_block_next` walks the block
    handles; the returned `AxlHandle` feeds the typed reader below:

    @code
    AxlHandle h = NULL;
    while ((h = axl_block_next(h)) != NULL) {
        AxlBlockMedia m;
        if (axl_block_get_media(h, &m) == AXL_OK && m.media_present) {
            uint64_t capacity = (m.last_block + 1) * m.block_size;
            // ... report device ...
        }
    }
    @endcode

    Device-path text needs no new API: the same `AxlHandle` resolves
    through the existing `axl_handle_get_protocol(h, "device-path",
    ...)` + `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).

    Scope is enumeration + the media descriptor. Block read/write
    is out of scope — diagnostic and inventory tools need the device
    list and geometry, not a block-level I/O path (use AxlFs for
    data access).
**/

#ifndef AXL_BLOCK_H
#define AXL_BLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlHandle */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Media descriptor for a block device.
 *
 * Typed projection of the firmware's `EFI_BLOCK_IO_MEDIA`. Fields
 * are raw readouts; the consumer derives presentation values such
 * as capacity (`(last_block + 1) * block_size`) and device type.
 *
 * The geometry fields (`block_size`, `last_block`) are meaningful
 * only when `media_present` is true — for an empty removable slot
 * (CD-ROM, card reader) the firmware may report stale or zero
 * values, so capacity must be computed only after checking
 * `media_present`.
 *
 * Physical-geometry and alignment fields the firmware also tracks
 * (`IoAlign`, and the revision-gated `LowestAlignedLba` /
 * `LogicalBlocksPerPhysicalBlock` / `OptimalTransferLengthGranularity`)
 * are intentionally omitted: they require gating on the media
 * revision and matter only to a block-level I/O path, which is out
 * of scope for enumeration.
 */
typedef struct {
    uint32_t media_id;            ///< media ID; a change between reads of the same handle means the media was swapped (cached geometry is then stale)
    bool     removable_media;     ///< the *media* is removable (CD-ROM, USB stick), not that the device is hot-pluggable
    bool     media_present;       ///< media is currently present and accessible
    bool     logical_partition;   ///< handle is a logical partition, not a whole device
    bool     read_only;           ///< media is write-protected
    bool     write_caching;       ///< device has a write-back cache enabled
    uint32_t block_size;          ///< logical (not physical) block size in bytes
    uint64_t last_block;          ///< LBA of the last addressable block (count = last_block + 1); valid only when media_present
} AxlBlockMedia;

/**
 * @brief Iterate handles publishing the block-I/O protocol.
 *
 * Cursor-style enumeration: pass NULL to get the first block
 * handle, then pass each returned handle back to get the next.
 * Returns NULL once the enumeration is exhausted (including when
 * no block devices exist).
 *
 * The handle set is snapshotted from the firmware on the first
 * call and cached for the image lifetime (like AxlUsb) — a device
 * hot-added afterward will not appear; the cache mirrors the boot
 * device set. Position is recovered from the handle you pass back,
 * not from a hidden shared cursor, so iteration carries no global
 * state: passing NULL — or any handle not in the cached set —
 * starts again from the first device, and independent walks (even
 * nested ones over the same stable set) do not interfere.
 *
 * The returned handle is owned by the firmware — do not free it. It
 * is valid to pass to `axl_block_get_media` and to
 * `axl_handle_get_protocol(h, "device-path", ...)`.
 *
 * @return next block-I/O handle, or NULL at end of enumeration.
 */
AxlHandle
axl_block_next(
    AxlHandle prev   ///< previous handle, or NULL to start
);

/**
 * @brief Read a block device's media descriptor.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle does not publish
 *     the block-I/O protocol or @p out is NULL.
 */
int
axl_block_get_media(
    AxlHandle      handle,   ///< handle from axl_block_next
    AxlBlockMedia *out       ///< [out] populated on success
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_BLOCK_H */
