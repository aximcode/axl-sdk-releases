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
    void          *launcher_image;   /* EFI_HANDLE; driver-side liveness gate — a
                                        consult is skipped if this image has
                                        exited (its stdin_h would be dangling) */
    void          *launcher_image_proto; /* EFI_LOADED_IMAGE_PROTOCOL* recorded at
                                        install; liveness also requires the
                                        handle's CURRENT LoadedImage protocol
                                        pointer to still match this. Narrows
                                        but does not eliminate the handle-reuse
                                        false-alive — see bridge_launcher_alive()
                                        for the residual risk and mitigation */
    uint64_t       pending_status;   /* driver-armed exit status reflected here */
    bool           has_pending;      /* true when a driver armed a status this dispatch */
} AxlStdioBridge;

/* uuid c8f517d7-36cc-458d-98d6-b116825e30bf — fixed identity of the
   stdio-bridge protocol. */
extern const AxlGuid AXL_STDIO_BRIDGE_GUID;

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
