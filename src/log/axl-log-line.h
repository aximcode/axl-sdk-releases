/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-line.h
    Internal: the one formatted-log-line builder, shared by every sink that
    writes a transcript (file, serial). Not a public header.

    It exists so the sinks cannot drift: a consumer reading a box's log file
    and its UART should see the same line twice, not two dialects of it.
**/

#ifndef AXL_LOG_LINE_H
#define AXL_LOG_LINE_H

#include <stddef.h>
#include <axl/axl-time.h>   /* AxlRealtime */

/* Build one log line into @buf:

       2026-03-27T14:05:32.123456 [INFO ] domain: message<eol>

   The level tag is omitted for an out-of-range @level, and the "domain: "
   field for a NULL @domain or when it would not leave room for at least one
   byte of message. The message is then TRUNCATED to fit rather than dropped,
   and @eol is emitted, so a record can never run into the next one.

   Degenerate capacities are the exception and are signalled by a 0 return:
   a NULL @buf, a zero @cap, or a @cap too small to hold @eol writes nothing
   (beyond a NUL where there is room for one) and formats no record.

   Runs from the log dispatcher, which is not re-entrant and may be at raised
   TPL: no allocation, no logging, no waiting.

   @return characters written, excluding the NUL. */
size_t
axl_log_format_line(
    char        *buf,      ///< destination
    size_t       cap,      ///< capacity of @buf, including the NUL
    int          level,    ///< AXL_LOG_ERROR..AXL_LOG_TRACE
    const char  *domain,   ///< domain tag, or NULL to omit
    const char  *message,  ///< message text, or NULL to omit
    const AxlRealtime *stamp,  ///< record's instant (NULL = clock unavailable)
    const char  *eol       ///< line ending, e.g. "\n" or "\r\n"
    );

/* The console redirect behind axl_log_set_console_serial (axl-log-serial.c):
   while a sink is set, the engine (axl-log.c) hands it each finished console
   line -- CRLF-ended, @len bytes, NUL after -- instead of writing standard
   error. An indirection so that the engine, linked into every image that
   logs, references no serial code: only an image that redirects pulls it.
   NULL restores standard error. Runs from the dispatcher (not re-entrant,
   may be at raised TPL): no allocation, no logging, no waiting. */
typedef void (*AxlLogConsoleSink)(const char *line, size_t len);

void
_axl_log_set_console_sink(
    AxlLogConsoleSink sink   ///< the sink, or NULL for standard error
    );

#endif /* AXL_LOG_LINE_H */
