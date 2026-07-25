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

/**
 * @brief Per-file callback for @ref axl_fv_for_each_file.
 *
 * @param file_guid  the file's name GUID (borrowed — copy if kept past return).
 * @param file_type  the EFI_FV_FILETYPE (e.g. 0x07 DRIVER, 0x0B FIRMWARE_VOLUME_IMAGE).
 * @param ctx        the opaque pointer passed to axl_fv_for_each_file.
 * @return true to stop the walk early, false to continue.
 */
typedef bool (*AxlFvFileFn)(
    const AxlGuid *file_guid,
    uint8_t        file_type,
    void          *ctx
);

/**
 * @brief Enumerate every file in a firmware volume, by name GUID.
 *
 * The runtime sibling of AxlFw's offline tree walk: invokes @p fn once per
 * FFS file in @p handle (all file types), giving each file's name GUID and
 * type. Pair with @ref axl_fv_find_file_name to turn a GUID into a name.
 *
 * @param handle  a volume handle from @ref axl_fv_next.
 * @param fn      per-file callback; must not be NULL. Return true to stop early.
 * @param ctx     opaque pointer forwarded to @p fn.
 * @return AXL_OK on a clean end-of-enumeration or an early stop (@p fn
 *     returned true); AXL_ERR if @p handle does not publish the FV2 protocol,
 *     @p fn is NULL, or the walk hits a hard read error.
 */
int
axl_fv_for_each_file(
    AxlHandle    handle,   ///< handle from axl_fv_next
    AxlFvFileFn  fn,       ///< per-file callback
    void        *ctx       ///< opaque pointer for @p fn
);

/**
 * @brief Resolve a firmware-file GUID to its human UI name.
 *
 * Searches every live firmware volume for the FFS file named @p file_guid
 * and reads its user-interface section (the `CHAR16` string a build tool
 * stamps from a module's `.inf` name, e.g. "Ip4Dxe"), returning it as UTF-8.
 * This is what turns an FV-embedded driver's raw `FvFile(<GUID>)` device-path
 * text into a name a human recognizes — the missing half of @ref
 * axl_handle_name for drivers whose image lives in a firmware volume.
 *
 * The first volume that both contains the file and carries a UI section for
 * it wins. A file present but with no UI section, or a GUID no volume
 * contains, reports AXL_NOT_FOUND (not AXL_ERR) — a normal "no name here"
 * outcome the caller can fall back from.
 *
 * @param file_guid  the FFS file's name GUID (as it appears in an FvFile
 *                   device-path node). Must not be NULL.
 * @param out        [out] buffer for the NUL-terminated UTF-8 name; truncated
 *                   to @p cap. Set to "" on any non-OK return. Must not be NULL.
 * @param cap        capacity of @p out in bytes; must be > 0.
 * @return AXL_OK and @p out set to the name; AXL_NOT_FOUND if no volume has
 *     the file with a UI section; AXL_ERR on NULL @p file_guid / @p out,
 *     zero @p cap, or a hard firmware read error.
 */
int
axl_fv_find_file_name(
    const AxlGuid *file_guid,   ///< FFS file name GUID to resolve
    char          *out,         ///< [out] UTF-8 name, NUL-terminated
    size_t         cap          ///< capacity of @p out in bytes
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_FV_H */
