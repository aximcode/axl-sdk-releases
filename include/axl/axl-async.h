/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-async.h
 *
 * AP-offloaded async work queue with main-loop integration.
 *
 * Bridges AxlTask (AP core dispatch) with AxlLoop (main loop events).
 * Submit CPU-heavy work to an Application Processor while the BSP
 * continues servicing network, timers, and UI.  The done callback
 * fires on the BSP during an idle poll in the event loop.
 *
 * On single-core systems, work runs synchronously on the BSP (same
 * API, just blocking).
 *
 * @code
 * AxlAsync *async = axl_async_new(loop, 4);
 * AxlAsyncHandle h = axl_async_submit(async, verify_crc,
 *                                      chunk, arena, on_done);
 * // BSP continues — on_done fires when AP finishes
 * axl_async_free(async);
 * @endcode
 *
 * AP constraints: work functions cannot call Boot Services, protocol
 * calls, logging, or axl_malloc.  Use the arena for AP-side allocation.
 */

#ifndef AXL_ASYNC_H
#define AXL_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <axl/axl-task.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;
typedef struct AxlAsync AxlAsync;
typedef uint32_t AxlAsyncHandle;
#define AXL_ASYNC_INVALID  0

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Create a new async work queue.
 *
 * Creates an internal task pool and slot array.
 *
 * @return async handle, or NULL on failure.
 */
AxlAsync *
axl_async_new(
    AxlLoop *loop,         ///< event loop (idle source installed here)
    size_t   max_pending   ///< maximum concurrent async jobs
);

/**
 * @brief Free the async work queue. Drains pending work.
 */
void
axl_async_free(
    AxlAsync *async  ///< async handle (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlAsync, axl_async_free)
#endif

// ---------------------------------------------------------------------------
// Submit / cancel
// ---------------------------------------------------------------------------

/**
 * @brief Submit work to an AP core.
 *
 * On multi-core, dispatches @a work_fn to an AP and returns
 * immediately.  On single-core, runs @a work_fn and @a done_cb
 * synchronously before returning.
 *
 * @return handle for axl_async_cancel, or AXL_ASYNC_INVALID on failure.
 */
AxlAsyncHandle
axl_async_submit(
    AxlAsync        *async,   ///< async handle
    AxlTaskProc      work_fn, ///< AP work function (same as AxlTaskProc)
    void            *data,    ///< argument passed to work_fn and done_cb
    AxlArena        *arena,   ///< arena for AP allocations (NULL OK)
    AxlTaskComplete  done_cb  ///< BSP callback when done (NULL = fire-and-forget)
);

/**
 * @brief Cancel pending async work (best-effort).
 *
 * AP work continues to completion, but done_cb is suppressed.
 *
 * @return true if cancelled, false if handle invalid or already completed.
 */
bool
axl_async_cancel(
    AxlAsync       *async,   ///< async handle
    AxlAsyncHandle  handle   ///< handle from axl_async_submit
);

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

/**
 * @brief Get the number of pending (in-flight) async jobs.
 *
 * @return number of jobs submitted but not yet completed.
 */
size_t
axl_async_pending(
    AxlAsync *async  ///< async handle
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ASYNC_H */
