/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo.h — vtable contract shared between the driver
 * and launcher images. Both halves of the consumer #include this
 * header so they agree on the layout of the function table the
 * launcher dispatches into.
 *
 * Cross-image ABI: if either half's struct layout drifts, the
 * launcher's typed call jumps into the wrong driver-image function.
 * Treat this header as part of the consumer's public contract and
 * rebuild both binaries together when it changes.
 */

#ifndef SHARED_DRIVER_DEMO_H
#define SHARED_DRIVER_DEMO_H

/* Identity string for axl_shared_driver_publish / _locate. Same
 * string MUST be passed by both halves — the GUID is derived from
 * it via axl_guid_v5 against AXL's shared-driver namespace. */
#define SHARED_DRIVER_DEMO_NAME  "shared-driver-demo"

/* Consumer-owned vtable. One entry for the demo; a real tool would
 * expose one function per verb (cdump, find, cfg, capId, ...). */
typedef struct {
    int (*do_run)(int argc, char **argv);
} SharedDriverDemoVtable;

#endif /* SHARED_DRIVER_DEMO_H */
