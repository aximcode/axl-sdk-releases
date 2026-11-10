/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * log-redirect-selftest.c -- the console redirect of axl_log_set_console_serial
 * (<axl/axl-log.h>), for test-log-redirect-qemu.sh.
 *
 * A unit binary cannot drive it: the redirect writes to the UART the harness
 * parses results from, and nothing in-guest can read a console write back.
 * So this writes three WARN lines -- WARN because standard error colours it,
 * which is what tells the two paths apart in the raw serial log -- and the
 * host reads the bytes:
 *
 *   LOGREDIR-ON      redirected: one line, no colour
 *   LOGREDIR-SILENT  redirected, console logging off: nothing at all
 *   LOGREDIR-BACK    after axl_log_set_console_serial(NULL): standard error
 *                    again, so coloured
 *
 * then "LOGREDIR-DONE" by printf, so a missing line cannot be mistaken for an
 * image that died before writing it.
 */

#include <axl.h>
#include <axl/axl-log.h>
#include <axl/axl-serial.h>

AXL_LOG_DOMAIN("logredir");

int
main(void)
{
    AxlSerial *port = NULL;

    if (axl_serial_open(axl_serial_next(NULL), &port) != AXL_OK) {
        axl_printf("LOGREDIR-NOPORT\n");
        return 1;
    }
    axl_log_set_console_serial(port);
    axl_warning("LOGREDIR-ON");
    axl_log_set_console_enabled(false);
    axl_warning("LOGREDIR-SILENT");
    axl_log_set_console_enabled(true);
    axl_log_set_console_serial(NULL);
    axl_warning("LOGREDIR-BACK");
    axl_serial_close(port);
    axl_printf("LOGREDIR-DONE\n");
    return 0;
}
