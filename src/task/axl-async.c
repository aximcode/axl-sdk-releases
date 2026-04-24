/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-async.c
    AP-offloaded async work queue — bridges AxlTask with AxlLoop.

    Thin wrapper around axl_task_pool_submit.  An idle source polls
    for AP completion and fires done callbacks on the BSP.  Single-core
    fallback runs work synchronously (same API).
**/

#include <axl/axl-async.h>
#include <axl/axl-loop.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("async");

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct {
    AxlAsync        *async;      /* back-pointer for completion wrapper */
    AxlAsyncHandle   handle;
    AxlTaskId        task_id;
    AxlTaskProc      work_fn;
    AxlTaskComplete  done_cb;
    void            *data;
    bool             active;
    bool             cancelled;
} AsyncSlot;

// ---------------------------------------------------------------------------
// Async struct
// ---------------------------------------------------------------------------

struct AxlAsync {
    AxlTaskPool   *pool;
    AxlLoop       *loop;
    AsyncSlot     *slots;
    size_t         max_slots;
    size_t         pending_count;
    uint32_t       next_handle;
    uint32_t       idle_source_id;
};

// ---------------------------------------------------------------------------
// Internal: work + completion wrappers
// ---------------------------------------------------------------------------

static void
async_work_wrapper(
    void     *arg,
    AxlArena *arena
    )
{
    AsyncSlot *slot = (AsyncSlot *)arg;

    slot->work_fn(slot->data, arena);
}

static void
async_complete_wrapper(
    void     *arg,
    AxlArena *arena
    )
{
    AsyncSlot *slot = (AsyncSlot *)arg;
    AxlAsync  *async = slot->async;

    if (!slot->cancelled && slot->done_cb != NULL) {
        slot->done_cb(slot->data, arena);
    }

    slot->active = false;
    if (async->pending_count > 0) {
        async->pending_count--;
    } else {
        axl_warning("async complete with pending_count already 0");
    }
}

// ---------------------------------------------------------------------------
// Internal: idle source for polling AP completion
// ---------------------------------------------------------------------------

static bool
async_idle_poll(void *data)
{
    AxlAsync *async = (AxlAsync *)data;

    axl_task_pool_poll(async->pool);

    if (async->pending_count == 0) {
        async->idle_source_id = 0;
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

static void
ensure_idle_source(AxlAsync *async)
{
    if (async->idle_source_id != 0) {
        return;
    }
    async->idle_source_id = axl_loop_add_idle(async->loop,
                                              async_idle_poll, async);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlAsync *
axl_async_new(
    AxlLoop *loop,
    size_t   max_pending
    )
{
    AxlAsync *async;

    if (loop == NULL) {
        return NULL;
    }

    if (max_pending == 0) {
        max_pending = 4;
    }

    async = axl_calloc(1, sizeof(AxlAsync));
    if (async == NULL) {
        return NULL;
    }

    async->slots = axl_calloc(max_pending, sizeof(AsyncSlot));
    if (async->slots == NULL) {
        axl_free(async);
        return NULL;
    }

    async->max_slots = max_pending;
    async->next_handle = 1;
    async->loop = loop;

    async->pool = axl_task_pool_new();
    if (async->pool == NULL) {
        axl_free(async->slots);
        axl_free(async);
        return NULL;
    }

    return async;
}

void
axl_async_free(
    AxlAsync *async
    )
{
    if (async == NULL) {
        return;
    }

    if (async->idle_source_id != 0) {
        axl_loop_remove_source(async->loop, async->idle_source_id);
    }

    /* Drain remaining completions */
    axl_task_pool_poll(async->pool);

    axl_free(async->slots);
    axl_task_pool_free(async->pool);
    axl_free(async);
}

AxlAsyncHandle
axl_async_submit(
    AxlAsync        *async,
    AxlTaskProc      work_fn,
    void            *data,
    AxlArena        *arena,
    AxlTaskComplete  done_cb
    )
{
    AsyncSlot *slot;
    AxlTaskId  tid;
    size_t     i;

    if (async == NULL || work_fn == NULL) {
        return AXL_ASYNC_INVALID;
    }

    /* Find free slot */
    slot = NULL;
    for (i = 0; i < async->max_slots; i++) {
        if (!async->slots[i].active) {
            slot = &async->slots[i];
            break;
        }
    }

    if (slot == NULL) {
        axl_warning("async queue full (%zu pending)", async->pending_count);
        return AXL_ASYNC_INVALID;
    }

    /* Fill slot */
    slot->async     = async;
    slot->handle    = async->next_handle++;
    if (slot->handle == AXL_ASYNC_INVALID) {
        slot->handle = async->next_handle++;
    }
    slot->work_fn   = work_fn;
    slot->done_cb   = done_cb;
    slot->data      = data;
    slot->active    = true;
    slot->cancelled = false;

    /* Increment before submit — single-core path runs complete_wrapper
       synchronously during submit, which decrements pending_count */
    async->pending_count++;

    tid = axl_task_pool_submit(async->pool, async_work_wrapper, slot, arena,
                               async_complete_wrapper);

    if (tid == AXL_TASK_ID_INVALID) {
        /* All AP workers busy (multi-core only — single-core always succeeds) */
        slot->active = false;
        async->pending_count--;
        return AXL_ASYNC_INVALID;
    }

    slot->task_id = tid;

    /* Install idle source to poll for completion (no-op if single-core
       already completed synchronously — pending_count is back to 0) */
    if (async->pending_count > 0) {
        ensure_idle_source(async);
    }

    return slot->handle;
}

bool
axl_async_cancel(
    AxlAsync       *async,
    AxlAsyncHandle  handle
    )
{
    size_t i;

    if (async == NULL || handle == AXL_ASYNC_INVALID) {
        return false;
    }

    for (i = 0; i < async->max_slots; i++) {
        if (async->slots[i].active && async->slots[i].handle == handle) {
            async->slots[i].cancelled = true;
            return true;
        }
    }

    return false;
}

size_t
axl_async_pending(
    AxlAsync *async
    )
{
    if (async == NULL) {
        return 0;
    }
    return async->pending_count;
}
