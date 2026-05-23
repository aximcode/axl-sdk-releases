/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-time.h:
 *
 * Time utilities: ISO 8601 formatting, a POSIX-style
 * `clock_gettime`, monotonic counters, and a wallclock reader.
 *
 * The core primitive is @ref axl_clock_gettime, modelled on the
 * Linux/POSIX call of the same name. Two clock IDs are supported:
 * @ref AXL_CLOCK_MONOTONIC (boot-relative, hardware cycle counter)
 * and @ref AXL_CLOCK_REALTIME (firmware RTC, Unix seconds). The
 * convenience helpers @ref axl_time_get_us and @ref axl_time_get_ms
 * are thin wrappers over @ref AXL_CLOCK_MONOTONIC.
 *
 * The monotonic clock's epoch is CPU power-on, so timestamps
 * captured in different UEFI processes within the same boot are
 * directly comparable for delta measurement — no per-process
 * calibration tick, no first-call sentinel.
 *
 * Sleep and wait primitives live in <axl/axl-wait.h> — that's
 * where to find axl_sleep / axl_msleep / axl_usleep (ergonomic
 * void-return sleep) and the axl_wait_* family (condvar-style
 * blocking with cancel + timeout).
 */

#ifndef AXL_TIME_H
#define AXL_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Format current time as ISO 8601 with microseconds.
 *
 * Example: "2026-03-27T14:05:32.123456"
 *
 * @return characters written (excluding NUL), 0 on error.
 */
size_t
axl_time_format(
    char   *buf,     ///< destination buffer (at least 28 bytes)
    size_t  buf_size ///< size of buffer
);

// ===================================================================
// POSIX-style clock_gettime
// ===================================================================

/**
 * @brief Clock identifier for @ref axl_clock_gettime /
 *     @ref axl_clock_getres.
 *
 * Modelled on the Linux/POSIX `clockid_t` values, restricted to the
 * two flavours that have a meaningful interpretation in UEFI.
 */
typedef enum {
    /// Boot-relative monotonic clock, backed by the architecture's
    /// cycle counter (x86_64: RDTSC; aarch64: CNTPCT_EL0). The
    /// epoch is CPU power-on (when the counter started), so values
    /// captured in different UEFI processes within the same boot are
    /// directly comparable for delta measurement. Not affected by
    /// any firmware-side time adjustments.
    AXL_CLOCK_MONOTONIC = 0,

    /// Wall-clock time read from the firmware real-time clock
    /// (`EFI_RUNTIME_SERVICES.GetTime`). Reported as seconds and
    /// nanoseconds since the Unix epoch (1970-01-01 00:00:00 UTC),
    /// with timezone applied if the firmware reports one. May be
    /// inaccurate or zero on systems without a battery-backed RTC.
    AXL_CLOCK_REALTIME  = 1,
} AxlClockId;

/**
 * @brief Time value with nanosecond precision.
 *
 * Layout-compatible with POSIX `struct timespec` field-wise (in
 * spirit — UEFI is freestanding C so we don't include `<time.h>`).
 * Consumers that interop with hosted-Linux code (cross-builds for
 * a Linux backend) can `memcpy` directly into a `struct timespec`
 * when sizes match.
 */
typedef struct {
    int64_t  tv_sec;    ///< whole seconds
    int32_t  tv_nsec;   ///< nanoseconds in [0, 999999999]
} AxlTimespec;

/**
 * @brief Read the current time from a clock.
 *
 * Single-entry API for both monotonic and wallclock readings:
 *
 * @code
 *     AxlTimespec t;
 *     axl_clock_gettime(AXL_CLOCK_MONOTONIC, &t);
 *     // t.tv_sec + t.tv_nsec since boot
 *
 *     AxlTimespec wall;
 *     axl_clock_gettime(AXL_CLOCK_REALTIME, &wall);
 *     // wall.tv_sec = Unix seconds; wall.tv_nsec = sub-second
 * @endcode
 *
 * Under @ref AXL_CLOCK_MONOTONIC AXL reads the architecture's cycle
 * counter and converts via a once-per-boot calibrated frequency
 * (cached across UEFI processes via a hidden boot-services protocol
 * — on x86 the first AXL process in a boot pays a 10 ms calibration
 * stall, subsequent processes pick the value up in microseconds).
 *
 * Under @ref AXL_CLOCK_REALTIME AXL calls the firmware RTC and
 * converts the calendar components to Unix seconds.
 *
 * @return AXL_OK on success, AXL_ERR if @p clockid is unknown,
 *     @p out is NULL, or the underlying hardware/firmware call
 *     fails.
 */
int
axl_clock_gettime(
    AxlClockId    clockid,   ///< clock to read
    AxlTimespec  *out        ///< [out] populated on success
);

/**
 * @brief Read the resolution of a clock.
 *
 * For @ref AXL_CLOCK_MONOTONIC the resolution is `1e9 / freq_hz`
 * nanoseconds (~0.3 ns on a 3 GHz TSC, ~42 ns on a 24 MHz aarch64
 * counter). For @ref AXL_CLOCK_REALTIME the resolution is reported
 * as 1 nanosecond when the firmware fills @c EFI_TIME.Nanosecond
 * and 1 second otherwise; UEFI RTCs typically advance once per
 * second so consumers should treat sub-second precision as
 * best-effort.
 *
 * @return AXL_OK on success, AXL_ERR on bad arguments.
 */
int
axl_clock_getres(
    AxlClockId    clockid,   ///< clock to query
    AxlTimespec  *out_res    ///< [out] resolution on success
);

// ===================================================================
// Convenience monotonic readers (wrappers over AXL_CLOCK_MONOTONIC)
// ===================================================================

/**
 * @brief Get a monotonic millisecond counter.
 *
 * Wrapper over @ref axl_clock_gettime with @ref AXL_CLOCK_MONOTONIC.
 * Convenient for code that measures elapsed time at millisecond
 * granularity and doesn't care about sub-millisecond detail.
 *
 * @return milliseconds since boot, or 0 on architectures with no
 *     usable cycle counter.
 */
uint64_t
axl_time_get_ms(void);

/**
 * @brief Get a high-resolution monotonic microsecond counter.
 *
 * Wrapper over @ref axl_clock_gettime with @ref AXL_CLOCK_MONOTONIC.
 * Each call reads the cycle counter directly — no per-process
 * calibration tick, no first-call sentinel. Two values captured in
 * different UEFI processes are directly comparable for delta
 * measurement within the same boot.
 *
 * @return microseconds since boot, or 0 on architectures with no
 *     usable cycle counter.
 */
uint64_t
axl_time_get_us(void);

// ===================================================================
// Wallclock (real time)
// ===================================================================

/// Sentinel for `AxlRealtime.timezone_minutes` when the firmware
/// does not report a timezone (corresponds to UEFI's
/// `EFI_UNSPECIFIED_TIMEZONE` = 2047).
#define AXL_TIME_TZ_UNSPECIFIED  INT16_MIN

/// Bit set in `AxlRealtime.flags` if the time is currently in DST.
#define AXL_TIME_FLAG_DAYLIGHT   0x01

/**
 * @brief Wallclock time snapshot as exposed by the firmware
 *     real-time clock.
 *
 * Layout-stable; the firmware-side `EFI_TIME` struct is private to
 * the SDK. Consumers see UTF-8-friendly snake_case field names and
 * a sentinel-based timezone encoding rather than UEFI's
 * `EFI_UNSPECIFIED_TIMEZONE` magic value.
 */
typedef struct {
    uint16_t year;              ///< full year (e.g. 2026)
    uint8_t  month;             ///< 1-12
    uint8_t  day;               ///< 1-31
    uint8_t  hour;              ///< 0-23
    uint8_t  minute;            ///< 0-59
    uint8_t  second;            ///< 0-60 (UEFI allows leap-second 60)
    uint8_t  flags;             ///< AXL_TIME_FLAG_* bits
    uint32_t nanosecond;        ///< 0-999,999,999
    int16_t  timezone_minutes;  ///< UTC offset, or AXL_TIME_TZ_UNSPECIFIED
} AxlRealtime;

/**
 * @brief Read the firmware real-time clock.
 *
 * Backend-neutral wrap of `EFI_RUNTIME_SERVICES.GetTime`. Allocation-
 * free; safe to call from CPU exception context. The returned
 * `AxlRealtime` carries the full GetTime payload (year/month/day/
 * hour/minute/second/nanosecond/timezone/dst) — consumers that
 * only need a packed timestamp pack the fields themselves.
 *
 * @return AXL_OK on success, AXL_ERR if the firmware reports a
 *     failure or @p out is NULL. On failure @p out is unmodified.
 */
int
axl_time_realtime(
    AxlRealtime *out
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TIME_H */
