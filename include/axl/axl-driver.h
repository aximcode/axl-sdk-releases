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
#include <axl/axl-attempt.h>   /* AxlAttempt — axl_driver_load_dir_guarded */

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a loaded driver image.
typedef void *AxlDriverHandle;

/**
 * @brief Caller-supplied identity for a buffer/embedded driver load.
 *
 * Passed to axl_driver_load_buffer_with_image_info (and the
 * shared-driver `_with_image_info` locate) to give a memory-loaded
 * image a real, renderable device path. A buffer load has no on-disk
 * `DevicePath`, so the firmware would otherwise leave
 * `LoadedImage->FilePath` and the handle's
 * `gEfiLoadedImageDevicePathProtocol` interface NULL — which the
 * aarch64 UEFI shell faults on while rendering (`dh -p` / `dh -v`).
 * Every field is optional; AXL fills sensible non-NULL defaults so the
 * resulting device path is never NULL.
 */
typedef struct {
    const char    *file_name;     ///< MEDIA_FILEPATH leaf, e.g. "doDriver.efi"; NULL -> derived from the load's filename, else the app's basename
    AxlHandle      device_handle; ///< optional: set LoadedImage->DeviceHandle; NULL -> left as the firmware set it
    const AxlGuid *vendor_guid;   ///< optional Vendor() node prepended to the synthesized path; NULL -> omitted
} AxlEmbeddedImageInfo;

// ===================================================================
// Protocol publishing
//
// AXL-typed wrappers over the firmware's InstallProtocolInterface /
// UninstallProtocolInterface. A driver (or any image) uses these to
// publish a protocol interface other code can locate — the core of a
// Type-A resident service / protocol-publisher driver. No <uefi/...>
// include or gBS-> drop-down required: AxlHandle is EFI_HANDLE and
// AxlGuid is binary-compatible with EFI_GUID.
// ===================================================================

/**
 * @brief Install a protocol interface on a handle.
 *
 * Publishes @p iface under @p guid so other images can find it via the
 * locate/handle APIs. To create a new handle for a fresh service, pass
 * a pointer to a NULL handle: the firmware allocates one and writes it
 * back through @p handle. To add another protocol to a handle you
 * already own, pass that handle.
 *
 * The interface is borrowed, not copied: @p iface must stay valid until
 * it is uninstalled. A resident driver that must outlive its own image
 * unload should allocate the interface from a boot-services pool (so the
 * AXL leak tracker doesn't reclaim it at image exit), not via
 * `axl_malloc`.
 *
 * @code
 * static AxlHandle  my_handle = NULL;   // fresh handle on first install
 * static MyIface    iface     = { ... };
 * axl_protocol_install(&MY_PROTOCOL_GUID, &iface, &my_handle);
 * @endcode
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_protocol_install(
    const AxlGuid  *guid,     ///< protocol GUID (non-NULL)
    void           *iface,    ///< interface pointer to publish (non-NULL); borrowed, must outlive the install
    AxlHandle      *handle    ///< [in,out] handle to install on; if `*handle` is NULL a fresh handle is allocated and written back. Must be non-NULL.
);

/**
 * @brief Uninstall a protocol interface from a handle.
 *
 * Removes the @p guid / @p iface pairing previously installed with
 * axl_protocol_install. Pass the same interface pointer that was
 * installed. The firmware rejects the removal (AXL_ERR) if another
 * image still has the protocol open; uninstall during clean unload,
 * after dependents have closed it.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_protocol_uninstall(
    AxlHandle       handle,   ///< handle the protocol was installed on (non-NULL)
    const AxlGuid  *guid,     ///< protocol GUID (non-NULL)
    void           *iface     ///< the same interface pointer passed to axl_protocol_install (non-NULL)
);

// ===================================================================
// Driver Model binding — Type B (write a UEFI Driver Model driver)
// ===================================================================
//
// A Type-B driver binds to controllers that expose a particular protocol
// (PCI I/O, USB I/O, a custom bus protocol) — the standard UEFI Driver
// Model. The fiddly mechanics are what AXL manages for you:
//   - the EFIAPI Supported / Start / Stop thunks (marshalling
//     EFI_HANDLE <-> AxlHandle and EFI_STATUS <-> int, hiding the calling
//     convention);
//   - the OpenProtocol(BY_DRIVER) / CloseProtocol ownership bookkeeping
//     around `binds` — the load-bearing mechanic that tags controller
//     ownership and prevents double-binding;
//   - building and installing EFI_DRIVER_BINDING_PROTOCOL (Version,
//     ImageHandle, DriverBindingHandle) + EFI_COMPONENT_NAME2_PROTOCOL.
// You write three callbacks in pure AXL C — `AxlHandle`, no `EFI_HANDLE`,
// no EFIAPI, no OpenProtocol dance.
//
// v1 scope is the 90% case: "this driver binds to controllers exposing
// protocol X" (a device / function driver). Bus drivers
// (`RemainingDevicePath`, child-handle creation, Stop's child buffer) are
// deferred to v2; the raw EFI_DRIVER_BINDING_PROTOCOL stays available as the
// escape hatch for them.

/**
 * @brief A UEFI Driver Model binding — what to bind to, plus the three
 *        callbacks AXL drives for you.
 *
 * Fill this in and install it with axl_driver_binding_install from your
 * AXL_DRIVER entry point. AXL **copies** the descriptor, so a stack or static
 * AxlDriverBinding is fine — but @c name and the GUID @c binds points at are
 * borrowed and must outlive the driver (string and GUID literals are the
 * norm).
 */
typedef struct {
    /// Human-readable driver name, surfaced via EFI_COMPONENT_NAME2_PROTOCOL
    /// (the shell `drivers` listing). Required (non-NULL). Borrowed.
    const char    *name;

    /// The protocol GUID a controller must expose for this driver to manage
    /// it. AXL opens it BY_DRIVER to test and claim ownership. Borrowed.
    const AxlGuid *binds;

    /// Optional extra gate (NULL = manage any controller exposing @c binds).
    /// Called during Supported AFTER AXL has confirmed @c binds is present
    /// and openable BY_DRIVER. Return true to manage @p controller. Keep it
    /// side-effect-free — Supported is a pure query the firmware may run
    /// against many controllers.
    bool (*supported)(AxlHandle controller, void *ctx);

    /// Start managing @p controller. AXL has already opened @c binds
    /// BY_DRIVER (claiming ownership) and hands you the bound interface as
    /// @p iface — cast it to the real protocol type (e.g.
    /// `EFI_PCI_IO_PROTOCOL *`); operating it is the driver's job, the one
    /// unavoidable raw-EFI-type touch. Initialise the device, publish any
    /// child protocols. Return AXL_OK on success; on any other value AXL
    /// rolls back (CloseProtocol) and reports failure to ConnectController.
    int (*start)(AxlHandle controller, void *iface, void *ctx);

    /// Stop managing @p controller (DisconnectController / driver unload).
    /// Tear down what start built (uninstall child protocols, quiesce the
    /// device). AXL closes @c binds afterward. Return AXL_OK on success.
    int (*stop)(AxlHandle controller, void *ctx);

    /// Borrowed context passed to every callback (NULL if unused). Shared
    /// across all controllers this driver manages — key per-controller state
    /// by @p controller.
    void *ctx;
} AxlDriverBinding;

/**
 * @brief Install a Driver Model binding (Type B) — call once from your
 *        AXL_DRIVER entry point.
 *
 * Builds and installs an EFI_DRIVER_BINDING_PROTOCOL (with AXL's managed
 * Supported / Start / Stop thunks) plus an EFI_COMPONENT_NAME2_PROTOCOL on
 * the driver's own image handle, so the firmware's ConnectController drives
 * your @p db callbacks against matching controllers. AXL copies @p db (see
 * the lifetime note on AxlDriverBinding) and retains the binding record while
 * it is installed.
 *
 * **Teardown:** a *driver* must call axl_driver_binding_uninstall from its
 * unload callback — firmware-driven driver unload does NOT drain axl_atexit.
 * AXL does register an axl_atexit hook as a safety net, but that fires only at
 * app exit (AXL_APP / CRT0), so it covers an *app* that installs a binding,
 * not a driver being unloaded.
 *
 * v1 installs **one binding per driver image** (on the image handle) — the
 * "device driver binds to protocol X" case. A second call returns AXL_ERR
 * (the firmware rejects a duplicate EFI_DRIVER_BINDING_PROTOCOL on the image
 * handle). A driver that must bind several protocols from one image uses the
 * raw EFI_DRIVER_BINDING_PROTOCOL escape hatch; multi-binding-per-image (one
 * binding per fresh handle) is a planned follow-on.
 *
 * @return AXL_OK on success; AXL_ERR on bad arguments (NULL @p db / @c name /
 *     @c binds / @c start / @c stop) or installation failure.
 */
int
axl_driver_binding_install(
    const AxlDriverBinding  *db   ///< the binding descriptor (copied)
);

/**
 * @brief Uninstall the Driver Model binding installed by
 *        axl_driver_binding_install.
 *
 * Removes the EFI_DRIVER_BINDING_PROTOCOL + EFI_COMPONENT_NAME2_PROTOCOL from
 * the image handle and frees the binding record. v1 tracks one binding per
 * image, so this takes no argument — it removes that binding.
 *
 * **Call this from a driver's unload callback.** AXL also registers the same
 * teardown via axl_atexit, but that fires only at app exit (AXL_APP / CRT0),
 * NOT on firmware-driven driver unload — so a Type-B *driver* must uninstall
 * its binding explicitly here, after disconnecting any controllers it manages
 * (otherwise the firmware still references the binding and the uninstall
 * fails). Apps that install a binding can rely on the axl_atexit hook instead.
 * Calling this removes that hook, so it never double-runs.
 *
 * @return AXL_OK if the binding was uninstalled and freed; AXL_ERR if no
 *     binding is installed, or if the firmware still references it (e.g. a
 *     controller is still bound — disconnect it first). On the latter the
 *     record is deliberately kept alive rather than freed-and-dangled.
 */
int
axl_driver_binding_uninstall(void);

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
 * @brief Load a driver from the running app's own directory.
 *
 * Resolves @p file_name strictly within the directory the current
 * application image was loaded from (derived from
 * axl_app_image_path()), then loads it via axl_driver_load — so the
 * loaded image gets a real on-disk MEDIA_FILEPATH device path (never
 * NULL). Use this to load a companion driver staged next to the app
 * while refusing to pick one up from anywhere else (an
 * attacker-controlled volume root, another `fsN:`, a parent
 * directory).
 *
 * @p file_name must be a bare filename: any `/`, `\\`, or `:` is
 * rejected so the name cannot escape the app directory.
 *
 * @return AXL_OK on success (`*out_handle` is set); AXL_INVALID if
 *     @p file_name contains a path separator or drive prefix; AXL_ERR
 *     on NULL arguments or when the app has no filesystem image path
 *     (network / RAM-disk boot); AXL_NOT_FOUND if the file is not
 *     present in the app directory. `*out_handle` is set to NULL on
 *     any failure.
 */
AxlStatus
axl_driver_load_sibling(
    const char       *file_name,  ///< bare driver filename (no '/', '\\', or ':'), e.g. "doDriver.efi"
    AxlDriverHandle  *out_handle  ///< [out] receives driver handle
);

/**
 * @brief Load a driver image from a memory buffer.
 *
 * Buffer-source counterpart to axl_driver_load. Calls
 * `gBS->LoadImage` with `SourceBuffer`/`SourceSize`, then synthesizes
 * a renderable device path for the loaded image (see
 * axl_driver_load_buffer_with_image_info) so the image's
 * `LoadedImage->FilePath` and the handle's
 * `gEfiLoadedImageDevicePathProtocol` interface are never NULL. The
 * synthesized leaf name defaults to the app's basename here; callers
 * that want to name the driver (or set DeviceHandle / a Vendor GUID)
 * use axl_driver_load_buffer_with_image_info.
 *
 * Used by tools that `.incbin` a companion driver into the app to
 * ship as a single binary. For the higher-level AxlService case use
 * axl_service_start_embedded — this primitive is for
 * non-AxlService drivers that still need per-call LoadOptions or
 * explicit handle tracking.
 *
 * The driver is loaded but NOT started.
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
 * @brief Load a driver from a memory buffer with a caller-set identity.
 *
 * Like axl_driver_load_buffer, but lets the caller populate the
 * loaded image's identity via @p info so `dh -p` / `dh -v` show real
 * values and the aarch64 shell does not fault on a NULL device path.
 * After `gBS->LoadImage`, AXL reads the image's ImageBase/ImageSize
 * back from EFI_LOADED_IMAGE_PROTOCOL and installs a device path of
 * the form
 * `[Vendor(info->vendor_guid)] / MemoryMapped(EfiBootServicesCode,
 * ImageBase, ImageBase+ImageSize) / FilePath("\\<file_name>")`,
 * setting both `LoadedImage->FilePath` (the FilePath portion) and the
 * handle's `gEfiLoadedImageDevicePathProtocol` interface, plus
 * `LoadedImage->DeviceHandle` when @p info supplies one.
 *
 * @p info may be NULL (equivalent to axl_driver_load_buffer). Any
 * unset @p info field falls back to a non-NULL default; identity
 * synthesis is best-effort — if it fails the driver still loads
 * (AXL_OK) and a warning is logged, leaving the firmware's defaults.
 *
 * @return AXL_OK on success (`*out_handle` is set); AXL_ERR on
 *     argument validation failure or LoadImage failure
 *     (`*out_handle` is set to NULL).
 */
int
axl_driver_load_buffer_with_image_info(
    const unsigned char        *buf,        ///< driver image bytes (must be non-NULL)
    size_t                      len,        ///< length in bytes (must be > 0)
    const AxlEmbeddedImageInfo *info,       ///< loaded-image identity; NULL -> defaults
    AxlDriverHandle            *out_handle  ///< [out] driver handle for set_load_options/start/unload
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
 * Same contract as axl_app_image_path for the same condition: an image
 * that was NOT loaded from a file — a buffer / memory load, whose device
 * path AXL synthesizes after the fact — returns NULL rather than a
 * volume-less path naming a file it never came from.
 *
 * @return path string, or NULL if this image was not loaded from a file
 *     (or the loaded-image protocol carried no FILEPATH node).
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
 * @brief Disconnect all drivers from a specific controller handle.
 *
 * Symmetric counterpart to axl_driver_connect_handle: triggers the Stop
 * sequence of every driver currently managing @p handle (the firmware is
 * asked to disconnect all drivers and all children). Use it to release a
 * controller you bound — for example a Type-B `AxlDriverBinding` example
 * tearing down the controller it drove, so its `stop` callback runs.
 *
 * Note the handle kinds differ from axl_driver_disconnect: that takes a
 * loaded *driver-image* handle (from axl_driver_load) and detaches that
 * driver from the devices it bound; this takes a *controller* handle and
 * detaches the drivers bound to it. They are the two sides of the same
 * relation.
 *
 * @return AXL_OK on success, including when no driver was managing the
 *     handle (a no-op). EFI_NOT_FOUND is also mapped to AXL_OK as a
 *     defensive symmetry with axl_driver_connect_handle; AXL_ERR on a NULL
 *     handle or a real disconnect failure.
 */
int
axl_driver_disconnect_handle(
    void *handle  ///< controller handle to disconnect (non-NULL)
);

/**
 * @brief Find a driver file on disk without loading it.
 *
 * Walks the same search order axl_driver_ensure() uses (image's own
 * directory, image's `drivers/<arch>/`, image's `drivers/`, other
 * volumes' `drivers/<arch>/`) and writes the first matching existing
 * path to @p out.
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
 *   1. `<image_dir>/<driver_name>` in the running image's own directory
 *      (the sibling) — a co-located driver is the most specific intent
 *      and wins over a stale copy elsewhere
 *   2. `drivers/<arch>/<driver_name>` on the volume the running image
 *      booted from
 *   3. `drivers/<driver_name>` at the running image's volume root
 *   4. `drivers/<arch>/<driver_name>` on every other mounted FAT volume
 *
 * The arch suffix is "x64" or "aa64", matching the running image's
 * architecture. After a candidate load+start, LocateProtocol is
 * re-checked; if the protocol still isn't registered that candidate is
 * unloaded and the search continues, ultimately returning
 * `AXL_NOT_FOUND` if no candidate publishes it.
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
 *     loading the driver); AXL_NOT_FOUND if no candidate path yielded a
 *     driver that loaded, started, and registered the protocol; AXL_ERR
 *     only on a NULL @p protocol_guid or @p driver_name.
 */
AxlStatus
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
 *     loading); AXL_NOT_FOUND if the disk search (and the embedded
 *     fallback, when attempted) all failed to produce a registered
 *     protocol; AXL_ERR only on a NULL @p protocol_guid or @p driver_name.
 */
AxlStatus
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
 * @brief Ensure a protocol-providing driver is loaded from **exactly one
 *     named file** — no search, no embedded fallback.
 *
 * The pinned sibling of axl_driver_ensure_with_embedded. That
 * function's 4-path search is a convenience; it is also a hazard when
 * two copies of the same driver filename exist on the box, because the
 * search order — not the caller — decides which one wins. A caller that
 * staged the image it wants and knows where it put it says so here and
 * takes the search out of the picture entirely. `override_name` does
 * NOT cover this: it substitutes a *name* into the same search, so it
 * cannot separate two files that share a name.
 *
 * Resolution:
 *   1. `LocateProtocol(protocol_guid)` — short-circuit if the protocol
 *      is already registered (same as the searching variants; the
 *      already-published instance is not the caller's to re-configure,
 *      so @p load_options is ignored on this path).
 *   2. Load @p driver_path, install @p load_options if any, `StartImage`,
 *      then verify the protocol got registered. If it did not, the image
 *      is unloaded and this reports failure — it does not fall back to
 *      anything.
 *
 * @return AXL_OK if the protocol is registered (was already, or after
 *     loading @p driver_path); AXL_NOT_FOUND if @p driver_path could not
 *     be loaded, failed to start, or started without registering the
 *     protocol; AXL_INVALID on a NULL @p protocol_guid or
 *     @p driver_path.
 */
AxlStatus
axl_driver_ensure_from_path(
    const AxlGuid *protocol_guid,     ///< protocol GUID to look up (must be non-NULL)
    const char    *driver_path,       ///< exact path to the driver .efi, e.g. "fs0:\\drivers\\x64\\my-dxe.efi"
    const void    *load_options,      ///< LoadOptions to install pre-Start (may be NULL)
    size_t         load_options_size  ///< size of @p load_options in bytes (0 if NULL)
);

/**
 * @brief Load, start, and connect all .efi drivers in a directory.
 *
 * Scans @p dir_path for files matching @p pattern (glob, e.g. "*.efi").
 * Each matching file is loaded, started, and connected. On return,
 * @p loaded_count receives the number of drivers successfully started.
 * Pass NULL for @p pattern to match all .efi files.
 *
 * Loading an arbitrary .efi can hang or fault the box, and this
 * function offers no protection against that: one bad driver in
 * @p dir_path wedges the machine, and the next boot walks the same
 * directory in the same order and wedges identically. Use
 * axl_driver_load_dir_guarded() where that matters.
 *
 * @return AXL_OK on success (even if no drivers found), AXL_ERR on error.
 */
int
axl_driver_load_dir(
    const char *dir_path,      ///< directory to scan (UTF-8)
    const char *pattern,       ///< glob pattern (NULL = "*.efi")
    size_t     *loaded_count   ///< [out] number of drivers loaded (may be NULL)
);

/**
 * @brief Load a directory of drivers under crash-culprit protection.
 *
 * axl_driver_load_dir() plus an AxlAttempt guard, which is what makes
 * the sweep survivable: each driver is breadcrumbed by filename before
 * it is loaded, so one that hangs or resets the box is identified and
 * quarantined on the next boot and skipped from then on. The sweep then
 * makes progress instead of dying in the same place every boot.
 *
 * With @p guard non-NULL, per matching file:
 *   - already quarantined → skipped, not loaded, not counted;
 *   - otherwise breadcrumbed, then loaded/started/connected, then the
 *     breadcrumb is cleared.
 *
 * A crash left over from a previous boot is recovered once, up front,
 * before the walk — so the culprit is on the quarantine list before the
 * walk can reach it. Only the breadcrumb and the quarantine list are
 * used; nothing is written to the guard's result log, since the outcome
 * vocabulary belongs to the caller.
 *
 * @p guard must be an initialized AxlAttempt (see axl_attempt_init()) —
 * the namespace and vendor GUID are the caller's, so two consumers
 * sweeping different directories keep separate quarantine lists.
 * Passing NULL is explicitly allowed and is exactly
 * axl_driver_load_dir(): no breadcrumb, no skipping.
 *
 * @return AXL_OK on success (even if no drivers found or all were
 *     quarantined), AXL_ERR on error.
 */
int
axl_driver_load_dir_guarded(
    const char       *dir_path,      ///< directory to scan (UTF-8)
    const char       *pattern,       ///< glob pattern (NULL = "*.efi")
    const AxlAttempt *guard,         ///< crash-culprit guard, or NULL for none
    size_t           *loaded_count   ///< [out] number of drivers loaded (may be NULL)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DRIVER_H */
