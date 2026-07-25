/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-nosh.h
    Backend-internal file access and volume naming for the case where there
    is NO shell at all.

    BdsDxe launching an app straight from the removable-media boot slot
    (`\EFI\BOOT\BOOTx64.EFI`) publishes neither EFI_SHELL_PROTOCOL nor the
    EFI 1.x SHELL_ENVIRONMENT, so both of the backend's previous file paths
    dead-end. Everything here is built directly on firmware primitives:
    `LocateHandleBuffer(SimpleFileSystem)` for the volume set, `OpenVolume`
    plus `EFI_FILE_PROTOCOL.Open` for the files.

    **Volume naming.** With no shell there is no map to consult, so a volume
    is named by its position in the `LocateHandleBuffer(SimpleFileSystem)`
    array: `fs0`, `fs1`, ... That is the same namespace, and the same
    ordering, `axl_volume_enumerate` already falls back to when no shell
    mapping covers a handle, so the two agree. It is NOT guaranteed to agree
    with what a shell would call the same volume — measured, the orders do
    diverge — which is why callers must rule out BOTH shells before reaching
    anything here (`no_shell_at_all` in axl-backend-native.c is that gate).
    Handing a positional name to a live shell resolves a DIFFERENT volume.

    There is no current working directory without a shell, so a path must
    name its volume (`fs0:\dir\file`); a bare `\dir\file` is refused rather
    than silently resolved against volume 0.

    Internal header — not installed, not part of the public API.
**/

#ifndef AXL_BACKEND_NATIVE_NOSH_H
#define AXL_BACKEND_NATIVE_NOSH_H

#include "axl-backend.h"

/**
 * @brief Open a file by an `fsN:`-qualified path using firmware primitives.
 *
 * Resolves the `fsN:` prefix to the Nth `LocateHandleBuffer` SimpleFileSystem
 * handle, opens its volume, and opens the remainder relative to the root.
 * Separators are normalized to `\` and a trailing separator is stripped;
 * `fs0:` and `fs0:\` both name the volume root. Because there is no current
 * directory, a volume-relative spelling (`fs0:dir\file`) is treated as
 * root-relative.
 *
 * The returned `EFI_FILE_PROTOCOL *` is the same handle shape the old-shell
 * path produces, so every handle-based backend op works on it unchanged.
 *
 * @return AXL_OK on success; AXL_ERR on a path with no `fsN:` prefix, an
 *     index past the end of the volume array, a volume that will not open,
 *     or any firmware failure (including the ordinary "no such file").
 */
int
axl_nosh_file_open(
    const unsigned short  *path,        ///< UCS-2 file path, `fsN:`-qualified
    uint64_t               mode,        ///< EFI file mode flags
    uint64_t               attributes,  ///< EFI file attributes (create only)
    EFI_FILE_PROTOCOL    **out          ///< [out] receives the open file
    );

/**
 * @brief Name the volume a handle represents, positionally: `fs0`, `fs1`, ...
 *
 * The handle is compared by identity against the
 * `LocateHandleBuffer(SimpleFileSystem)` array — for an image loaded from a
 * file, `EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle` IS the SimpleFileSystem
 * handle, so no device-path matching is involved.
 *
 * @return AXL_OK with the bare name (no trailing ':') in @p out; AXL_ERR if
 *     the handle publishes no SimpleFileSystem, the enumeration fails, or
 *     the name does not fit.
 */
int
axl_nosh_volume_name_for_handle(
    void   *device_handle,  ///< EFI_HANDLE to name
    char   *out,            ///< [out] receives e.g. "fs0"
    size_t  out_size        ///< capacity of @p out in bytes
    );

/**
 * @brief Name the volume a device path points at, positionally.
 *
 * The device-path counterpart of @ref axl_nosh_volume_name_for_handle, for
 * callers that hold a path rather than a handle: each SimpleFileSystem
 * handle's own device path is byte-compared against @p device_path.
 *
 * @return AXL_OK with the bare name (no trailing ':') in @p out; AXL_ERR
 *     when no SimpleFileSystem volume has that exact device path.
 */
int
axl_nosh_map_fs_name_from_dp(
    void   *device_path,  ///< EFI_DEVICE_PATH_PROTOCOL * to match
    char   *out,          ///< [out] receives e.g. "fs0"
    size_t  out_size      ///< capacity of @p out in bytes
    );

#endif /* AXL_BACKEND_NATIVE_NOSH_H */
