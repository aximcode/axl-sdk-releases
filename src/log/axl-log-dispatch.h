/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-log-dispatch.h
    The seam between the log *emitters* (axl_log_full / axl_log, in
    axl-log-emit.c) and the log *engine* (axl-log.c).

    WHY THIS EXISTS. Every emitter call site in the library is an ordinary
    strong reference to axl_log_full — 682 of them across 140 files — and 27 of
    the 51 archive members in a do-nothing image carry one. So for as long as
    axl_log_full lived in the same object as the engine, EVERY image ever
    linked pulled the engine, and through it the printf engine (axl_vformat,
    axl_dtoa, kCachedPowers) and the console. That is why `--minimal-runtime`
    measured ~0 bytes saved against the full runtime.

    Splitting the emitters into their own object moves the decision to ONE
    symbol. axl-log-emit.c declares _axl_log_vdispatch WEAK and NULL-checks it,
    so nothing pulls the engine implicitly; every link that wants logging asks
    for it with `-u _axl_log_vdispatch` ($(LOG_ENGINE_PULL) in the Makefile,
    the default in axl-cc, `--minimal-runtime=log` when opting back in).

    THE WEAKNESS IS PER-TRANSLATION-UNIT, and deliberately so. A `weak`
    attribute on a declaration that the SAME translation unit then defines
    makes the DEFINITION weak — which would leave the engine as a weak
    definition in libaxl.a and make `-u` unreliable. So the attribute is
    applied only where the reference is made: axl-log-emit.c defines
    AXL_LOG_DISPATCH_WEAK before including this header, and axl-log.c does not.
    One prototype either way, so the two cannot drift.

    Part of the AximCode AXL SDK.
**/

#ifndef AXL_LOG_DISPATCH_H
#define AXL_LOG_DISPATCH_H

#include <stdarg.h>

/**
 * @brief Format and deliver one log record. The log engine proper.
 *
 * Defined in axl-log.c; referenced WEAKLY from axl-log-emit.c, which is
 * what makes the whole engine opt-in at link time. Applies the env-var
 * configuration on first call, filters on the effective level for
 * @a domain, renders @a fmt into a stack buffer and hands the result to
 * the console renderer and to every registered handler.
 *
 * Takes an already-started @c va_list: the emitter owns va_start/va_end
 * so that an image WITHOUT an engine pays a load-and-branch rather than
 * a register-save-area spill.
 */
#ifdef AXL_LOG_DISPATCH_WEAK
__attribute__((weak))
#endif
void
_axl_log_vdispatch(
    int         level,   ///< log level (AXL_LOG_ERROR..AXL_LOG_TRACE)
    const char *domain,  ///< module name, or NULL
    const char *func,    ///< __func__, or NULL
    int         line,    ///< __LINE__, or 0
    const char *fmt,     ///< standard C printf format string
    va_list     args     ///< started argument list for @a fmt
) __attribute__((format(printf, 5, 0)));

#endif /* AXL_LOG_DISPATCH_H */
