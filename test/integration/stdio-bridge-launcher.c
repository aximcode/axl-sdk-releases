/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-launcher.c — thin launcher for the stdio-bridge test.
 *
 * Embeds the driver's .efi bytes (.incbin via EMBED_BLOB). Locates
 * the resident driver — axl_shared_driver_locate installs the stdio
 * bridge on success — then dispatches the verb (argv[1..]) into the
 * driver image. argv[0] (the launcher's own name) is skipped so the
 * driver's argv[0] is the verb.
 */

#include <axl.h>
#include "stdio-bridge-fix.h"

/* Driver bytes baked in at link time. The bare-identifier embed
   symbol is stdio_bridge_fix; AXL_EMBED_* prepend axl_embedded_. */
AXL_EMBED_DECLARE(stdio_bridge_fix);

int
main(int argc, char **argv)
{
    FixVtable *vt = NULL;
    if (axl_shared_driver_locate(
            STDIO_BRIDGE_FIX_NAME,
            "stdio-bridge-driver.efi",
            AXL_EMBED_DATA(stdio_bridge_fix),
            AXL_EMBED_SIZE(stdio_bridge_fix),
            (void **)&vt) != AXL_OK) {
        axl_printf("stdio-bridge: failed to load driver\n");
        return 1;
    }

    /* Skip argv[0] (launcher name) so argv[0] becomes the verb. */
    return vt->run(argc - 1, argv + 1);
}
