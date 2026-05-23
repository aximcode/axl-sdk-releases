/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-image.h
    Executable-image lifecycle: load, start, unload.

    A backend-neutral abstraction for what UEFI calls
    `LoadImage`/`StartImage`/`UnloadImage`. On a future Linux
    backend the same shape would map to `posix_spawn` or
    `execve`-style entry; on coreboot stages, to their loader.
    Consumer code never references `EFI_HANDLE` or
    `EFI_LOADED_IMAGE_PROTOCOL` directly — the AxlImage handle
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
#include <axl/axl-macros.h>

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
 * @return AXL_OK on success, AXL_ERR if the file can't be read or the
 *     image format is rejected.
 */
int
axl_image_load(
    const char  *path,   ///< image path (e.g. "fs0:\\boot\\hello.efi")
    AxlImage   **out     ///< [out] receives the image handle
);

/**
 * @brief Set load options on a loaded image before starting it.
 *
 * Mirrors @ref axl_driver_set_load_options for the image-level API:
 * installs @p data as the loaded image's
 * `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` so the started image sees
 * it via the same surface a shell launch would expose
 * (@ref axl_app_argc / @ref axl_app_argv on the loaded side, or the
 * raw byte buffer via @ref axl_driver_get_load_options_raw).
 *
 * The data is copied internally — caller's buffer can be freed after.
 * The copy is owned by AXL and released by @ref axl_image_unload (or
 * by a subsequent set on the same handle, which replaces the previous
 * copy). Pass NULL data (or size == 0) to clear load options and free
 * any previous copy. Call between @ref axl_image_load and
 * @ref axl_image_start.
 *
 * Encoding: pass-through. UEFI shells encode argv as UCS-2 strings;
 * programmatic launchers can pass arbitrary bytes — the started image
 * is responsible for interpreting the buffer.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments, alloc failure,
 *     or firmware protocol error.
 */
int
axl_image_set_load_options(
    AxlImage    *img,    ///< image handle from axl_image_load
    const void  *data,   ///< option bytes (copied; NULL to clear)
    size_t       size    ///< option size in bytes
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
 * @return AXL_OK on successful start (regardless of the image's exit
 *     code), AXL_ERR if the image could not be started at all.
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
 * @return AXL_OK on success, AXL_ERR if the firmware refuses (e.g. the image
 *     has installed protocols that aren't released yet).
 */
int
axl_image_unload(
    AxlImage  *img   ///< image handle from axl_image_load
);

/**
 * @brief Snapshot of a loaded image as visible to the firmware.
 *
 * Returned by `axl_image_enumerate`'s callback and
 * `axl_image_self_get_range`. The string fields point into runtime-
 * owned storage that's valid for the duration of the callback (or
 * until the next call for `_self_get_range`); copy if you need it
 * longer.
 */
typedef struct {
    void       *base;   ///< image load address in memory
    size_t      size;   ///< image size in bytes
    const char *path;   ///< UTF-8 path (e.g. "fs0:\\drivers\\foo.efi"), or NULL
                        ///< if the firmware doesn't expose it
} AxlImageInfo;

/**
 * @brief Iterator callback for `axl_image_enumerate`.
 *
 * @return 0 to continue iteration, non-zero to stop. The non-zero
 *     value is returned to the `axl_image_enumerate` caller.
 */
typedef int (*AxlImageIterFn)(
    const AxlImageInfo *info,
    void               *ctx
);

/**
 * @brief Walk every currently-loaded image, invoking @p cb once per image.
 *
 * Backend-neutral abstraction over UEFI's
 * `LocateHandleBuffer(EFI_LOADED_IMAGE_PROTOCOL)` + per-handle
 * `HandleProtocol`. The callback receives a layout-stable
 * `AxlImageInfo` — consumer code never sees `EFI_LOADED_IMAGE_PROTOCOL`.
 *
 * @p info->path may be NULL for images whose firmware FilePath
 * couldn't be decoded (e.g. synthetic loads or in-memory images).
 * Callers that use the path for display should fall back to a
 * placeholder.
 *
 * @return AXL_OK if the walk completed, the callback's non-zero
 *     value if it stopped early, or AXL_ERR on enumeration failure.
 */
int
axl_image_enumerate(
    AxlImageIterFn  cb,
    void           *ctx
);

/**
 * @brief Get the base address and size of the currently-running image.
 *
 * Convenience over `axl_image_enumerate` when the caller only wants
 * the self image's range — used for fault attribution and similar
 * "where am I in memory" checks. Equivalent to walking
 * `axl_image_enumerate` and matching the entry whose path equals
 * `axl_app_image_path()`, but cheaper.
 *
 * @return AXL_OK on success (@p out_base and @p out_size populated);
 *     AXL_ERR if firmware doesn't expose the loaded-image protocol
 *     for the current image (extremely unusual).
 */
int
axl_image_self_get_range(
    void   **out_base,   ///< [out] image base load address
    size_t  *out_size    ///< [out] image size in bytes (NULL allowed)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_IMAGE_H */
