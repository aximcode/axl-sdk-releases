/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-image-internal.h
    Internal helpers shared between the POSIX startup shim
    (`src/posix/axl-app.c`) and the public `axl_image_enumerate`
    surface (`src/util/axl-image.c`).

    Not part of the public API.
**/

#ifndef AXL_IMAGE_INTERNAL_H
#define AXL_IMAGE_INTERNAL_H

#include <uefi/axl-uefi.h>

/**
 * Decode an EFI_LOADED_IMAGE_PROTOCOL.FilePath device path chain
 * into a UTF-8 filesystem path (e.g. `"\\drivers\\foo.efi"`).
 *
 * Walks every `MEDIA_FILEPATH_DP` node and concatenates their UCS-2
 * components with `\\` separators where needed. Caller frees with
 * `axl_free`.
 *
 * Returns NULL if @p dp is NULL, has no FILEPATH nodes, or
 * allocation fails.
 */
char *
_axl_decode_image_filepath(EFI_DEVICE_PATH_PROTOCOL *dp);

/**
 * Prepend the UEFI Shell volume mapping for @p device_handle
 * (e.g. `"fs0:"`) to @p file_path.
 *
 * Returns a new UTF-8 string on success (caller frees with
 * `axl_free`), or NULL if the EFI_SHELL_PROTOCOL is unavailable,
 * the handle has no device-path protocol, or no mapping covers
 * the handle. Caller falls back to the prefix-less path.
 */
char *
_axl_prepend_volume_mapping(void *device_handle, const char *file_path);

#endif /* AXL_IMAGE_INTERNAL_H */
