/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-stdio-bridge.h
    Internal: carries a launcher's shell stdio handles across the
    shared-driver image boundary so a resident driver's axl_stdin/
    axl_stdout reflect the launching app's. NOT a public API. */
#ifndef AXL_STDIO_BRIDGE_H
#define AXL_STDIO_BRIDGE_H
#include <axl/axl-sys.h>    /* AxlGuid */
#include "axl-backend.h"    /* AxlFileHandle */

typedef struct {
    AxlFileHandle  stdin_h;
    AxlFileHandle  stdout_h;
    AxlFileHandle  stderr_h;
    void          *launcher_image;   /* EFI_HANDLE; reserved for a liveness check */
} AxlStdioBridge;

/* uuid c8f517d7-36cc-458d-98d6-b116825e30bf — fixed identity of the
   stdio-bridge protocol. */
extern const AxlGuid AXL_STDIO_BRIDGE_GUID;

/* Returns AXL_OK when the bridge is installed or there were no launcher
   shell handles to bridge (no-op); AXL_ERR if the protocol install failed. */
int  axl_backend_stdio_bridge_install(void);
void axl_backend_stdio_bridge_uninstall(void);

#endif /* AXL_STDIO_BRIDGE_H */
