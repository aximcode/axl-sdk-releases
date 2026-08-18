/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cstr.hpp
 *
 * The C-string seam: crossing between AXL's C ownership and a C++ string
 * value, in one call instead of four lines.
 *
 * @code
 * #include <axl/axl-cstr.hpp>
 *
 * std::string      path  = axl::adopt(axl_env_get("PATH"));   // frees for you
 * std::string_view label = axl::view(axl_device_name(dev));   // NULL-safe
 * @endcode
 *
 * @par What the seam is for
 *
 * Roughly twenty public AXL functions hand back a string the caller must
 * release — axl_env_get(), axl_fs_volume_label(), axl_hmac_hex(),
 * axl_driver_config_get(), axl_console_read_line() and their neighbours. In C
 * that is an `AXL_AUTOPTR` and the matter is closed. In C++ it is:
 *
 * @code
 * char *raw = axl_env_get("PATH");            // DON'T
 * std::string path(raw != nullptr ? raw : "");
 * axl_free(raw);
 * @endcode
 *
 * — four lines that leak on any early return between the second and the
 * third, and that get the NULL case wrong the first time somebody writes them
 * from memory (`std::string(nullptr)` is undefined behaviour, not an empty
 * string). #axl::adopt() is that block, correct, as an expression.
 *
 * @par The C allocator stops here
 *
 * The two heaps are genuinely separate and this is the boundary between them:
 * `axl_malloc` reaches `gBS->AllocatePool` and writes an `AXL_MEM_HEADER`
 * ahead of the bytes it returns, while `operator new` reaches libstdc++, then
 * newlib `malloc`, then dlmalloc over `_sbrk`. So a pointer AXL gave you can
 * only be released by axl_free(): handing it to `free` or `delete` makes
 * dlmalloc read a chunk header nobody wrote, and drops the allocation out of
 * the leak accounting the suite gates on.
 *
 * What #axl::adopt() removes is not the axl_free() call — that call is
 * required — but every hand-written instance of it. Past the seam the value is
 * an ordinary C++ object on the ordinary C++ heap, and nothing downstream
 * needs to know which allocator it came from.
 *
 * @par Why a copy, and when to skip it
 *
 * #axl::adopt() copies the bytes because it must: the destination owns its own
 * storage on the other heap, and there is no way to donate an
 * `AllocatePool` block to `std::string`. For a short string the copy is
 * usually free anyway — it lands in the small-string buffer with no allocation
 * at all.
 *
 * When the copy really is unwanted — a large payload read once and scanned in
 * place — do not use this seam. Keep the C pointer in an
 * `axl::unique_handle`-shaped owner or an `AXL_AUTOPTR` local and read it
 * through #axl::view(), which allocates nothing. That is a deliberate gap
 * rather than a missing feature: a "string" that borrows its bytes is a
 * `std::string_view`, and #axl::view() already is one.
 *
 * @par Choosing the destination type
 *
 * #axl::adopt() defaults to `std::string` and is templated on the
 * destination, so `axl::adopt<axl::string>(p)` works too. They differ only in
 * what happens when the copy cannot be allocated, and AXL-Cxx-Design.md §9c is
 * the guidance: `std::string` HALTS, `axl::string` sets a sticky `bad()` the
 * caller can read. Reach for the second on a path that must survive
 * exhaustion. Either way @a p is released.
 *
 * `axl::string` lives in `<axl/axl-string.hpp>`, which this header does not
 * include — including it is how you ask for it.
 *
 * @par Not provided: an `AxlString *` overload
 *
 * `AxlString` stays exactly where AXL-Cxx-Design.md §4.5 puts it, as the
 * streaming builder behind the JSON and XML writer sinks, and a seam to it was
 * scoped for this phase and then dropped on a count: across every C++ tree
 * built on this SDK there are ZERO references to the type. That is §4.4's
 * measured inversion again — the interop that argues loudest for a bridge is
 * the interop nobody is doing. Reading one is `axl::view(axl_string_str(s))`
 * in the meantime, which allocates nothing and is what the overload would have
 * done.
 */

#ifndef AXL_CSTR_HPP
#define AXL_CSTR_HPP

#ifndef __cplusplus
#error "axl-cstr.hpp is C++ only; C consumers want the axl-*.h headers"
#endif

#include <ranges>
#include <string>
#include <string_view>

#include <axl/axl-mem.h>

namespace axl {

/**
 * Borrow a C string as a `std::string_view`, treating NULL as empty.
 *
 * The NULL handling is the entire reason to call this rather than the
 * `string_view` constructor: `std::string_view(nullptr)` is undefined
 * behaviour, and a NULL is the ordinary "absent" answer from most of the C
 * API. Everything else is what the constructor does.
 *
 * Borrows — the bytes must outlive the view, and every mutation of whatever
 * owns them invalidates it.
 *
 * @return a view of @a p, or an empty view if @a p is NULL. Never a view over
 *     a null pointer, so `.data()` on the result is always dereferenceable up
 *     to `.size()`.
 */
[[nodiscard]] inline std::string_view
view(
    const char *p    ///< borrowed C string, or NULL
) noexcept
{
    /* `""` rather than a default-constructed view: that one has a NULL
       data(), which would put the caller back where they started. */
    return std::string_view(p != nullptr ? p : "");
}

/**
 * Take ownership of a string AXL allocated: copy it out, release it, return
 * the copy.
 *
 * @a p is released whether or not it was NULL, so the call is the last legal
 * use of it. Pass only a pointer the C API told you to axl_free() — a string
 * literal, a borrowed `axl_string_str()` result, or a `new`ed buffer will
 * corrupt the heap.
 *
 * The one path on which the release does NOT happen is @a S's constructor
 * halting, which is what `std::string` does when the copy cannot be
 * allocated. Nothing runs after that, so it is not a leak anyone can observe;
 * it is stated because "always frees" would otherwise be read as a guarantee
 * on a path where there is no code left to make one.
 *
 * @tparam S destination string type; `std::string` unless you say otherwise.
 *     `axl::string` is the alternative and the two differ ONLY in their
 *     behaviour when the copy cannot be allocated — see the file docs and
 *     AXL-Cxx-Design.md §9c.
 *
 * @return @a S holding a copy of @a p's bytes, or an empty @a S if @a p is
 *     NULL.
 */
template <class S = std::string>
[[nodiscard]] S
adopt(
    char *p    ///< string to take ownership of, or NULL
)
{
    /* A BORROWING destination would compile and dangle: `adopt<std::
       string_view>(axl_env_get("PATH"))` builds a view over @a p and then
       frees @a p, and the result inspects correctly right up until the bytes
       are reused -- under AXL_MEM_DEBUG they are overwritten with the fill
       pattern, in RELEASE they go back to the UEFI pool. That is the exact
       bug this header exists to remove, so it is refused rather than
       documented. `borrowed_range` is the precise discriminator: true for
       `string_view` and `span`, false for `std::string` and `axl::string`. */
    static_assert(!std::ranges::borrowed_range<S>,
                  "axl::adopt needs a destination that OWNS its bytes -- a "
                  "borrowing one (std::string_view, std::span) would view the "
                  "buffer adopt() frees. To borrow instead, keep the pointer "
                  "in an owner and read it through axl::view().");

    /* The copy stops at the first NUL: axl::view() is
       std::string_view(const char *), which is strlen-bounded. An earlier
       revision of this comment claimed a length-counted view preserved
       embedded NULs -- it does not, and no @a S could have, because the
       length never reaches it. The C functions this seam serves all return
       NUL-terminated strings, so nothing is lost; a counted buffer is a
       different call that does not exist yet.

       The free comes AFTER the copy because the copy needs the bytes, which
       makes the ordering worth stating: if S's allocation cannot be served,
       std::string halts here and axl_free never runs. That is not a leak
       worth guarding — the image is going down, and the alternative (copy
       into a temporary, free, then move) allocates twice to protect an
       allocation that already failed. axl::string reaches the free normally,
       because its failure is a value. */
    S out{axl::view(p)};
    axl_free(p);
    return out;
}

} // namespace axl

#endif /* AXL_CSTR_HPP */
