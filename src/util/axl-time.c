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

/**
 * Inverse of the backend's days_from_civil (Howard Hinnant's
 * civil_from_days, public domain): convert a count of days since the
 * Unix epoch (1970-01-01) into a Gregorian year/month/day. Valid for
 * the full proleptic Gregorian range; axl_time_set_unix only feeds it
 * the post-epoch range it pre-validates.
 */
static void
civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era  = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                  // [0, 146096]
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);       // [0, 365]
    unsigned mp  = (5 * doy + 2) / 153;                           // [0, 11]
    *d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
    *m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
    *y = (int)((int64_t)yoe + era * 400 + (*m <= 2));
}

// ---------------------------------------------------------------------------
// Public API — formatting
// ---------------------------------------------------------------------------

/* Shared renderer. Takes the time as a VALUE so the log dispatcher can stamp
   a record once and have every sink render that same instant -- see
   axl_time_format_at. */
static size_t
format_realtime(const AxlRealtime *t, char *buf, size_t buf_size)
{
    size_t   pos = 0;
    unsigned usec;

    /* Format: YYYY-MM-DDThh:mm:ss.uuuuuu */
    pos += format_uint(buf + pos, buf_size - pos, t->year, 4);
    buf[pos++] = '-';
    pos += format_uint(buf + pos, buf_size - pos, t->month, 2);
    buf[pos++] = '-';
    pos += format_uint(buf + pos, buf_size - pos, t->day, 2);
    buf[pos++] = 'T';
    pos += format_uint(buf + pos, buf_size - pos, t->hour, 2);
    buf[pos++] = ':';
    pos += format_uint(buf + pos, buf_size - pos, t->minute, 2);
    buf[pos++] = ':';
    pos += format_uint(buf + pos, buf_size - pos, t->second, 2);
    buf[pos++] = '.';
    /* Prefer EFI_TIME.Nanosecond, but firmware leaves it 0 on every platform
       we test on, which stamps .000000 on every line and makes lines within
       one second impossible to order. Fall back to the monotonic counter.
       CAVEAT: the fraction then comes from an unrelated epoch, so it can
       appear to run backwards inside one wallclock second -- it conveys
       precision, NOT ordering. */
    usec = t->nanosecond / 1000;
    /* Trigger on the firmware reporting NOTHING, not on the microseconds
       rounding to zero: a genuine 0 < nanosecond < 1000 is a real reading and
       keeping it beats substituting an unrelated epoch's fraction. */
    if (t->nanosecond == 0) {
        uint64_t mono = axl_backend_get_monotonic_us();
        if (mono > 0) {
            usec = (unsigned)(mono % 1000000u);
        }
    }
    pos += format_uint(buf + pos, buf_size - pos, usec, 6);
    buf[pos] = '\0';
    return pos;
}

/* The "clock unavailable" rendering, kept fixed-width so a transcript stays
   columnar rather than shifting every following field. */
static size_t
format_unavailable(char *buf, size_t buf_size)
{
    const char *zero = "0000-00-00T00:00:00.000000";
    size_t i;
    for (i = 0; zero[i] != '\0' && i < buf_size - 1; i++) {
        buf[i] = zero[i];
    }
    buf[i] = '\0';
    return i;
}

size_t
axl_time_format_at(const AxlRealtime *t, char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 28) {
        return 0;
    }
    if (t == NULL) {
        return format_unavailable(buf, buf_size);
    }
    return format_realtime(t, buf, buf_size);
}

size_t
axl_time_format(char *buf, size_t buf_size)
{
    AxlRealtime now;

    if (buf == NULL || buf_size < 28) {
        return 0;
    }
    if (axl_time_realtime(&now) != AXL_OK) {
        return format_unavailable(buf, buf_size);
    }
    return format_realtime(&now, buf, buf_size);
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

int
axl_time_set_realtime(const AxlRealtime *in)
{
    if (in == NULL) {
        return AXL_ERR;
    }
    AxlTime t = { 0 };
    t.year             = in->year;
    t.month            = in->month;
    t.day              = in->day;
    t.hour             = in->hour;
    t.minute           = in->minute;
    t.second           = in->second;
    t.nanosecond       = in->nanosecond;
    t.timezone_minutes = in->timezone_minutes;
    t.daylight         = (in->flags & AXL_TIME_FLAG_DAYLIGHT) ? 1 : 0;
    return axl_backend_set_time(&t);
}

int
axl_time_set_unix(int64_t unix_secs)
{
    /* Reject pre-epoch and beyond-year-9999 up front — the calendar
       year is a 16-bit field, so an out-of-range input is a caller
       error rather than something to silently wrap. 253402300800 ==
       10000-01-01T00:00:00Z. */
    if (unix_secs < 0 || unix_secs >= 253402300800LL) {
        return AXL_ERR;
    }

    /* unix_secs >= 0, so both quotient and remainder are non-negative
       (no implementation-defined negative-modulo concern). */
    int64_t days = unix_secs / 86400;
    int64_t rem  = unix_secs % 86400;

    int      y;
    unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);

    AxlRealtime rt = { 0 };
    rt.year             = (uint16_t)y;
    rt.month            = (uint8_t)mo;
    rt.day              = (uint8_t)d;
    rt.hour             = (uint8_t)(rem / 3600);
    rt.minute           = (uint8_t)((rem % 3600) / 60);
    rt.second           = (uint8_t)(rem % 60);
    rt.nanosecond       = 0;
    rt.flags            = 0;
    rt.timezone_minutes = 0;  /* RTC ends up holding UTC */
    return axl_time_set_realtime(&rt);
}

int
axl_time_get_wakeup(bool *enabled, bool *pending, AxlRealtime *when)
{
    AxlTime t = { 0 };
    int rc = axl_backend_get_wakeup(enabled, pending,
                                    (when != NULL) ? &t : NULL);
    if (rc != AXL_OK || when == NULL) {
        return rc;
    }
    when->year             = t.year;
    when->month            = t.month;
    when->day              = t.day;
    when->hour             = t.hour;
    when->minute           = t.minute;
    when->second           = t.second;
    when->nanosecond       = t.nanosecond;
    when->timezone_minutes = t.timezone_minutes;
    when->flags            = t.daylight ? AXL_TIME_FLAG_DAYLIGHT : 0;
    return AXL_OK;
}

int
axl_time_set_wakeup(const AxlRealtime *when)
{
    if (when == NULL) {
        return axl_backend_set_wakeup(NULL);   /* disarm */
    }
    AxlTime t = { 0 };
    t.year             = when->year;
    t.month            = when->month;
    t.day              = when->day;
    t.hour             = when->hour;
    t.minute           = when->minute;
    t.second           = when->second;
    t.nanosecond       = when->nanosecond;
    t.timezone_minutes = when->timezone_minutes;
    t.daylight         = (when->flags & AXL_TIME_FLAG_DAYLIGHT) ? 1 : 0;
    return axl_backend_set_wakeup(&t);
}
