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
#include <axl/axl-driver.h>   /* AxlEmbeddedImageInfo */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The SDK-standard shared-driver entry vtable.
 *
 * A driver built with @c AXL_SHARED_DRIVER publishes this; a launcher
 * built with @c AXL_SHARED_DRIVER_LAUNCHER (or calling
 * @ref axl_shared_driver_run / @ref axl_shared_driver_dispatch) drives it.
 * The single @c run entry has the canonical @c main signature, so once
 * stdin and exit status are bridged by the SDK the cross-image contract
 * is just "call an int(int,char**)". Consumers no longer define a custom
 * vtable or protocol header.
 *
 * @c run receives the launcher's argv verbatim — @c argv[0] is the program
 * name (as in @c int @c main); parse args/verb from @c argv[1].
 */
typedef struct {
    int (*run)(int argc, char **argv);   ///< per-dispatch entry (== int main); argv[0] is the program name, verb/args from argv[1]
} AxlSharedDriverVtable;

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
 * Wraps @ref axl_protocol_install — derives the GUID from
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
 * Wraps @ref axl_protocol_uninstall. Call from the driver's
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
 * @return AXL_OK on success; AXL_NOT_FOUND when a usable vtable can't be
 *     obtained -- not resolvable from any candidate path or the embedded
 *     blob, or a driver loaded but didn't start / publish the expected
 *     protocol; AXL_ERR on invalid arguments.
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
 * @return AXL_OK on success; AXL_NOT_FOUND when a usable vtable can't be
 *     obtained -- not resolvable from any candidate path or the embedded
 *     blob, or a driver loaded but didn't start / publish the expected
 *     protocol; AXL_ERR on invalid arguments.
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

/**
 * @brief Locate a shared-driver vtable with both config and a
 *     caller-set loaded-image identity.
 *
 * Superset of @ref axl_shared_driver_locate_with_load_options -- when
 * the driver is loaded from the embedded blob, @p info is threaded
 * down to the buffer load so the resulting image gets a non-NULL,
 * renderable device path (see @ref axl_driver_load_buffer_with_image_info)
 * — fixing the NULL `LoadedImage->FilePath` /
 * `gEfiLoadedImageDevicePathProtocol` that makes the aarch64 shell
 * fault under `dh -p` / `dh -v`.
 *
 * When @p info (or its @c file_name) is NULL the synthesized leaf name
 * defaults to @p driver_filename, so even the plain
 * @ref axl_shared_driver_locate already yields a non-NULL path. Use
 * this variant only when you also need to set @c DeviceHandle or a
 * Vendor() GUID, or to override the leaf name.
 *
 * Like the load-options variant, @p info / @p load_options apply only
 * on a fresh load; both are skipped on the resident-driver
 * short-circuit.
 *
 * @return AXL_OK on success; AXL_NOT_FOUND when a usable vtable can't be
 *     obtained -- not resolvable from any candidate path or the embedded
 *     blob, or a driver loaded but didn't start / publish the expected
 *     protocol; AXL_ERR on invalid arguments.
 */
int
axl_shared_driver_locate_with_image_info(
    const char                 *name,              ///< shared-driver identity
    const char                 *driver_filename,   ///< on-disk driver filename
    const unsigned char        *embed_blob,        ///< embedded driver bytes
    size_t                      embed_len,         ///< length of @p embed_blob
    const void                 *load_options,      ///< bytes to install (NULL → skip)
    size_t                      load_options_size, ///< @p load_options length
    const AxlEmbeddedImageInfo *info,              ///< loaded-image identity (NULL → defaults)
    void                      **out_iface          ///< [out] receives the vtable pointer
);

/**
 * @brief Locate a shared-driver vtable, SIBLING-ONLY (version-pinned).
 *
 * Warm resident short-circuit; else cold-load @p driver_filename from the
 * LAUNCHER's OWN directory only (@ref axl_driver_load_sibling) and start it —
 * no /drivers, no volume-root, no cross-volume search. Hard-fails
 * (AXL_NOT_FOUND) if the driver isn't staged beside the launcher. For
 * version-pinned launchers that must pair with the exact driver co-staged with
 * them. Thin by construction (no embedded-blob arg). Installs the stdio bridge
 * like the other locate* variants.
 *
 * NOTE: pinning governs only the COLD path. Once ANY driver of this identity is
 * resident, the warm short-circuit returns it regardless of version — the first
 * cold load pins for the boot. (Same model as do.efi today.)
 *
 * @return AXL_OK; AXL_NOT_FOUND if no usable vtable is obtained -- not staged
 *     beside the launcher, or it loaded but didn't start / publish the expected
 *     protocol (uniform with the multi-path family); AXL_INVALID on a non-bare
 *     @p driver_filename; AXL_ERR on invalid args or no filesystem anchor
 *     (network / RAM-disk boot).
 */
int
axl_shared_driver_locate_sibling(
    const char  *name,              ///< shared-driver identity
    const char  *driver_filename,   ///< bare on-disk driver filename (sibling of this image)
    void       **out_iface          ///< [out] receives the vtable pointer
);

/**
 * @brief Resolve a SIBLING-ONLY resident shared-driver and dispatch — the
 *     whole launcher, version-pinned.
 *
 * Composes @ref axl_shared_driver_locate_sibling (resident short-circuit, else
 * sibling-only cold-load) and @ref axl_shared_driver_dispatch. This IS a
 * turnkey `int main` body: @c AXL_SHARED_DRIVER_LAUNCHER_SIBLING expands to a
 * call to it. For version-pinned launchers that must hard-fail rather than
 * fall back to /drivers or a cross-volume search when the paired driver isn't
 * staged beside them.
 *
 * @return the driver's exit code when it dispatched; a launcher-side error
 *     (nonzero; also arms @c EFI_NOT_FOUND via axl_set_exit_status) when the
 *     driver could not be located (including the AXL_NOT_FOUND hard-fail from
 *     @ref axl_shared_driver_locate_sibling).
 */
int
axl_shared_driver_run_sibling(
    const char  *name,              ///< shared-driver identity
    const char  *driver_filename,   ///< bare on-disk driver filename (sibling of this image)
    int          argc,              ///< argc from main
    char       **argv               ///< argv from main
);

/**
 * @brief Bridge THIS launcher's shell StdIn into the resident driver
 *     so the driver's @c axl_stdin reflects the launcher's piped /
 *     redirected / interactive input.
 *
 * Call from the LAUNCHER image, before dispatching into the driver.
 * Only needed by consumers that resolve the resident driver
 * **themselves** — @ref axl_shared_driver_guid +
 * @ref axl_protocol_find_guid (warm fast-path), @ref axl_driver_load_sibling,
 * an embedded-blob fallback, etc. — instead of through
 * @ref axl_shared_driver_locate (every @c axl_shared_driver_locate*
 * variant already installs the bridge automatically, so launchers using
 * those never call this).
 *
 * The driver image has no shell parameters of its own (it's a resident
 * DXE driver, not a shell invocation), so without the bridge its
 * @c axl_stdin is EOF and @c echo @c args @c | @c tool reads nothing.
 * Output redirection (@c >) needs no bridge — it already works via the
 * shell's @c ConOut handoff during the launcher window.
 *
 * Re-publishes on each call (every launcher invocation carries its own
 * StdIn) and is auto-uninstalled at launcher exit via @c axl_atexit, so
 * the bridge never outlives the invocation. Read piped text through
 * @c axl_stdin_text() in the driver verb (the default @c | pipe carries
 * UCS-2).
 *
 * @return AXL_OK when the bridge is installed, or when the caller has no
 *     shell handles to bridge (nothing to do — e.g. not launched from a
 *     shell); AXL_ERR only if the bridge-protocol install itself failed.
 */
int
axl_shared_driver_install_stdio_bridge(void);

/**
 * @brief Apply a resident driver's armed exit status to THIS launcher.
 *
 * Call from the LAUNCHER, immediately after dispatching into the resident
 * driver (i.e. after the driver's vtable call returns). If a driver verb
 * armed an exact status via @c axl_set_exit_status(), that status was
 * reflected across the stdio bridge into this launcher's pending-status
 * cell; this drains it into the launcher's own @c axl_set_exit_status so
 * the launcher's CRT0 returns it verbatim to the shell (`%lasterror%`).
 *
 * A no-op when the driver armed nothing this dispatch (the launcher then
 * exits by its own @c main return code, per the normal convention). Only
 * needed by launchers that dispatch into a resident driver and did NOT go
 * through a future @c axl_shared_driver_dispatch wrapper.
 *
 * Does NOT clear a previously-armed launcher exit status — it only drains
 * the bridge's pending cell into @c axl_set_exit_status when one is
 * pending. A REPL-style launcher that dispatches repeatedly and wants
 * strict per-dispatch semantics (this round's status only, not a stale one
 * left over from an earlier round) should clear its own armed status
 * between dispatches, or rely on the AXL_ERR return here to know nothing
 * was applied this round.
 *
 * @return AXL_OK if a reflected status was applied; AXL_ERR if none was
 *     pending (nothing to apply — not an error condition, just a signal).
 */
int
axl_shared_driver_apply_exit_status(void);

/**
 * @brief Dispatch into a resident driver with stdio + exit-status bridged.
 *
 * Brackets the cross-image vtable call: installs the stdio bridge (so the
 * driver's @c axl_stdin / stderr reflect THIS launcher), calls
 * @c vt->run(argc, argv) — forwards @p argc / @p argv unchanged, so @c run()
 * sees them exactly as the launcher's @c main did (@c argv[0] the program
 * name, verb/args from @c argv[1]) — then applies any exit status the driver armed
 * (@ref axl_shared_driver_apply_exit_status) so the launcher exits with it.
 * For launchers that resolve the driver themselves; @ref axl_shared_driver_run
 * calls this after resolving.
 *
 * @return the driver's @c run return code (the launcher should return it
 *     from @c main); AXL_ERR if @p vt / @p vt->run is NULL.
 */
int
axl_shared_driver_dispatch(
    const AxlSharedDriverVtable *vt,    ///< resolved standard vtable
    int                          argc,  ///< forwarded argc
    char                       **argv   ///< forwarded argv
);

/**
 * @brief Resolve a resident shared-driver and dispatch — the whole launcher.
 *
 * Composes @ref axl_shared_driver_locate (resident → on-disk → embedded)
 * and @ref axl_shared_driver_dispatch. This IS a turnkey `int main` body:
 * @c AXL_SHARED_DRIVER_LAUNCHER expands to a call to it. Pass
 * @c embed_blob == NULL / @c embed_len == 0 for a thin (no-embed) launcher.
 *
 * @return the driver's exit code when it dispatched; a launcher-side error
 *     (nonzero; also arms @c EFI_NOT_FOUND via axl_set_exit_status) when the
 *     driver could not be located.
 */
int
axl_shared_driver_run(
    const char           *name,             ///< shared-driver identity
    const char           *driver_filename,  ///< on-disk filename
    const unsigned char  *embed_blob,       ///< embedded driver bytes (NULL → thin)
    size_t                embed_len,        ///< length of @p embed_blob (0 → thin)
    int                   argc,             ///< argc from main
    char                **argv              ///< argv from main
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SHARED_DRIVER_H */
