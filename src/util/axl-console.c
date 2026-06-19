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

// ===================================================================
// Text-console modes (SimpleTextOutput QueryMode / SetMode) — the
// graphics-free peer of the AxlGfx display-mode API. Thin policy over the
// backend's raw protocol access: bounds + signed-field guards, and the
// inventory-walking find / max helpers that skip QueryMode failures.
// ===================================================================

uint32_t
axl_console_text_mode_count(
    void
    )
{
    return axl_backend_console_text_mode_count();
}

int
axl_console_text_query_mode(
    uint32_t             index,
    AxlConsoleTextMode  *out
    )
{
    if (out == NULL || index >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    uint32_t cols = 0, rows = 0;
    if (axl_backend_console_text_query_mode(index, &cols, &rows) != AXL_OK) {
        return AXL_ERR;
    }
    out->index   = index;
    out->columns = cols;
    out->rows    = rows;
    return AXL_OK;
}

int
axl_console_text_current_mode(
    uint32_t  *out_index
    )
{
    if (out_index == NULL) {
        return AXL_ERR;
    }
    int cur = axl_backend_console_text_current_mode();
    /* -1 == no mode set; also reject a value the firmware reports outside
       the enumerable range (malformed). */
    if (cur < 0 || (uint32_t)cur >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    *out_index = (uint32_t)cur;
    return AXL_OK;
}

int
axl_console_text_find_mode(
    uint32_t   columns,
    uint32_t   rows,
    uint32_t  *out_index
    )
{
    if (out_index == NULL) {
        return AXL_ERR;
    }
    uint32_t n = axl_console_text_mode_count();
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        /* Skip modes whose QueryMode fails (a legal optional-mode reject). */
        if (axl_console_text_query_mode(i, &m) == AXL_OK
            && m.columns == columns && m.rows == rows) {
            *out_index = i;   /* lowest-numbered match (ascending walk) */
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

int
axl_console_text_max_mode(
    AxlConsoleTextMode  *out
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    uint32_t           n     = axl_console_text_mode_count();
    bool               found = false;
    AxlConsoleTextMode best  = { 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        AxlConsoleTextMode m;
        if (axl_console_text_query_mode(i, &m) != AXL_OK
            || m.columns == 0 || m.rows == 0) {
            continue;   /* skip QueryMode failures and degenerate geometry */
        }
        uint64_t area      = (uint64_t)m.columns * m.rows;
        uint64_t best_area = (uint64_t)best.columns * best.rows;
        /* Greatest area; ties -> more columns; full ties -> lowest index
           (strict comparisons + ascending walk keep the first such mode). */
        if (!found || area > best_area
            || (area == best_area && m.columns > best.columns)) {
            best  = m;
            found = true;
        }
    }
    if (!found) {
        return AXL_ERR;
    }
    *out = best;
    return AXL_OK;
}

int
axl_console_text_set_mode(
    uint32_t  index
    )
{
    /* Range-check before the firmware call (count == 0 -> always rejects). */
    if (index >= axl_console_text_mode_count()) {
        return AXL_ERR;
    }
    return axl_backend_console_text_set_mode(index);
}
