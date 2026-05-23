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

/**
 * Initialize the per-image sidecar-discovery anchor (`axl_app_image_path`)
 * for an image whose CRT0 didn't run `_axl_args_init`. Specifically:
 * DXE drivers go through `axl_driver_init`, not the application CRT,
 * so they reach this entry point to capture their LoadedImage->FilePath.
 *
 * Idempotent — a follow-up call after `_axl_args_init` is a no-op.
 *
 * The walk falls back to LoadedImage->ParentHandle when the current
 * image has no FilePath (the common case for buffer-loaded driver
 * images: `axl_driver_load_buffer` and `axl_driver_ensure_with_embedded`'s
 * step-4 embedded-blob path). This makes sidecar autodiscovery work
 * for embedded drivers by anchoring on the launcher's path.
 */
void
_axl_init_image_path(void *image_handle);

#endif /* AXL_IMAGE_INTERNAL_H */
