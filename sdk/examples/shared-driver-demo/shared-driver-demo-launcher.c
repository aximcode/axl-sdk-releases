/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-launcher.c — thin launcher image.
 *
 * Single-binary distribution: the driver's .efi bytes are embedded
 * via `axl-cc --embed` (or CMake's `EMBEDS` option). On first
 * invocation the launcher loads the embedded blob; subsequent
 * invocations within the same boot session hit
 * `axl_driver_ensure_with_embedded`'s LocateProtocol short-circuit
 * — the driver stays resident, the launcher just locates and
 * dispatches.
 *
 * Build (manual):
 *   axl-cc --type driver shared-driver-demo-driver.c \
 *          -o shared-driver-demo-dxe.efi
 *   axl-cc --embed shared-driver-demo-dxe.efi=shared_driver_demo \
 *          shared-driver-demo-launcher.c \
 *          -o shared-driver-demo.efi
 *
 * Build (CMake):  see the sibling CMakeLists.txt.
 */

#include <axl.h>
#include "shared-driver-demo.h"
#include "shared-driver-demo-format.h"  /* defined in -format.c (shared TU) */

/* Driver bytes baked in at link time. The `=shared_driver_demo`
 * suffix is the embed-symbol name — the AXL_EMBED_DATA / _SIZE
 * macros prepend the axl_embedded_ prefix to reach the linker
 * symbols. Convention: bare-identifier form of the consumer's
 * SHARED_DRIVER_DEMO_NAME string. */
AXL_EMBED_DECLARE(shared_driver_demo);

int
main(int argc, char **argv)
{
    /* Banner prints from the shared format TU. demo-format.c is
     * compiled into BOTH the launcher's and driver's source lists
     * (see CMakeLists.txt) — if it were missing from either,
     * `ld --no-undefined` would catch it at link time. */
    demo_print_banner("starting");

    SharedDriverDemoVtable *vt = NULL;

    /* One call wraps the cold-and-warm paths:
     *   - LocateProtocol short-circuits when the driver is resident.
     *   - Otherwise on-disk lookup, falling back to the embedded blob.
     *   - Then resolves the published vtable. */
    if (axl_shared_driver_locate(
            SHARED_DRIVER_DEMO_NAME,
            "shared-driver-demo-dxe.efi",
            AXL_EMBED_DATA(shared_driver_demo),
            AXL_EMBED_SIZE(shared_driver_demo),
            (void **)&vt) != AXL_OK) {
        axl_printf("shared-driver-demo: failed to load driver\n");
        return 1;
    }

    /* Cross-image call. From here on the verb body runs in the
     * resident driver image; the launcher's job is over. */
    return vt->do_run(argc, argv);
}
