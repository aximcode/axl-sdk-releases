/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxrt.h
    Internal contract for the exceptions-build C++ runtime glue.

    Internal, not public: these are called by AXL's own startup/teardown, not
    by consumers. They exist as declarations at all so the definitions and
    their callers cannot disagree about a prototype -- the alternative is each
    caller hand-declaring them, which is a silent-mismatch generator.
**/

#ifndef AXL_CXXRT_H
#define AXL_CXXRT_H

/**
 * @brief Hand libgcc the unwind tables. Call before the first throw.
 *
 * Must run BEFORE global constructors, since one may throw. Idempotent.
 */
void
axl_cxxrt_init(
    void
    );

/**
 * @brief Release what the C++ runtime acquired. Call at teardown.
 *
 * Ordering is constrained on both sides: AFTER atexit handlers (a destructor
 * may throw, which needs the frame table still registered) and BEFORE the
 * leak report (this frees ~15 KB that would otherwise be reported as leaked).
 * Registering it with axl_atexit() at init satisfies both -- atexit is LIFO,
 * so the earliest registration runs last.
 *
 * Idempotent, and safe when init never ran.
 */
void
axl_cxxrt_fini(
    void
    );

#endif /* AXL_CXXRT_H */
