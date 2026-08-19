/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-emit.c
    The two public log emitters, and nothing else.

    axl_log_full / axl_log are the entry points every axl_error / axl_warning /
    axl_info / axl_debug / axl_trace expands to, so an object that logs at all
    references one of them. Keeping them HERE — in an object that pulls nothing
    — is what lets an image link the emitters without linking the engine.

    See axl-log-dispatch.h for why the seam is a weak symbol rather than a
    vtable, and Makefile's $(LOG_ENGINE_PULL) for how a link asks for the
    engine.

    Part of the AximCode AXL SDK.
**/

#include <stdarg.h>
#include <stddef.h>
#include <axl/axl-log.h>

/* Before the include: the attribute belongs on the REFERENCE, and applying it
 * in axl-log.c would make that file's DEFINITION weak. */
#define AXL_LOG_DISPATCH_WEAK
#include "axl-log-dispatch.h"

void
axl_log_full(int level, const char *domain, const char *func,
             int line, const char *fmt, ...)
{
    /* The NULL check comes BEFORE va_start, not after. Both orders are
     * correct; this one costs an engine-less image a load and a branch
     * instead of a register-save-area spill (up to 6 GPRs and 8 XMMs under
     * the SysV ABI these internal calls use), on a path taken by every
     * suppressed axl_debug in the library.
     *
     * An image with no engine linked therefore does not merely discard the
     * record — it never formats it. That is the point: axl_vformat is 4 KB of
     * the ~6 KB this seam saves. */
    if (_axl_log_vdispatch == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    _axl_log_vdispatch(level, domain, func, line, fmt, args);
    va_end(args);
}

void
axl_log(int level, const char *domain, const char *fmt, ...)
{
    if (_axl_log_vdispatch == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    /* No source location: this is the variant callers use when __func__ /
     * __LINE__ would name this wrapper rather than anything useful. The
     * console renderer already omits the location when func is NULL. */
    _axl_log_vdispatch(level, domain, NULL, 0, fmt, args);
    va_end(args);
}
