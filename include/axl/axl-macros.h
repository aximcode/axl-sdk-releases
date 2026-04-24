/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-macros.h:
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

#define AXL_OK         0   ///< operation succeeded
#define AXL_ERR      (-1)  ///< operation failed
#define AXL_CANCELLED (-2) ///< operation cancelled (AxlCancellable signalled or Ctrl-C)

// ---------------------------------------------------------------------------
// Compiler attributes
// ---------------------------------------------------------------------------

/** Mark a return value as must-use (GLib: G_GNUC_WARN_UNUSED_RESULT). */
#define AXL_WARN_UNUSED  __attribute__((warn_unused_result))

/** Mark a function as never returning (C11 _Noreturn equivalent). */
#define AXL_NORETURN     __attribute__((noreturn))

// ---------------------------------------------------------------------------
// Autoptr (GLib: g_autoptr)
// ---------------------------------------------------------------------------

#define AXL_HAVE_AUTOPTR 1

#define AXL_DEFINE_AUTOPTR_CLEANUP(Type, free_func)                     \
    static inline void _axl_autoptr_cleanup_##Type(Type **p) {          \
        if (*p) { free_func(*p); }                                      \
    }

#define AXL_AUTOPTR(Type)  __attribute__((cleanup(_axl_autoptr_cleanup_##Type))) Type *

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
