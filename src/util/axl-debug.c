/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-debug.c
    AXL_DEBUG_ASSERT failure path — see include/axl/axl-debug.h.

    Emits a loud, grep-able marker and increments a process-global counter,
    then returns (never aborts). The marker is what integration tests grep
    for; the counter is what unit tests read.
**/

#include <axl/axl-debug.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("assert");

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static size_t g_assert_failures;

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

void
_axl_debug_assert_fail(
    const char *file,
    int         line,
    const char *func,
    const char *expr,
    const char *msg)
{
    g_assert_failures++;

    /* axl_error injects this file's own __func__/__LINE__, so the caller's
       location goes in the message text. "AXL_DEBUG_ASSERT FAILED:" is the
       grep marker integration tests key on. */
    if (msg != NULL) {
        axl_error("AXL_DEBUG_ASSERT FAILED: %s:%d %s(): %s -- %s",
                  file, line, func, expr, msg);
    } else {
        axl_error("AXL_DEBUG_ASSERT FAILED: %s:%d %s(): %s",
                  file, line, func, expr);
    }
}

size_t
_axl_debug_assert_count(void)
{
    return g_assert_failures;
}
