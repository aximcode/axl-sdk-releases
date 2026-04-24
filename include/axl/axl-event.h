/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-event.h:
 *
 * Foundational one-shot latch. Wraps a UEFI event with signalled /
 * reset state. The building block for producer-waiter rendezvous
 * across the library. `AxlCancellable` is a typed contract on top:
 * same mechanical behavior, stop-token semantics.
 *
 * Typical use: an async callback signals the event; the main thread
 * waits for it. Internally wraps a UEFI event — waits are driven by
 * AxlLoop, so they idle the CPU (not busy-wait) and are interrupted
 * by Ctrl-C.
 *
 * Ordering: AXL targets UEFI BSP, which is single-threaded and
 * cooperative. Event ops are safe from any context that single
 * thread naturally reaches (protocol notifications, nested
 * callbacks, interrupt-like handlers). No cross-core ordering
 * guarantee between signal and is_set on platforms with real
 * parallelism — use an explicit AP-to-BSP channel (`AxlAsync`) if
 * you need one.
 *
 * @code
 * AxlEvent *e = axl_event_new();
 * start_async_op(on_done, e);
 * if (axl_event_wait_timeout(e, NULL, 5000000) != 0) {
 *     // -1 timeout, AXL_CANCELLED on Ctrl-C / cancel
 * }
 * axl_event_free(e);
 *
 * static void on_done(void *user) { axl_event_signal(user); }
 * @endcode
 *
 * Relationship to the event loop: the "event loop" (`AxlLoop`)
 * dispatches events — timer expirations, key presses, raw UEFI
 * event signals. An `AxlEvent` is one such event (a one-shot
 * latch). The name is overloaded but consistent: AXL is a thin
 * layer over UEFI events, and the loop is the dispatcher.
 */

#ifndef AXL_EVENT_H
#define AXL_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlEvent       AxlEvent;
typedef struct AxlCancellable AxlCancellable;  /* forward */

/**
 * AxlEventHandle:
 *
 * Raw UEFI event handle (`EFI_EVENT`). Used where firmware owns
 * the event — protocol completion tokens, protocol-notify events.
 * For AXL-managed events use `AxlEvent` and pass
 * `axl_event_handle(e)` where a handle is required
 * (e.g., `axl_loop_add_event`).
 */
typedef void *AxlEventHandle;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Create a new, unsignalled event.
 *
 * @return new AxlEvent, or NULL on failure. Free with
 *     axl_event_free().
 */
AxlEvent *
axl_event_new_impl(const char *file, int line);

/**
 * Captures the caller's file/line for leak reporting via the tier-1
 * resource registry. See docs/AXL-Runtime.md §4.2.1.
 */
#define axl_event_new() axl_event_new_impl(__FILE__, __LINE__)

/**
 * @brief Free an event. NULL-safe.
 */
void
axl_event_free(
    AxlEvent *e  ///< event (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlEvent, axl_event_free)
#endif

// ---------------------------------------------------------------------------
// Signal / reset / observe
// ---------------------------------------------------------------------------

/**
 * @brief Signal the event. Idempotent, NULL-safe.
 *
 * Safe from any context — protocol notifications, nested callbacks,
 * interrupt-like handlers.
 */
void
axl_event_signal(
    AxlEvent *e  ///< event (NULL-safe)
);

/**
 * @brief Reset the event to an unsignalled state. NULL-safe.
 *
 * Drops any pending signal so the same event can be reused across
 * multiple wait cycles.
 */
void
axl_event_reset(
    AxlEvent *e  ///< event (NULL-safe)
);

/**
 * @brief Fast check: is this event currently signalled?
 *
 * Reads an internal flag without driving the loop. Transitions:
 * - `axl_event_signal(e)`                → is_set becomes true
 * - `axl_event_reset(e)`                 → is_set becomes false
 * - successful `axl_event_wait[_timeout]` → is_set becomes false
 *   (the wait consumed the signal via CheckEvent in the loop
 *   dispatch; the flag mirrors the backend state)
 *
 * For the full wait-for-signal behavior with timeout and cancel
 * support, use axl_event_wait_timeout().
 *
 * @return true between signal and the next reset / successful wait,
 *     else false. Returns false for NULL.
 */
AXL_WARN_UNUSED bool
axl_event_is_set(
    const AxlEvent *e  ///< event (NULL-safe)
);

/**
 * @brief Get the raw UEFI event handle wrapped by this AxlEvent.
 *
 * Used when registering the event with the loop via
 * axl_loop_add_event, which takes a handle so the same entry point
 * serves AXL-managed events and firmware-owned ones alike.
 *
 * @return the wrapped handle, or NULL for NULL / uninitialized.
 */
AxlEventHandle
axl_event_handle(
    const AxlEvent *e  ///< event (NULL-safe, returns NULL)
);

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

/**
 * @brief Wait indefinitely for the event to be signalled.
 *
 * The CPU idles between events. Returns early on Ctrl-C or a
 * signalled cancellable. Equivalent to
 * axl_event_wait_timeout(e, cancel, 0).
 *
 * @return 0 on signal, -1 on invalid arg, AXL_CANCELLED on Ctrl-C
 *     or cancel.
 */
AXL_WARN_UNUSED int
axl_event_wait(
    AxlEvent       *e,      ///< event
    AxlCancellable *cancel  ///< optional cancel token (NULL = only Ctrl-C)
);

/**
 * @brief Wait for the event with a timeout.
 *
 * The CPU idles between events. A @a timeout_us of 0 means wait
 * forever. Returns early on Ctrl-C or a signalled cancellable.
 *
 * @return 0 on signal, -1 on timeout or invalid arg, AXL_CANCELLED
 *     on Ctrl-C or cancel.
 */
AXL_WARN_UNUSED int
axl_event_wait_timeout(
    AxlEvent       *e,          ///< event
    AxlCancellable *cancel,     ///< optional cancel token
    uint64_t        timeout_us  ///< timeout in microseconds (0 = forever)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_EVENT_H */
