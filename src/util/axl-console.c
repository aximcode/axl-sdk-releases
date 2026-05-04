/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console.c
    Interactive console input wrapper around the backend's ConIn
    primitives. See @ref axl-console.h for the public contract.
**/

#include <axl/axl-console.h>
#include "../backend/axl-backend.h"

/* 100ns ticks per millisecond — matches EFI's TIMER_PERIODIC unit. */
#define HUNDRED_NS_PER_MS  10000ULL

int
axl_console_read_key(
    uint64_t   timeout_ms,
    AxlKey    *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }

    AxlEventHandle key_evt = axl_backend_console_wait_for_key();
    if (key_evt == NULL) {
        /* No console (driver app, raw firmware ctx, BDS hand-off
           that hasn't published ConIn yet). Caller can fall back
           to whatever degraded UI they want. */
        return AXL_ERR;
    }

    if (timeout_ms == 0) {
        /* Non-blocking: poll the event without waiting. */
        if (axl_backend_event_check(key_evt) != 0) {
            return AXL_ERR;
        }
    } else if (timeout_ms == UINT64_MAX) {
        /* Block forever on the key event alone. */
        size_t fired = 0;
        if (axl_backend_event_wait(1, &key_evt, &fired) != AXL_OK) {
            return AXL_ERR;
        }
    } else {
        /* Bounded wait: union of {key event, timer}. The timer event
           is closed unconditionally on return so a slow key path
           doesn't leak it. */
        AxlEventHandle timer = NULL;
        if (axl_backend_event_create_timer(&timer) != AXL_OK) {
            return AXL_ERR;
        }
        /* Cap the multiplication to avoid overflow when the caller
           passes a near-UINT64_MAX timeout; UEFI's interval field
           is uint64 so anything that overflows the * 10000 multiply
           is well past any reasonable wait. */
        uint64_t interval_100ns = (timeout_ms > UINT64_MAX / HUNDRED_NS_PER_MS)
                                      ? UINT64_MAX
                                      : timeout_ms * HUNDRED_NS_PER_MS;
        if (axl_backend_event_set_timer(timer, AXL_TIMER_RELATIVE,
                                        interval_100ns) != AXL_OK) {
            axl_backend_event_close(timer);
            return AXL_ERR;
        }
        AxlEventHandle events[2];
        events[0] = key_evt;
        events[1] = timer;
        size_t fired = 0;
        int rc = axl_backend_event_wait(2, events, &fired);
        axl_backend_event_close(timer);
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
        if (fired == 1) {
            /* Timer beat the key — timeout. */
            return AXL_ERR;
        }
    }

    /* Key is available. Read it via the backend (non-blocking;
       returns -1 only if the firmware lost it between event-fire
       and read, which shouldn't happen but mirrors the backend
       contract). */
    out->scan_code    = 0;
    out->unicode_char = 0;
    return axl_backend_console_read_key(&out->scan_code,
                                        &out->unicode_char);
}

void
axl_console_flush_input(
    void
    )
{
    /* Drain ConIn until ReadKeyStroke reports nothing left. The
       backend returns -1 when the queue is empty, so this loop
       terminates after the firmware has handed back every buffered
       keystroke. NULL-safe: backend's ConIn check inside
       read_key short-circuits when the protocol isn't published. */
    uint16_t scan = 0;
    uint16_t uni  = 0;
    while (axl_backend_console_read_key(&scan, &uni) == AXL_OK) {
        /* discard */
    }
}
