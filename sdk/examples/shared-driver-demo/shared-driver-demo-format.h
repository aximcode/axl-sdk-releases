/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-format.h — small output helper used by the
 * driver image.
 *
 * In the earlier custom-vtable version of this example, this module
 * was compiled into BOTH the launcher's and driver's source lists —
 * a real multi-TU consumer commonly has helper code (formatters,
 * argv parsing) that both halves call; see
 * docs/AXL-Shared-Driver-Recipe.md's "Sharing helpers between
 * launcher and driver" section for that general pattern.
 *
 * The turnkey launcher in this example (shared-driver-demo-launcher.c)
 * has no code of its own left to call a shared helper from — its
 * ENTIRE `int main` is the one `AXL_SHARED_DRIVER_LAUNCHER` macro
 * invocation, which is the turnkey macros' whole point (zero
 * plumbing). So this module now lives in the DRIVER's source list
 * only (see CMakeLists.txt) — still its own translation unit, just no
 * longer cross-image.
 */

#ifndef SHARED_DRIVER_DEMO_FORMAT_H
#define SHARED_DRIVER_DEMO_FORMAT_H

/**
 * Print a "demo: <message>" banner line to stdout. Called from the
 * driver's demo_init / demo_run / demo_unload
 * (shared-driver-demo-driver.c) so the demo's DriverEntry / dispatch /
 * unload lifecycle is visible in the shell output.
 */
void demo_print_banner(const char *message);

#endif /* SHARED_DRIVER_DEMO_FORMAT_H */
