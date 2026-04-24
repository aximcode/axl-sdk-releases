/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-task.h:
 *
 * AP worker pool and region-based arena allocator.
 * The arena provides lock-free bump allocation suitable for AP use.
 * The task pool dispatches work to Application Processors (APs).
 */

#ifndef AXL_TASK_H
#define AXL_TASK_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Arena allocator
// ---------------------------------------------------------------------------

typedef struct AxlArena AxlArena;

/**
 * @brief Create a new arena with fixed capacity. BSP-only.
 *
 * All memory is zeroed.
 *
 * @return arena handle, or NULL on failure.
 */
AxlArena *
axl_arena_new_impl(
    size_t      capacity,  ///< total bytes available
    const char *file,      ///< caller file (via macro)
    int         line       ///< caller line (via macro)
);

/**
 * Captures the caller's file/line for leak reporting via the tier-1
 * resource registry. See docs/AXL-Runtime.md §4.2.1.
 */
#define axl_arena_new(capacity) \
    axl_arena_new_impl((capacity), __FILE__, __LINE__)

/**
 * @brief Free backing memory. BSP-only.
 */
void
axl_arena_free(
    AxlArena *arena  ///< arena to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlArena, axl_arena_free)
#endif

/**
 * @brief Allocate from arena. AP-safe (lock-free CAS).
 *
 * Returns zeroed, 8-byte-aligned memory. Returns NULL on exhaustion.
 */
void *
axl_arena_alloc(
    AxlArena *arena, ///< arena
    size_t    size   ///< bytes to allocate
);

/**
 * @brief Reset arena, freeing all allocations.
 *
 * Backing memory retained and zeroed. BSP-only.
 */
void
axl_arena_reset(
    AxlArena *arena  ///< arena to reset
);

/**
 * @brief Get bytes remaining in the arena.
 *
 * @return bytes remaining.
 */
size_t
axl_arena_remaining(
    AxlArena *arena  ///< arena to query
);

/**
 * @brief Get total capacity of the arena.
 *
 * @return total capacity in bytes.
 */
size_t
axl_arena_capacity(
    AxlArena *arena  ///< arena to query
);

// ---------------------------------------------------------------------------
// Task pool
// ---------------------------------------------------------------------------

typedef struct AxlTaskPool AxlTaskPool;
typedef uint32_t AxlTaskId;
#define AXL_TASK_ID_INVALID  0

/**
 * AxlTaskProc:
 *
 * Task procedure — runs on an AP. Must be AP-safe: no Boot Services,
 * no protocol calls, no axl_print. Use the arena for memory.
 */
typedef void (*AxlTaskProc)(
    void     *arg,  ///< caller-provided argument
    AxlArena *arena ///< arena for AP-safe allocations (may be NULL)
);

/**
 * AxlTaskComplete:
 *
 * Completion callback — runs on BSP during axl_task_pool_poll.
 */
typedef void (*AxlTaskComplete)(
    void     *arg,  ///< same argument from submit
    AxlArena *arena ///< arena used by the task
);

/**
 * @brief Create a new task pool.
 *
 * Locates MP Services and dispatches APs.
 *
 * @return pool handle, or NULL on error.
 */
AxlTaskPool *
axl_task_pool_new(void);

/**
 * @brief Free the task pool. Signals all workers to exit.
 */
void
axl_task_pool_free(
    AxlTaskPool *pool  ///< pool to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTaskPool, axl_task_pool_free)
#endif

/**
 * @brief Submit a task. Non-blocking.
 *
 * On single-core, runs synchronously.
 *
 * @return task ID, or AXL_TASK_ID_INVALID if all workers busy.
 */
AxlTaskId
axl_task_pool_submit(
    AxlTaskPool    *pool,        ///< task pool
    AxlTaskProc     proc,        ///< task procedure (AP-safe)
    void           *arg,         ///< argument passed to proc and on_complete
    AxlArena       *arena,       ///< arena for task allocations (NULL = no arena)
    AxlTaskComplete on_complete  ///< BSP callback when done (NULL = fire-and-forget)
);

/**
 * @brief Poll for completed tasks. Call once per event loop iteration.
 *
 * @return number of tasks completed this poll cycle.
 */
size_t
axl_task_pool_poll(
    AxlTaskPool *pool  ///< task pool
);

/**
 * @brief Check if a task is done.
 *
 * @return true if task is done (or ID is invalid).
 */
bool
axl_task_pool_done(
    AxlTaskPool *pool,  ///< task pool
    AxlTaskId    id     ///< task ID
);

/**
 * @brief Get idle worker count.
 *
 * @return number of idle workers.
 */
size_t
axl_task_pool_available(
    AxlTaskPool *pool  ///< task pool
);

/**
 * @brief Get total worker count.
 *
 * @return total worker count (0 on single-core).
 */
size_t
axl_task_pool_worker_count(
    AxlTaskPool *pool  ///< task pool
);

/**
 * @brief Check if the pool is in single-core fallback mode.
 *
 * @return true if single-core (no APs available).
 */
bool
axl_task_pool_is_single_core(
    AxlTaskPool *pool  ///< task pool
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TASK_H */
