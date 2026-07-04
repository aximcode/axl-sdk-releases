/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-launcher.c — thin launcher image.
 *
 * TEACHING: the launcher is the ENTIRE macro invocation at the bottom
 * of this file — no int main written by hand, no vtable, no
 * axl_shared_driver_locate call. AXL_SHARED_DRIVER_LAUNCHER expands to
 * a complete `int main(int argc, char **argv)` that:
 *
 *   1. Declares the embedded driver blob (AXL_EMBED_DECLARE) — the
 *      driver's .efi bytes are baked into THIS launcher binary at
 *      build time by the CMake EMBEDS clause (see the sibling
 *      CMakeLists.txt), so a consumer only has to copy around one
 *      file, shared-driver-demo.efi.
 *   2. Resolves the driver: already resident this boot (LocateProtocol
 *      short-circuit, the common case after the first invocation) ->
 *      on-disk "shared-driver-demo-dxe.efi" -> falls back to the
 *      embedded blob when neither of those hit.
 *   3. Installs the stdio bridge and calls demo_run(argc, argv) in the
 *      DRIVER image, forwarding argv verbatim.
 *   4. Applies any exit status demo_run armed via axl_set_exit_status()
 *      to THIS launcher's own exit, so `%lasterror%` after the shell
 *      command reflects what the driver decided.
 *
 * See shared-driver-demo-driver.c for what demo_run does with
 * stdin/stdout/stderr/exit-status once it's running, and
 * docs/AXL-Shared-Driver-Recipe.md for the full walkthrough.
 *
 * Build (manual):
 *   axl-cc --type driver shared-driver-demo-driver.c \
 *          shared-driver-demo-format.c \
 *          -o shared-driver-demo-dxe.efi
 *   axl-cc --embed shared-driver-demo-dxe.efi=shared_driver_demo \
 *          shared-driver-demo-launcher.c \
 *          -o shared-driver-demo.efi
 *
 * Build (CMake): see the sibling CMakeLists.txt.
 */

#include <axl.h>
#include "shared-driver-demo.h"

/* This one line IS the launcher: resolve, bridge stdio, dispatch into
 * demo_run(), reflect its exit status. The third argument
 * (shared_driver_demo) is the AXL_EMBED symbol name — it must match
 * the CMake EMBEDS clause / axl-cc --embed suffix above, see
 * CMakeLists.txt. */
AXL_SHARED_DRIVER_LAUNCHER(SHARED_DRIVER_DEMO_NAME,
                           "shared-driver-demo-dxe.efi",
                           shared_driver_demo)
