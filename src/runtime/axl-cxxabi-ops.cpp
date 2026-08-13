/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxxabi-ops.cpp
    Itanium C++ ABI runtime bits that need C++ linkage: operator
    `new` / `delete` in every form a consumer can write — scalar,
    array, sized, nothrow (including the `std::nothrow` object itself)
    and over-aligned — plus the `__cxa_pure_virtual` stub.  Companion to
    `src/runtime/axl-cxxabi.c` (which holds the C-linkage bits:
    `__dso_handle`, `__cxa_atexit`, and the `.init_array` walker).

    Lives in `libaxl-cxx.a` (NOT `libaxl.a`) so pure-C consumers
    don't need a C++ toolchain installed.  `axl-cc` auto-appends
    `libaxl-cxx.a` to the link line when it sees any .cpp source.

    Heap routing: all allocating operators forward to `axl_malloc`;
    all freeing operators forward to `axl_free`.

    Failure semantics: the throwing forms HALT, via
    `std::__throw_bad_alloc` below.  They cannot return NULL, because
    the standard guarantees they never do and the library takes that
    guarantee literally — `__new_allocator::allocate` returns
    `operator new`'s result to the container without a null check.
    Handing it a NULL means the container constructs objects through
    a null pointer, so the observable failure is a `#PF` near address
    0 somewhere inside libstdc++, not the allocation site.  An earlier
    revision here did return NULL and asked callers to be defensive;
    that was workable only while nothing but hand-written `new`
    expressions reached it, and `axl-c++ --hosted` ended that.

    A caller that genuinely wants NULL asks for it the way the
    standard provides: `new (std::nothrow) T`, whose overloads are
    also defined below.

    The allocating forms are NOT marked `noexcept`, and that is not an
    oversight: `<new>` declares them without one, and a replacement whose
    exception specification differs from the declaration is ill-formed.
    gcc accepts the mismatch; clang rejects it outright — which nothing
    in this tree noticed until `lint.sh` began linting C++ at all, since
    the compile database contained zero C++ translation units before
    then.  The deletes DO keep `noexcept`, because `<new>` declares those
    that way.
**/

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

// <new> is in the freestanding subset. It supplies std::nothrow_t and,
// usefully, the real declarations of the operator new / delete overloads
// defined below -- so a signature that drifts from the standard's becomes a
// compile error here rather than a link error in a consumer.
#include <new>

#include <axl.h>

// These MUST be declared before anything else in this TU declares them: gcc
// rejects a definition whose declaration appears LATER in the same
// translation unit ("declared here, later in the translation unit"). Adding a
// libstdc++ container header above this point is what would do it. The
// definitions are at the bottom of the file.
namespace std {

AXL_NORETURN void __throw_bad_alloc(void);
AXL_NORETURN void __throw_bad_array_new_length(void);
AXL_NORETURN void __throw_length_error(const char *msg);
AXL_NORETURN void __throw_logic_error(const char *msg);
AXL_NORETURN void __throw_out_of_range_fmt(const char *fmt, ...);
AXL_NORETURN void __throw_out_of_range(const char *msg);
AXL_NORETURN void __throw_invalid_argument(const char *msg);

/* std::terminate is NOT declared here, unlike its neighbours: <new> pulls in
   <bits/c++config.h>, which already declares it __noreturn__. Repeating the
   declaration puts [[noreturn]] on a non-FIRST declaration, which gcc accepts
   and clang rejects outright -- the same asymmetry that caught the operator
   new exception specifications. The definition at the bottom therefore omits
   AXL_NORETURN too and inherits it from libstdc++'s declaration. */

} // namespace std

// ---------------------------------------------------------------------------
// operator new / delete
// ---------------------------------------------------------------------------
//
// Six required forms per Itanium C++ ABI § 3.2.6 + C++14 sized-delete.
//
// Placement new (operator new(size_t, void*)) is intentionally NOT here:
// it allocates nothing — it just returns the passed-in pointer — so it is
// header-only in <new> (`inline void *operator new(size_t, void *p) { return
// p; }`). A consumer that writes `new (buf) T{...}` must therefore
// `#include <new>`: the freestanding toolchain does NOT supply the placement
// overload implicitly, so without the include the call fails to compile
// ("no matching function for operator new(size_t, void*)"). It never reaches
// axl_malloc.

void *
operator new(size_t sz)
{
    void *p = axl_malloc(sz);

    if (p == nullptr) {
        std::__throw_bad_alloc();
    }
    return p;
}

void *
operator new[](size_t sz)
{
    void *p = axl_malloc(sz);

    if (p == nullptr) {
        std::__throw_bad_alloc();
    }
    return p;
}

/* std::nothrow itself, not just the overloads that take it.
 *
 * The object lives in libsupc++, which a firmware image does not link, so
 * defining only the operators left `new (std::nothrow) T` failing at link
 * with "undefined reference to `std::nothrow'" -- an escape hatch that the
 * documentation pointed at and that did not exist.
 *
 * Weak, because hosted builds DO have libstdc++.a on the line. Ours is seen
 * first so that archive member is never pulled for this symbol (which is a
 * bonus: the member that defines it also carries the emergency
 * exception-handling pool, and that wants pthread). Weak keeps the link
 * working anyway if it ever arrives for some other reason. */
namespace std {
/* `extern` restates the external linkage <new> already gave it -- at
 * namespace scope a bare `const` object would be internal, and an internal
 * one satisfies nobody's undefined reference.
 *
 * NOT weak: gcc rejects a weak const object here, and weak is unnecessary.
 * An archive member is pulled only for a symbol that is still undefined, and
 * libaxl-cxx.a precedes libstdc++.a on the link line, so libstdc++'s
 * definition is never reached for this. */
extern const nothrow_t nothrow;
const nothrow_t nothrow;
}

/* The nothrow forms: the standard's own way to ask for a NULL instead
 * of a halt, and the only supported way to get one.  The matching
 * deletes exist because the compiler calls them when a nothrow-new
 * expression's constructor throws -- unreachable under
 * -fno-exceptions, but their absence would be a link error the day
 * anything changes. */
void *
operator new(size_t sz, const std::nothrow_t &) noexcept
{
    return axl_malloc(sz);
}

void *
operator new[](size_t sz, const std::nothrow_t &) noexcept
{
    return axl_malloc(sz);
}

void
operator delete(void *p, const std::nothrow_t &) noexcept
{
    axl_free(p);
}

void
operator delete[](void *p, const std::nothrow_t &) noexcept
{
    axl_free(p);
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
// Over-aligned new / delete (C++17 aligned-new, P0035)
// ---------------------------------------------------------------------------
//
// The compiler emits calls to these -- NOT to the plain forms above -- for any
// type whose alignment exceeds __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 on both
// our targets). `struct alignas(32) V { ... }; new V;` is the whole trigger,
// and it is ordinary code: a cache-line-aligned structure or any SIMD vector
// type reaches it.
//
// Nothing defined them, so that `new` failed to LINK -- in hosted builds too,
// because these live in libsupc++ rather than in the libstdc++ members the
// containers pull. The error names a mangled `operator new(unsigned long,
// std::align_val_t)` and says nothing about alignment being the cause.
//
// axl_malloc aligns to sizeof(size_t), so anything stricter is built here:
// over-allocate, step up to the boundary, and stash the original pointer in
// the word immediately below the result for the delete to recover. The
// deletes therefore must NOT be handed a pointer from the plain operators,
// and cannot be -- the compiler pairs them by the same alignment rule that
// chose the allocating form.

namespace {

void *
aligned_alloc_impl(
    size_t sz,     ///< bytes requested
    size_t align   ///< required alignment, a power of two
)
{
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    /* Room for the worst-case adjustment plus the stashed original. */
    size_t total = sz + align + sizeof(void *);
    if (total < sz) {
        return nullptr;                     /* size_t overflow */
    }
    void *raw = axl_malloc(total);
    if (raw == nullptr) {
        return nullptr;
    }
    uintptr_t base    = (uintptr_t) raw + sizeof(void *);
    uintptr_t aligned = (base + align - 1) & ~(uintptr_t) (align - 1);
    ((void **) aligned)[-1] = raw;
    return (void *) aligned;
}

void
aligned_free_impl(
    void *p   ///< pointer from aligned_alloc_impl (NULL-safe)
)
{
    if (p == nullptr) {
        return;
    }
    axl_free(((void **) p)[-1]);
}

} // namespace

void *
operator new(size_t sz, std::align_val_t al)
{
    void *p = aligned_alloc_impl(sz, (size_t) al);

    if (p == nullptr) {
        std::__throw_bad_alloc();
    }
    return p;
}

void *
operator new[](size_t sz, std::align_val_t al)
{
    void *p = aligned_alloc_impl(sz, (size_t) al);

    if (p == nullptr) {
        std::__throw_bad_alloc();
    }
    return p;
}

void *
operator new(size_t sz, std::align_val_t al, const std::nothrow_t &) noexcept
{
    return aligned_alloc_impl(sz, (size_t) al);
}

void *
operator new[](size_t sz, std::align_val_t al, const std::nothrow_t &) noexcept
{
    return aligned_alloc_impl(sz, (size_t) al);
}

void
operator delete(void *p, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void *p, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete(void *p, size_t /*sz*/, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void *p, size_t /*sz*/, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    aligned_free_impl(p);
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
    axl_print("[axl-cxxabi] __cxa_pure_virtual called - aborting\r\n");
    axl_exit(1);
}

// ---------------------------------------------------------------------------
// abort
// ---------------------------------------------------------------------------
//
// Under -fno-exceptions, libstdc++ lowers EVERY throw site to a call to
// abort() -- not just the obvious ones. `std::expected::value()` on an error,
// `std::optional::value()` on an empty, `std::get` on the wrong variant
// alternative: all of them, and more we cannot enumerate ahead of time.
//
// So the choice is not "should .value() work". It is whether an arbitrary
// future use of a freestanding std header fails to LINK with an undefined
// reference that names nothing a caller recognises, or halts with a message
// that says what happened. Defining it once, loudly, is the maintainable half
// of that -- and it is why the C++ layer can hand out std::expected at all
// (see axl::result in <axl/axl-cxx.hpp>).
//
// It is still a crash, and the header says so. AXL_NORETURN because the
// compiler assumes throw sites do not return; letting this one return would
// resume execution in a state the optimizer already proved unreachable.
//
// Lives here rather than in compat/stdlib.h because it is a C++-lowering
// concern: a pure-C consumer links libaxl.a alone and must not acquire an
// `abort` symbol it never asked for.

extern "C" AXL_NORETURN void
abort(void)
{
    axl_print("[axl-cxxabi] abort() - a -fno-exceptions throw site was "
              "reached (bad optional/expected/variant access?)\r\n");
    axl_exit(1);
    __builtin_unreachable();
}

// ---------------------------------------------------------------------------
// std::__throw_*
// ---------------------------------------------------------------------------
//
// The container headers do not `throw` inline; they CALL these. That call is
// emitted whether or not exceptions are enabled -- only the throw INSIDE the
// function is conditional -- so under -fno-exceptions the symbols still have
// to be satisfied by somebody.
//
// If that somebody is libstdc++.a, its functexcept.o arrives compiled WITH
// exceptions and brings the unwinder, std::logic_error, and the locale
// machinery behind it: precisely the cascade that makes people believe the
// standard containers are unreachable from firmware. Defining them here in
// libaxl-cxx.a -- which axl-cc places ahead of libstdc++.a -- means that
// member is never pulled, and the container half of libstdc++ links with two
// archive members and no undefined symbols.
//
// Five, not "some": <string>'s append/replace/insert reach length_error and
// logic_error, at() reaches out_of_range_fmt, and any allocation path reaches
// bad_alloc / bad_array_new_length.
//
// AXL_NORETURN is load-bearing, not documentation. gcc compiles the code
// after a throw site on the assumption it is unreachable; a stub that
// RETURNED would resume there, in a state no analysis modelled.

namespace {

AXL_NORETURN void
throw_halt(
    const char *fn,    ///< the std:: function that was called
    const char *what   ///< its message, already formatted
)
{
    axl_printf("[axl-cxxabi] std::%s() - %s\r\n", fn, what);
    axl_exit(1);
    __builtin_unreachable();
}

} // namespace

namespace std {

AXL_NORETURN void
__throw_bad_alloc(void)
{
    throw_halt("__throw_bad_alloc", "out of memory");
}

AXL_NORETURN void
__throw_bad_array_new_length(void)
{
    throw_halt("__throw_bad_array_new_length", "array new length is invalid");
}

AXL_NORETURN void
__throw_length_error(
    const char *msg
)
{
    throw_halt("__throw_length_error", msg);
}

AXL_NORETURN void
__throw_logic_error(
    const char *msg
)
{
    throw_halt("__throw_logic_error", msg);
}

/* libstdc++ passes a printf-style literal here plus the offending values --
 * "__n (which is %zu) >= this->size() (which is %zu)". Formatting it is the
 * difference between a message naming the bad index and one that only names
 * the check. The format string is a compile-time literal from the library,
 * never caller data. */
AXL_NORETURN void
__throw_out_of_range_fmt(
    const char *fmt,
    ...
)
{
    char    buf[192];
    va_list args;

    va_start(args, fmt);
    axl_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    throw_halt("__throw_out_of_range_fmt", buf);
}

/* The UNFORMATTED sibling. Distinct symbol, and not interchangeable: with
   only the _fmt form defined, std::vector::at linked and std::map::at did
   not -- the containers pick different entry points for the same condition,
   and the missing one is a link error a consumer meets, not us. */
AXL_NORETURN void
__throw_out_of_range(
    const char *msg
)
{
    throw_halt("__throw_out_of_range", msg);
}

/* std::stoi / std::stod on unparseable text. */
AXL_NORETURN void
__throw_invalid_argument(
    const char *msg
)
{
    throw_halt("__throw_invalid_argument", msg);
}

/* Reached through libstdc++'s inline `std::__terminate()`, which several
 * `noexcept` helpers in <bits/char_traits.h> and <string_view> call on a
 * path the standard says cannot be recovered from. Freestanding
 * <string_view> is otherwise self-contained, so this one symbol is the
 * difference between axl::string's search family linking and not -- and
 * without it here, libstdc++.a's eh_terminate.o supplies it and brings the
 * unwinder with it, which is the whole cascade this file exists to prevent. */
void
terminate(void) noexcept
{
    throw_halt("terminate", "unrecoverable error in a noexcept context");
}

} // namespace std
