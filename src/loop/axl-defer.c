/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-defer.c
    Deferred work queue — ring buffer backed by AxlRingBuf, FIFO ordering.

    State is owned by AxlLoop (no global state). The main loop drains
    the queue at the start of each iteration.
**/

#include "axl-loop-internal.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("defer");

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

uint32_t
axl_defer(
    AxlLoop          *loop,
    AxlDeferCallback  fn,
    void             *data
    )
{
    DeferEntry entry;

    if (loop == NULL || fn == NULL) {
        return 0;
    }

    entry.fn        = fn;
    entry.data      = data;
    entry.id        = loop->defer_next_id;
    entry.cancelled = false;

    if (axl_ring_buf_push_elem(&loop->defer_ring, &entry) != AXL_OK)
    {
        axl_debug("defer queue full");
        return 0;
    }

    loop->defer_next_id++;
    if (loop->defer_next_id == 0) {
        loop->defer_next_id = 1;
    }

    return entry.id;
}

bool
axl_defer_cancel(
    AxlLoop *loop,
    uint32_t handle
    )
{
    uint32_t   count;
    uint32_t   i;
    DeferEntry entry;

    if (loop == NULL || handle == 0) {
        return false;
    }

    count = axl_ring_buf_get_length(&loop->defer_ring);
    for (i = 0; i < count; i++) {
        if (axl_ring_buf_peek_nth_elem(&loop->defer_ring, i, &entry) != AXL_OK) {
            continue;
        }

        if (entry.id == handle && !entry.cancelled) {
            entry.cancelled = true;
            axl_ring_buf_set_nth_elem(&loop->defer_ring, i, &entry);
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Internal — called by axl-loop.c
// ---------------------------------------------------------------------------

void
axl_defer_drain_internal(AxlLoop *loop)
{
    uint32_t   count;
    uint32_t   i;
    DeferEntry entry;

    if (loop == NULL) {
        return;
    }

    count = axl_ring_buf_get_length(&loop->defer_ring);
    for (i = 0; i < count; i++) {
        if (axl_ring_buf_pop_elem(&loop->defer_ring, &entry) != AXL_OK)
        {
            break;
        }

        if (!entry.cancelled && entry.fn != NULL) {
            _axl_loop_cb_enter();
            entry.fn(entry.data);
            _axl_loop_cb_leave();
        }
    }
}
