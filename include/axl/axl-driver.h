/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-driver.h:
 *
 * UEFI driver lifecycle — load, start, connect, disconnect, unload.
 * No GLib equivalent (UEFI-specific).
 *
 * @code
 * AxlDriverHandle drv;
 * if (axl_driver_load("fs0:\\MyDriver.efi", &drv) == 0) {
 *     axl_driver_start(drv);
 *     axl_driver_connect(drv);
 *     // ... driver is active ...
 *     axl_driver_disconnect(drv);
 *     axl_driver_unload(drv);
 * }
 * @endcode
 */

#ifndef AXL_DRIVER_H
#define AXL_DRIVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a loaded driver image.
typedef void *AxlDriverHandle;

/**
 * @brief Load a driver image from a file path.
 *
 * Loads the .efi file into memory but does not start it.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_load(
    const char       *path,   ///< path to .efi driver (UTF-8)
    AxlDriverHandle  *handle  ///< [out] receives driver handle
);

/**
 * @brief Start a loaded driver image.
 *
 * Calls the driver's entry point. The driver registers its
 * binding protocol(s) but does not yet bind to devices.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_start(
    AxlDriverHandle handle  ///< driver handle from axl_driver_load
);

/**
 * @brief Connect a driver to all matching device handles.
 *
 * Triggers the driver's Supported/Start sequence for each
 * compatible device. Call after axl_driver_start.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_connect(
    AxlDriverHandle handle  ///< driver handle
);

/**
 * @brief Disconnect a driver from all devices.
 *
 * Triggers the driver's Stop sequence for each bound device.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_disconnect(
    AxlDriverHandle handle  ///< driver handle
);

/**
 * @brief Unload a driver image from memory.
 *
 * The driver must be disconnected first.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_unload(
    AxlDriverHandle handle  ///< driver handle
);

/**
 * @brief Set load options on a loaded driver image.
 *
 * Provides configuration data (e.g., a URL) that the driver reads
 * from EFI_LOADED_IMAGE_PROTOCOL.LoadOptions during startup.
 * The data is copied internally — caller's buffer can be freed after.
 * Pass NULL data to clear load options.
 * Call between axl_driver_load and axl_driver_start.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_set_load_options(
    AxlDriverHandle  handle,  ///< driver handle from axl_driver_load
    const void      *data,    ///< option data (copied; NULL to clear)
    size_t           size     ///< option data size in bytes
);

/**
 * @brief Initialize the AXL runtime for a DXE driver.
 *
 * Drivers don't use AXL_APP / int main(). Call this from
 * DriverEntry to set up firmware table pointers (gST/gBS/gRT)
 * and I/O streams so axl_printf, axl_malloc, etc. work.
 *
 * @code
 * EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE ImageHandle,
 *                                EFI_SYSTEM_TABLE *SystemTable) {
 *     axl_driver_init(ImageHandle, SystemTable);
 *     axl_printf("Driver loaded\n");
 *     ...
 * }
 * @endcode
 */
void
axl_driver_init(
    void *image_handle,   ///< EFI_HANDLE from DriverEntry
    void *system_table    ///< EFI_SYSTEM_TABLE* from DriverEntry
);

/**
 * @brief Set the unload callback for the current driver image.
 *
 * Call from DriverEntry to register a cleanup function that runs
 * when the driver is unloaded. The callback has EFIAPI calling
 * convention — declare it as:
 *   EFI_STATUS EFIAPI MyUnload(EFI_HANDLE ImageHandle)
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_set_unload(
    void *unload_fn   ///< EFIAPI unload function pointer
);

/**
 * @brief Get the load options that were passed to the current image.
 *
 * Returns a UTF-8 copy of the load options string. Caller frees
 * with axl_free(). Useful for drivers that receive configuration
 * (e.g., a URL) via LoadOptions.
 *
 * @return options string, or NULL if no options or on error.
 */
char *
axl_driver_get_load_options(void);

/**
 * @brief Get the filesystem path the current image was loaded from.
 *
 * Returns a UTF-8 path like "fs0:\\drivers\\MyDriver.efi".
 * Useful for finding companion files next to the driver.
 * Caller frees with axl_free().
 *
 * @return path string, or NULL if unavailable.
 */
char *
axl_driver_get_image_path(void);

/**
 * @brief Connect controllers on a specific handle.
 *
 * Triggers driver binding for one handle (e.g., after installing
 * a filesystem protocol on a new handle). More targeted than
 * axl_driver_connect which reconnects all handles.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_driver_connect_handle(
    void *handle  ///< handle to connect (from axl_service_register, etc.)
);

/**
 * @brief Load, start, and connect all .efi drivers in a directory.
 *
 * Scans @p dir_path for files matching @p pattern (glob, e.g. "*.efi").
 * Each matching file is loaded, started, and connected. On return,
 * @p loaded_count receives the number of drivers successfully started.
 * Pass NULL for @p pattern to match all .efi files.
 *
 * @return 0 on success (even if no drivers found), -1 on error.
 */
int
axl_driver_load_dir(
    const char *dir_path,      ///< directory to scan (UTF-8)
    const char *pattern,       ///< glob pattern (NULL = "*.efi")
    size_t     *loaded_count   ///< [out] number of drivers loaded (may be NULL)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DRIVER_H */
