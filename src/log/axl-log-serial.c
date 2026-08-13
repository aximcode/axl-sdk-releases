/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-serial.c
    Serial log handler — writes formatted lines to an open serial port.

    The UART is the channel that survives what the others do not: a headless
    box, a wedged HTTP server, a console the firmware redirected elsewhere.
    Lines are formatted identically to the file sink's (both call
    axl_log_format_line) apart from CRLF endings, so a transcript read off a
    terminal and one read out of a log file line up. The longer terminator
    leaves one byte less for a maximal message -- the only other difference.

    The log dispatcher is not re-entrant and can run at raised TPL, so this
    handler allocates nothing, logs nothing, and never retries a short write.
**/

#include <stddef.h>
#include <axl/axl-log.h>
#include <axl/axl-serial.h>
#include "axl-log-line.h"

/* One port at a time, mirroring the file sink. The port is CALLER-owned:
   attach does not open it and detach does not close it, which is what lets a
   consumer share a port it already has open (a SOL bridge, say) rather than
   forcing this sink to own the selection policy. */
static AxlSerial *mSerialPort  = NULL;

static void
serial_handler(int level, const char *domain, const char *message,
               const AxlRealtime *stamp, void *data)
{
    (void)data;

    /* Level filtering is the dispatcher's, via axl_log_add_domain_handler's
       max_level -- no second copy of it here. */
    AxlSerial *port = mSerialPort;
    if (port == NULL) {
        return;
    }

    char line[640];
    size_t len = axl_log_format_line(line, sizeof(line), level, domain,
                                     message, stamp, "\r\n");
    if (len == 0) {
        return;
    }

    /* Single write, remainder dropped on a short one. A log line must never
       stall its caller: this can run at raised TPL from a driver poll tick,
       where a retry loop would spin against a UART that is not draining.
       Losing the tail of one line beats wedging the box that was logging it.
       The status is dropped for the same reason the file sink drops it --
       there is no actor for a log-write failure, and reporting it through the
       log would be a loop. */
    axl_serial_write(port, line, len, NULL);
}

int
axl_log_serial_attach(
    AxlSerial  *port,
    int         max_level
    )
{
    if (port == NULL) {
        return AXL_ERR;
    }
    /* Re-attach transparently replaces the previous port, matching
       axl_log_file_attach: the handler is registered at most once, so a
       second attach must not install a duplicate. */
    if (mSerialPort != NULL) {
        axl_log_remove_handler(serial_handler);
        mSerialPort = NULL;
    }
    if (max_level < AXL_LOG_ERROR) {
        max_level = AXL_LOG_ERROR;
    } else if (max_level > AXL_LOG_TRACE) {
        max_level = AXL_LOG_TRACE;
    }
    mSerialPort = port;

    if (axl_log_add_domain_handler(NULL, max_level, serial_handler, NULL)
            != AXL_OK) {
        mSerialPort = NULL;
        return AXL_ERR;
    }
    return AXL_OK;
}

void
axl_log_serial_detach(void)
{
    if (mSerialPort == NULL) {
        return;   /* idempotent */
    }
    axl_log_remove_handler(serial_handler);
    mSerialPort = NULL;
}
