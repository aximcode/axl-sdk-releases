/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-watchdog.h
    Boot-services watchdog timer.

    UEFI starts every loaded image with a 5-minute boot-services
    watchdog armed (UEFI 2.11 §7.5). If the image hasn't called
    `Exit()` or disarmed the timer within that window the firmware
    resets the system. Long-running diagnostics get killed without
    warning unless they take action; this module is the typed
    wrapper for that.

    @code
    // Run diagnostics that may exceed 5 minutes.
    axl_watchdog_disarm();

    // Or extend the budget without disabling protection entirely.
    axl_watchdog_set(900);   // 15 minutes
    @endcode
**/

#ifndef AXL_WATCHDOG_H
#define AXL_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Disarm the boot-services watchdog.
 *
 * Equivalent to @c axl_watchdog_set(0). After this call, the
 * firmware will not reset the image for failing to make progress.
 *
 * @return 0 on success, -1 if the firmware refuses (rare).
 */
int
axl_watchdog_disarm(
    void
);

/**
 * @brief Arm or extend the boot-services watchdog.
 *
 * @p seconds == 0 disarms (same as axl_watchdog_disarm). Otherwise
 * the firmware resets the system after @p seconds elapse without
 * another call to this function or to Exit(). Each call replaces
 * the prior timeout.
 *
 * The internal "last set" value is remembered for axl_watchdog_pet.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_watchdog_set(
    uint64_t  seconds   ///< new timeout in seconds; 0 disarms
);

/**
 * @brief Re-arm the watchdog to the value last passed to axl_watchdog_set.
 *
 * Convenient for "feed the dog" loops where the same window is
 * extended periodically. If axl_watchdog_set has not been called
 * yet this session, _pet is a no-op and returns 0.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_watchdog_pet(
    void
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_WATCHDOG_H */
