/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-time.c
    Time utilities: ISO 8601 formatting and a monotonic counter.
    Sleep / wait primitives live in src/event/axl-wait.c.
**/

#include <axl/axl-time.h>
#include <axl/axl-log.h>
#include "../backend/axl-backend.h"

AXL_LOG_DOMAIN("time");

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

/**
 * Write a zero-padded unsigned integer into buf (exactly @width digits).
 * Returns number of characters written.
 */
static size_t
format_uint(char *buf, size_t buf_size, unsigned val, size_t width)
{
    if (width > buf_size) {
        return 0;
    }

    for (size_t i = width; i > 0; i--) {
        buf[i - 1] = '0' + (val % 10);
        val /= 10;
    }
    return width;
}

// ---------------------------------------------------------------------------
// Public API — formatting
// ---------------------------------------------------------------------------

size_t
axl_time_format(char *buf, size_t buf_size)
{
    AxlTime time;
    size_t pos = 0;
    unsigned usec;

    if (buf == NULL || buf_size < 28) {
        return 0;
    }

    if (axl_backend_get_time(&time) != AXL_OK) {
        /* Fallback: zero timestamp */
        const char *zero = "0000-00-00T00:00:00.000000";
        size_t i;
        for (i = 0; zero[i] != '\0' && i < buf_size - 1; i++) {
            buf[i] = zero[i];
        }
        buf[i] = '\0';
        return i;
    }

    /* Format: YYYY-MM-DDThh:mm:ss.uuuuuu */
    pos += format_uint(buf + pos, buf_size - pos, time.year, 4);
    buf[pos++] = '-';
    pos += format_uint(buf + pos, buf_size - pos, time.month, 2);
    buf[pos++] = '-';
    pos += format_uint(buf + pos, buf_size - pos, time.day, 2);
    buf[pos++] = 'T';
    pos += format_uint(buf + pos, buf_size - pos, time.hour, 2);
    buf[pos++] = ':';
    pos += format_uint(buf + pos, buf_size - pos, time.minute, 2);
    buf[pos++] = ':';
    pos += format_uint(buf + pos, buf_size - pos, time.second, 2);
    buf[pos++] = '.';
    usec = time.nanosecond / 1000;
    pos += format_uint(buf + pos, buf_size - pos, usec, 6);
    buf[pos] = '\0';

    return pos;
}

// ---------------------------------------------------------------------------
// Monotonic counter
// ---------------------------------------------------------------------------

uint64_t
axl_time_get_ms(void)
{
    AxlTime time;

    if (axl_backend_get_time(&time) != AXL_OK) {
        return 0;
    }

    /* Convert to ms since midnight — not truly monotonic across days
       but sufficient for elapsed-time measurement within a session. */
    uint64_t ms = 0;
    ms += (uint64_t)time.hour * 3600000;
    ms += (uint64_t)time.minute * 60000;
    ms += (uint64_t)time.second * 1000;
    ms += (uint64_t)(time.nanosecond / 1000000);
    return ms;
}

uint64_t
axl_time_get_us(void)
{
    /* Thin pass-through to the backend cycle-counter. Lives here
       (rather than as a backend stub) so consumer code reaches
       it through the public <axl/axl-time.h> surface without
       needing the internal backend header. */
    return axl_backend_get_monotonic_us();
}
