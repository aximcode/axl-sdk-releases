/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-driver.c — resident driver image for the stdio-bridge
 * acceptance test.
 *
 * Publishes a one-verb vtable. The verb runs in THIS image's address
 * space when the launcher dispatches it, so its axl_stdin/axl_stdout
 * are the driver image's — which the launcher hook in
 * axl_shared_driver_locate must have pointed at the launching app's
 * shell handles via the backend stdio bridge.
 *
 * Build: axl-cc --type driver stdio-bridge-driver.c -o
 *               stdio-bridge-driver.efi
 */

#include <axl.h>
#include "stdio-bridge-fix.h"

AXL_LOG_DOMAIN("stdio-bridge-fix");

static FixVtable  gVtable;
static AxlHandle  gPublishedHandle;

/* argv[0] is the verb (the launcher strips its own name).
 *   "echo" — read one line from axl_stdin, print GOT:<line>.
 *   "emit" — print DRIVEROUT (the > redirect probe). */
static int
fix_run(int argc, char **argv)
{
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        axl_printf("GOT:<NOARG>\n");
        return 1;
    }

    if (axl_strcmp(argv[0], "echo") == 0) {
        char *l = axl_readline(axl_stdin);
        if (l != NULL) {
            /* axl_readline keeps a trailing newline; strip it so the
             * serial token is exactly GOT:<line>. */
            size_t n = axl_strlen(l);
            while (n > 0 && (l[n - 1] == '\n' || l[n - 1] == '\r')) {
                l[--n] = '\0';
            }
        }
        axl_printf("GOT:%s\n", l != NULL ? l : "<EOF>");
        axl_free(l);
        return 0;
    }

    /* Like "echo" but reads through axl_stdin_text(), which decodes the
     * shell's UCS-2 pipe (the default `|`) to UTF-8 — so `echo x | fix
     * echotext` works WITHOUT the `|a` ASCII-pipe operator. */
    if (axl_strcmp(argv[0], "echotext") == 0) {
        AxlStream *t = axl_stdin_text();
        char *l = (t != NULL) ? axl_readline(t) : NULL;
        if (l != NULL) {
            size_t n = axl_strlen(l);
            while (n > 0 && (l[n - 1] == '\n' || l[n - 1] == '\r')) {
                l[--n] = '\0';
            }
        }
        axl_printf("GOT:%s\n", l != NULL ? l : "<EOF>");
        axl_free(l);
        if (t != NULL) {
            axl_fclose(t);
        }
        return 0;
    }

    if (axl_strcmp(argv[0], "emit") == 0) {
        axl_printf("DRIVEROUT\n");
        return 0;
    }

    axl_printf("GOT:<BADVERB>\n");
    return 1;
}

static int
driver_main(AxlHandle h, AxlSystemTable *st)
{
    (void)h; (void)st;
    gVtable.run = fix_run;
    return axl_shared_driver_publish(STDIO_BRIDGE_FIX_NAME,
                                     &gVtable, &gPublishedHandle);
}

static int
driver_unload(AxlHandle h)
{
    (void)h;
    if (gPublishedHandle == NULL) {
        return AXL_OK;
    }
    return axl_shared_driver_unpublish(STDIO_BRIDGE_FIX_NAME,
                                       gPublishedHandle, &gVtable);
}

AXL_DRIVER(driver_main, driver_unload)
