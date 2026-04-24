/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-task-pool.c
    Persistent AP worker pool with lock-free task dispatch.
    Single-core systems get transparent synchronous fallback.

    Migrated from AxlTaskPool.c(EDK2-style) to GLib-style API.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-task.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("task");

static inline void
cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct {
    volatile uintptr_t   task;         /* AxlTaskProc cast to uintptr_t(0 = idle) */
    void                *arg;
    AxlArena            *arena;
    AxlTaskComplete      on_complete;
    volatile uint32_t    done;        /* 1 = task complete */
    volatile uint32_t    quit;        /* 1 = shutdown */
    volatile uint32_t    exited;      /* 1 = worker has exited */
    AxlTaskId            id;
    size_t               ap_number;   /* processor number for this worker */
} WorkerSlot;

// ---------------------------------------------------------------------------
// Pool struct
// ---------------------------------------------------------------------------

struct AxlTaskPool {
    AxlMpContext  *mp_ctx;
    WorkerSlot    *slots;
    size_t         worker_count;
    uint32_t       next_task_id;
    bool           single_core;
};

// ---------------------------------------------------------------------------
// AP worker loop
// ---------------------------------------------------------------------------

static
void
EFIAPI
worker_proc(
    void *arg
    )
{
    WorkerSlot  *slot;
    AxlTaskProc  proc;

    slot = (WorkerSlot *)arg;

    while (!slot->quit) {
        if (slot->task != 0) {
            proc = (AxlTaskProc)(uintptr_t)slot->task;
            slot->task = 0;
            __sync_synchronize();          /* acquire -- ensure arg/arena visible */
            proc(slot->arg, slot->arena);
            __sync_synchronize();          /* release -- ensure task results visible */
            slot->done = 1;
        }
        cpu_pause();
    }

    slot->exited = 1;               /* signal that this worker has exited */
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlTaskPool *
axl_task_pool_new(void)
{
    AxlTaskPool  *pool;
    size_t        i;
    size_t        launched;
    size_t        ap_count;

    pool = axl_calloc(1, sizeof(AxlTaskPool));
    if (pool == NULL) {
        return NULL;
    }

    pool->next_task_id = 1;

    pool->mp_ctx = axl_backend_mp_init(&ap_count);
    if (pool->mp_ctx == NULL) {
        axl_info("MP Services not available -- single-core fallback");
        pool->single_core = true;
        return pool;
    }

    pool->slots = axl_calloc(ap_count, sizeof(WorkerSlot));
    if (pool->slots == NULL) {
        axl_error("failed to allocate %llu worker slots",
                 (unsigned long long)ap_count);
        axl_backend_mp_cleanup(pool->mp_ctx);
        pool->mp_ctx = NULL;
        pool->single_core = true;
        return pool;
    }

    /* Dispatch each AP into its persistent worker loop (non-blocking).
       Compact slots on failure so slots[0..launched-1] are all live. */
    launched = 0;
    for (i = 0; i < ap_count; i++) {
        pool->slots[launched].ap_number =
            axl_backend_mp_get_ap_number(pool->mp_ctx, i);

        if (axl_backend_mp_start_ap(pool->mp_ctx, i, worker_proc,
                                    &pool->slots[launched]) != 0) {
            axl_warning("failed to start AP #%llu",
                       (unsigned long long)pool->slots[launched].ap_number);
            continue;
        }
        launched++;
    }

    pool->worker_count = launched;
    if (pool->worker_count == 0) {
        axl_info("no APs launched -- single-core fallback");
        axl_free(pool->slots);
        pool->slots = NULL;
        axl_backend_mp_cleanup(pool->mp_ctx);
        pool->mp_ctx = NULL;
        pool->single_core = true;
        return pool;
    }

    axl_info("%llu AP workers running", (unsigned long long)pool->worker_count);
    return pool;
}

void
axl_task_pool_free(
    AxlTaskPool *pool
    )
{
    size_t  i;
    bool    all_exited;

    if (pool == NULL) {
        return;
    }

    if (!pool->single_core && pool->slots != NULL) {
        /* Signal all workers to quit with memory fence */
        for (i = 0; i < pool->worker_count; i++) {
            pool->slots[i].quit = 1;
        }
        __sync_synchronize();

        /* Spin until all workers have acknowledged exit */
        all_exited = false;
        for (size_t timeout = 0; timeout < 1000000; timeout++) {
            all_exited = true;
            for (i = 0; i < pool->worker_count; i++) {
                if (!pool->slots[i].exited) {
                    all_exited = false;
                    break;
                }
            }
            if (all_exited) {
                break;
            }
            cpu_pause();
        }

        if (!all_exited) {
            axl_warning("not all AP workers exited cleanly");
        }
    }

    axl_free(pool->slots);

    if (pool->mp_ctx != NULL) {
        axl_backend_mp_cleanup(pool->mp_ctx);
    }

    axl_free(pool);
}

AxlTaskId
axl_task_pool_submit(
    AxlTaskPool    *pool,
    AxlTaskProc     proc,
    void           *arg,
    AxlArena       *arena,
    AxlTaskComplete on_complete
    )
{
    AxlTaskId  id;
    size_t     i;

    if (pool == NULL || proc == NULL) {
        return AXL_TASK_ID_INVALID;
    }

    id = pool->next_task_id++;
    if (id == AXL_TASK_ID_INVALID) {
        id = pool->next_task_id++;
    }

    /* Single-core fallback: run synchronously */
    if (pool->single_core || pool->slots == NULL) {
        proc(arg, arena);
        if (on_complete != NULL) {
            on_complete(arg, arena);
        }
        return id;
    }

    /* Find idle worker */
    for (i = 0; i < pool->worker_count; i++) {
        if (pool->slots[i].task == 0 && pool->slots[i].done == 0) {
            pool->slots[i].arg = arg;
            pool->slots[i].arena = arena;
            pool->slots[i].on_complete = on_complete;
            pool->slots[i].id = id;
            __sync_synchronize();          /* release -- ensure fields visible before task */
            pool->slots[i].task = (uintptr_t)proc;
            return id;
        }
    }

    axl_warning("all %llu workers busy, cannot submit task",
               (unsigned long long)pool->worker_count);
    return AXL_TASK_ID_INVALID;
}

size_t
axl_task_pool_poll(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  completed;

    if (pool == NULL || pool->single_core || pool->slots == NULL) {
        return 0;
    }

    completed = 0;
    for (i = 0; i < pool->worker_count; i++) {
        if (pool->slots[i].done) {
            __sync_synchronize();          /* acquire -- ensure task results visible */
            if (pool->slots[i].on_complete != NULL) {
                pool->slots[i].on_complete(pool->slots[i].arg,
                                           pool->slots[i].arena);
            }
            pool->slots[i].on_complete = NULL;
            pool->slots[i].done = 0;
            completed++;
        }
    }

    return completed;
}

bool
axl_task_pool_done(
    AxlTaskPool *pool,
    AxlTaskId    id
    )
{
    size_t  i;

    if (pool == NULL || id == AXL_TASK_ID_INVALID || pool->single_core) {
        return true;
    }

    if (pool->slots == NULL) {
        return true;
    }

    for (i = 0; i < pool->worker_count; i++) {
        if (pool->slots[i].id == id) {
            return pool->slots[i].done != 0 || pool->slots[i].task == 0;
        }
    }

    return true;
}

size_t
axl_task_pool_available(
    AxlTaskPool *pool
    )
{
    size_t  i;
    size_t  count;

    if (pool == NULL || pool->single_core || pool->slots == NULL) {
        return 0;
    }

    count = 0;
    for (i = 0; i < pool->worker_count; i++) {
        if (pool->slots[i].task == 0 && pool->slots[i].done == 0) {
            count++;
        }
    }

    return count;
}

size_t
axl_task_pool_worker_count(
    AxlTaskPool *pool
    )
{
    if (pool == NULL) {
        return 0;
    }
    return pool->worker_count;
}

bool
axl_task_pool_is_single_core(
    AxlTaskPool *pool
    )
{
    if (pool == NULL) {
        return true;
    }
    return pool->single_core;
}
