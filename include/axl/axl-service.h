/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-service.h
 *
 * Driver-shaped lifecycle wrapper over AxlLoop. An AxlService is
 * the typed shape for a long-running event loop: a setup callback
 * that builds sources/timers/handlers, a teardown that releases
 * them, and an options descriptor that auto-applies into the
 * consumer's option struct via offsetof.
 *
 * **AxlService is a DXE driver.** Long-running work in UEFI lives
 * in driver images — foreground apps die when their main returns.
 * AxlService doesn't try to hide that; it embraces it. There is
 * no in-process foreground "run" path. The consumer's main() is a
 * launcher / supervisor, not the body of the service.
 *
 * Two operational entry points:
 *
 *   axl_service_attach_driver(loop, &svc)
 *       Driver-tick mode. Used by the AXL_SERVICE_DRIVER
 *       macro to attach the service to the firmware notify-timer.
 *       Tick period comes from `svc->driver_tick_ms` (0 = 50 ms
 *       default). Direct use is rare — the macro covers the
 *       common case.
 *
 *   axl_service_start_embedded(deploy)
 *       Foreground-side: load an embedded driver image carrying
 *       this service via .incbin, serialize the foreground's
 *       options into LoadOptions, pass to the driver. The
 *       driver's DriverEntry (built with AXL_SERVICE_DRIVER)
 *       decodes them on entry. Pair with axl_service_stop.
 *
 * **Cross-binary ABI tripwire.** When the same service descriptor
 * is initialized in two binaries (foreground app + driver image),
 * the option struct's layout must match across both. Build both
 * from the same source tree with identical compile flags
 * (AXL_TLS, AXL_MEM_DEBUG, arch). The Makefile's AXL_TLS state
 * detection only catches AXL-internal struct shifts; it cannot
 * see consumer struct shifts caused by toggling the consumer's
 * own conditional fields.
 */

#ifndef AXL_SERVICE_H
#define AXL_SERVICE_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-config.h>
#include <axl/axl-efi-status.h>  /* AxlEfiStatus (DriverEntry return width) */
#include <axl/axl-loop.h>
#include <axl/axl-sys.h>      /* AxlGuid */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lifecycle callbacks
// ---------------------------------------------------------------------------

/**
 * @brief Setup callback — builds sources, timers, handlers against @p loop.
 *
 * Called once at service startup, BEFORE the loop starts dispatching.
 * Receives the same @p loop the service will run on, so the consumer
 * can add accept/recv/timer sources directly.
 *
 * **Held-protocol hazard.** Any UEFI protocol the setup callback opens
 * with `OpenProtocol` (including the implicit opens that
 * `axl_http_server_start`, `axl_tcp_listen`, etc. do via
 * service-binding) MUST be closed before the matching teardown
 * returns, or the firmware's post-callback refcount check will refuse
 * `gBS->UnloadImage` with `EFI_ACCESS_DENIED`. This makes the service
 * un-stoppable for the lifetime of the process; the consumer's
 * `axl_service_stop` will fail with the SDK warning pointing at this
 * exact case. The SDK's wrappers (`axl_http_server_free`,
 * `axl_tcp_close`, etc.) close the protocols they opened — if your
 * setup uses `OpenProtocol` directly, your teardown owns the matching
 * `CloseProtocol`.
 *
 * **Failure contract:** on AXL_ERR return, the consumer is responsible
 * for unwinding any partial state (axl_free, axl_protocol_unregister,
 * etc.). The framework will NOT call teardown — teardown only fires
 * after a successful setup.
 *
 * @param loop  the event loop the service runs on
 * @param user  the descriptor's user pointer (typically the consumer's
 *              options struct, populated by AxlConfig auto-apply)
 * @return AXL_OK if the service is ready to dispatch; AXL_ERR on any
 *     setup failure (consumer has already cleaned up).
 */
typedef int (*AxlServiceSetup)(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT;

/**
 * @brief Teardown callback — releases what setup built.
 *
 * Called at service shutdown ONLY if setup returned AXL_OK. Receives
 * the same user pointer setup got. Return value is logged but doesn't
 * affect the framework's teardown sequence.
 *
 * **Must close every protocol setup opened.** See the
 * "Held-protocol hazard" note on AxlServiceSetup. The framework
 * cannot detect a missed `CloseProtocol`; the symptom is
 * `axl_service_stop` returning AXL_ERR with the SDK's
 * EFI_ACCESS_DENIED warning at axl-driver.c.
 */
typedef int (*AxlServiceTeardown)(void *user) AXL_CB_NOEXCEPT;

// ---------------------------------------------------------------------------
// Service descriptor
// ---------------------------------------------------------------------------

/**
 * @brief Service descriptor. Static-initializable; safe to share by
 *     reference between foreground app and driver image (same source
 *     tree, identical compile flags).
 *
 * The service's UEFI protocol identity is derived from @c name via
 * axl_guid_v5 with a fixed AXL_SERVICE namespace — consumers
 * don't allocate a UUID per service. The AXL_SERVICE_DRIVER
 * macro auto-publishes the derived GUID on the driver image's handle
 * when it starts, which gives @c axl_driver_ensure_with_embedded
 * (called from axl_service_start_embedded) something to verify
 * against AND gives axl_service_is_running a way to detect a
 * live deploy. Two binaries that share the same name string see the
 * same derived GUID by construction.
 *
 * Keep all fields trivially copyable — the descriptor is read in both
 * binaries' startup paths and any pointer ABI shift is hard to
 * diagnose. AxlConfig descriptors are themselves static const.
 */
typedef struct {
    const char           *name;          ///< short identifier, e.g. "axl-webfs".
                                         ///< REQUIRED — not just a log label.
                                         ///< AxlService derives the service's
                                         ///< UEFI protocol identity from this
                                         ///< via axl_guid_v5 with a fixed
                                         ///< AXL_SERVICE namespace, so the
                                         ///< name uniquely picks out a service.
                                         ///< Two binaries that share the same
                                         ///< source tree's name string see the
                                         ///< same derived GUID by construction.
    const AxlConfigDesc  *opts_descs;    ///< NULL = no configurable options
    AxlServiceSetup       setup;         ///< required (must not be NULL)
    AxlServiceTeardown    teardown;      ///< NULL = nothing to release
    void                 *user;          ///< -> caller's options struct;
                                         ///< AxlConfig auto-applies into here
    uint64_t              driver_tick_ms;///< driver-mode dispatch period in
                                         ///< milliseconds; 0 means use the
                                         ///< 50 ms default. Single source
                                         ///< of truth for both
                                         ///< axl_service_attach_driver and
                                         ///< the AXL_SERVICE_DRIVER macro.
} AxlService;

/**
 * @brief Resolve a service's UEFI protocol identity GUID.
 *
 * AxlService publishes its identity by deriving a deterministic GUID
 * from @c svc->name via axl_guid_v5 with the AXL_SERVICE
 * namespace. Consumers normally don't need this — axl_service_is_running, axl_service_stop, and the
 * AXL_SERVICE_DRIVER macro do the derivation internally — but if
 * you want to call @c LocateProtocol from outside AxlService (e.g. to
 * publish a service-specific child interface on the same handle), use
 * this to get the same GUID those code paths use.
 *
 * @return AXL_OK on success (@p out populated); AXL_ERR if @p svc is
 *     NULL, @p svc->name is NULL, or @p out is NULL.
 */
int
axl_service_guid(
    const AxlService *svc,
    AxlGuid          *out
);

/**
 * @brief Default driver-tick dispatch period (milliseconds).
 *
 * Used when AxlService.driver_tick_ms is 0. 50 ms balances loop
 * responsiveness against the firmware notify-timer's overhead.
 */
#define AXL_SERVICE_DEFAULT_TICK_MS 50

// ---------------------------------------------------------------------------
// Driver-mode deployment
// ---------------------------------------------------------------------------

/**
 * @brief Attach a service to firmware-tick dispatch on the supplied loop.
 *
 * Calls setup(loop, svc->user) then axl_loop_attach_driver. The tick
 * period comes from @c svc->driver_tick_ms; if that field is 0,
 * AXL_SERVICE_DEFAULT_TICK_MS (50 ms) applies. Returns
 * immediately — the firmware notify-timer drives the loop from
 * here. Pair with axl_service_detach_driver from DriverUnload.
 *
 * The caller (typically the AXL_SERVICE_DRIVER macro) owns the
 * loop; this function does NOT free it on failure — the caller must
 * axl_loop_free if attach fails.
 *
 * @return AXL_OK if setup + attach both succeeded; AXL_ERR otherwise.
 */
int
axl_service_attach_driver(
    AxlLoop          *loop,
    const AxlService *svc
);

/**
 * @brief Detach a service's firmware-tick dispatch.
 *
 * Cancels the periodic notify-timer the matching
 * axl_service_attach_driver installed. Returns AXL_ERR if the
 * loop was never attached.
 *
 * **Does NOT call svc->teardown.** That's the caller's responsibility
 * — invoke axl_service_teardown after this returns AXL_OK if
 * setup ran. (The contract changed under P1: previously detach_driver
 * also called teardown internally, but the call was hidden inside
 * detach_driver's success path which made the macro unread-able and
 * silently skipped teardown if detach_driver gained any new failure
 * mode. Splitting makes each function's responsibility narrow.)
 *
 * The AXL_SERVICE_DRIVER macro's unload stub does this in
 * sequence; consumers using @c axl_service_attach_driver directly
 * must mirror it.
 *
 * **Do NOT call this if axl_service_attach_driver returned
 * AXL_ERR.** The attach-failure path already invoked teardown
 * internally; calling detach + teardown manually would re-fire
 * teardown. The safe pattern is "only detach what attach succeeded
 * for."
 */
int
axl_service_detach_driver(
    AxlLoop          *loop,    ///< loop the dispatch was attached to
    const AxlService *svc      ///< accepted but currently only NULL-checked;
                               ///< reserved for future per-service detach work
                               ///< and kept in the signature for source-compat
);

/**
 * @brief Invoke a service's teardown callback with consistent logging.
 *
 * Single source of truth for teardown invocation across the SDK:
 *   - @c axl_service_attach_driver's failure path
 *   - the AXL_SERVICE_DRIVER macro's unload stub after detach
 *
 * Logs `teardown ENTER` / `teardown EXIT rc=N` at debug level,
 * promotes a non-OK rc to a warning. Safe to call with @p svc == NULL
 * or @p svc->teardown == NULL — both return AXL_OK without invoking
 * anything.
 *
 * Consumers normally don't call this directly — the three public
 * sites above invoke it. Public so consumers rolling their own
 * dispatch (custom firmware-tick driver, alternative deploy mode)
 * can match the framework's logging shape.
 *
 * Idempotency is the consumer's responsibility. Calling teardown
 * twice on a setup that wasn't designed for it is undefined
 * behavior (axl_free of an already-freed handle, etc.).
 *
 * @return The teardown callback's return code (AXL_OK if no
 *     callback or @p svc is NULL). The AXL_SERVICE_DRIVER
 *     macro propagates this into the EFI_STATUS gBS->UnloadImage
 *     sees, so a teardown failure surfaces to the firmware rather
 *     than getting absorbed into a "successful" unload.
 */
int
axl_service_teardown(
    const AxlService *svc
);

// ---------------------------------------------------------------------------
// Internal — called from the AXL_SERVICE_DRIVER macro shim. Not user API.
// ---------------------------------------------------------------------------

/**
 * @brief Internal driver-image entry. Implements everything the
 *     AXL_SERVICE_DRIVER macro used to inline: backend init,
 *     LoadOptions decode, protocol publish, loop creation, attach.
 *     Library-side per-image static state holds @p svc, the loop,
 *     the cfg, and the published handle for the matching unload.
 *
 * Not user API — call AXL_SERVICE_DRIVER instead.
 *
 * **Returns the firmware's own status width.** `AxlEfiStatus` is
 * 64-bit; every failure code carries the error bit
 * (`0x8000000000000000`). Narrowing this to `int` — as the
 * declaration did before v2.9.1 — silently strips that bit, so
 * `AXL_EFI_ABORTED` reaches the firmware as `0x15`, `StartImage`
 * reports success, and a service whose setup failed is treated as
 * running by @ref axl_service_reload and by
 * `axl_driver_start`. Keep this type as wide as the firmware's.
 *
 * @return AXL_EFI_SUCCESS when the service is attached and
 *     dispatching; AXL_EFI_INVALID_PARAMETER for an incomplete
 *     descriptor; AXL_EFI_OUT_OF_RESOURCES when the loop could not
 *     be created; AXL_EFI_ABORTED when the identity protocol could
 *     not be published or setup/attach failed. Suitable to return
 *     directly from a firmware DriverEntry.
 */
AxlEfiStatus
_axl_service_driver_init(
    void             *image_handle,   ///< EFI_HANDLE for this driver image
    void             *system_table,   ///< EFI_SYSTEM_TABLE *
    const AxlService *svc             ///< service descriptor (non-NULL)
);

/* Note: the matching unload stub is registered by
 * _axl_service_driver_init via axl_driver_set_unload and lives
 * inside the SDK with the right EFIAPI calling convention — no
 * separate decl is needed (or wanted) in the consumer-visible header. */

// ---------------------------------------------------------------------------
// Embedded-driver deployment ("ship as one binary")
// ---------------------------------------------------------------------------

/**
 * @brief Cross-binary deployment glue. Combines a service descriptor
 *     (the part both binaries share) with the embedded driver image
 *     and disk-search filename (foreground-only fields).
 *
 * The driver image links the same `service` descriptor; the foreground
 * app additionally fills in the blob fields. Protocol identity is
 * derived from @c service->name via axl_service_guid in both
 * binaries — they see the same GUID because they share the descriptor
 * (and therefore the name string).
 */
typedef struct {
    const AxlService    *service;          ///< shared descriptor
    const unsigned char *driver_blob;      ///< embedded driver .efi bytes
                                           ///< (optional when @c driver_path
                                           ///< pins the image)
    size_t               driver_blob_len;  ///< length in bytes
    const char          *driver_name;      ///< filename the disk search looks
                                           ///< for (optional when
                                           ///< @c driver_path pins the image)
    const char          *driver_path;      ///< OPTIONAL: load exactly this file
                                           ///< (e.g. "fs0:\\svc\\my-dxe.efi").
                                           ///< NULL = the default
                                           ///< search-then-embedded resolution.
                                           ///< When set, neither the search nor
                                           ///< the embedded blob is consulted,
                                           ///< so a stale copy of
                                           ///< @c driver_name elsewhere on the
                                           ///< box cannot shadow the image the
                                           ///< launcher staged. Because neither
                                           ///< is consulted, @c driver_name and
                                           ///< @c driver_blob are both unread
                                           ///< on this path and neither is
                                           ///< required.
    bool                 embedded_only;    ///< OPTIONAL: load the embedded
                                           ///< @c driver_blob DIRECTLY, skipping
                                           ///< the disk search entirely. Use this
                                           ///< for the "ship as one binary, never
                                           ///< touch disk" model: the default
                                           ///< resolution searches
                                           ///< `<image_dir>/<driver_name>` FIRST,
                                           ///< so a stale loose driver left beside
                                           ///< the launcher by an older install
                                           ///< silently shadows the newer embedded
                                           ///< one — this flag closes that hole
                                           ///< from the embedded side, as
                                           ///< @c driver_path does from the disk
                                           ///< side. Requires @c driver_blob /
                                           ///< @c driver_blob_len; @c driver_name
                                           ///< is used only to name the loaded
                                           ///< image (optional). Mutually
                                           ///< exclusive with @c driver_path
                                           ///< (setting both is an error).
} AxlServiceDeploy;

/**
 * @brief Launch the embedded driver image carrying this service.
 *
 * Serializes the foreground app's currently-set option values via
 * axl_config_to_string, then calls
 * axl_driver_ensure_with_embedded(...) with that payload as
 * LoadOptions. The driver image's DriverEntry (built with
 * AXL_SERVICE_DRIVER) decodes the payload back into its own
 * AxlConfig and the framework auto-applies it into svc->user.
 *
 * Callers wanting `argv -> svc->user` overlay before launch must
 * populate `*deploy->service->user` themselves. Two patterns:
 *
 *   - Standard verbs only: axl_service_main packages
 *     argv-parse + populate + start in one call (and the
 *     AXL_SERVICE convenience macro emits a one-line `main()`
 *     that calls it). This is what most consumers want.
 *   - Custom verb tree: copy AxlArgs values into the user struct
 *     yourself via `axl_args_get_uint` / `_bool` / `_string` from
 *     your verb handler, then call this. STRING pointers returned
 *     by `axl_args_get_string` live at least until your verb
 *     handler returns — call this BEFORE return so the serialize
 *     pass reads them while still valid. See
 *     `sdk/examples/service-demo-custom.c` for the worked example.
 *
 * The AxlConfig path (`axl_config_new(descs, NULL, user)` +
 * `axl_config_from_string` per argv element) also works and adds
 * default-application + STRING strdup ownership; reach for it when
 * the consumer is already AxlConfig-shaped.
 *
 * If the deploy's protocol GUID is already published when this is
 * called (e.g. a previous launch is still active), AXL's
 * ensure_with_embedded short-circuits and this returns AXL_OK
 * without re-loading. Use axl_service_is_running to detect
 * that case explicitly if your CLI wants to report "already serving".
 *
 * **Pinning the image** (`deploy->driver_path`): with that field set,
 * this routes through axl_driver_ensure_from_path instead — exactly
 * one file, no 4-path search and no embedded fallback, so a stale
 * `drivers/<arch>/<driver_name>` left by an older install cannot
 * shadow the copy the launcher just staged. `driver_name`,
 * `driver_blob` and `driver_blob_len` all feed the default resolution
 * only, so none of them is read — or required — when a path is
 * pinned; `service` and `driver_path` are the whole descriptor.
 *
 * **Embedded-only** (`deploy->embedded_only`): routes through
 * axl_driver_ensure_embedded_only — the embedded `driver_blob` is loaded
 * DIRECTLY, with no disk search at all. This closes the same stale-shadow
 * hole as `driver_path` but from the other side: the default search looks
 * at `<image_dir>/<driver_name>` first, so a loose driver an older install
 * left beside the launcher would otherwise run in place of the newer blob
 * baked into this binary. Requires `driver_blob`/`driver_blob_len`;
 * `driver_name` only names the loaded image. `embedded_only` and
 * `driver_path` are mutually exclusive — setting both returns AXL_ERR.
 *
 * @return AXL_OK if the protocol is registered (was already, or after
 *     loading); AXL_ERR on serialize overflow or an incomplete deploy
 *     descriptor; AXL_NOT_FOUND when no candidate — the pinned path, or
 *     the search plus the embedded blob — produced a registered
 *     protocol.
 */
AxlStatus
axl_service_start_embedded(
    const AxlServiceDeploy *deploy
);

/**
 * @brief Stop a running service launched via axl_service_start_embedded.
 *
 * Resolves the running driver image's handle by looking up the
 * GUID derived from `deploy->service->name` via `LocateHandleBuffer`, then
 * calls `axl_driver_unload` on each match. The driver image's
 * AXL_SERVICE_DRIVER unload stub fires synchronously inside
 * UnloadImage and runs the framework's teardown sequence in this
 * order:
 *   1. axl_loop_detach_driver — stops the firmware notify-timer
 *   2. service teardown(user) — releases what setup built
 *   3. axl_protocol_uninstall — removes the marker protocol
 *   4. axl_config_free — drops cached LoadOptions strings
 *   5. axl_loop_free
 *
 * Idempotent: if the protocol isn't currently published (the
 * service was never launched, or was already stopped), returns
 * AXL_OK without doing anything. Use axl_service_is_running
 * first if you want to differentiate "stopped it" from "wasn't
 * running."
 *
 * **Dangling-interface hazard.** UEFI doesn't ref-count protocols.
 * If a third-party consumer obtained `service->user` (or any other
 * interface) via `LocateProtocol` and held the pointer past the
 * stop call, that pointer dangles. The same hazard applies to
 * the shell's `unload -n` and is fundamental to UEFI's lifetime
 * model. Callers that publish service interfaces meant for live
 * cross-image consumption should advertise their stop semantics
 * to those consumers (e.g. a marker GUID change or an explicit
 * "I am about to unload" event) before calling stop.
 *
 * @return AXL_OK on success (or already-stopped); AXL_ERR if
 *     @p deploy or @p deploy->service is NULL, or any
 *     axl_driver_unload call returned an error. If multiple
 *     handles publish the GUID, all are unloaded; rc reflects
 *     whether any of them failed (the loop continues on individual
 *     failures rather than aborting on the first — partial
 *     cleanup beats leaking the rest).
 */
int
axl_service_stop(
    const AxlServiceDeploy *deploy
);

/**
 * @brief In-place self-upgrade: replace this running service with a new
 *     image, handing off its ports, and unload this one.
 *
 * Call this from **inside** a running `AXL_SERVICE_DRIVER` service (e.g. from
 * an "/upgrade" request handler, on the service loop) to hot-swap the service
 * to a new version with no reboot and — with an `AXL_TEARDOWN_RESET`
 * @ref axl_http_server_free in your teardown — no port downtime:
 *
 *   1. Loads @p new_path as the replacement (validating it *before* anything is
 *      released — a load failure tears down nothing).
 *   2. Runs your @ref AxlServiceTeardown (release the listen ports; use the
 *      abortive HTTP-server free so they are free on return).
 *   3. Starts the replacement service driver, passing it this service's current
 *      options AND a handoff (this image's handle + a signal event) via
 *      LoadOptions. The replacement's `AXL_SERVICE_DRIVER` entry rebinds the
 *      ports, comes up resident, and arms the handoff event on its own loop.
 *   4. Detaches this service's driver loop and signals the handoff. The
 *      replacement, from its own timer tick (this image now off-stack), unloads
 *      this image and reclaims it.
 *
 * The old image is fully reclaimed; the new one keeps serving. This is the
 * whole self-reload — no shell, no supervisor script, no reboot, safe on a
 * read-only volume (see @ref axl_service_reload_buffer for a downloaded image).
 *
 * Requirements: the caller must be an `AXL_SERVICE_DRIVER`-hosted service (not
 * the foreground launcher), and @p svc must be the running service. Your
 * teardown must be **idempotent** and must NOT free the service loop (the
 * framework owns it).
 *
 * Failure semantics: if the replacement cannot be **loaded** (bad path /
 * corrupt image — the common upgrade failure), nothing is torn down and this
 * service keeps running. Teardown happens only *after* a successful load (the
 * ports must be released before the replacement binds them), so a replacement
 * that loads but fails to **start** leaves this service down: its teardown has
 * run and its loop is detached (it no longer serves), but this image stays
 * resident and still publishes the service GUID (so @ref axl_service_is_running
 * still reports true) until it is unloaded or the system resets.
 *
 * **`AXL_ERR` means — and only means — this service is now DOWN.** That is
 * the one code a caller must treat as fatal (roll back and reset, or fall
 * back to a watchdog). Every other non-OK code reports a failure that
 * happened *before* anything was released, so the service is still serving
 * and the caller can simply disarm the upgrade and carry on with zero
 * downtime:
 *
 * | return | meaning | this service |
 * |---|---|---|
 * | `AXL_OK` | replacement resident, handoff armed | replaced, serving |
 * | `AXL_INVALID` | NULL / not-the-running @p svc, NULL path or image | untouched, serving |
 * | `AXL_NOT_FOUND` | the replacement could not be **loaded** | untouched, serving |
 * | `AXL_NO_RESOURCES` | options would not serialize, handoff event / LoadOptions could not be installed | untouched, serving |
 * | `AXL_ERR` | the replacement loaded but failed to **start** | **DOWN — fatal** |
 *
 * Source-compatible with the previous `int` signature and with any caller
 * that only tests `!= AXL_OK`; what changed is that the pre-teardown
 * failures no longer report `AXL_ERR` (they were indistinguishable from
 * the fatal case before).
 *
 * @return AXL_OK once the replacement is resident and this image's unload is
 *     armed (this call returns; the image is reclaimed shortly after by the
 *     replacement); otherwise one of the codes in the table above.
 */
AxlStatus
axl_service_reload(
    const AxlService *svc,       ///< the running service (must be AXL_SERVICE_DRIVER-hosted)
    const char       *new_path   ///< path to the replacement driver .efi (e.g. "fs0:\\svc-v2.efi")
);

/**
 * @brief Like @ref axl_service_reload, but the replacement image comes from a
 *     memory buffer instead of a file path.
 *
 * The read-only-volume / network-upgrade form: the new version is a driver
 * image already in memory (downloaded over the network, decompressed, etc.),
 * so nothing is read from — or written to — a filesystem. Otherwise identical
 * to @ref axl_service_reload.
 *
 * @return the same as @ref axl_service_reload; AXL_INVALID additionally on a
 *     NULL / zero-length @p image.
 */
AxlStatus
axl_service_reload_buffer(
    const AxlService *svc,       ///< the running service
    const void       *image,     ///< replacement driver image bytes (PE/COFF .efi)
    size_t            image_len  ///< image size in bytes
);

/**
 * @brief Predicate: is this deploy's service currently running?
 *
 * Internally derives the service GUID from `deploy->service->name`
 * and calls `LocateProtocol` —
 * succeeds if any image (firmware-shipped or a previous
 * `axl_service_start_embedded` of this deploy) has published the
 * protocol. Useful before axl_service_start_embedded to
 * differentiate "started fresh" from "was already running."
 *
 * @return true if a previous launch is still publishing the protocol.
 */
bool
axl_service_is_running(
    const AxlServiceDeploy *deploy
);

/**
 * @brief Block on the default loop, then stop the service.
 *
 * The standard supervise-and-stop body: blocks on @c axl_loop_default
 * until @c axl_loop_run returns (Ctrl-C, @c axl_loop_quit, or any
 * source removing itself with no others left), then calls
 * axl_service_stop. Returns process-exit-shaped @c int (0 on
 * clean shutdown, 1 on stop failure or unexpected loop error).
 *
 * Intended for consumers that want the standard supervise body but
 * still need to register their own loop sources (ESC key handler,
 * a status-print timer, etc.) before blocking. Add those to
 * @c axl_loop_default first; call this once you're ready to block:
 *
 *     int rc = axl_service_start_embedded(&deploy);
 *     if (rc != AXL_OK) return 1;
 *     if (detach) return 0;
 *     axl_loop_add_key_press(axl_loop_default(), esc_handler, NULL);
 *     return axl_service_supervise(&deploy);
 *
 * axl_service_main uses this internally; consumers writing their
 * own @c main() can compose this directly instead of duplicating the
 * loop-run / stop / rc-translate dance.
 *
 * @return 0 if @c axl_loop_run quit cleanly (rc 0 or -1 from a Ctrl-C
 *     break) AND @c axl_service_stop succeeded; 1 otherwise.
 */
int
axl_service_supervise(
    const AxlServiceDeploy *deploy
);

// ---------------------------------------------------------------------------
// One-line main(): standard launcher / supervisor for a service driver
// ---------------------------------------------------------------------------

/**
 * @brief Standard main() body for a service consumer.
 *
 * Builds a default `axl_args_run` verb tree (`launch [--detach]`,
 * `stop`, `status`) from @p deploy and dispatches argv. The
 * `launch` verb populates `deploy->service->user` from the parsed
 * args (synthesizing an `AxlArgDesc[]` from `svc->opts_descs` so
 * the consumer doesn't repeat the descriptor in two formats),
 * calls axl_service_start_embedded, and either:
 *
 *   - exits if `--detach` was passed (driver continues to run);
 *   - blocks on @c axl_loop_default until Ctrl-C, then calls
 *     axl_service_stop on its way out.
 *
 * Most service consumers don't need their own `main()` — the
 * AXL_SERVICE macro emits one for you that calls this. Direct
 * use of axl_service_main is for consumers who want to mix
 * the default verbs with their own (extra verbs, custom help
 * prolog, etc.) — they wire `axl_args_run` themselves and dispatch
 * the standard verbs into axl_service_main.
 *
 * @return process-exit-shaped int: 0 on success, 1 on failure
 *     (matching @c axl_args_run conventions).
 */
int
axl_service_main(
    const AxlServiceDeploy *deploy,  ///< deploy descriptor (service + driver blob)
    int                     argc,    ///< argc from main()
    char                  **argv     ///< argv from main()
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SERVICE_H */
