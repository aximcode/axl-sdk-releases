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
#include <axl/axl-signal.h>
#include "stdio-bridge-fix.h"

AXL_LOG_DOMAIN("stdio-bridge-fix");

static FixVtable  gVtable;
static AxlHandle  gPublishedHandle;

/* argv[0] is the verb (the launcher strips its own name); see also "err".
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

    /* "err" — print DRIVERERR on standard error: where the LAUNCHER's
       standard error is, this dispatch (test-stderr-row-qemu.sh). */
    if (axl_strcmp(argv[0], "err") == 0) {
        axl_printerr("DRIVERERR\n");
        return 0;
    }

    /* "wait" — stay in the dispatch for 2 s: a launcher whose bridge is live
       that long, while another image writes on its own clock. WAIT-BEGIN and
       WAIT-END on stdout bracket the 2 s, so a test can show the other
       image's lines fell inside them. */
    if (axl_strcmp(argv[0], "wait") == 0) {
        axl_printf("WAIT-BEGIN\n");
        axl_msleep(2000);
        axl_printf("WAIT-END\n");
        return 0;
    }

    /* Raw stdout, which the launcher's console turns into text with a
       decoder that carries state from one write to the next. Each dispatch
       is a different launcher: one must not inherit another's state.
       "rawbom"   -- UTF-16LE with its mark: BOM, a line.
       "rawsplit" -- SPLIT, a line, then a lone C3: a character left open.
       "rawplain" -- RAWPLAIN, a line, in plain UTF-8. */
    if (axl_strcmp(argv[0], "rawbom") == 0) {
        static const unsigned char b[] = {
            0xFF, 0xFE, 'B', 0, 'O', 0, 'M', 0, '\n', 0,
        };
        return axl_fwrite(b, 1, sizeof(b), axl_stdout_raw) == sizeof(b) ? 0 : 1;
    }
    if (axl_strcmp(argv[0], "rawsplit") == 0) {
        static const unsigned char b[] = { 'S', 'P', 'L', 'I', 'T', '\n', 0xC3 };
        return axl_fwrite(b, 1, sizeof(b), axl_stdout_raw) == sizeof(b) ? 0 : 1;
    }
    if (axl_strcmp(argv[0], "rawplain") == 0) {
        static const char b[] = "RAWPLAIN\n";
        return axl_fwrite(b, 1, sizeof(b) - 1, axl_stdout_raw) == sizeof(b) - 1 ? 0 : 1;
    }

    if (axl_strcmp(argv[0], "exitstatus") == 0) {
        /* Arm an exact, non-collapsible status; the launcher must exit
           with THIS value, proving cross-image reflection. 0x12345678 is
           large + non-zero (never produced by rc=0 or the launcher's own
           0x0 default) but has bit 63 CLEAR: the UEFI reference Shell
           strips MAX_BIT from an error-class .efi exit status before
           setting %lasterror% (ShellPkg RunCommand `Status & ~MAX_BIT`),
           so an error-class value would arrive truncated. A success-class
           value like this passes through verbatim. */
        axl_set_exit_status((AxlEfiStatus)0x12345678ULL);
        axl_printf("EXITSET\n");
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
                                       &gVtable, gPublishedHandle);
}

AXL_DRIVER(driver_main, driver_unload)
