/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-macros.h
 *
 * Compiler attribute macros. Zero dependencies.
 * Equivalent to GLib's gmacros.h.
 */

#ifndef AXL_MACROS_H
#define AXL_MACROS_H

#include <stddef.h>  /* for NULL */

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------

/**
 * AxlStatus:
 *
 * Project-wide status enum for functions whose return value carries
 * more information than success/failure. Multi-outcome operations
 * return `AxlStatus` so callers can branch on the specific code
 * (cancellation vs. timeout vs. generic error) without consulting
 * a table of magic numbers. Single-outcome predicates use `bool`;
 * POSIX-exit-shaped functions like `axl_args_run` keep their `int`
 * return because their value flows directly into the process exit
 * code.
 *
 * Values are stable; new codes only ever extend the negative range.
 * The numeric values are part of the contract — code that compares
 * against the constants (`rc == AXL_CANCELLED`) and code that
 * compares against the literal integers (`rc == -2`) both work.
 */
typedef enum {
    AXL_OK           =  0,  ///< operation succeeded
    AXL_ERR          = -1,  ///< operation failed (generic)
    AXL_CANCELLED    = -2,  ///< operation cancelled (AxlCancellable signalled or Ctrl-C)
    AXL_TIMEOUT      = -3,  ///< operation deadline elapsed before completion
    /* Richer, mappable failure codes — the blessed vocabulary for consumers
       that need to distinguish or translate outcomes (e.g. a status->HTTP
       map) without coupling to EFI_*. New codes only ever extend the negative
       range; the numeric values are part of the contract. AXL_ERR remains the
       generic catch-all — an API returns a specific code only where it
       meaningfully discriminates, and callers must still treat ANY negative
       value as failure. */
    AXL_INVALID      = -4,  ///< invalid argument / malformed input
    AXL_NOT_FOUND    = -5,  ///< requested entity does not exist
    AXL_DENIED       = -6,  ///< permission / authorization denied
    AXL_UNSUPPORTED  = -7,  ///< operation not supported here
    AXL_NO_RESOURCES = -8,  ///< out of memory / handles / capacity
    AXL_IO_ERROR     = -9,  ///< underlying device / transport I/O failure
    AXL_BUSY         = -10, ///< resource temporarily unavailable (e.g. a prior async op still in flight) — retry later
} AxlStatus;

// ---------------------------------------------------------------------------
// Compiler attributes
// ---------------------------------------------------------------------------

/** Mark a return value as must-use (GLib: G_GNUC_WARN_UNUSED_RESULT).
    Expands to the C23 `[[nodiscard]]` attribute; the project requires
    `-std=gnu2x` (gcc 13+). */
#define AXL_WARN_UNUSED  [[nodiscard]]

/** Mark a function as never returning. C23 `[[noreturn]]`. */
#define AXL_NORETURN     [[noreturn]]

// ---------------------------------------------------------------------------
// Autoptr (GLib: g_autoptr)
// ---------------------------------------------------------------------------

#define AXL_HAVE_AUTOPTR 1

#define AXL_DEFINE_AUTOPTR_CLEANUP(Type, free_func)                     \
    static inline void _axl_autoptr_cleanup_##Type(Type **p) {          \
        if (*p) { free_func(*p); }                                      \
    }

/* Like AXL_DEFINE_AUTOPTR_CLEANUP but for a destructor that takes a fixed
 * second argument — e.g. a teardown-mode enum. Scope-exit cleanup always
 * passes @p arg (destruction is graceful; a caller that wants a different
 * mode calls the destructor explicitly). */
#define AXL_DEFINE_AUTOPTR_CLEANUP_ARG(Type, free_func, arg)            \
    static inline void _axl_autoptr_cleanup_##Type(Type **p) {          \
        if (*p) { free_func(*p, (arg)); }                              \
    }

#define AXL_AUTOPTR(Type)  __attribute__((cleanup(_axl_autoptr_cleanup_##Type))) Type *

// ---------------------------------------------------------------------------
// Callback exception boundary
// ---------------------------------------------------------------------------

/**
 * Marks a callback type AXL invokes from its own C frames.
 *
 * Expands to `noexcept` in C++ and to nothing in C, which makes the
 * contract a COMPILE ERROR rather than a documented hope:
 *
 * @code
 * error: invalid conversion from 'int (*)(void*)'
 *        to 'AxlHashTableForeachFunc' {aka 'int (*)(void*) noexcept'}
 * @endcode
 *
 * The contract is that an exception must never leave a callback, because
 * the frames it would unwind through are AXL's own C. Those frames are
 * compiled `-fno-exceptions` and carry no landing pads, so an unwind
 * crossing them runs NO cleanup: every `AXL_AUTO_FREE` in the path
 * leaks, and — measured — a `RaiseTPL` in the path is never restored,
 * which wedges the machine on return to the shell at *any* raised level.
 *
 * Since C++17 `noexcept` is part of a function's type, so a throwing
 * callback simply will not convert. A consumer that wants to use
 * exceptions catches at its own boundary and returns a status instead:
 *
 * @code
 * extern "C" int on_row(void *ctx) noexcept
 * {
 *     try { return do_work(ctx); }
 *     catch (const std::exception &e) { record(ctx, e); return -1; }
 *     catch (...)                     { return -1; }
 * }
 * @endcode
 *
 * A throw that escapes anyway calls `std::terminate` at the throw point
 * — fatal, but loud and located, rather than a silent wedge. GCC also
 * warns at compile time (`-Wterminate`) when it can see the throw.
 */
#if defined(__cplusplus)
#  define AXL_CB_NOEXCEPT  noexcept
#else
#  define AXL_CB_NOEXCEPT
#endif

// ---------------------------------------------------------------------------
// Steal pointer (GLib: g_steal_pointer)
// ---------------------------------------------------------------------------

/**
 * axl_steal_pointer:
 *
 * Atomically replace a pointer with NULL and return the old value.
 * The intended use is to transfer ownership of a resource held by an
 * AXL_AUTOPTR local to a caller (or to an output pointer), disarming
 * the cleanup attribute on the success path:
 *
 * @code
 * AxlFoo *
 * axl_foo_new(void)
 * {
 *     AXL_AUTOPTR(AxlFoo) foo = axl_calloc(1, sizeof(AxlFoo));
 *     if (foo == NULL) {
 *         return NULL;
 *     }
 *     foo->thing = axl_thing_new();
 *     if (foo->thing == NULL) {
 *         return NULL;  // AUTOPTR cleanup frees foo
 *     }
 *     return axl_steal_pointer(&foo);  // disarms the cleanup
 * }
 * @endcode
 *
 * The macro preserves the pointer type of @a pp so the result is
 * assignment-compatible with the caller's variable.
 */
static inline void *
(axl_steal_pointer)(void *pp)
{
    void **ptr = (void **)pp;
    void  *ref = *ptr;
    *ptr = NULL;
    return ref;
}

#define axl_steal_pointer(pp) \
    ((__typeof__(*(pp))) (axl_steal_pointer)(pp))

#endif /* AXL_MACROS_H */
