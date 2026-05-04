/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-wait.h:
 *
 * Interruptible wait helpers built on AxlLoop.
 *
 * These replace the common "busy-poll with axl_backend_stall" idiom
 * with event-driven waits that idle the CPU between checks and
 * return early on Ctrl-C. Every function returns @ref AxlStatus:
 *
 *     AXL_OK         — condition met / elapsed
 *     AXL_TIMEOUT    — deadline elapsed before condition
 *     AXL_ERR        — invalid arg / internal failure
 *     AXL_CANCELLED  — interrupted (Ctrl-C / shell break / cancel token)
 *
 * @code
 * // Wait for a hardware status flag, CPU idle between checks:
 * if (axl_wait_for_word(&mmio->status, 0, NULL, 500000) != AXL_OK) {
 *     return AXL_ERR;
 * }
 *
 * // Interruptible sleep:
 * (void)axl_wait_ms(NULL, 100);
 * @endcode
 */

#ifndef AXL_WAIT_H
#define AXL_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlCancellable AxlCancellable;

// ---------------------------------------------------------------------------
// Tier 1 — sleep (void return, no cancel, ergonomic)
// ---------------------------------------------------------------------------
//
// Thin void-return wrappers over axl_wait_ms(NULL, ...). The CPU
// idles for the duration via a one-shot timer event — no busy-wait.
//
// Ctrl-C semantics: Ctrl-C (shell break) returns from sleep early,
// the same as from any wait helper. UNLIKE Linux, Ctrl-C does NOT
// automatically terminate the process — execution continues past
// the sleep. Apps that want Ctrl-C to exit should check it at the
// main-loop boundary (axl_loop_run returns -1) or call
// axl_wait_ms() and observe the AXL_CANCELLED return. For truly
// uninterruptible sub-ms hardware timing, the backend-internal
// axl_backend_stall is the right primitive (not exposed publicly).

/// Sleep for the specified number of seconds.  CPU idles; Ctrl-C returns early.
void axl_sleep(uint64_t seconds);

/// Sleep for the specified number of milliseconds.  CPU idles; Ctrl-C returns early.
void axl_msleep(uint64_t milliseconds);

/// Sleep for the specified number of microseconds (rounded up to ms
/// granularity).  CPU idles; Ctrl-C returns early.
void axl_usleep(uint64_t microseconds);

/**
 * AxlCondFn:
 *
 * Condition predicate. Returns true when the wait should end.
 */
typedef bool (*AxlCondFn)(
    void *ctx  ///< opaque caller context
);

/**
 * AxlTickFn:
 *
 * Periodic side-effect called between waits. Typical use is to
 * drive a UEFI protocol state machine forward (e.g. call
 * protocol->Poll) so the condition can become true.
 */
typedef void (*AxlTickFn)(
    void *ctx  ///< opaque caller context
);

// ---------------------------------------------------------------------------
// Tier 2 — zero-callback convenience
// ---------------------------------------------------------------------------

/**
 * @brief Wait until *flag becomes true, with optional cancel + timeout.
 *
 * CPU idles between 1ms checks. Returns 0 immediately if *flag is
 * already true, or AXL_CANCELLED if @p cancel was already signalled.
 *
 * @return AXL_OK on true, AXL_TIMEOUT on deadline, AXL_ERR on invalid
 *     arg, AXL_CANCELLED on Ctrl-C or an observed cancellable.
 */
AXL_WARN_UNUSED AxlStatus
axl_wait_for_flag(
    volatile const bool *flag,       ///< flag to observe
    AxlCancellable      *cancel,     ///< optional cancel token (NULL = only Ctrl-C)
    uint64_t             timeout_us  ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait until *word stops matching @a not_ready_value.
 *
 * Covers UEFI completion-token Status polls, DMA flags, and any
 * "keep checking this memory word until it changes" pattern.
 * CPU idles between 1ms checks.
 *
 * @return AXL_OK on change, AXL_TIMEOUT on deadline, AXL_ERR on invalid
 *     arg, AXL_CANCELLED on Ctrl-C or an observed cancellable.
 */
AXL_WARN_UNUSED AxlStatus
axl_wait_for_word(
    volatile const uint64_t *word,             ///< memory word to observe
    uint64_t                 not_ready_value,  ///< value that means "keep waiting"
    AxlCancellable          *cancel,           ///< optional cancel token
    uint64_t                 timeout_us        ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Interruptible sleep with cancellable support.
 *
 * The long form of axl_msleep — use this when you need to inspect
 * the return code (Ctrl-C vs elapsed) or pass a shared AxlCancellable.
 * The CPU idles for the duration.
 *
 * Parameter order note: the rest of the wait family places cancel
 * between the subject and the timeout. Sleep has no subject, so
 * cancel comes first. The relative position (cancel immediately
 * before the duration/timeout) is consistent with the other helpers.
 *
 * @return AXL_OK on elapsed, AXL_CANCELLED on Ctrl-C or cancel.
 */
AXL_WARN_UNUSED AxlStatus
axl_wait_ms(
    AxlCancellable *cancel,  ///< optional cancel token (NULL = only Ctrl-C)
    uint64_t        ms       ///< milliseconds to sleep (0 returns immediately)
);

// ---------------------------------------------------------------------------
// Tier 3 — callback form for complex conditions
// ---------------------------------------------------------------------------

/**
 * @brief Wait until cond_fn returns true, with timeout + optional cancel.
 *
 * cond_fn is evaluated immediately and then every 1ms. CPU idles
 * between evaluations.
 *
 * @return AXL_OK on cond_fn true, AXL_TIMEOUT on deadline, AXL_ERR
 *     on invalid arg, AXL_CANCELLED on Ctrl-C or cancel.
 */
AXL_WARN_UNUSED AxlStatus
axl_wait_for(
    AxlCondFn       cond_fn,     ///< predicate
    void           *cond_ctx,    ///< opaque context passed to cond_fn
    AxlCancellable *cancel,      ///< optional cancel token
    uint64_t        timeout_us   ///< timeout in microseconds (0 = forever)
);

/**
 * @brief Wait until cond_fn returns true, running tick_fn each period.
 *
 * cond_fn is evaluated immediately and then each time tick_fn runs.
 * Use this form when the condition only becomes true after an
 * external state machine is advanced (e.g. calling protocol->Poll
 * on a UEFI driver).
 *
 * @return AXL_OK on cond_fn true, AXL_TIMEOUT on deadline, AXL_ERR
 *     on invalid arg, AXL_CANCELLED on Ctrl-C or cancel.
 */
AXL_WARN_UNUSED AxlStatus
axl_wait_for_with_tick(
    AxlCondFn       cond_fn,    ///< predicate (required)
    void           *cond_ctx,   ///< opaque context for cond_fn
    AxlTickFn       tick_fn,    ///< periodic side-effect (may be NULL)
    void           *tick_ctx,   ///< opaque context for tick_fn
    uint64_t        tick_us,    ///< tick period in microseconds (minimum 1ms)
    AxlCancellable *cancel,     ///< optional cancel token
    uint64_t        timeout_us  ///< timeout in microseconds (0 = forever)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_WAIT_H */
