/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-array.hpp
 *
 * Reading a borrowed `AxlArray *` as a `std::span`, so the standard
 * algorithms and views apply to it directly.
 *
 * @code
 * #include <axl/axl-array.hpp>
 * #include <axl/axl-array.h>
 *
 * void render(AxlArray *rows, AxlArray *items)     // both borrowed
 * {
 *     for (int start : axl::array_span<int>(rows)) { ... }
 *
 *     std::span<Item *> its = axl::array_ptr_span<Item>(items);
 *     auto shown = its | std::views::filter([](Item *i) { return i->visible; });
 * }
 * @endcode
 *
 * @par There is no container here, deliberately
 *
 * The phase this header closes was scoped as `axl::c_array_ref`, a view type
 * with an iterator calling axl_array_get() per dereference. It is not here
 * because it stopped being worth writing once axl_array_data() existed: with a
 * base pointer the elements are `T *`, so `std::span` IS the view, its
 * iterators are real pointers rather than a proxy, and AXL-Cxx-Design.md §2's
 * "we write no algorithms" collects the entire `<algorithm>` and `<ranges>`
 * surface for free.
 *
 * The two costs of the proxy shape were both measured, not guessed. §4.1: an
 * out-of-line call plus a bounds check per dereference ran indexed traversal
 * 4.2x and a sort 19.4x slower than the base-pointer loop. §2, second trap: a
 * hand-rolled proxy iterator satisfied `std::sort` and was then REJECTED by
 * `views::filter` for lacking a default constructor, because C++20's iterator
 * concepts are stricter than `iterator_traits`. A `T *` has neither problem.
 *
 * glibmm reached the same place from the other direction — its
 * `Glib::ArrayHandle` family wrapped C arrays for a decade and was deleted in
 * 2.68 in favour of standard containers plus conversion functions.
 *
 * @par Borrowed, and only for the length of a statement
 *
 * A span here is a `(pointer, length)` pair read out of the array at the
 * moment you ask, and it carries axl_array_data()'s invalidation rule
 * unchanged: appending, inserting, prepending, resizing or stealing can move
 * the buffer, and every span over it is dangling afterwards. Removal does not
 * reallocate but does move elements, so a span still stops meaning what it
 * meant. Take the span, use it, drop it; do not store one in a member beside
 * the array it views.
 *
 * This is the reading half of AXL-Cxx-Design.md §4.4's recommendation, and
 * only the reading half. Appending still goes through axl_array_append(),
 * which memcpys — so §4.1's soundness verdict stands and none of this makes
 * an `AxlArray` a place to keep non-trivially-copyable C++ objects.
 *
 * @par Owning an array is a different question
 *
 * These functions view an array somebody else owns. To OWN one from C++, use
 * `axl::unique_handle<AxlArray>` (`<axl/axl-handle.hpp>`) for the C
 * structure, or — where the elements are C++ objects — `std::vector`, which
 * is what §5 recommends and what the memcpy above requires.
 */

#ifndef AXL_ARRAY_HPP
#define AXL_ARRAY_HPP

#ifndef __cplusplus
#error "axl-array.hpp is C++ only; C consumers want axl-array.h"
#endif

#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <span>
#include <utility>

#include <axl/axl-array.h>
#include <axl/axl-stream.h>   /* axl_printf, for the mismatch diagnostic */

namespace axl {

/**
 * Halt, having said which two element sizes disagreed.
 *
 * A stride mismatch is a programming error with no runtime answer: when
 * `sizeof(T)` is the smaller the span reads correct memory as the wrong type,
 * and when it is the larger the span runs off the end of the buffer. Neither
 * can be recovered from, and an empty span — the other candidate — would turn
 * both into a loop that silently does nothing, which in firmware reads as "the
 * list is empty" rather than "the code is wrong".
 *
 * So this halts, the same way `axl::result::value()` on an error does, and it
 * names both sizes first because "which type did you mean" is the only
 * question worth asking afterwards. `T`'s name is not available to print:
 * `-fno-rtti` is the default link.
 *
 * The message goes through axl_printf() rather than axl_error(), matching
 * `src/cxxrt/axl-cxxrt-terminate.cpp`. A log call would be suppressible —
 * `axl_log_set_level()` is public and a consumer quieting their build would
 * silently turn this halt into an unexplained one.
 */
[[noreturn]] inline void
array_size_mismatch(
    const char *who,    ///< the calling factory, for the message
    size_t      want,   ///< the caller's `sizeof(T)`
    size_t      have    ///< the array's actual stride
) noexcept
{
    /* @a who rather than a fixed string: array_ptr_span() reaches here too,
       and naming the wrong function sends the reader to the wrong call site.
       std::abort, not abort -- <cstdlib> only guarantees the qualified name. */
    axl_printf("%s: element size %zu does not match the array's %zu\n",
               who, want, have);
    std::abort();
}

/// Implementation detail of the factories below; not a public surface.
namespace detail {

/**
 * The base pointer and length for a span of @a want-byte elements, after
 * checking the array agrees.
 *
 * Shared by both factories so the NULL rule and the mismatch rule are written
 * once — they differ only in what they name the element type, and a second
 * copy of this is where the two would drift apart.
 */
inline std::pair<void *, size_t>
array_extent(
    const AxlArray *a,     ///< array, or NULL
    size_t          want,  ///< required stride
    const char     *who    ///< calling factory, for a mismatch message
) noexcept
{
    if (a == nullptr) {
        return {nullptr, 0};
    }
    /* The C accessors take `AxlArray *` though none of the three mutates —
       axl-array.h predates any const-correctness pass and changing its
       signatures is a public-API break for every C caller. Absorbing that
       here is the point of a seam: the cast is written once, where it can be
       justified, instead of at every C++ call site that holds a
       `const AxlArray *`. */
    AxlArray *mut = const_cast<AxlArray *>(a);
    const size_t have = axl_array_element_size(mut);
    if (have != want) {
        array_size_mismatch(who, want, have);
    }
    return {axl_array_data(mut), axl_array_len(mut)};
}

/** The largest alignment axl_malloc can deliver.
 *
 * `AllocatePool` is 8-byte aligned and every user pointer sits an
 * `AXL_MEM_HEADER` past it — 56 bytes under AXL_MEM_DEBUG, 8 without, both
 * `8 mod 16`. So an over-aligned `T` is never correctly placed, whatever the
 * pool base happened to be. */
inline constexpr size_t array_max_align = 8;

} // namespace detail

/**
 * View a value-mode `AxlArray` as a `std::span<T>`.
 *
 * @tparam T the element type the array was created to hold; `sizeof(T)` must
 *     equal axl_array_element_size(), and #axl::array_size_mismatch() halts if
 *     it does not.
 *
 * `std::span<T>` converts implicitly to `std::span<const T>`, so a read-only
 * view of a mutable array needs no separate call; the `const AxlArray *`
 * overload below is for when the ARRAY POINTER is const.
 *
 * The span is mutable, so `array_span<int>(a)[0] = 5` writes through into the
 * array's buffer — the same latitude `g_array_index` assignment takes. Note
 * that overwriting an element in place does NOT run an
 * axl_array_set_clear_func(), which axl_array_remove_index() would have; if
 * the element owns anything, release it first.
 *
 * @return a span over the array's elements, or an EMPTY span if @a a is NULL.
 *     A NULL array is the one case that is not treated as a mismatch: it is
 *     the ordinary "absent" answer from the C API and iterating nothing is the
 *     right response to it.
 */
template <class T>
[[nodiscard]] std::span<T>
array_span(
    AxlArray *a    ///< borrowed value-mode array, or NULL
) noexcept
{
    /* Stride alone is not enough. An over-aligned T can MATCH the stride and
       still be misplaced: `struct alignas(16) Vec4 { float x,y,z,w; }` has
       sizeof 16, so axl_array_new(sizeof(Vec4)) passes the check below while
       the buffer is only 8-aligned — and gcc is then entitled to emit an
       aligned vector move over the span, which is a #GP with nothing to
       catch it. Refused at compile time because it is a compile-time
       property; the C side has the same exposure through axl_array_get, but
       this is the call that presents the stride test as THE check. */
    static_assert(alignof(T) <= detail::array_max_align,
                  "T is over-aligned for axl_malloc, which guarantees only "
                  "8 bytes -- an AxlArray cannot store it correctly");
    auto [base, len] = detail::array_extent(a, sizeof(T), "axl::array_span");
    if (base == nullptr) {
        /* Default-constructed rather than span(nullptr, 0): the pointer+count
           constructor's precondition is that [first, first + count) is a
           valid range, which a null pointer only arguably satisfies. Reached
           for a NULL array and for one whose buffer was stolen. */
        return {};
    }
    return std::span<T>(static_cast<T *>(base), len);
}

/**
 * View a `const AxlArray *` as a `std::span<const T>`.
 *
 * The read-only twin, for the very common case of a `const`-qualified member
 * function holding a `const AxlArray *`. Without it that member is unusable
 * from this header and the workaround is a `const_cast` at every call site;
 * the one cast this needs lives in #axl::detail::array_extent instead, with
 * its justification beside it.
 *
 * @return a read-only span, or an EMPTY span if @a a is NULL.
 */
template <class T>
[[nodiscard]] std::span<const T>
array_span(
    const AxlArray *a    ///< borrowed value-mode array, or NULL
) noexcept
{
    static_assert(alignof(T) <= detail::array_max_align,
                  "T is over-aligned for axl_malloc, which guarantees only "
                  "8 bytes -- an AxlArray cannot store it correctly");
    auto [base, len] = detail::array_extent(a, sizeof(T), "axl::array_span");
    if (base == nullptr) {
        return {};
    }
    return std::span<const T>(static_cast<const T *>(base), len);
}

/**
 * Disambiguate a literal `nullptr`.
 *
 * The overload pair above is ambiguous for `nullptr`, which converts equally
 * to both pointer types — and every parameter here is documented "or NULL".
 * Rather than make the docs weaker or the caller write a cast, the literal
 * gets its own overload. It resolves to the mutable form, matching what a
 * NULL `AxlArray *` variable would have selected.
 *
 * @return an empty span.
 */
template <class T>
[[nodiscard]] std::span<T>
array_span(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    return {};
}

/**
 * View a pointer-mode `AxlArray` as a `std::span<T *>`.
 *
 * The elements are the stored pointers themselves, so mutating a span element
 * repoints the array's slot; it does not touch the pointee. See the second
 * warning below before doing that on an owning array.
 *
 * @warning The check this can make is the only one the C API EXPOSES, and it
 *     is weaker than #axl::array_span()'s: pointer mode is `element_size ==
 *     sizeof(void *)`, which a value-mode array of `size_t` or `int64_t`
 *     satisfies just as well. axl_array_set_ptr_free_func() refuses on exactly
 *     this test and inherits exactly this gap — it comes from `GArray` and
 *     `GPtrArray` being one type here (see axl-array.h). Reading a value-mode
 *     `int64_t` array through this call yields garbage pointers and the
 *     library cannot tell. (`struct AxlArray` does record which of the two
 *     cleanup hooks was set, so a definitive answer exists for any array that
 *     has a pointer free func — it is simply not accessible from here.)
 *
 * @warning Assigning to a span element REPLACES the stored pointer. If the
 *     array has an axl_array_set_ptr_free_func(), the pointer that was there
 *     is leaked — the array will free whatever the slot holds at teardown,
 *     which is now the new one.
 *
 * @tparam T the POINTEE type. `array_ptr_span<Item>(a)` yields
 *     `std::span<Item *>`, matching axl_array_get_ptr()'s stored `void *`.
 *
 * @return a span over the stored pointers, or an EMPTY span if @a a is NULL.
 */
template <class T>
[[nodiscard]] std::span<T *>
array_ptr_span(
    AxlArray *a    ///< borrowed pointer-mode array, or NULL
) noexcept
{
    auto [base, len] =
        detail::array_extent(a, sizeof(void *), "axl::array_ptr_span");
    if (base == nullptr) {
        return {};
    }
    return std::span<T *>(static_cast<T **>(base), len);
}

/**
 * View a `const AxlArray *` as a `std::span<T *const>`.
 *
 * The stored pointers cannot be REPOINTED through this span, which is what
 * `const` on the array means; the objects they point at are untouched by it,
 * so `T` is deliberately not `const T` — a const array of mutable items is
 * the usual case.
 *
 * @return a span whose elements cannot be reassigned, or an EMPTY span if
 *     @a a is NULL.
 */
template <class T>
[[nodiscard]] std::span<T *const>
array_ptr_span(
    const AxlArray *a    ///< borrowed pointer-mode array, or NULL
) noexcept
{
    auto [base, len] =
        detail::array_extent(a, sizeof(void *), "axl::array_ptr_span");
    if (base == nullptr) {
        return {};
    }
    return std::span<T *const>(static_cast<T *const *>(base), len);
}

/// @copydoc array_span(std::nullptr_t)
template <class T>
[[nodiscard]] std::span<T *>
array_ptr_span(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    return {};
}

} // namespace axl

#endif /* AXL_ARRAY_HPP */
