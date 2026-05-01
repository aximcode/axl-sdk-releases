/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-image.h
    Executable-image lifecycle: load, start, unload.

    A backend-neutral abstraction for what UEFI calls
    `LoadImage`/`StartImage`/`UnloadImage`. On a future Linux
    backend the same shape would map to `posix_spawn` or
    `execve`-style entry; on coreboot stages, to their loader.
    Consumer code never references `EFI_HANDLE` or
    `EFI_LOADED_IMAGE_PROTOCOL` directly — the @ref AxlImage handle
    is opaque.

    @code
    AxlImage *img;
    if (axl_image_load("fs0:\\boot\\hello.efi", &img) == 0) {
        int exit_code = 0;
        axl_image_start(img, &exit_code);
        axl_image_unload(img);
        axl_printf("hello.efi exited with %d\n", exit_code);
    }
    @endcode
**/

#ifndef AXL_IMAGE_H
#define AXL_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a loaded executable image.
 *
 * Created by axl_image_load(); released by axl_image_unload(). The
 * struct is intentionally not defined here — consumers treat it as
 * a pointer-only type.
 */
typedef struct AxlImage AxlImage;

/**
 * @brief Load an executable image from a path on a mounted volume.
 *
 * Path syntax follows the UEFI Shell convention: a volume label,
 * a colon, and a backslash-separated path. Forward slashes are
 * accepted and normalized internally. The image is loaded but
 * not yet started.
 *
 * @return 0 on success, -1 if the file can't be read or the
 *     image format is rejected.
 */
int
axl_image_load(
    const char  *path,   ///< image path (e.g. "fs0:\\boot\\hello.efi")
    AxlImage   **out     ///< [out] receives the image handle
);

/**
 * @brief Start a loaded image and wait for it to return.
 *
 * Transfers control to the image's entry point. Returns when the
 * image calls Exit() or returns from its entry. The handle remains
 * valid after start; the caller still owns it and must
 * axl_image_unload() it.
 *
 * The image's exit code (low 32 bits of its EFI_STATUS) is reported
 * in @c *exit_code. For an image that calls @c Exit(EFI_SUCCESS, ...)
 * this is 0; for an explicit @c Exit(7, ...) it is 7. UEFI's
 * Exit() and propagated-error channels share the same encoding,
 * so callers that need to distinguish should treat any non-zero
 * value as "image did not succeed" rather than rely on a specific
 * code.
 *
 * @return 0 on successful start (regardless of the image's exit
 *     code), -1 if the image could not be started at all.
 */
int
axl_image_start(
    AxlImage  *img,         ///< image handle from axl_image_load
    int       *exit_code    ///< [out] image's exit status (NULL allowed)
);

/**
 * @brief Unload an image, releasing its memory.
 *
 * Safe to call on a never-started image. Frees the handle.
 *
 * @return 0 on success, -1 if the firmware refuses (e.g. the image
 *     has installed protocols that aren't released yet).
 */
int
axl_image_unload(
    AxlImage  *img   ///< image handle from axl_image_load
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_IMAGE_H */
