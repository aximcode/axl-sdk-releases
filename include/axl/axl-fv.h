/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-fv.h
    Firmware-volume enumeration and attributes.

    Enumerates the handles publishing the firmware's Firmware Volume 2
    protocol — the FFS containers the platform's DXE drivers and other
    firmware files live in — and reports each volume's access
    attributes and file count. This is a read-only inventory probe; it
    does not read file contents or sections.

    Cursor-style iteration matches the other platform readers and
    returns the firmware `AxlHandle` directly:

    @code
    AxlHandle h = NULL;
    while ((h = axl_fv_next(h)) != NULL) {
        AxlFvAttributes a;
        size_t files = 0;
        if (axl_fv_get_attributes(h, &a) == AXL_OK
            && axl_fv_count_files(h, &files) == AXL_OK) {
            // ... report a.readable / a.writable / a.locked, files ...
        }
    }
    @endcode

    Device-path text needs no new API: the same `AxlHandle` resolves
    through the existing `axl_handle_get_protocol(h, "device-path",
    ...)` + `axl_device_path_to_text()` (both in `<axl/axl-sys.h>`).
**/

#ifndef AXL_FV_H
#define AXL_FV_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-sys.h>   /* AxlHandle */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Access attributes of a firmware volume.
 *
 * Decoded from the volume's `GetVolumeAttributes` status bits
 * (`EFI_FV2_READ_STATUS` / `WRITE_STATUS` / `LOCK_STATUS`). These
 * are the current effective states, not the volume's capabilities.
 */
typedef struct {
    bool readable;   ///< reads are currently enabled (EFI_FV2_READ_STATUS)
    bool writable;   ///< writes are currently enabled (EFI_FV2_WRITE_STATUS)
    bool locked;     ///< the volume-level attribute lock is asserted (EFI_FV2_LOCK_STATUS); this freezes the attributes, it does not by itself imply read/write access — that is reported by readable / writable
} AxlFvAttributes;

/**
 * @brief Iterate handles publishing the Firmware Volume 2 protocol.
 *
 * Cursor-style enumeration: pass NULL to get the first FV handle,
 * then pass each returned handle back to get the next. Returns NULL
 * once exhausted (including when no firmware volumes are published).
 *
 * The handle set is located once and cached for the image lifetime
 * (like AxlBlock / AxlSerial) — a volume published afterward will not
 * appear; the cache mirrors the boot device set. Position is recovered
 * from the handle you pass back, not a hidden shared cursor: passing
 * NULL — or any
 * handle not in the cached set — starts again from the first volume,
 * and independent walks do not interfere. The returned handle is
 * firmware-owned (do not free) and valid to pass to the readers below
 * and to `axl_handle_get_protocol(h, "device-path", ...)`.
 *
 * @return next FV2 handle, or NULL at end of enumeration.
 */
AxlHandle
axl_fv_next(
    AxlHandle prev   ///< previous handle, or NULL to start
);

/**
 * @brief Read a firmware volume's access attributes.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle does not publish
 *     the FV2 protocol, the GetVolumeAttributes call fails, or
 *     @p out is NULL.
 */
int
axl_fv_get_attributes(
    AxlHandle        handle,   ///< handle from axl_fv_next
    AxlFvAttributes *out       ///< [out] populated on success
);

/**
 * @brief Count the files in a firmware volume.
 *
 * Walks the volume's `GetNextFile` enumeration over all file types
 * and reports how many files it contains. This is an O(files) walk,
 * not a cached field read (unlike `axl_fv_get_attributes`) — cache
 * the result if you serve it repeatedly.
 *
 * Reaching the end of the enumeration is success; an empty volume
 * succeeds with `*out == 0`. A hard read error part-way through the
 * walk returns AXL_ERR rather than a silently truncated count.
 *
 * @return AXL_OK on success, AXL_ERR if @p handle does not publish
 *     the FV2 protocol, the file walk hits a read error, or @p out
 *     is NULL.
 */
int
axl_fv_count_files(
    AxlHandle  handle,   ///< handle from axl_fv_next
    size_t    *out       ///< [out] file count, populated on success
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FV_H */
