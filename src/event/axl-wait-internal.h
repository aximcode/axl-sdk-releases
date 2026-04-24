/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-wait-internal.h
    Shared wait primitive used by AxlEvent, the `axl_wait_*` helpers
    (Tier 2/3), and Tier 4 per-protocol helpers in src/net/ and
    src/ipmi/. Private to the AXL tree.
**/

#ifndef AXL_WAIT_INTERNAL_H
#define AXL_WAIT_INTERNAL_H

#include <stdint.h>

#include <axl/axl-cancellable.h>
#include <axl/axl-event.h>
#include <axl/axl-loop.h>
#include <axl/axl-wait.h>

/**
 * @brief Drive a throwaway AxlLoop until an event fires, a condition
 *        holds, a timeout elapses, or Ctrl-C is received.
 *
 * Any combination of the wait sources may be supplied:
 * - @a event: wake on signal of this UEFI event (NULL = no event)
 * - @a cond_fn: evaluate this predicate (NULL = no condition)
 * - @a tick_fn: run this side-effect periodically (NULL = no tick)
 *
 * When neither an event nor a tick is supplied but @a cond_fn is,
 * the primitive installs a default 1ms tick so the condition is
 * re-checked without busy-waiting.
 *
 * A @a timeout_us of 0 means no timeout.
 *
 * Return convention matches the public wait helpers:
 *   0  — condition met or event fired
 *  -1  — timeout / allocation failure
 *  -2  — interrupted (Ctrl-C / shell break)
 */
int
_axl_event_wait_timeout_with_tick(
    AxlEventHandle  event,       ///< event handle to wake on (may be NULL)
    AxlCondFn       cond_fn,     ///< condition predicate (may be NULL)
    void           *cond_ctx,    ///< opaque context for cond_fn
    AxlTickFn       tick_fn,     ///< periodic side-effect (may be NULL)
    void           *tick_ctx,    ///< opaque context for tick_fn
    uint64_t        tick_us,     ///< tick period in microseconds (0 = auto)
    AxlCancellable *cancel,      ///< optional cancel token (may be NULL)
    uint64_t        timeout_us   ///< timeout in microseconds (0 = forever)
);

#endif /* AXL_WAIT_INTERNAL_H */
