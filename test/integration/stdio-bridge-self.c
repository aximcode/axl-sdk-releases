/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-self.c — self-locating launcher for the stdio-bridge test.
 *
 * Mirrors a consumer that resolves the resident driver ITSELF (warm
 * fast-path: axl_shared_driver_guid + axl_protocol_find_guid) instead of
 * through axl_shared_driver_locate. Because locate never runs, the stdio
 * bridge is NOT auto-installed — so this launcher calls the public escape
 * hatch axl_shared_driver_install_stdio_bridge() before dispatch. Without
 * that call the resident driver's axl_stdin would be EOF.
 *
 * Relies on the sibling locate launcher (stdio-bridge-fix.efi) having run
 * first in the same boot to make the driver resident; the driver image
 * stays loaded after that launcher exits, so find_guid resolves here.
 */

#include <axl.h>
#include "stdio-bridge-fix.h"

int
main(int argc, char **argv)
{
    AxlGuid guid;
    if (axl_shared_driver_guid(STDIO_BRIDGE_FIX_NAME, &guid) != AXL_OK) {
        axl_printf("stdio-bridge-self: guid derivation failed\n");
        return 1;
    }

    /* Warm fast-path — driver already resident (do.efi's primary path). */
    void *vt = NULL;
    if (axl_protocol_find_guid(&guid, &vt) != AXL_OK || vt == NULL) {
        axl_printf("stdio-bridge-self: driver not resident\n");
        return 1;
    }

    /* We self-located, so locate's auto-install never ran. Install the
       bridge explicitly so the resident driver's axl_stdin reflects THIS
       launcher's piped / redirected StdIn. */
    if (axl_shared_driver_install_stdio_bridge() != AXL_OK) {
        axl_printf("stdio-bridge-self: bridge install failed\n");
        return 1;
    }

    /* Skip argv[0] (launcher name) so argv[0] becomes the verb. */
    return ((FixVtable *)vt)->run(argc - 1, argv + 1);
}
