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
 * Capture the two per-image path values for an image whose CRT0 didn't run
 * `_axl_args_init`. Specifically: DXE drivers go through `axl_driver_init`,
 * not the application CRT, so they reach this entry point.
 *
 * Both are derived here because they answer different questions:
 *
 *   - `axl_app_image_path()` — the file THIS image was loaded from, or NULL
 *     when there is none. A synthetic load context (a buffer load, whose
 *     device path AXL synthesizes after the fact) has no such file, and the
 *     public contract in `<axl/axl-app.h>` promises NULL for it.
 *   - `_axl_app_image_anchor()` — the nearest image in the ParentHandle
 *     chain that WAS loaded from a file. The directory anchor for sidecar
 *     discovery: a buffer-loaded driver embedded into a launcher inherits
 *     the launcher's directory, which is where its data files live.
 *
 * For an ordinary file-loaded image the two are the same string.
 *
 * Idempotent — a follow-up call after `_axl_args_init` is a no-op.
 */
void
_axl_init_image_path(void *image_handle);

/**
 * The sidecar-discovery anchor described above. Borrowed pointer owned by
 * the runtime; never freed by the caller. NULL when no image in the chain
 * was loaded from a file (network / RAM-disk boot).
 *
 * Callers wanting "where is THIS image" want the public
 * `axl_app_image_path()` instead — this one may name an ancestor.
 */
const char *
_axl_app_image_anchor(void);

#endif /* AXL_IMAGE_INTERNAL_H */
