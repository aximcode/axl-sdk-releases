/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-defer.h
 *
 * Deferred work queue owned by the event loop.
 *
 * Allows code in constrained contexts (protocol notifications, nested
 * callbacks, interrupt-like handlers) to schedule work for "next tick"
 * without blocking or re-entering the loop.
 *
 * @code
 * // In a protocol notification (constrained context):
 * axl_defer(loop, initialize_protocol, ctx);
 *
 * // Fires safely on the next main loop iteration.
 * @endcode
 */

#ifndef AXL_DEFER_H
#define AXL_DEFER_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

/**
 * AxlDeferCallback:
 *
 * Deferred work function.  Runs on the BSP main loop thread.
 */
typedef void (*AxlDeferCallback)(
    void *data ///< opaque caller data
) AXL_CB_NOEXCEPT;

/**
 * @brief Schedule deferred work for the next loop tick.
 *
 * Safe to call from protocol notifications, nested callbacks, or
 * any context where complex work should not run immediately.
 *
 * The queue is drained at the start of each loop iteration. Work queued
 * after that -- from a deferred callback itself -- runs on the following
 * iteration: while work is queued the loop does not block waiting for
 * events.
 *
 * **Driver mode.** A loop attached with axl_loop_attach_driver() drains
 * the queue on every pass of a tick, and deferred work keeps the tick
 * going as a busy idle source does: a chain of deferred callbacks runs
 * back to back, at TPL_CALLBACK, for up to the tick's budget
 * (axl_loop_set_driver_budget_ms(), and its idle budget when the
 * foreground is waiting), and what is left runs on the next tick. So
 * deferring does NOT hand TPL_CALLBACK back to co-located firmware
 * drivers between hops. A one-shot timer (axl_loop_add_timeout()) yields
 * to them in most ticks -- it fires no earlier than the firmware's next
 * timer interrupt -- but not in a tick that is idle-extended past that
 * interrupt (axl_loop_set_driver_idle_budget_ms()): the interrupt signals
 * the timer mid-tick and the tick's next pass dispatches it, while the
 * firmware's own TPL_CALLBACK timers still wait for the tick to end. The
 * strict yield is a driver budget of 0, which ends every tick at its first
 * pass that dispatches nothing.
 *
 * @return handle for axl_defer_cancel(), or 0 if the queue is full.
 */
uint32_t
axl_defer(
    AxlLoop          *loop, ///< event loop
    AxlDeferCallback  fn,   ///< work function
    void             *data  ///< opaque data passed to fn
);

/**
 * @brief Cancel pending deferred work before it fires.
 *
 * No-op if the handle is invalid or already fired.
 *
 * @return true if the work was cancelled, false if already fired or invalid.
 */
bool
axl_defer_cancel(
    AxlLoop  *loop,   ///< event loop
    uint32_t  handle  ///< handle from axl_defer()
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DEFER_H */
