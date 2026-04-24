/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-runtime.h:
 *
 * AXL runtime surface -- the pieces an app interacts with around its
 * own lifecycle and interruptibility. The runtime is CRT0-owned:
 * _axl_init (called by the entry stub) sets up the default loop,
 * installs the shell break notify, and initializes the tier-1
 * resource registry; _axl_cleanup (called on both return-from-main
 * and axl_exit) runs atexit callbacks and sweeps the registry.
 *
 * Related headers:
 *   - axl-signal.h   Ctrl-C / interrupt handler API + axl_exit
 *   - axl-atexit.h   POSIX-flavored cleanup callback registry
 *   - axl-loop.h     Loop primitives, including axl_loop_iterate_until
 */

#ifndef AXL_RUNTIME_H
#define AXL_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

// ---------------------------------------------------------------------------
// Default loop
// ---------------------------------------------------------------------------

/**
 * @brief Return the runtime's default loop, lazy-creating on first call.
 *
 * The default loop is owned by the runtime and freed during
 * _axl_cleanup. Apps can run it directly (axl_loop_run(axl_loop_default())),
 * add their own sources to it, or ignore it entirely and create
 * private loops. axl_yield() dispatches immediately-ready work on
 * this loop opportunistically.
 *
 * @return the default loop, or NULL if allocation failed.
 */
AxlLoop *
axl_loop_default(void);

// ---------------------------------------------------------------------------
// Yield
// ---------------------------------------------------------------------------

/**
 * @brief Cooperative yield point.
 *
 * Call inside CPU-bound loops to keep the app interruptible. Cost is
 * ~nanoseconds when the default loop is idle. Safe from any context
 * except a raised-TPL notify handler.
 *
 * Behavior:
 *   1. If any immediately-ready work is pending on the default loop
 *      (timers, deferred callbacks), dispatch it -- bounded to one
 *      iteration.
 *   2. If Ctrl-C was observed during that dispatch, sets the
 *      interrupted flag so axl_interrupted() returns true.
 *   3. Otherwise returns immediately.
 */
void
axl_yield(void);

// ---------------------------------------------------------------------------
// Registry inspection (debug)
// ---------------------------------------------------------------------------

/**
 * @brief Return the number of tier-1 resources currently registered.
 *
 * Purely informational -- mostly useful in tests to verify
 * resource-balancing. Returns 0 if the registry has not been
 * initialized yet.
 */
size_t
axl_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RUNTIME_H */
