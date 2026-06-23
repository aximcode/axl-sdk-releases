/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-fix.h — vtable contract shared between the
 * stdio-bridge fixture's driver and launcher images.
 *
 * The launcher locates the resident driver by name and dispatches
 * a verb (argv[0]) into the driver image's address space. The
 * driver reads axl_stdin / writes axl_stdout to prove the launcher
 * hook in axl_shared_driver_locate bridged this invocation's shell
 * stdio across the image boundary.
 */

#ifndef STDIO_BRIDGE_FIX_H
#define STDIO_BRIDGE_FIX_H

/* Identity string for axl_shared_driver_publish / _locate. Both
   halves MUST pass the same string — the GUID is derived from it. */
#define STDIO_BRIDGE_FIX_NAME  "stdio-bridge-fix"

/* Cross-image vtable. run(argc, argv) dispatches on argv[0]:
   "echo" reads a line from axl_stdin and prints GOT:<line>;
   "emit" prints DRIVEROUT (the > redirect probe). */
typedef struct {
    int (*run)(int argc, char **argv);
} FixVtable;

#endif /* STDIO_BRIDGE_FIX_H */
