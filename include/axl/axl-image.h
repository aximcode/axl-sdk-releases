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
#include <axl/axl-sys.h>     /* AxlGuid */

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
 * it via the same surface a shell launch would expose (the `argc` /
 * `argv` the loaded image's `main` receives, or the raw byte buffer via
 * @ref axl_driver_get_load_options_raw).
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
 * @brief Load an image, run it to completion, and unload it.
 *
 * The one-call form of the common "launch a foreground UEFI app and get
 * its exit code" pattern: @ref axl_image_load + (optionally)
 * @ref axl_image_set_load_options + @ref axl_image_start (which **blocks**
 * until the image returns) + @ref axl_image_unload. Use it for any
 * blocking UEFI application — a diagnostic tool, a vendor setup app, a
 * recovery menu, or (via @ref axl_shell_launch) the UEFI Shell.
 *
 * @p args is a command-line string installed as the image's
 * `LoadOptions`, encoded to UCS-2 the way a shell launch encodes a
 * command line; pass NULL (or "") for none. The started image parses it
 * as its arguments per the shell convention — whether to include a
 * leading program name depends on that image's argv parser. The encoding
 * buffer is internal; @p args may be freed after the call.
 *
 * Pair this with @ref axl_console_mirror_install to mirror the launched
 * app's console to a remote terminal.
 *
 * @return AXL_OK if the image was started and has now returned (its exit
 *     code is in @p out_exit_code); AXL_ERR if @p path is NULL or the
 *     image could not be loaded.
 */
int
axl_image_run(
    const char *path,          ///< image path (UEFI shell syntax)
    const char *args,          ///< command-line / LoadOptions (UTF-8), or NULL
    int        *out_exit_code  ///< [out] image's exit code (NULL allowed)
);

/**
 * @brief Load + run an image embedded in a firmware volume, by file GUID.
 *
 * The FV-embedded counterpart of @ref axl_image_run: instead of a path on a
 * mounted volume, it locates the firmware file whose name GUID is
 * @p name_guid in a readable Firmware Volume (`EFI_FV2_READ_STATUS`),
 * `LoadImage`s it directly out of the FV (no file staged on any
 * filesystem), installs @p args as `LoadOptions`, `StartImage` (which
 * **blocks** until the image returns), and unloads it. This is how a
 * consumer runs a firmware-embedded tool — most notably the
 * vendor-supplied UEFI Shell (see @ref axl_shell_launch_fv) — with nothing
 * staged on disk.
 *
 * @p name_guid is the FFS file name GUID — an @ref AxlGuid (write it with the
 * @ref AXL_GUID macro), the same value the firmware's file directory carries.
 * Only files of type `EFI_FV_FILETYPE_APPLICATION` are matched. All readable
 * FVs are searched and the first match wins; the order among multiple FVs
 * carrying the same GUID is firmware-dependent and unspecified, so a consumer
 * needing one specific FV's build should not rely on it (harmless for the
 * Shell — every instance is equivalent).
 *
 * @p args is installed as the image's `LoadOptions`, UTF-8 encoded to UCS-2
 * exactly as @ref axl_image_run does; pass NULL (or "") for none.
 *
 * Cleanup is atomic: on any failure after the image loads, it is unloaded
 * before returning. @p out_exit_code is set to 0 up front, so it reads 0 on
 * every failure path.
 *
 * @return AXL_OK if the file was found, started, and has now returned (its
 *     exit code is in @p out_exit_code); AXL_ERR if @p name_guid is NULL,
 *     no readable FV carries a matching application, or load/start failed.
 */
int
axl_image_run_fv_file(
    const AxlGuid  *name_guid,      ///< FFS file name GUID (see AXL_GUID)
    const char     *args,           ///< command-line / LoadOptions (UTF-8), or NULL
    int            *out_exit_code   ///< [out] image's exit code (NULL allowed)
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
