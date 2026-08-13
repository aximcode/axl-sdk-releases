/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-line.c
    The shared formatted-log-line builder (see axl-log-line.h).

    Kept in its own translation unit so a consumer that only attaches the
    in-memory ring sink does not link it (libaxl.a is selectively linked).
**/

#include <stddef.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include <axl/axl-time.h>
#include "axl-log-line.h"

static const char *mLevelTag[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
    "TRACE"
};

/* Timestamp plus its trailing space. axl_time_format_at supplies the
   sub-second fallback that used to live here privately -- see its
   docstring for why the fraction is precision, not an ordering key. */
static size_t
append_timestamp(char *buf, size_t cap, const AxlRealtime *stamp)
{
    char ts[32];
    /* No local fallback: axl_time_format_at renders the fixed-width
       "clock unavailable" placeholder itself for a NULL stamp, so a
       transcript stays columnar either way. With a 32-byte non-NULL buffer it
       cannot return 0, and duplicating the placeholder string here would just
       be a second copy to keep in sync. */
    size_t n = axl_time_format_at(stamp, ts, sizeof(ts));
    if (n + 1 >= cap) {
        return 0;
    }
    axl_memcpy(buf, ts, n);
    buf[n++] = ' ';
    return n;
}

size_t
axl_log_format_line(
    char        *buf,
    size_t       cap,
    int          level,
    const char  *domain,
    const char  *message,
    const AxlRealtime *stamp,
    const char  *eol
    )
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    if (eol == NULL) {
        eol = "\n";
    }
    const size_t eol_len = axl_strlen(eol);
    /* Every field below is written only if the line ending and the NUL still
       fit after it, so the caller always gets a terminated, complete record. */
    if (cap <= eol_len) {
        buf[0] = '\0';
        return 0;
    }
    const size_t limit = cap - eol_len - 1;   /* room reserved for eol + NUL */
    size_t pos = append_timestamp(buf, limit + 1, stamp);

    if (level >= 0 && level <= AXL_LOG_TRACE) {
        const char *tag = mLevelTag[level];
        size_t tag_len = axl_strlen(tag);
        if (pos + tag_len + 3 <= limit) {
            buf[pos++] = '[';
            axl_memcpy(buf + pos, tag, tag_len);
            pos += tag_len;
            buf[pos++] = ']';
            buf[pos++] = ' ';
        }
    }

    if (domain != NULL) {
        size_t dlen = axl_strlen(domain);
        /* STRICTLY less: leaves at least one byte for the message, so a
           record can never degenerate into a domain-only line. This is also
           exactly the bound the file sink used before the extraction
           (`pos + dlen + 2 < sizeof(line_buf) - 2`), which keeps the refactor
           byte-for-byte. */
        if (pos + dlen + 2 < limit) {
            axl_memcpy(buf + pos, domain, dlen);
            pos += dlen;
            buf[pos++] = ':';
            buf[pos++] = ' ';
        }
    }

    if (message != NULL) {
        size_t mlen = axl_strlen(message);
        if (mlen > limit - pos) {
            mlen = limit - pos;   /* truncate, never drop */
        }
        axl_memcpy(buf + pos, message, mlen);
        pos += mlen;
    }

    axl_memcpy(buf + pos, eol, eol_len);
    pos += eol_len;
    buf[pos] = '\0';
    return pos;
}
