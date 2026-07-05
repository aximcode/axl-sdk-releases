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

/* uuid c8f517d7-36cc-458d-98d6-b116825e30bf — fixed identity of the
   stdio-bridge protocol. */
extern const AxlGuid AXL_STDIO_BRIDGE_GUID;

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

/* Returns AXL_OK when the bridge is installed or there were no launcher
   shell handles to bridge (no-op); AXL_ERR if the protocol install failed. */
int  axl_backend_stdio_bridge_install(void);
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
