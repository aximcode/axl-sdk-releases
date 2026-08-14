/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-watchdog.c
    Boot-services watchdog wrapper.

    Single-threaded; the static last_seconds is only consulted by
    axl_watchdog_pet, called from the same thread that armed it.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-watchdog.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("watchdog");

static uint64_t  last_seconds = 0;

int
axl_watchdog_disarm(
    void
    )
{
    return axl_watchdog_set(0);
}

int
axl_watchdog_set(
    uint64_t  seconds
    )
{
    EFI_STATUS status = axl_bs()->SetWatchdogTimer(
        (UINTN)seconds,
        0,        /* WatchdogCode — 0 means "no diagnostic context" */
        0,        /* DataSize */
        NULL);    /* WatchdogData */
    if (EFI_ERROR(status)) {
        axl_debug("SetWatchdogTimer(%llu) failed: 0x%llx",
                  (unsigned long long)seconds,
                  (unsigned long long)status);
        return AXL_ERR;
    }
    last_seconds = seconds;
    return AXL_OK;
}

int
axl_watchdog_pet(
    void
    )
{
    if (last_seconds == 0) {
        /* Nothing armed yet — pet is a no-op. The watchdog is
           either disarmed or still on the firmware-default
           5-minute boot timer; in the latter case the consumer
           needs to call axl_watchdog_set explicitly to take
           ownership. */
        return AXL_OK;
    }
    return axl_watchdog_set(last_seconds);
}
