/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-time.h:
 *
 * Time utilities: ISO 8601 formatting and a monotonic counter.
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
// Monotonic counter
// ===================================================================

/**
 * @brief Get a monotonic millisecond counter.
 *
 * Based on firmware time — not wall-clock accurate but monotonically
 * increasing within a boot session. Useful for measuring elapsed time.
 *
 * @return milliseconds since an arbitrary epoch (typically boot).
 */
uint64_t
axl_time_get_ms(void);

/**
 * @brief Get a high-resolution monotonic microsecond counter.
 *
 * Reads the architecture's cycle counter (x86 TSC / aarch64
 * CNTPCT_EL0). The first call calibrates against a brief firmware
 * stall and returns 0; subsequent calls are cheap (one counter
 * read + a multiply) and return microseconds since the calibration
 * call. No defined relationship to wallclock time — pair with
 * axl_time_get_ms when you need both elapsed-microsecond
 * resolution and a wallclock anchor.
 *
 * Use this instead of axl_time_get_ms when the measurement
 * window is on the order of milliseconds or shorter (firmware
 * stall calibration, network round-trips, parser benchmarking).
 *
 * @return microseconds since the implicit calibration epoch.
 *     Returns 0 on the very first call (calibration tick) and on
 *     architectures with no usable cycle counter.
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
