/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo.h — identity shared between the driver and
 * launcher images.
 *
 * The turnkey AXL_SHARED_DRIVER / AXL_SHARED_DRIVER_LAUNCHER macros
 * (see shared-driver-demo-driver.c / shared-driver-demo-launcher.c)
 * own the cross-image vtable — the SDK's fixed AxlSharedDriverVtable
 * (a single `int run(int argc, char **argv)` entry) — so, unlike the
 * Advanced custom-vtable pattern in docs/AXL-Shared-Driver-Recipe.md,
 * this header does NOT declare a consumer-owned vtable type.
 *
 * The one thing that still MUST agree between the two images is the
 * identity string below: both AXL_SHARED_DRIVER's and
 * AXL_SHARED_DRIVER_LAUNCHER's first argument derive the SAME
 * protocol GUID from it (axl_guid_v5 against AXL's shared-driver
 * namespace) — that derived GUID is how the launcher finds the
 * driver's published vtable at runtime. A typo on one side silently
 * breaks the pairing (the launcher's resolve step just fails to find
 * the driver), so keep the string in one shared header rather than
 * typing the literal twice.
 */

#ifndef SHARED_DRIVER_DEMO_H
#define SHARED_DRIVER_DEMO_H

/* Shared-driver identity — passed as the first argument to both
 * AXL_SHARED_DRIVER (driver side, shared-driver-demo-driver.c) and
 * AXL_SHARED_DRIVER_LAUNCHER (launcher side,
 * shared-driver-demo-launcher.c). Convention: "vendor/tool" so
 * unrelated consumers don't collide on a generic name. */
#define SHARED_DRIVER_DEMO_NAME  "axl/demo"

#endif /* SHARED_DRIVER_DEMO_H */
