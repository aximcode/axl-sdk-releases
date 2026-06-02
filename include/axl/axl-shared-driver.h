/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-shared-driver.h
 *
 * Convenience layer for the "thin launcher + resident driver"
 * pattern — sibling to @c axl-service.h but for synchronous-RPC
 * consumers that don't need a long-running event loop.
 *
 * Use this when a tool's per-invocation cost is dominated by setup
 * (sidecar parsing, opening firmware protocols) and you want to
 * amortize that cost across many launcher invocations within a
 * single boot. Each launcher invocation collapses to a
 * @c LocateProtocol + a vtable call once the driver is resident.
 *
 * The two halves compose existing primitives:
 *
 *   Driver image (DriverEntry):
 *     axl_pci_ids_load(NULL);        // heavy init once per boot
 *     axl_shared_publish("my-tool", &gVtable, &gHandle);
 *
 *   Launcher (int main):
 *     MyVtable *vt;
 *     axl_shared_locate("my-tool", "myToolDxe.efi",
 *                       AXL_EMBED_DATA(my_tool_driver),
 *                       AXL_EMBED_SIZE(my_tool_driver),
 *                       (void**)&vt);
 *     return vt->do_run(argc, argv);
 *
 * No new SDK type is added: the consumer owns its vtable struct and
 * its CRT wiring (@c AXL_DRIVER on the driver side, @c int @c main on
 * the launcher side). These three functions only hide the
 * GUID-derivation convention (name → v5 from a fixed
 * AXL_SHARED_DRIVER namespace) and the
 * axl_driver_ensure_with_embedded + axl_protocol_find_guid
 * choreography.
 *
 * For the full pattern walkthrough see
 * @c docs/AXL-Shared-Driver-Recipe.md and
 * @c sdk/examples/shared-driver-demo/.
 */

#ifndef AXL_SHARED_DRIVER_H
#define AXL_SHARED_DRIVER_H

#include <stddef.h>

#include <axl/axl-sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Derive the protocol-identity GUID for a shared driver.
 *
 * @c axl_shared_publish / @c axl_shared_locate use this internally;
 * exposed for consumers that want to call @c LocateProtocol directly
 * (e.g. when publishing a secondary interface on the same handle).
 *
 * Implementation: @c axl_guid_v5 against a fixed
 * AXL_SHARED_DRIVER namespace. Result is deterministic from @p name
 * alone — both halves of the consumer reach the same GUID by passing
 * the same string.
 *
 * @return AXL_OK on success, AXL_ERR if @p name or @p out is NULL.
 */
int
axl_shared_driver_guid(
    const char *name,   ///< shared-driver identity (e.g. "do-tool")
    AxlGuid    *out     ///< [out] derived GUID
);

/**
 * @brief Publish a shared-driver vtable from a driver image.
 *
 * Wraps @ref axl_protocol_register_guid — derives the GUID from
 * @p name via @ref axl_shared_driver_guid, installs @p iface on a new
 * UEFI handle (or @p *handle if non-NULL), returns the handle in
 * @p *handle.
 *
 * Call from @c DriverEntry (under @c AXL_DRIVER) after any per-boot
 * setup work (sidecar loads, protocol opens, cached state). The
 * paired teardown is @ref axl_shared_driver_unpublish from the
 * driver's unload callback.
 *
 * @return AXL_OK on success, AXL_ERR on argument validation or
 *     install failure.
 */
int
axl_shared_driver_publish(
    const char  *name,         ///< shared-driver identity
    void        *iface,        ///< consumer-owned vtable pointer
    AxlHandle   *out_handle    ///< [out] receives the handle for unpublish
);

/**
 * @brief Unregister a shared-driver vtable from a driver image.
 *
 * Wraps @ref axl_protocol_unregister_guid. Call from the driver's
 * unload callback. After this returns the launcher's
 * @ref axl_shared_driver_locate will fail to resolve until a new
 * publish call.
 *
 * @return AXL_OK on success, AXL_ERR on argument validation or
 *     uninstall failure.
 */
int
axl_shared_driver_unpublish(
    const char  *name,         ///< shared-driver identity
    AxlHandle    handle,       ///< handle returned by @ref axl_shared_driver_publish
    void        *iface         ///< the vtable pointer originally published
);

/**
 * @brief Locate (or load + locate) a shared-driver vtable from a launcher.
 *
 * Composes @ref axl_driver_ensure_with_embedded and
 * @ref axl_protocol_find_guid in three steps:
 *
 *   1. Derive the GUID via @ref axl_shared_driver_guid (from @p name).
 *   2. @c axl_driver_ensure_with_embedded ensures the driver is loaded
 *      (already-resident → short-circuit; otherwise on-disk
 *      @p driver_filename, falling back to the embedded blob
 *      @p embed_blob / @p embed_len).
 *   3. @c axl_protocol_find_guid resolves the published vtable.
 *
 * On success @p *out_iface points at the consumer's vtable struct
 * (consumer-owned types, AXL doesn't validate the layout — same ABI
 * contract as @c axl-service.h cross-image data).
 *
 * @return AXL_OK on success, AXL_ERR if the driver fails to load /
 *     start, the protocol isn't published after start, or any
 *     argument is invalid.
 */
int
axl_shared_driver_locate(
    const char           *name,             ///< shared-driver identity (must match the driver's publish)
    const char           *driver_filename,  ///< on-disk driver filename (e.g. "myToolDxe.efi")
    const unsigned char  *embed_blob,       ///< embedded driver bytes (.incbin via AXL_EMBED_DATA)
    size_t                embed_len,        ///< length of @p embed_blob in bytes
    void                **out_iface         ///< [out] receives the vtable pointer
);

/**
 * @brief Tear down a shared-driver: uninstall its protocol and unload
 *     its image.
 *
 * Symmetric counterpart to @ref axl_shared_driver_publish, callable
 * from a launcher (or any image OTHER than the driver itself).
 *
 * Resolution:
 *
 *   1. Derive the protocol GUID via @ref axl_shared_driver_guid.
 *   2. @c LocateHandleBuffer(ByProtocol, ...) finds the image handle
 *      that installed the protocol. Since `axl_shared_driver_publish`
 *      installs on the driver's `gImageHandle` (when the consumer
 *      leaves @c *out_handle null), the protocol lives on exactly one
 *      handle: the driver's loaded-image handle.
 *   3. @ref axl_driver_unload releases the AXL-tracked LoadOptions
 *      copy (if any) and calls `gBS->UnloadImage`. The driver's
 *      registered unload callback (via `AXL_DRIVER`) runs as part of
 *      UnloadImage — that's where the driver's own cleanup happens
 *      (`axl_shared_driver_unpublish` + any consumer-side free).
 *
 * After this returns, the next @ref axl_shared_driver_locate will
 * fall through to disk / embed because LocateProtocol misses.
 *
 * Safe to call when the driver isn't loaded — returns AXL_OK because
 * the post-condition (driver not resident) already holds.
 *
 * **MUST NOT** be called from inside the driver image itself; UEFI's
 * UnloadImage semantics on a self-executing image are undefined. The
 * driver-side use case is served by @ref axl_shared_driver_unpublish
 * already.
 *
 * Typical use case: a launcher's `--reload` developer flag that
 * forces a fresh driver image to load on the next invocation,
 * skipping the resident-driver short-circuit.
 *
 * @return AXL_OK on success or when the driver wasn't loaded;
 *     AXL_ERR if LocateHandleBuffer returned a handle but the
 *     subsequent unload failed.
 */
int
axl_shared_driver_unload(
    const char *name   ///< shared-driver identity (same name passed to publish/locate)
);

/**
 * @brief Locate a shared-driver vtable, passing config to the driver.
 *
 * Like @ref axl_shared_driver_locate but also installs @p load_options
 * bytes into the driver image's @c EFI_LOADED_IMAGE_PROTOCOL.LoadOptions
 * before the driver's @c DriverEntry runs. The driver reads them via
 * @ref axl_driver_get_load_options_raw (or @ref axl_driver_get_load_options
 * if UCS-2-shaped) to receive per-invocation config — typical use cases:
 * verb args, log level, an override sidecar path.
 *
 * Skipped on the resident-driver short-circuit (step 1 of
 * @ref axl_driver_ensure_with_embedded): if the driver was published
 * by a previous invocation, its already-installed LoadOptions are
 * preserved. Pass per-call args via the vtable instead when they
 * need to vary across invocations within a boot.
 *
 * Pass @c load_options == NULL or @c load_options_size == 0 to skip
 * the install — equivalent to calling @ref axl_shared_driver_locate.
 *
 * @return AXL_OK on success, AXL_ERR on load/start/locate failure.
 */
int
axl_shared_driver_locate_with_load_options(
    const char           *name,              ///< shared-driver identity
    const char           *driver_filename,   ///< on-disk driver filename
    const unsigned char  *embed_blob,        ///< embedded driver bytes
    size_t                embed_len,         ///< length of @p embed_blob
    const void           *load_options,      ///< bytes to install (NULL → skip)
    size_t                load_options_size, ///< @p load_options length
    void                **out_iface          ///< [out] receives the vtable pointer
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SHARED_DRIVER_H */
