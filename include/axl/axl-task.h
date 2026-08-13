/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-task.h
 *
 * AP worker pool and region-based arena allocator.
 * The arena provides lock-free bump allocation suitable for AP use.
 * The task pool dispatches work to Application Processors (APs).
 */

#ifndef AXL_TASK_H
#define AXL_TASK_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

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
 * resource registry. See docs/AXL-Lifecycle.md §4.2.1.
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
 * Ceiling on workers running concurrently across every live pool.
 *
 * Worker bookkeeping lives in fixed storage rather than the heap,
 * because the firmware may deliver a dispatched AP into its worker
 * long after the pool that dispatched it was freed — so that memory
 * can never be handed back to an allocator. A machine with more
 * enabled APs than this uses the first `AXL_TASK_MAX_WORKERS` of them.
 *
 * A worker whose AP never acknowledges exit permanently consumes its
 * entry, since nothing can rule out a later write. That is bounded by
 * the machine's AP count and is warned about when it happens.
 */
#define AXL_TASK_MAX_WORKERS  256

/**
 * AxlTaskProc:
 *
 * Task procedure — runs on an AP. Must be AP-safe: no Boot Services,
 * no protocol calls, no axl_print. Use the arena for memory.
 *
 * SIMD state is per-core. `CR4.OSXSAVE` and `XCR0` are not shared
 * between processors, so AVX enabled on the BSP is *not* enabled here:
 * a task that executes AVX without first calling `axl_cpu_enable_avx`
 * on this core takes a \#UD. Call it at the top of the task — it is
 * idempotent and reads live hardware state, so the cost after the first
 * task on a given core is a register read.
 *
 * The enable is left to the task rather than done by the dispatcher on
 * every core, because most tasks never execute vector code and the
 * dispatcher has no way to know which do.
 *
 * `axl_cpu_features` reports the machine, not the calling core. On a
 * hybrid part the two differ; `axl_cpu_enable_avx` and
 * `axl_cpu_enable_avx512` always answer for the core they run on, so
 * branch on their return value rather than on the feature bits.
 */
typedef void (*AxlTaskProc)(
    AxlArena *arena, ///< arena for AP-safe allocations (may be NULL)
    void     *arg    ///< caller-provided argument
) AXL_CB_NOEXCEPT;

/**
 * AxlTaskComplete:
 *
 * Completion callback — runs on BSP during axl_task_pool_poll.
 */
typedef void (*AxlTaskComplete)(
    AxlArena *arena, ///< arena used by the task
    void     *arg    ///< same argument from submit
) AXL_CB_NOEXCEPT;

/**
 * @brief Create a new task pool.
 *
 * Locates MP Services, dispatches the APs, then counts only those that
 * answer a liveness probe — so `axl_task_pool_worker_count` excludes an
 * AP that the firmware accepted but that is not actually running, which
 * would otherwise surface as a task that never completes. Note this is
 * established at creation: a worker that dies later stays counted, and
 * shows up as a task that never completes rather than as a smaller
 * worker count.
 *
 * A machine with no usable APs falls back to running submitted tasks
 * synchronously; that is reported by `axl_task_pool_is_single_core`,
 * not by failure.
 *
 * The pool registers a before-ExitBootServices handler that stops its
 * workers, so a resident driver may hold a pool across OS handoff. The
 * pool keeps working afterwards, synchronously.
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
