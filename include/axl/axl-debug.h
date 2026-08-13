/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-debug.h
 *
 * Debug-build invariant assertions.
 *
 * `AXL_DEBUG_ASSERT` guards an internal invariant: a condition the
 * code's own logic guarantees, which a future edit could quietly
 * break. Unlike a runtime error check (which validates *input* and
 * returns an `AxlStatus`), a debug assert documents and enforces an
 * *internal* contract, and it is compiled out under `NDEBUG` so it
 * costs release builds nothing.
 *
 * The purpose is to catch a concurrency/lifecycle fault **at its
 * cause**, in a debug or test build, instead of as a downstream
 * symptom (a wedge, a desync, a corrupted dispatch) that only a
 * consumer's integration surfaces days later. See
 * `docs/AXL-Concurrency.md` § "Testing the model".
 *
 * Behavior on failure (debug/test builds only): emit a loud,
 * grep-able log line
 *   `AXL_DEBUG_ASSERT FAILED: <file>:<line> <func>(): <expr>`
 * and **continue** — it does not abort or wedge, so an integration
 * test can observe the marker on the serial log and still run to its
 * results footer. `_axl_debug_assert_count()` is a test hook for
 * unit tests to confirm a guard fired without parsing logs.
 *
 * Under `NDEBUG` (RELEASE builds), the macros expand to `((void)0)`
 * and the guarded expression is not evaluated.
 */

#ifndef AXL_DEBUG_H
#define AXL_DEBUG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record + log a failed debug assertion (internal; invoked by
 *        the AXL_DEBUG_ASSERT macros — do not call directly).
 *
 * Emits the `AXL_DEBUG_ASSERT FAILED:` marker and increments the
 * process-global failure counter, then returns (never aborts).
 */
void
_axl_debug_assert_fail(
    const char *file,   ///< source file (__FILE__)
    int         line,   ///< source line (__LINE__)
    const char *func,   ///< enclosing function (__func__)
    const char *expr,   ///< the failed expression, stringized
    const char *msg     ///< optional extra context, or NULL
);

/**
 * @brief Number of `AXL_DEBUG_ASSERT` failures observed this run.
 *
 * Test hook: a unit test triggers a guard and asserts the count
 * incremented. Always defined (returns 0 under NDEBUG, where the
 * macros never call the failure path).
 *
 * @return cumulative failure count since process start.
 */
size_t
_axl_debug_assert_count(void);

#ifdef NDEBUG

#define AXL_DEBUG_ASSERT(expr)          ((void)0)
#define AXL_DEBUG_ASSERT_MSG(expr, msg) ((void)0)

#else

/**
 * Assert an internal invariant. No-op under NDEBUG. On failure (debug/
 * test builds) logs the `AXL_DEBUG_ASSERT FAILED:` marker and continues.
 */
#define AXL_DEBUG_ASSERT(expr)                                              \
    ((expr) ? (void)0                                                       \
            : _axl_debug_assert_fail(__FILE__, __LINE__, __func__, #expr,   \
                                     NULL))

/** As AXL_DEBUG_ASSERT, with an extra human-readable context string. */
#define AXL_DEBUG_ASSERT_MSG(expr, msg)                                     \
    ((expr) ? (void)0                                                       \
            : _axl_debug_assert_fail(__FILE__, __LINE__, __func__, #expr,   \
                                     (msg)))

#endif /* NDEBUG */

#ifdef __cplusplus
}
#endif

#endif /* AXL_DEBUG_H */
