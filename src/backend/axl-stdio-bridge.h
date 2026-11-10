/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-stdio-bridge.h
    Internal: carries a launcher's shell stdio handles across the
    shared-driver image boundary so a resident driver's axl_stdin/
    axl_stdout reflect the launching app's. NOT a public API. */
#ifndef AXL_STDIO_BRIDGE_H
#define AXL_STDIO_BRIDGE_H
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-sys.h>    /* AxlGuid */
#include "axl-backend.h"    /* AxlFileHandle */

/* The original bridge layout, 56 bytes on both arches, FROZEN: drivers built
   before AxlStdioBridge2 read it under AXL_STDIO_BRIDGE_GUID. */
typedef struct {
    AxlFileHandle  stdin_h;
    AxlFileHandle  stdout_h;
    AxlFileHandle  stderr_h;
    void          *launcher_image;   /* EFI_HANDLE; debug/identity only — no
                                        longer the liveness gate (see token) */
    uint64_t       token;            /* per-dispatch monotonic token (was
                                        launcher_image_proto; same 8-byte
                                        offset). The driver's liveness gate:
                                        a bridge is live iff this equals the
                                        driver-resident AxlDispatchToken.current
                                        the launcher stamped this dispatch. */
    uint64_t       pending_status;   /* driver-armed exit status reflected here */
    bool           has_pending;      /* true when a driver armed a status this dispatch */
} AxlStdioBridge;

/* uuid c8f517d7-36cc-458d-98d6-b116825e30bf — the original bridge, an
   AxlStdioBridge. A launcher still installs it, for drivers built before
   AxlStdioBridge2; a driver no longer reads it -- one built since reading it
   from an older launcher would read `target` past the end of that
   launcher's static -- so a launcher that installs only this one lends a
   newer driver nothing (EOF stdin, no bridging). */
extern const AxlGuid AXL_STDIO_BRIDGE_GUID;

/* The bridge drivers read, under AXL_STDIO_BRIDGE2_GUID. `base` is the very
   AxlStdioBridge a launcher installs under the original GUID -- one copy, so
   an exit status a driver of either age arms lands in the cell the launcher
   reads. `size` is sizeof the launcher's struct: a field appended later is
   read only when `size` covers it. */
typedef struct {
    uint32_t        size;     /* sizeof(AxlStdioBridge2) as the launcher built it */
    AxlStdioBridge  base;
    const void     *target;   /* the published interface this dispatch goes
                                 through: only the image that published it
                                 reads this bridge (NULL = no image does) */
} AxlStdioBridge2;

/* uuid ce4cacac-e9c8-4efc-926b-66be5876db55 — an AxlStdioBridge2. */
extern const AxlGuid AXL_STDIO_BRIDGE2_GUID;

/* How many published interfaces one image lends launchers' stdio to. */
#define AXL_BRIDGE_TARGETS_MAX  8

/* Driver-resident per-dispatch liveness reference. A resident driver installs
   exactly one of these (install-if-absent) at publish time on a dedicated
   persistent handle; each dispatch the launcher stamps the fresh token into
   both its bridge and this cell. `current == 0` means "no active dispatch". */
typedef struct {
    uint64_t  current;
} AxlDispatchToken;

/* uuid 02dd6813-d275-4734-98f8-c7f60331958d — fixed identity of the
   dispatch-token protocol. */
extern const AxlGuid AXL_DISPATCH_TOKEN_GUID;

/* Install the dispatch-token cell if none exists yet (idempotent, cross-image).
   Called by a resident driver at publish. AXL_OK when the cell exists
   afterward. CONTRACT: every resident shared-driver MUST reach this (the
   AXL_SHARED_DRIVER macro / axl_shared_driver_publish do so automatically) —
   a driver that publishes its vtable via raw axl_protocol_install without
   calling this never creates the cell, so ALL bridge consults see no cell and
   stdin/stderr/exit-status bridging silently degrades to EOF fallback (safe:
   no crash, no wrong data, just no bridging). */
int axl_backend_dispatch_token_ensure(void);

/* Install this launcher's bridge, aimed at @p target: the interface the
   dispatch goes through, as the driver published it. Returns AXL_OK when the
   bridge is installed or there were no launcher shell handles to bridge
   (no-op); AXL_ERR if the protocol install failed. */
int  axl_backend_stdio_bridge_install(const void *target);

/* Driver side: @p iface is (@p published true) or is no longer (false) an
   interface THIS image published for launchers to dispatch through. A bridge
   is read only by the image that published its target, so a launcher's
   handles never reach another resident image -- a service writing on its own
   clock, a second shared driver. Returns false when @p published and the
   image already lends to AXL_BRIDGE_TARGETS_MAX interfaces: @p iface is not
   recorded, and its launchers' stdio does not reach it. */
bool axl_backend_stdio_bridge_target(const void *iface, bool published);
void axl_backend_stdio_bridge_uninstall(void);

/* Uninstall every bridge instance whose launcher image has exited. A launcher
   that skips CRT0's atexit uninstall (--minimal-runtime, or gBS->Exit) leaves
   its bridge installed; each fresh launcher image can't see prior images'
   handles, so without an active sweep these dead instances accumulate. install
   calls this before publishing; axl_shared_driver_unload calls it so `do -u`
   clears the residual. Best-effort, cross-image, safe to call anytime. */
void axl_backend_stdio_bridge_reap(void);

/* Launcher-side: drain a driver-reflected exit status from the local bridge
   cell. Returns true + writes *out (and clears) when one is pending. */
bool axl_backend_bridge_take_exit_status(uint64_t *out);

#endif /* AXL_STDIO_BRIDGE_H */
