/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cancellable.h
 *
 * Generic cancellation primitive for async operations. Parallels
 * GLib's GCancellable, mapped onto AXL's single-threaded event loop.
 *
 * A cancellable is an optional "stop token" that any async operation
 * can accept. The caller holds it; the op observes it. Signaling
 * `axl_cancellable_cancel` aborts every op currently referencing it.
 *
 * Typical use:
 *
 * @code
 * AxlCancellable *cancel = axl_cancellable_new();
 *
 * axl_tcp_connect_async(host, port, loop, cancel, on_connected, ctx);
 * axl_loop_add_timeout(loop, 5000, cancel_on_timeout, cancel);
 * axl_loop_run(loop);
 *
 * axl_cancellable_free(cancel);
 *
 * static bool cancel_on_timeout(void *data) {
 *     axl_cancellable_cancel(data);
 *     return AXL_SOURCE_REMOVE;
 * }
 *
 * // The op's callback fires exactly once, with status either AXL_OK
 * // on success, or AXL_CANCELLED if the cancellable fired first.
 * static void on_connected(AxlTcp *sock, AxlStatus status, void *ctx) {
 *     if (status == AXL_CANCELLED) { axl_tcp_close(sock); return; }
 *     // sock is ready to use
 * }
 * @endcode
 *
 * Group cancellation — one cancellable covers many ops:
 *
 * @code
 * axl_tcp_connect_async(h, p, loop, app->shutdown, cb1, c1);
 * axl_http_get_async   (u,   loop, app->shutdown, cb2, c2);
 *
 * // Cancels both ops on app shutdown — each callback fires with
 * // AXL_CANCELLED.
 * axl_cancellable_cancel(app->shutdown);
 * @endcode
 *
 * Ownership rule: the cancellable must outlive every async op that
 * observes it. Same discipline as AxlLoop outliving its sources.
 */

#ifndef AXL_CANCELLABLE_H
#define AXL_CANCELLABLE_H

#include <stdbool.h>

#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlCancellable AxlCancellable;

/**
 * @brief Create a new, unsignalled cancellable.
 *
 * @return new AxlCancellable, or NULL on failure.
 *     Free with axl_cancellable_free().
 */
AxlCancellable *
axl_cancellable_new_impl(const char *file, int line);

/**
 * Captures the caller's file/line for leak reporting via the tier-1
 * resource registry. See docs/AXL-Lifecycle.md §4.2.1.
 */
#define axl_cancellable_new() axl_cancellable_new_impl(__FILE__, __LINE__)

/**
 * @brief Free a cancellable. NULL-safe.
 *
 * Must only be called after every async op that observes this
 * cancellable has completed (via its callback) or is otherwise no
 * longer holding a reference. Freeing while an op still references
 * it results in a dangling event handle.
 */
void
axl_cancellable_free(
    AxlCancellable *c  ///< cancellable (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlCancellable, axl_cancellable_free)
#endif

/**
 * @brief Cancel every async op currently observing this cancellable.
 *
 * Idempotent — calling more than once is safe and has no additional
 * effect. Safe to call from any context (protocol notifications,
 * nested callbacks). NULL-safe.
 */
void
axl_cancellable_cancel(
    AxlCancellable *c  ///< cancellable (NULL-safe)
);

/**
 * @brief Check whether the cancellable has been signalled.
 *
 * @return true if axl_cancellable_cancel() was called, else false.
 *     Returns false for NULL.
 */
AXL_WARN_UNUSED bool
axl_cancellable_is_cancelled(
    const AxlCancellable *c  ///< cancellable (NULL-safe)
);

/**
 * @brief Reset the cancellable to an unsignalled state. NULL-safe.
 *
 * Drops any pending cancel signal so the same cancellable can be
 * reused for a fresh batch of async ops. Only call once all ops
 * that might have observed the prior signal have completed.
 */
void
axl_cancellable_reset(
    AxlCancellable *c  ///< cancellable (NULL-safe)
);

/**
 * @brief Also cancel @p c when the Shell's Ctrl-C (break) arrives during a
 *     synchronous wait that observes it -- even in resident code. NULL-safe.
 *
 * The counterpart of axl_loop_set_intercept_break() for a cancellable. A
 * foreground application's waits end on Ctrl-C already; resident code's (a
 * driver's) do not, because the Shell's break belongs to whatever the Shell
 * is running and reading it consumes it. This is for resident code acting
 * FOR a foreground command that waits on it: a driver's setup while the
 * command that loaded it waits in its start.
 *
 * While set, each synchronous wait that takes @p c (`axl_wait_*`,
 * axl_net_auto_init_opts()) also waits on the Shell's `ExecutionBreak`: a
 * Ctrl-C then cancels @p c -- so every later wait on it ends at once too --
 * and that wait returns AXL_CANCELLED. Async ops on a loop do not look at it.
 *
 * - **The wait consumes the break.** The Shell does not see that Ctrl-C:
 *   clear the claim (@p on false) as soon as the foreground command has its
 *   answer. Freeing @p c ends the claim too.
 * - **A break already latched ends the first claiming wait at once.** The
 *   Shell's event stays signalled until someone reads it, so a Ctrl-C pressed
 *   earlier -- while the command ran a step that is not a wait, such as a
 *   driver connect -- cancels @p c as soon as a claiming wait starts. That is
 *   intended: it was pressed during the command it stops.
 * - **Inert without the Shell protocol** (no Shell, or an EFI 1.x shell): the
 *   waits then behave as without the claim.
 * - **The waits must run at TPL_APPLICATION**, as every blocking wait must
 *   (WaitForEvent).
 */
void
axl_cancellable_set_cancel_on_break(
    AxlCancellable *c,   ///< cancellable (NULL-safe)
    bool            on   ///< true to claim the break, false to give it back
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_CANCELLABLE_H */
