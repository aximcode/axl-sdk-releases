/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-atexit.h:
 *
 * POSIX-flavored cleanup registry. Callbacks registered via
 * axl_atexit run in LIFO order (last-registered-first-run) during
 * _axl_cleanup, before the tier-1 resource-registry sweep. Matches
 * C's atexit(3) contract and gives library consumers a place to
 * release long-lived resources (top-level loops, HTTP clients,
 * caches) without hand-threading cleanup through main's tail.
 *
 * @code
 * static void free_app_state(void *data) {
 *     app_state_free((AppState *)data);
 * }
 *
 * int main(int argc, char **argv) {
 *     AppState *s = app_state_new();
 *     axl_atexit(free_app_state, s);
 *     return run(argc, argv, s);
 * }
 * @endcode
 *
 * See docs/AXL-Runtime.md §4.3 for the design rationale.
 */

#ifndef AXL_ATEXIT_H
#define AXL_ATEXIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AxlAtexitFn:
 *
 * Cleanup callback. @a data is the opaque pointer supplied at
 * registration time. Runs on the main thread during _axl_cleanup.
 */
typedef void (*AxlAtexitFn)(void *data);

/**
 * @brief Register a callback to run during _axl_cleanup.
 *
 * Callbacks fire in LIFO order on every exit path (return from
 * main, axl_exit, or Ctrl-C through the default signal handler).
 *
 * @return non-zero handle on success, 0 on failure (fn is NULL or
 *     registration-time allocation failed).
 */
uint32_t
axl_atexit(
    AxlAtexitFn  fn,   ///< cleanup function
    void        *data  ///< opaque user data passed to fn
);

/**
 * @brief Remove a previously-registered atexit callback.
 *
 * Safe to call with handle==0 or with an already-removed handle
 * (no-op in both cases). Useful when a resource is freed explicitly
 * before exit and the atexit entry would otherwise run with a
 * dangling pointer.
 */
void
axl_atexit_remove(
    uint32_t handle  ///< handle returned by axl_atexit
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ATEXIT_H */
