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
// POSIX-style clock_gettime
// ---------------------------------------------------------------------------

int
axl_clock_gettime(AxlClockId clockid, AxlTimespec *out)
{
    return axl_backend_clock_gettime(clockid, out);
}

int
axl_clock_getres(AxlClockId clockid, AxlTimespec *out_res)
{
    if (out_res == NULL) {
        return AXL_ERR;
    }
    if (clockid != AXL_CLOCK_MONOTONIC && clockid != AXL_CLOCK_REALTIME) {
        return AXL_ERR;
    }
    /* Both clocks report 1 ns nominal resolution. The MONOTONIC
       counter is finer-grained in hardware (sub-ns on a 3 GHz TSC)
       but `struct timespec` is ns-quantized, so 1 ns is the
       smallest representable. The REALTIME RTC is typically
       second-granularity in firmware; the nanosecond field in
       EFI_TIME is best-effort. Consumers that need true measured
       resolution should benchmark via repeated gettime calls.

       Intentionally NO calibration here: clock_getres must be
       cheap and side-effect-free, so we don't probe the backend. */
    out_res->tv_sec  = 0;
    out_res->tv_nsec = 1;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Monotonic convenience wrappers
// ---------------------------------------------------------------------------

uint64_t
axl_time_get_ms(void)
{
    AxlTimespec ts;
    if (axl_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t
axl_time_get_us(void)
{
    AxlTimespec ts;
    if (axl_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int
axl_time_realtime(AxlRealtime *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    AxlTime t = { 0 };
    if (axl_backend_get_time(&t) != AXL_OK) {
        return AXL_ERR;
    }
    out->year             = t.year;
    out->month            = t.month;
    out->day              = t.day;
    out->hour             = t.hour;
    out->minute           = t.minute;
    out->second           = t.second;
    out->nanosecond       = t.nanosecond;
    out->timezone_minutes = t.timezone_minutes;
    out->flags            = (t.daylight & 0x01) ? AXL_TIME_FLAG_DAYLIGHT : 0;
    return AXL_OK;
}
