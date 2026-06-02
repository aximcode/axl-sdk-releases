/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-driver.h
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

#include <axl/axl-sys.h>

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
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_driver_load(
    const char       *path,   ///< path to .efi driver (UTF-8)
    AxlDriverHandle  *handle  ///< [out] receives driver handle
);

/**
 * @brief Load a driver image from a memory buffer.
 *
 * Buffer-source counterpart to axl_driver_load. Calls
 * `gBS->LoadImage` with `SourceBuffer`/`SourceSize` and no
 * `DevicePath`, returning the resulting handle for use with
 * axl_driver_set_load_options, axl_driver_start, and
 * axl_driver_unload.
 *
 * Used by tools that `.incbin` a companion driver into the app to
 * ship as a single binary. For the higher-level AxlService case use
 * axl_service_start_embedded — this primitive is for
 * non-AxlService drivers that still need per-call LoadOptions or
 * explicit handle tracking.
 *
 * The driver is loaded but NOT started. The image's
 * `LoadedImage->FilePath` is left NULL; drivers that read FilePath
 * at startup (some Driver-Binding-style drivers do, notably iPXE)
 * will not work via this entry point — load them by path instead.
 *
 * @return AXL_OK on success (`*out_handle` is set); AXL_ERR on
 *     argument validation failure or LoadImage failure
 *     (`*out_handle` is set to NULL).
 */
int
axl_driver_load_buffer(
    const unsigned char *buf,         ///< driver image bytes (must be non-NULL)
    size_t               len,         ///< length in bytes (must be > 0)
    AxlDriverHandle     *out_handle   ///< [out] driver handle for set_load_options/start/unload
);

/**
 * @brief Start a loaded driver image.
 *
 * Calls the driver's entry point. The driver registers its
 * binding protocol(s) but does not yet bind to devices.
 *
 * @return AXL_OK on success, AXL_ERR on error.
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
 * @return AXL_OK on success, AXL_ERR on error.
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
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_driver_disconnect(
    AxlDriverHandle handle  ///< driver handle
);

/**
 * @brief Unload a driver image from memory.
 *
 * The driver must be disconnected first. Also frees any load-options
 * copy installed via axl_driver_set_load_options() on this handle —
 * the firmware retains the LoadOptions pointer for the loaded-image
 * lifetime, so the AXL-side copy must be released here. Release runs
 * BEFORE gBS->UnloadImage so a UnloadImage failure still doesn't leak
 * the copy.
 *
 * @return AXL_OK on success, AXL_ERR on error.
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
 * The copy is owned by AXL and freed by axl_driver_unload() (or by
 * a subsequent set on the same handle, which replaces the previous
 * copy). Pass NULL data to clear load options and free any previous
 * copy. Call between axl_driver_load and axl_driver_start.
 *
 * AXL tracks at most 16 outstanding driver handles with load options
 * — a 17th call returns AXL_ERR and frees the would-be copy without
 * touching firmware state. Sequential load/unload is the realistic
 * case; if you legitimately need more concurrent driver instances
 * with load options, bump LOAD_OPTIONS_TABLE_SIZE in
 * src/util/axl-driver.c.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments, alloc failure,
 *     HandleProtocol failure, or tracking-table-full.
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
 * Most drivers don't need to call this directly — the
 * `AXL_DRIVER(entry, unload)` macro in `<axl.h>` emits the
 * DriverEntry stub and wires `axl_driver_init` automatically.
 * Use this manual path only when your driver publishes
 * spec-defined UEFI protocols (`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`,
 * `EFI_BLOCK_IO_PROTOCOL`, etc.) and you've opted into
 * `<uefi/axl-uefi.h>` for the spec types — in that case cast
 * the firmware-supplied `EFI_HANDLE` / `EFI_SYSTEM_TABLE *` to
 * the AXL parameter types at the call site (the underlying
 * pointers are bit-identical; the cast is a typing-only
 * formality).
 *
 * @code
 * // AXL-only driver:
 * static int my_main(AxlHandle image, AxlSystemTable *st);
 * static int my_unload(AxlHandle image);
 * AXL_DRIVER(my_main, my_unload)
 *
 * // Spec-protocol publisher (tier 2):
 * EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE h, EFI_SYSTEM_TABLE *st) {
 *     axl_driver_init((AxlHandle)h, (AxlSystemTable *)st);
 *     ...
 * }
 * @endcode
 */
void
axl_driver_init(
    AxlHandle        image_handle,  ///< image handle from DriverEntry
    AxlSystemTable  *system_table   ///< system table from DriverEntry
);

/**
 * @brief Set the unload callback for the current driver image.
 *
 * Call from DriverEntry to register a cleanup function that runs
 * when the driver is unloaded. The callback has EFIAPI calling
 * convention — declare it as:
 *   EFI_STATUS EFIAPI MyUnload(EFI_HANDLE ImageHandle)
 *
 * Cleanup contract — what the firmware does NOT do for you:
 *
 *   - Services registered via `axl_protocol_register` /
 *     `axl_protocol_register_multiple` are NOT auto-released. The
 *     AXL protocol registry never owned the install; it issued a
 *     `gBS->InstallProtocolInterface` and forgot. The unload
 *     callback must walk every protocol the driver published and
 *     call `axl_protocol_unregister` for each. Forgetting leaves
 *     dangling handle entries that point at freed driver memory —
 *     subsequent `LocateProtocol` calls hand consumers a stale
 *     vtable and the next dispatch faults.
 *   - Heap allocations made via `axl_malloc` are not auto-freed.
 *     `axl_mem_dump_leaks` (DEBUG builds) prints what was missed.
 *   - Events / timers created via the AxlLoop or backend layer
 *     stay live; close them with the matching `_close` calls.
 *
 * `sdk/examples/driver.c` shows the canonical shape.
 *
 * @return AXL_OK on success, AXL_ERR on error.
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
 * @brief Get the LoadOptions buffer as raw bytes (no encoding conversion).
 *
 * UEFI shell launches pass `LoadOptions` as a UCS-2 string —
 * axl_driver_get_load_options is the right entry point for that.
 * Programmatic loaders (axl_driver_set_load_options) pass arbitrary
 * bytes; this entry point hands them back unchanged.
 *
 * Used by AxlService to read its UTF-8 axl_config_to_string payload
 * without misinterpreting it as UCS-2.
 *
 * @return AXL_OK on success (out params populated with a borrowed
 *     pointer into the firmware's LoadedImage struct — do NOT free),
 *     AXL_ERR if the image has no LoadOptions or HandleProtocol
 *     failed.
 */
int
axl_driver_get_load_options_raw(
    const void **out_buf,    ///< [out] borrowed pointer into LoadedImage
    size_t      *out_size    ///< [out] LoadOptionsSize in bytes
);

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
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_driver_connect_handle(
    void *handle  ///< handle to connect (from axl_protocol_register, etc.)
);

/**
 * @brief Find a driver file on disk without loading it.
 *
 * Walks the same search order axl_driver_ensure() uses (image's
 * `drivers/<arch>/`, image's own directory, image's `drivers/`,
 * other volumes' `drivers/<arch>/`) and writes the first matching
 * existing path to @p out.
 *
 * Useful when the caller needs to control the LoadImage / StartImage
 * lifecycle directly — for example, to set load options between the
 * two for a driver that takes per-invocation configuration:
 * @code
 * char path[256];
 * if (axl_driver_locate("axl-webfs-dxe.efi", path, sizeof(path)) != 0) {
 *     axl_printf("axl-webfs-dxe.efi not found\n");
 *     return 1;
 * }
 * AxlDriverHandle h;
 * axl_driver_load(path, &h);
 * axl_driver_set_load_options(h, url_w, url_size);
 * axl_driver_start(h);
 * @endcode
 *
 * Same trust caveat as axl_driver_ensure: searches every mounted FAT
 * volume. Don't pass attacker-controlled @p driver_name.
 *
 * @return AXL_OK on success (path written to @p out), AXL_ERR if the driver
 *     wasn't found or @p out is too small to hold the result.
 */
int
axl_driver_locate(
    const char *driver_name,  ///< driver filename (e.g. "axl-webfs-dxe.efi")
    char       *out,          ///< [out] receives full path on success
    size_t      out_size      ///< capacity of @p out in bytes
);

/**
 * @brief Ensure a protocol-providing driver is loaded.
 *
 * If @p protocol_guid is already registered (LocateProtocol succeeds),
 * returns 0 immediately. Otherwise searches for @p driver_name and
 * loads + starts the first match found, in this order:
 *
 *   1. `drivers/<arch>/<driver_name>` on the volume the running image
 *      booted from
 *   2. `<image_dir>/<driver_name>` in the running image's own directory
 *   3. `drivers/<driver_name>` at the running image's volume root
 *   4. `drivers/<arch>/<driver_name>` on every other mounted FAT volume
 *
 * The arch suffix is "x64" or "aa64", matching the running image's
 * architecture. After load+start, LocateProtocol is re-checked; if
 * the protocol still isn't registered, the driver is unloaded and
 * the function returns -1.
 *
 * Safe to call multiple times — repeats short-circuit at step 1.
 * EFI_ALREADY_STARTED on StartImage is treated as success.
 *
 * Typical use, before touching a driver-provided protocol. Note the
 * cast: `EFI_RAM_DISK_PROTOCOL_GUID` is an `EFI_GUID` from the
 * generated UEFI headers; AxlGuid is layout-compatible, so a const
 * cast lets callers pass it through without including any UEFI
 * headers in their own public surface:
 * @code
 * if (axl_driver_ensure((const AxlGuid *)&EFI_RAM_DISK_PROTOCOL_GUID,
 *                       "RamDiskDxe.efi") != 0) {
 *     axl_printf("RamDiskDxe.efi not available\n");
 *     return 1;
 * }
 * @endcode
 *
 * **Trust model.** This function will load the first matching .efi
 * file off any mounted FAT volume — including a USB stick the user
 * just plugged in. UEFI executes loaded drivers with full firmware
 * privileges. Only call this with driver names you trust, and don't
 * call it with attacker-controlled @p driver_name values.
 *
 * @return AXL_OK if the protocol is registered (was already, or after
 *     loading the driver); AXL_ERR if the driver wasn't found, failed to
 *     load/start, or didn't register the protocol after starting.
 */
int
axl_driver_ensure(
    const AxlGuid *protocol_guid,  ///< protocol GUID to look up (must be non-NULL)
    const char    *driver_name     ///< driver filename (e.g. "RamDiskDxe.efi")
);

/**
 * @brief Ensure a protocol-providing driver is loaded, with embedded
 *        fallback and optional caller override.
 *
 * Generalizes axl_driver_ensure() for tools that ship a driver blob
 * baked into the .efi binary itself, so they work as self-contained
 * binaries on minimal firmware that omits the corresponding optional
 * UEFI 2.6+ DXE module.
 *
 * Resolution order:
 *   1. `LocateProtocol(protocol_guid)` — short-circuit if firmware
 *      already provides the protocol. Most OEM firmware (Dell, HP,
 *      Supermicro) ships RamDiskDxe and similar in their firmware
 *      volume; this is the common path.
 *   2. If `override_name` is non-NULL, search disk for that name only
 *      using axl_driver_ensure()'s 4-path search. The embedded blob
 *      is NOT used as a fallback — caller explicitly opted into a
 *      specific external driver.
 *   3. Otherwise, search disk for `driver_name` using the same 4-path
 *      search. If found, load + start it.
 *   4. If still not registered and `embedded_buf` is non-NULL, call
 *      `LoadImage(SourceBuffer=embedded_buf, SourceSize=embedded_len)`
 *      followed by `StartImage`. No filesystem access.
 *
 * The embedded path is the safety net — it lets the tool work on
 * firmware that ships neither the protocol nor a user-staged copy.
 *
 * `axl_driver_ensure(g, n)` is exactly `axl_driver_ensure_with_embedded(
 * g, n, NULL, 0, NULL, NULL, 0)`.
 *
 * **LoadOptions** (`load_options` / `load_options_size`): when
 * non-NULL, AXL installs the bytes into the loaded image's
 * `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` BEFORE `StartImage` is
 * called, via the same axl_driver_set_load_options path (so
 * unload-time release is automatic). Applied on BOTH the disk-load
 * path (steps 2/3) and the embedded path (step 4). Skipped on the
 * step-1 short-circuit — the firmware-provided protocol implies a
 * driver instance the consumer doesn't own. Used by AxlService to
 * ship a foreground process's options through to the driver image
 * (typically via axl_config_to_string).
 *
 * Trust caveat (same as axl_driver_ensure): step 3 will load the first
 * matching .efi off any mounted FAT volume. Don't pass attacker-
 * controlled @p driver_name or @p override_name. The embedded buffer
 * is whatever the build system baked in — caller's responsibility to
 * verify provenance.
 *
 * @return AXL_OK if the protocol is registered (was already, or after
 *     loading); AXL_ERR if all four steps failed.
 */
int
axl_driver_ensure_with_embedded(
    const AxlGuid       *protocol_guid,    ///< protocol GUID to look up (must be non-NULL)
    const char          *driver_name,      ///< canonical driver filename, e.g. "RamDiskDxe.efi"
    const unsigned char *embedded_buf,     ///< embedded .efi bytes (may be NULL)
    size_t               embedded_len,     ///< length of embedded_buf in bytes (0 if NULL)
    const char          *override_name,    ///< user-provided override name (may be NULL)
    const void          *load_options,     ///< LoadOptions to install pre-Start (may be NULL)
    size_t               load_options_size ///< size of @p load_options in bytes (0 if NULL)
);

/**
 * @brief Load, start, and connect all .efi drivers in a directory.
 *
 * Scans @p dir_path for files matching @p pattern (glob, e.g. "*.efi").
 * Each matching file is loaded, started, and connected. On return,
 * @p loaded_count receives the number of drivers successfully started.
 * Pass NULL for @p pattern to match all .efi files.
 *
 * @return AXL_OK on success (even if no drivers found), AXL_ERR on error.
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
