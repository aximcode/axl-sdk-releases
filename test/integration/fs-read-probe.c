/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* fs-read-probe.c — launcher for the resident-driver file-read fixture.
 *
 * Reads the staged file BOTH ways in one run so the two are directly
 * comparable on the same shell:
 *   1. In the LAUNCHER (a normal shell app, standalone context) — tag "app".
 *   2. In the RESIDENT DRIVER dispatched via axl_shared_driver_locate_sibling
 *      (an image with no SHELL_INTERFACE) — tag "drv".
 * On the old EFI 1.x shell the app read works but the driver read was
 * reported empty; the runner asserts the two lines MATCH.
 *
 * argv[1], if given, overrides the default path (passed through to the driver).
 */
#include <axl.h>

#include "fs-read.h"

int
main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1] != NULL) ? argv[1]
                                                     : FSREAD_DEFAULT_PATH;

    /* 1. Standalone (launcher context) — the known-good baseline. */
    fsread_report("app", path);

    /* 2. Resident driver (no SHELL_INTERFACE) — the reported failure. Dispatch
       through axl_shared_driver_dispatch exactly like the consumer's launcher
       (do.c), so the stdio bridge is installed around the cross-image call —
       that bracket is the variable a direct vt->run() would miss. */
    AxlSharedDriverVtable *vt = NULL;
    int rc = axl_shared_driver_locate_sibling(
        FSREAD_NAME, "fs-read-driver.efi", (void **)&vt);
    if (rc != AXL_OK || vt == NULL || vt->run == NULL) {
        axl_printf("FSREAD:locate=FAIL rc=%d\n", rc);
        axl_printf("FSREAD:done\n");
        return 1;
    }
    char *drv_argv[1];
    drv_argv[0] = (char *)path;
    /* Dispatch exactly like the consumer's launcher (do.c) — installs the
       stdio bridge around the cross-image call. The driver tags itself "drv". */
    axl_shared_driver_dispatch(vt, 1, drv_argv);

    axl_printf("FSREAD:done\n");
    return 0;
}
