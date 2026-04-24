/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-time.h:
 *
 * Time utilities: ISO 8601 formatting and a monotonic counter.
 *
 * Sleep and wait primitives live in <axl/axl-wait.h> -- that's
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_TIME_H */
