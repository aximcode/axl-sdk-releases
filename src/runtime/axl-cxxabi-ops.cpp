/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi-ops.cpp
    Itanium C++ ABI runtime bits that need C++ linkage: operator
    `new` / `delete` (scalar / array / sized / placement) plus the
    `__cxa_pure_virtual` stub.  Companion to
    `src/runtime/axl-cxxabi.c` (which holds the C-linkage bits:
    `__dso_handle`, `__cxa_atexit`, and the `.init_array` walker).

    Lives in `libaxl-cxx.a` (NOT `libaxl.a`) so pure-C consumers
    don't need a C++ toolchain installed.  `axl-cc` auto-appends
    `libaxl-cxx.a` to the link line when it sees any .cpp source.

    Heap routing: all allocating operators forward to `axl_malloc`;
    all freeing operators forward to `axl_free`.  Failure semantics:
    we're built with `-fno-exceptions`, so the "throwing" operator
    overloads cannot throw `std::bad_alloc` — they return NULL on
    allocation failure (the compiler still generates calls to them,
    just without the implicit try/catch).  Callers must be defensive.
**/

#include <stddef.h>

#include <axl.h>

// ---------------------------------------------------------------------------
// operator new / delete
// ---------------------------------------------------------------------------
//
// Six required forms per Itanium C++ ABI § 3.2.6 + C++14 sized-delete.
// Placement new (operator new(size_t, void*)) is header-only in <new>
// and not provided here — it just returns the passed-in pointer.

void *
operator new(size_t sz) noexcept
{
    return axl_malloc(sz);
}

void *
operator new[](size_t sz) noexcept
{
    return axl_malloc(sz);
}

void
operator delete(void *p) noexcept
{
    axl_free(p);
}

void
operator delete[](void *p) noexcept
{
    axl_free(p);
}

/* C++14+ sized delete.  Compiler picks this overload when the size
 * is known at the call site; we just ignore the size and dispatch
 * to axl_free, which carries its own size metadata. */
void
operator delete(void *p, size_t /*sz*/) noexcept
{
    axl_free(p);
}

void
operator delete[](void *p, size_t /*sz*/) noexcept
{
    axl_free(p);
}

// ---------------------------------------------------------------------------
// __cxa_pure_virtual
// ---------------------------------------------------------------------------
//
// Compiler-emitted slot in the vtable for pure virtual functions.
// Called only if a pure virtual is actually invoked — typically a
// destructor running during base-class construction or destruction,
// when the vtable is in a transitional state.  Indicates a real
// programming error; we log and exit.

extern "C" void
__cxa_pure_virtual(void)
{
    axl_print("[axl-cxxabi] __cxa_pure_virtual called — aborting\r\n");
    axl_exit(1);
}
