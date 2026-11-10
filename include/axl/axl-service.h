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
 * (AXL_MEM_DEBUG, arch, anything reaching CFLAGS). The build's
 * flag tracking only catches AXL-internal shifts; it cannot
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
 * **Failure contract:** on a non-OK return, the consumer is responsible
 * for unwinding any partial state (axl_free, axl_protocol_unregister,
 * etc.). The framework will NOT call teardown — teardown only fires
 * after a setup that returned AXL_OK or #AXL_SERVICE_SETUP_HELD.
 *
 * **A setup that cannot unwind** -- it took something that calls into
 * this image (a console take-over, say), then failed, and the firmware
 * will not let go of it -- returns #AXL_SERVICE_SETUP_HELD instead. The
 * driver then stays resident in #AXL_SERVICE_STOPPING, exactly as after
 * a failed teardown: published, its loop never ticking, and the next
 * unload calls teardown to finish the unwinding (see
 * @ref AxlServiceTeardown). Any other non-OK return unloads the image.
 *
 * @param loop  the event loop the service runs on
 * @param user  the descriptor's user pointer (typically the consumer's
 *              options struct, populated by AxlConfig auto-apply)
 * @return AXL_OK if the service is ready to dispatch;
 *     #AXL_SERVICE_SETUP_HELD if setup failed but still holds what it
 *     could not release (only for a service with a teardown); any other
 *     value -- AXL_ERR, any AxlStatus, any other positive number such as
 *     a stray `return true;` -- on a setup failure the consumer has fully
 *     unwound.
 */
typedef int (*AxlServiceSetup)(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT;

/**
 * @brief An #AxlServiceSetup return: setup failed, but still holds
 *     resources it could not release -- keep the image resident and let
 *     the next unload's teardown finish.
 *
 * The value is 'HELD' in ASCII: positive, so it can never be an
 * AxlStatus (those are 0 or negative), and a value no setup returns by
 * accident -- a `return true;` or a source id passed up is 1 or small,
 * and is an ordinary failure.
 *
 * **It needs a teardown.** The teardown is what finishes the unwinding;
 * a service whose descriptor has none cannot be finished, so HELD from it
 * is refused with a warning and treated as an ordinary failure (the image
 * unloads).
 *
 * Only the AXL_SERVICE_DRIVER framework acts on it; to a hand-rolled
 * caller of axl_service_attach_driver it is a setup failure (AXL_ERR).
 */
#define AXL_SERVICE_SETUP_HELD  0x48454C44

/**
 * @brief Teardown callback — releases what setup built.
 *
 * Called at service shutdown ONLY if setup returned AXL_OK or
 * #AXL_SERVICE_SETUP_HELD. Receives the same user pointer setup got.
 *
 * **A non-OK return keeps the image resident, and teardown is retried.**
 * Teardown runs at two points, and a failure leaves the same state at
 * both, #AXL_SERVICE_STOPPING: the image stays resident and published
 * (@ref axl_service_stop finds it, @ref axl_service_state reports it
 * stopping), its loop stays detached -- not ticked again, since this
 * teardown may already have freed what the loop's sources point at --
 * and the next unload calls teardown AGAIN. A reset also clears it.
 *   - At unload, the AXL_SERVICE_DRIVER stub returns `EFI_ABORTED` and
 *     changes nothing else.
 *   - When setup succeeded but the loop's tick could not be attached,
 *     the framework rolls setup back with this teardown. If that fails,
 *     the driver's entry point still returns success: an error there
 *     would have the firmware free the image without calling its Unload,
 *     under whatever teardown failed to release.
 *
 * Two consequences. A teardown that can fail must RESUME: release only
 * what it still holds, clearing each pointer as it frees it, and return
 * non-OK only while something it must release is still held (a console
 * device whose `axl_console_device_uninstall` returned AXL_ERR, for one).
 * And non-OK is not a way to report a soft error: a teardown that has
 * released everything returns AXL_OK, or the next unload runs it again
 * over freed state, and one that always fails can never be unloaded.
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
 * When setup succeeds but the tick cannot be attached, setup is rolled
 * back with axl_service_teardown.
 *
 * **What AXL_ERR cannot tell a hand-rolled caller.** Two failures leave
 * setup's resources HELD: a rollback teardown that fails, and a setup
 * that returns #AXL_SERVICE_SETUP_HELD. Both still return AXL_ERR here,
 * so this function's answer stays OK or ERR, and a caller that frees the
 * loop and fails its DriverEntry on AXL_ERR has the firmware free an
 * image those resources still call into. The AXL_SERVICE_DRIVER macro
 * tells the cases apart and keeps the image resident in
 * #AXL_SERVICE_STOPPING; a driver whose setup or teardown can fail that
 * way should use it.
 *
 * @return AXL_OK if setup + attach both succeeded; AXL_ERR otherwise --
 *     for every setup failure, whatever setup returned (its own code is
 *     logged), and for a failed attach.
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
 * AXL_ERR or AXL_BUSY.** The attach-failure path already invoked
 * teardown internally and nothing was attached. After AXL_ERR do not
 * run teardown again either; after AXL_BUSY run only the teardown
 * retry. The safe pattern is "only detach what attach succeeded
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
 * Logs `teardown ENTER` / `teardown EXIT rc=N` at debug level only: a
 * caller that acts on a failure reports it once, saying what it leaves
 * behind (the framework's two sites each log one warning). Safe to call with @p svc == NULL
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
 * behavior (axl_free of an already-freed handle, etc.) -- and the
 * unload stub DOES call it again after it returns non-OK (see
 * @ref AxlServiceTeardown).
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
// Staged upgrade -- types (functions after axl_service_reload below)
// ---------------------------------------------------------------------------

/// Options for @ref axl_service_stage. NULL means none.
typedef struct {
    /// Optional acceptance check, run before anything is written: AXL_OK
    /// accepts, anything else refuses (returned as AXL_DENIED). For policy
    /// Secure Boot cannot express -- a pinned hash or signer, and downgrade
    /// refusal: the library knows no image version and enforces no
    /// anti-rollback. Secure Boot, where the platform enforces it on this
    /// medium, applies again at the trial's LoadImage.
    int (*verify)(const void *image, size_t image_len, void *ud) AXL_CB_NOEXCEPT;
    void *verify_ud;            ///< passed to @c verify
} AxlServiceStageOpts;

/// Options for @ref axl_service_start_staged, carried by
/// AxlServiceDeploy::staged_opts so the `start` verb of
/// @ref axl_service_main applies a staged image with them. NULL, or a
/// zero field, means the default.
typedef struct {
    uint32_t tries;             ///< boots that may die trying one candidate before it
                                ///< is given up (0 = 2): one ordinary reset during the
                                ///< service's setup does not roll back a good image.
                                ///< Keep it at 2 or more: with 1, a boot that ends
                                ///< with the trial record still set -- a reset, or a
                                ///< `promote:bak` record NV refused while the good
                                ///< candidate served -- gives that candidate up and
                                ///< quarantines it
    uint32_t watchdog_sec;      ///< UEFI watchdog armed from a trial's start until it
                                ///< publishes (0 = 300)
    uint32_t commit_grace_sec;  ///< seconds the candidate must keep running after it
                                ///< publishes before it is promoted (0 = 10;
                                ///< #AXL_SERVICE_COMMIT_AT_PUBLISH for none)
} AxlServiceStartOpts;

/// AxlServiceStartOpts::commit_grace_sec for "promote as soon as the
/// candidate publishes" (0 there means the 10 s default).
#define AXL_SERVICE_COMMIT_AT_PUBLISH  UINT32_MAX

/// What @ref axl_service_start_staged did, and what
/// @ref axl_service_last_upgrade reports.
typedef enum {
    AXL_SERVICE_STAGED_NONE = 0,     ///< no outcome this start: nothing staged, the
                                     ///< service already running (a staged image then
                                     ///< waits), or the NV store refused a record -- P
                                     ///< runs, or, when a promotion record was refused, the
                                     ///< trialled candidate keeps serving UNPROMOTED and
                                     ///< the next start resumes it (see "When NV refuses a
                                     ///< write"), or `P.new` was replaced or removed during
                                     ///< the trial -- the trialled candidate keeps serving
                                     ///< UNPROMOTED and whatever `P.new` holds now is
                                     ///< trialled at the next start. NONE never overwrites
                                     ///< the last outcome
    AXL_SERVICE_STAGED_APPLIED,      ///< the candidate came up on its first trial, stayed up
                                     ///< through the grace period, and was promoted to P;
                                     ///< the previous P is P.bak
    AXL_SERVICE_STAGED_RETRIED,      ///< an earlier boot died during this candidate's
                                     ///< trial; tried again, it was promoted (as APPLIED)
    AXL_SERVICE_STAGED_ROLLED_BACK,  ///< the candidate did not become P: it failed (now, or
                                     ///< `tries` boots died trying it) -- it is then
                                     ///< P.failed and quarantined -- or a promotion's rename
                                     ///< lost it (not quarantined; staging it again is
                                     ///< correct). P, or P restored from P.bak, is running
} AxlServiceStagedResult;

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
    const AxlServiceStartOpts *staged_opts; ///< OPTIONAL: options for the staged
                                           ///< upgrade @ref axl_service_start_staged
                                           ///< applies (the `start` verb's too).
                                           ///< NULL = the defaults. Read only
                                           ///< when @c driver_path is set.
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
 * **TPL:** an already-published protocol is AXL_OK at any TPL. A load is
 * made only below `TPL_CALLBACK`; at `TPL_CALLBACK` or above it returns
 * AXL_UNSUPPORTED and logs why (UEFI 2.11 Table 7.3).
 *
 * **A half-stopped service is not a running one.** When the service is
 * #AXL_SERVICE_STOPPING -- already, or because the image this call just
 * started came up that way (its setup returned #AXL_SERVICE_SETUP_HELD,
 * or its tick could not be attached and the rollback teardown failed) --
 * this returns AXL_BUSY and leaves the image resident: nothing is
 * serving, and a start can succeed only after a stop finishes its
 * teardown (or a reset).
 *
 * @return AXL_OK if the protocol is registered (was already, or after
 *     loading) and the service is not stopping; AXL_BUSY when it is
 *     #AXL_SERVICE_STOPPING; AXL_ERR on serialize overflow or an
 *     incomplete deploy descriptor; AXL_NOT_FOUND when no candidate —
 *     the pinned path, or the search plus the embedded blob — produced a
 *     registered protocol.
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
 * If teardown (step 2) fails, the unload is refused and steps 3-5 do
 * not run: the service stays resident and published with its loop
 * detached, in #AXL_SERVICE_STOPPING, this returns AXL_ERR, and the
 * next stop retries the teardown (see @ref AxlServiceTeardown). There is
 * no way to force the unload -- freeing an image something still calls
 * into is a use-after-free -- so the last resort is a reset (the `stop
 * --reset` verb of @ref axl_service_main, which resets only for a
 * service that is #AXL_SERVICE_STOPPING).
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
 * @brief In-place upgrade, asked for by an application: replace the running
 *     service with a new image.
 *
 * Call this from an **application** -- a Shell command, or the service's
 * launcher -- never from inside the service. UEFI allows `LoadImage()` and
 * `StartImage()` only below `TPL_CALLBACK` (UEFI 2.11 Table 7.3), and a
 * resident service's own code runs in event callbacks at `TPL_CALLBACK`; the
 * only `TPL_APPLICATION` context it ever has is an application's call into
 * it, and an image cannot unload itself with that call still on its stack.
 * The caller must not be running under any call from the service's image
 * either -- an application the service itself started, for instance: the
 * service's code is then on the stack below it. A call from inside the
 * service's own image is detected and refused; one further down the stack
 * is not. A service that wants to be upgraded unattended (an `/upgrade`
 * request, a downloaded image) has no application to ask; it stages the
 * image and resets instead (@ref axl_service_stage).
 *
 * The application, at `TPL_APPLICATION`:
 *
 *   1. Loads @p new_path as the replacement (validating it *before* anything
 *      is stopped -- a load failure leaves the service serving).
 *   2. Stops the running service -- `UnloadImage`, as @ref axl_service_stop
 *      does: its loop is detached and your @ref AxlServiceTeardown runs
 *      (release the listen ports; free an HTTP server with
 *      `AXL_TEARDOWN_RESET` so they are free on return), and its image is
 *      gone before the next step.
 *   3. Starts the replacement with the options @p svc carries (serialized as
 *      @ref axl_service_start_embedded serializes them). **The running
 *      service's current options are NOT inherited**: a service started with
 *      non-default options comes back with whatever the caller's descriptor
 *      holds -- its defaults, unless the caller sets them again. Its
 *      `AXL_SERVICE_DRIVER` entry rebinds the ports and comes up resident.
 *   4. Confirms the replacement published the service protocol --
 *      `StartImage` succeeding is not proof it attached.
 *
 * **Downtime.** Clients are disconnected (abortively, with
 * `AXL_TEARDOWN_RESET`), and the port is unbound from the old teardown until
 * the replacement's setup binds it again -- the old image's unload plus the
 * replacement's whole entry, which includes any network bring-up its setup
 * does. The replacement is loaded first, so a bad image costs none.
 *
 * No reboot, safe on a read-only volume (see @ref axl_service_reload_buffer
 * for an image already in memory). @p svc is the descriptor the service was
 * built from, or one with the same `name`: the running image is found by the
 * GUID derived from it.
 *
 * Failure semantics: if the replacement cannot be **loaded** (bad path /
 * corrupt image -- the common upgrade failure), nothing is stopped and the
 * service keeps running. From step 2 on, a failure leaves the service down.
 *
 * **`AXL_ERR` means -- and only means -- the service is now DOWN.** The old
 * image has been unloaded: nothing is resident, @ref axl_service_is_running
 * is false, and this call cannot bring it back. Recovery is the caller's:
 * keep the old image (its path, or its bytes) until this returns, and on
 * `AXL_ERR` start it again with @ref axl_service_start_embedded -- or reset,
 * or fall back to a watchdog. Every other non-OK code reports a failure that
 * left the running service untouched and serving:
 *
 * | return | meaning | the service |
 * |---|---|---|
 * | `AXL_OK` | replacement resident and published; the old image unloaded | replaced, serving |
 * | `AXL_INVALID` | NULL @p svc, NULL path or image; or no image publishes the service | untouched |
 * | `AXL_UNSUPPORTED` | called at `TPL_CALLBACK` or above, or from inside the service's own image | untouched, serving |
 * | `AXL_NOT_FOUND` | the replacement could not be **loaded** | untouched, serving |
 * | `AXL_NO_RESOURCES` | options would not serialize, or LoadOptions could not be installed | untouched, serving |
 * | `AXL_DENIED` | the running image would not stop and still publishes the service (no Unload handler, or its unload refused); the replacement was not started | untouched, serving -- unless its teardown failed (see below) |
 * | `AXL_ERR` | the running image was stopped (it no longer publishes the service) but did not unload cleanly, or the replacement failed to **start** or publish | **DOWN -- fatal** |
 *
 * `AXL_DENIED` is judged by the service GUID still being published. With
 * several images publishing it, one may already have stopped while another
 * refused; and a foreign image that tears down and then refuses its unload
 * still publishes, so it reads as serving although it is not. The same
 * holds for an `AXL_SERVICE_DRIVER` image whose teardown failed: resident
 * and published but half-stopped, its loop no longer running, and the next
 * stop or reload retries its teardown (see @ref AxlServiceTeardown). To
 * tell the two apart after `AXL_DENIED`, check @ref axl_service_state --
 * #AXL_SERVICE_STOPPING is the half-stopped case, serving nothing it
 * released.
 *
 * @return AXL_OK once the replacement is resident and the old image is gone;
 *     otherwise one of the codes in the table above.
 */
AxlStatus
axl_service_reload(
    const AxlService *svc,       ///< the service to replace (its name finds the running image)
    const char       *new_path   ///< path to the replacement driver .efi (e.g. "fs0:\\svc-v2.efi")
);

/**
 * @brief Like @ref axl_service_reload, but the replacement image comes from a
 *     memory buffer instead of a file path.
 *
 * The read-only-volume / network-upgrade form: the new version is a driver
 * image already in memory (downloaded, decompressed, etc.), so nothing is
 * read from -- or written to -- a filesystem. Otherwise identical to
 * @ref axl_service_reload, including that an application calls it, at
 * `TPL_APPLICATION`.
 *
 * @return the same as @ref axl_service_reload; AXL_INVALID additionally on a
 *     NULL / zero-length @p image.
 */
AxlStatus
axl_service_reload_buffer(
    const AxlService *svc,       ///< the service to replace
    const void       *image,     ///< replacement driver image bytes (PE/COFF .efi)
    size_t            image_len  ///< image size in bytes
);

// ---------------------------------------------------------------------------
// Staged upgrade: stage from the service, apply at the next start
// ---------------------------------------------------------------------------
//
// The unattended-upgrade path for a resident service, which has no
// application to perform an axl_service_reload. The service -- in a request
// handler or a timer on its own loop -- stages the new image with
// axl_service_stage() and resets (axl_reset) when it suits it. At the next
// boot its launcher, run from startup.nsh, applies it: the `start` verb of
// axl_service_main calls axl_service_start_staged() whenever the deploy pins
// driver_path (`start --no-staged` still resolves what the last boot left,
// and skips only the trial and promotion). See the service README,
// "Staged upgrade".
//
// With P = deploy->driver_path, the files are:
//   P            the image the service runs from -- the known-good image
//   P.new        a staged candidate (its presence is the "staged" marker)
//   P.bak        a copy of the image P held before the last promotion
//   P.failed     the last candidate that failed its trial (overwritten)
//   P.new.tmp, ~AXLhhhh.TMP   write/rename leftovers a power loss can leave
//
// An image's KEY is the CRC32 and size of what it loads as: a CRC over the
// PE's headers and the sections up to the last section's end, with
// OptionalHeader.CheckSum, the security data-directory entry, the
// certificate table and any overlay left out, as the Authenticode hash
// leaves them out -- so a re-signed copy, or one with bytes appended, keys
// the same. An AxlPack is unpacked and keyed by the PE inside, so an image
// keys the same packed or bare, and a quarantined one is refused either way.
//
// The launcher side takes the deploy (it knows driver_path); the service
// side's axl_service_last_upgrade and axl_service_clear_quarantine take the
// AxlService, because a running service has its descriptor, not a deploy.

/**
 * @brief Stage a replacement image for the service's next start.
 *
 * Called by the service itself, from its own callbacks (a request handler,
 * a timer): legal at any TPL up to `TPL_CALLBACK`. It validates and writes a
 * file and loads no image, so the TPL rule of axl-driver.h does not apply
 * and the call is not refused there. It **blocks the calling callback for
 * the read of P and the write** (about twice the image's size, plus a
 * flush), so the loop serves nothing meanwhile. The launcher may call it
 * too.
 *
 * Writes the image to `P.new` with axl_file_write_atomic() -- written to
 * `P.new.tmp`, flushed, then put in place -- so `P.new` is either absent or
 * a complete image, and its presence is the "staged" marker. Staging again
 * replaces an earlier `P.new`; a leftover `P.new.tmp` is overwritten. P is
 * not touched, and nothing is applied until the next start, so staging while
 * the service runs is the normal case.
 *
 * **Refused before writing** (`P.new` is then as it was):
 *   - bytes that are not a PE32+ image (or an AxlPack wrapping one) for this
 *     firmware's machine type, whose subsystem is not a boot-service or
 *     runtime driver (an application would start, exit and publish
 *     nothing), or an AxlPack whose header or payload CRC is wrong;
 *   - an image whose key is quarantined by an earlier failed trial;
 *   - an image @p opts->verify refuses.
 *
 * **Refused while an upgrade is in flight** (AXL_BUSY, nothing written):
 * while the launcher is trialling or promoting a candidate -- the commit
 * grace of @ref axl_service_start_staged, when the candidate already
 * serves and its handlers can call this -- see
 * @ref axl_service_upgrade_in_flight. Staging there would put an untrialled
 * image where the promotion is about to take the trialled one from. Try
 * again once the grace period is over.
 *
 * **An image byte-identical to P** (a full compare, not a CRC) is not
 * written: the caller has said "this version", which is the running one, so
 * any staged `P.new` is removed (unstaged) and AXL_OK returned; a log line
 * says so.
 *
 * **Not a security gate.** The checks above are format and policy checks.
 * Authenticating the image -- who may upgrade the service, and with what --
 * is the caller's, before it calls this; @c verify is where a pinned hash
 * or signer goes on a box without Secure Boot.
 *
 * **Do not restage automatically after a failed trial.** A service that reads
 * ROLLED_BACK from @ref axl_service_last_upgrade must not stage the same
 * upgrade again without new input; the quarantine is a backstop against a
 * reboot loop, not the policy. (After a lost promotion the candidate is not
 * quarantined, and staging it again is correct.)
 *
 * @return AXL_OK when `P.new` holds the image, or the image is P's (any
 *     `P.new` then removed);
 *     AXL_UNSUPPORTED when @p deploy pins no driver_path (an embedded-only
 *     or searched deploy has no file to stage beside);
 *     AXL_INVALID for a NULL @p deploy, service or @p image, a zero
 *     @p image_len, or bytes that fail the format checks above;
 *     AXL_DENIED for a quarantined image or one @c verify refused;
 *     AXL_BUSY while an upgrade is in flight (above);
 *     AXL_ERR when P could not be read for the comparison or the write
 *     failed (read-only volume, no room, device error -- the log says
 *     which).
 */
AXL_WARN_UNUSED AxlStatus
axl_service_stage(
    const AxlServiceDeploy    *deploy,     ///< the service's deploy (driver_path required)
    const AxlServiceStageOpts *opts,       ///< may be NULL
    const void                *image,      ///< replacement driver image bytes
    size_t                     image_len   ///< image size in bytes
);

/**
 * @brief Cancel a staged upgrade: remove `P.new` (and a leftover
 *     `P.new.tmp`).
 *
 * Legal at any TPL up to `TPL_CALLBACK`. A trial already recorded in NV --
 * the box reset during one -- is not erased by this; the next start finds
 * the record with no `P.new` and resolves it by starting P (see
 * @ref axl_service_start_staged, step 1). Refused, removing nothing, while
 * an upgrade is in flight (@ref axl_service_upgrade_in_flight): the
 * candidate being trialled IS `P.new`, and removing it would leave the
 * promotion nothing to promote.
 *
 * @return AXL_OK when nothing is staged afterwards (including when nothing
 *     was); AXL_UNSUPPORTED without driver_path; AXL_INVALID for a NULL
 *     @p deploy; AXL_BUSY while an upgrade is in flight; AXL_ERR when the
 *     file could not be removed.
 */
AXL_WARN_UNUSED AxlStatus
axl_service_unstage(
    const AxlServiceDeploy *deploy   ///< the service's deploy
);

/**
 * @brief Whether an upgrade is staged: `P.new` exists.
 *
 * Legal at any TPL up to `TPL_CALLBACK`. False without driver_path, for a
 * NULL @p deploy, and when the volume cannot be read.
 *
 * @return true when `P.new` exists.
 */
bool
axl_service_is_staged(
    const AxlServiceDeploy *deploy   ///< the service's deploy
);

/**
 * @brief Start the service, first resolving and applying a staged upgrade
 *     -- the boot-time half.
 *
 * Call it from the service's launcher (an application, at
 * `TPL_APPLICATION`); the `start` verb of @ref axl_service_main does
 * whenever driver_path is pinned. The options are @p deploy->staged_opts.
 * Without driver_path it is @ref axl_service_start_embedded and @p result
 * is NONE. If the service is already running, nothing is applied, a staged
 * `P.new` waits for the next start, and @p result is NONE.
 *
 * The phase is kept in an AxlAttempt breadcrumb -- an AxlNvstore namespace
 * named after the service, bound to its GUID -- and written BEFORE each
 * change it describes: `trial:N:<crc>:<size>` (the candidate's key),
 * `promote:bak:<crc>:<size>`, `promote:swap:<crc>:<size>`. The quarantine
 * (failed candidates' keys) and the last outcome live in the same
 * namespace. Steps, in order:
 *
 *   1. **Resolve what the last boot left.** The rows are tried in this
 *      order, and the promote rows take precedence over "P missing":
 *      | recorded | files | action |
 *      |---|---|---|
 *      | `promote:bak` or `promote:swap` | `P.new` present with a key other than the record's (restaged since) | clear the record, a warning; trial the new candidate from 1 (step 3) |
 *      | `promote:bak` | `P.new` present (P still the old image) | redo step 5 from the copy |
 *      | `promote:bak` | `P.new` absent (FAT damage only) | clear the record, error line, start P; ROLLED_BACK |
 *      | `promote:swap` | `P.new` present | finish: replace P with `P.new` (a plain rename if P is missing); APPLIED |
 *      | `promote:swap` | `P.new` absent, P's key is the candidate's | the swap was done; clear the record; APPLIED |
 *      | `promote:swap` | `P.new` absent, P present with another key | the rename lost the candidate: clear the record, error line, start P; ROLLED_BACK (not quarantined) |
 *      | `promote:swap` | P and `P.new` both absent | restore P from `P.bak` by copy, clear the record, error line; ROLLED_BACK -- with no `P.bak` (a first upgrade), AXL_NOT_FOUND and a "no fallback" error |
 *      | `trial:N` | `P.new` absent (unstaged, or a lost rename) | clear the record, start P; ROLLED_BACK if the record's key is quarantined, else NONE |
 *      | `trial:N` | `P.new`'s key is quarantined | finish step 4 without starting it; ROLLED_BACK |
 *      | `trial:N` | `P.new`'s key differs from the record's | a new candidate: its trial starts again at 1 (step 3) |
 *      | `trial:N`, N >= tries | `P.new` is the candidate | give up on it -- step 4, without starting it |
 *      | `trial:N`, N < tries | `P.new` is the candidate | try it again as trial N + 1 (step 3); a success is RETRIED |
 *      | (none) | P missing, `P.bak` present | restore P from `P.bak` by copy (as before EVERY start of P, whatever the row) |
 *      | (none) | `P.new`'s key is quarantined (put there by hand: a stage refuses it) | step 4 without starting it; ROLLED_BACK |
 *      Then asides (`~AXLhhhh.TMP`) in P's directory that are byte-identical
 *      to P, `P.bak`, `P.new` or `P.failed` are removed. An aside that
 *      matches none -- an earlier candidate a restage replaced while a power
 *      loss interrupted it -- is left and logged by name: no file is ever
 *      deleted without a full match.
 *      A `P.new` that cannot be read or is not a driver image for this
 *      machine (a hand-copied or truncated file -- a stage refuses one) has
 *      no key: it cannot be trialled or quarantined. With or without a
 *      record, the record is cleared, it is renamed to `P.failed` with a
 *      warning, and P starts; ROLLED_BACK. A recorded candidate is neither
 *      retried nor quarantined on its account.
 *   2. **Nothing staged:** start P. NONE.
 *   3. **Trial.** Record `trial:N:<key>`, arm the UEFI watchdog
 *      (@c watchdog_sec), read `P.new` and load it under P's device path (so
 *      it runs with P's identity; a pack is unpacked as any load is), start
 *      it, and require that it publish the service GUID. At publish, re-arm
 *      the watchdog for @c commit_grace_sec + 60 s -- so the grace period
 *      always fits inside it, whatever the setup took -- and wait the grace
 *      period at `TPL_APPLICATION` with the record still set. A candidate
 *      that wedges a callback in that time never lets the launcher continue,
 *      the watchdog resets the box, and step 1 retries it or gives up.
 *      Disarm the watchdog. P is not touched.
 *   4. **Trial failed** (the load, the start, no publish): unload the
 *      candidate; quarantine its key (NV) FIRST; then rename `P.new` to
 *      `P.failed` (overwriting the last one -- the only file renamed is the
 *      bad one); then clear the record; log the key; start P. ROLLED_BACK.
 *   5. **Promote.** Record `promote:bak`, write `P.bak` as a COPY of P (P is
 *      untouched); record `promote:swap`, replace P with `P.new`
 *      (axl_file_replace); record the outcome; clear the record. APPLIED
 *      (or RETRIED). The candidate is already the running image. `P.new`
 *      is re-keyed before the copy and again before the swap: if it is no
 *      longer the trialled candidate (written over or removed during the
 *      grace period by something other than @ref axl_service_stage, which
 *      refuses then), nothing is promoted -- the record is cleared with a
 *      warning, the candidate keeps serving UNPROMOTED, and the next start
 *      trials whatever `P.new` holds (NONE). **First upgrade with P
 *      missing** (and no `P.bak`): there is nothing to copy, so
 *      `promote:swap` is recorded and `P.new` renamed to P directly, and a
 *      warning says there is no fallback image.
 *
 * **Crash safety.** A complete runnable image -- P, and from step 5 also
 * `P.bak` -- is never the object of the rename or write in flight, every
 * change is preceded by its NV record, and step 1 decides what to do from
 * the record AND the files actually present, so redoing a step is safe. What
 * remains: FAT has no journal, so metadata damage that is not a rename is
 * not covered; and the unmatched asides above.
 *
 * **When NV refuses a write.** No step runs without its record. If
 * `trial:N` cannot be written, P starts, `P.new` is kept and a warning says
 * so (NONE); the next start tries again. If `promote:bak` or `promote:swap`
 * cannot be written, the promotion stops there with an error line: the
 * candidate keeps running this boot, UNPROMOTED, and @p result is NONE;
 * the record already set -- the trial's, or `promote:bak` -- resumes it at
 * the next start (a `trial:N` counts that boot as one that died, so it is
 * RETRIED; a `promote:bak` finishes the copy and swap, APPLIED). Because a
 * refused `promote:bak` costs the candidate a try, @c tries of 1 would give
 * a good candidate up and quarantine it: keep @c tries at 2 or more (the
 * default). The quarantine is read back after
 * it is written; one that did not stick is an error line, and the
 * candidate, already renamed to `P.failed`, is not tried again.
 *
 * **Hangs and faults.** The UEFI watchdog is a timer event above
 * `TPL_CALLBACK`, so a candidate that hangs -- in its entry, or a callback
 * during the grace period -- is reset and retried or given up. A CPU
 * exception is NOT covered by it: the firmware's default exception handlers
 * halt with interrupts masked, so no timer fires. A fault is covered only
 * where something resets on it -- a hardware or BMC watchdog; AXL's crash
 * handler records a fault but does not reset.
 *
 * **Forced downgrade.** Anyone who can power-cycle the box `tries` times
 * during a trial makes the previous image win. That is the rollback design
 * working; the library enforces no anti-rollback (see
 * AxlServiceStageOpts::verify).
 *
 * **What commits.** Publishing the service GUID and then staying up for
 * @c commit_grace_sec commits the candidate. A failure after that is out of
 * scope: it is not rolled back.
 *
 * The last outcome (for @ref axl_service_last_upgrade) is written only by a
 * start that resolved or tried a candidate; a NONE start leaves the last
 * APPLIED or ROLLED_BACK in place.
 *
 * A service that is #AXL_SERVICE_STOPPING is not started over and no
 * staged image is touched: this returns AXL_BUSY with @p result NONE. A
 * candidate that comes up stopping is not a successful trial.
 *
 * @return AXL_OK when the service is running -- @p result says which image;
 *     AXL_BUSY when the service is #AXL_SERVICE_STOPPING (see above), or
 *     the image this start brought up is;
 *     AXL_UNSUPPORTED at `TPL_CALLBACK` or above (it loads an image);
 *     AXL_INVALID for a NULL or incomplete @p deploy;
 *     AXL_NOT_FOUND when neither the candidate nor P could be started;
 *     AXL_ERR when a file could not be moved or written -- the files are as
 *     the log names them, and P was still started if it resolves.
 *     @p result, when non-NULL, is always written: on AXL_OK, as above; on
 *     AXL_ERR, the outcome reached so far (ROLLED_BACK or NONE); on
 *     AXL_NOT_FOUND, ROLLED_BACK if a candidate was tried, else NONE; on
 *     AXL_INVALID and AXL_UNSUPPORTED, NONE.
 */
AXL_WARN_UNUSED AxlStatus
axl_service_start_staged(
    const AxlServiceDeploy *deploy,   ///< the service's deploy (options: staged_opts)
    AxlServiceStagedResult *result    ///< [out] always written when non-NULL
);

/**
 * @brief The outcome of the last staged-upgrade attempt, from the NV state
 *     @ref axl_service_start_staged writes.
 *
 * For the service to report it (an `/upgrade` status) after the launcher
 * started it: legal at any TPL up to `TPL_CALLBACK`, and it survives later
 * restarts until the next attempt overwrites it. While an upgrade is in
 * flight (@ref axl_service_upgrade_in_flight) the stored outcome is the
 * PREVIOUS attempt's, so this answers AXL_BUSY instead. An attempt that
 * reaches a verdict writes it before it stops being in flight, so a reader
 * never sees the old verdict once it has ended; one that ends NONE (a
 * refused record, a `P.new` changed during the trial) leaves the previous
 * verdict in place, as NONE always does.
 *
 * @return AXL_OK with @p out set; AXL_BUSY while an upgrade is in flight;
 *     AXL_NOT_FOUND when no attempt was ever recorded; AXL_INVALID for a
 *     NULL @p svc or @p out.
 */
AXL_WARN_UNUSED AxlStatus
axl_service_last_upgrade(
    const AxlService       *svc,   ///< the service (its name finds the NV state)
    AxlServiceStagedResult *out    ///< [out] the last outcome
);

/**
 * @brief Whether the launcher is trialling or promoting a candidate of
 *     this service right now.
 *
 * True from the start of a trial in @ref axl_service_start_staged (step 3)
 * until its promotion (step 5) is over, in THIS boot -- in practice the
 * commit grace period, when the candidate serves while the launcher waits
 * to promote it. @ref axl_service_stage and @ref axl_service_unstage answer
 * AXL_BUSY then, and @ref axl_service_last_upgrade has no verdict yet; a
 * service reports "an upgrade is in progress" from this without trying a
 * write.
 *
 * The launcher keeps a VOLATILE marker in the service's NV namespace for
 * this, not the `trial:N` / `promote:*` record: a record outlives the boot
 * that set it -- a boot that died in a trial, a promotion whose record NV
 * refused, `start --no-staged` -- and is not in flight then; the next start
 * resolves it whatever was staged over it. If the marker itself cannot be
 * written, this stays false and the promotion's re-key of `P.new` is the
 * guard that remains.
 *
 * Legal at any TPL up to `TPL_CALLBACK`. False for a NULL @p svc.
 *
 * @return true while an upgrade of @p svc is in flight.
 */
bool
axl_service_upgrade_in_flight(
    const AxlService *svc   ///< the service (its name finds the NV state)
);

/**
 * @brief Forget every quarantined candidate, so an image that once failed
 *     its trial can be staged again.
 *
 * For an operator who has fixed what made it fail (a configuration, a
 * dependency) -- not for a service's own retry loop (see
 * @ref axl_service_stage). Legal at any TPL up to `TPL_CALLBACK`.
 *
 * @return AXL_OK (also when nothing was quarantined); AXL_INVALID for a
 *     NULL @p svc; AXL_ERR when the NV store could not be written.
 */
AXL_WARN_UNUSED AxlStatus
axl_service_clear_quarantine(
    const AxlService *svc   ///< the service (its name finds the NV state)
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
 * It answers "published", not "serving": a service in
 * #AXL_SERVICE_STOPPING is published too. Ask @ref axl_service_state
 * when the difference matters.
 *
 * @return true if a previous launch is still publishing the protocol.
 */
bool
axl_service_is_running(
    const AxlServiceDeploy *deploy
);

// ---------------------------------------------------------------------------
// Service state
// ---------------------------------------------------------------------------

/**
 * @brief What a service is doing, as any image can see it.
 *
 * Two axes in one answer, the worst winning: the LIFECYCLE the framework
 * owns (stopped, running, stopping) and the HEALTH the service reports
 * about itself (degraded). The order of the values is the order of
 * precedence: with several images publishing one service, the state is
 * the highest any of them is in.
 */
typedef enum {
    AXL_SERVICE_STOPPED  = 0,  ///< no image publishes the service
    AXL_SERVICE_RUNNING  = 1,  ///< published, and nothing below holds
    AXL_SERVICE_DEGRADED = 2,  ///< running, but the service has said part of
                               ///< it is not working (@ref axl_service_set_degraded)
    AXL_SERVICE_STOPPING = 3,  ///< half-stopped: a teardown failed (at an unload,
                               ///< or rolling back a failed attach) or setup
                               ///< returned #AXL_SERVICE_SETUP_HELD. Resident and
                               ///< published, its loop stopped, serving nothing
                               ///< it released; the next stop retries the
                               ///< teardown, and a reset clears it.
} AxlServiceState;

/** The most bytes of a @ref axl_service_set_degraded reason that are kept. */
#define AXL_SERVICE_REASON_MAX  80

/**
 * @brief The `status` verb's exit code for #AXL_SERVICE_STOPPING.
 *
 * @ref axl_service_main's `status` exits 0 for running and degraded (a
 * degraded service is up, as systemd counts it active) and 1 for stopped;
 * a half-stopped service gets its own code, chosen outside the LSB init
 * script range (0-4) so a script that knows those codes cannot misread it.
 */
#define AXL_SERVICE_EXIT_STOPPING  5

/**
 * @brief The state of the service @p svc names, read from any image.
 *
 * Read from the firmware's handle database, not from any image's memory:
 * the driver tags its own image handle with name-derived GUIDs -- the
 * service GUID itself, and the `NAME/stopping` and `NAME/degraded`
 * tags while those hold -- and a tag goes with the image when it unloads.
 * The tags are derived in a namespace of their own, so no service name
 * (not even one containing `/stopping`) can publish another's tag.
 * The framework sets and clears the stopping tag; the service sets and
 * clears the degraded one.
 *
 * Callable at any TPL up to `TPL_NOTIFY`.
 *
 * @return the state (see #AxlServiceState for the precedence);
 *     #AXL_SERVICE_STOPPED for a NULL @p svc or one without a name.
 */
AxlServiceState
axl_service_state(
    const AxlService *svc   ///< the service (its name finds it)
);

/**
 * @brief The one line that explains a stopping or degraded service.
 *
 * For #AXL_SERVICE_STOPPING, the framework's own line, `teardown failed;
 * 'stop' retries; a reset clears it`. For #AXL_SERVICE_DEGRADED, the
 * reason the service gave @ref axl_service_set_degraded, read from the
 * degraded tag's interface (a copy the driver owns for as long as the tag
 * is published). With several images, one of those in the highest state
 * answers: STOPPING always gives the framework's line, and between
 * several degraded images the reason is any one of theirs. Truncated to
 * fit @p size, always NUL-terminated.
 *
 * The degraded tag's interface is a frozen layout, read across SDK
 * versions: `{ uint32_t size; char why[]; }`, where `size` is the whole
 * struct's. A reader accepts any `size` that holds at least one byte of
 * `why`, reads no more of it than both versions have, and terminates it
 * itself; fields are only ever added at the end.
 *
 * @return AXL_OK with @p buf set; AXL_NOT_FOUND when the service is
 *     neither stopping nor degraded (@p buf set to ""); AXL_INVALID for a
 *     NULL argument or a zero @p size.
 */
int
axl_service_state_reason(
    const AxlService *svc,   ///< the service (its name finds it)
    char             *buf,   ///< [out] the reason
    size_t            size   ///< bytes at @p buf
);

/**
 * @brief Say, from inside the running service, that part of it is not
 *     working -- or that it is again.
 *
 * For a condition the service cannot resolve on its own and an operator
 * should see in `status`: a console take-over the firmware would not give
 * back, say, while the service stays up. The service is still running
 * (`status` exits 0 and prints the reason); a stop and a reload work as
 * usual. Only the service sets and clears it, and the framework clears it
 * when the image unloads, so it never outlives the image -- a reload's
 * replacement starts healthy.
 *
 * Callable from the service's setup and from its loop at `TPL_CALLBACK`,
 * in an AXL_SERVICE_DRIVER image. Calling it again while degraded
 * replaces the reason.
 *
 * @return AXL_OK; AXL_INVALID for a NULL @p why with @p degraded true;
 *     AXL_ERR outside an AXL_SERVICE_DRIVER image whose entry has
 *     published the service, or when the firmware refused the tag (then
 *     logged: status cannot show it).
 */
int
axl_service_set_degraded(
    bool        degraded,   ///< true to report, false to clear
    const char *why         ///< when @p degraded: one line, copied; at most
                            ///< #AXL_SERVICE_REASON_MAX bytes are kept.
                            ///< Ignored when clearing.
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
 * Builds a default `axl_args_run` verb tree (`start [--detach]
 * [--no-staged]`, `stop [--reset]`, `status`) from @p deploy and
 * dispatches argv.
 *
 * What the verbs say about the state (@ref axl_service_state):
 *
 * | state | `status` prints / exits | `start` |
 * |---|---|---|
 * | stopped | `NAME: stopped` / 1 | starts it |
 * | running | `NAME: running` / 0 | `NAME: already running` / 0 |
 * | degraded | `NAME: degraded (REASON)` / 0 | `NAME: already running (degraded: REASON)` / 0 |
 * | stopping | `NAME: stopping (teardown failed; 'stop' retries; a reset clears it)` / #AXL_SERVICE_EXIT_STOPPING | refuses with the same line / 1, touching no staged image |
 *
 * `start` refuses with that line too when the image it just started came
 * up stopping. `stop --reset` is the last resort for a service whose
 * teardown keeps failing: it stops as `stop` does -- which retries the
 * teardown -- and if the service is then #AXL_SERVICE_STOPPING (and ONLY
 * then), flushes the log, warns and resets the machine (axl_reset, cold).
 * A stop that fails for any other reason -- a foreign image with no
 * Unload handler publishing the name, a protocol another agent holds, an
 * allocation failure -- is reported as `stop` reports it, with a line
 * saying the reset does not apply: a reset could not be relied on to help
 * (a foreign image comes back at boot). There is no forced unload: an
 * image something still calls into cannot be freed safely.
 *
 * The `start` verb populates `deploy->service->user` from the parsed
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
