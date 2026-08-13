/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx.hpp
 *
 * The C++ layer's foundation: the `axl` namespace, the error vocabulary, and
 * the conventions everything under it obeys. Include this rather than
 * restating any of it.
 *
 * This is the FIRST `.hpp` in the SDK. Everything else in `include/axl/` is a
 * C header that happens to be callable from C++; this one is C++-only, and
 * consuming it needs `axl-c++` (or any g++ with the SDK's `-std=c++23
 * -ffreestanding -fno-exceptions -fno-rtti` flag set). See
 * `docs/AXL-Cxx-Design.md`.
 *
 * @par We write neither containers nor algorithms
 *
 * GCC 14 ships the C++23 freestanding library subset (P1642), verified running
 * under UEFI: `<algorithm>`, `<ranges>`, `<iterator>`, `<concepts>`,
 * `<expected>`, `<memory>`, `<span>` and more all work, and `std::sort` plus a
 * `views::filter` pipeline costs ~545 bytes. So a type here supplies
 * `begin()`/`end()` and INHERITS every standard algorithm and view. There is
 * no `axl::sort`, no `axl::find`, and there should never be one.
 *
 * The containers go the same way, and an earlier revision of this file said
 * the opposite — that `std::vector`, `std::string` and `std::map` "require
 * exceptions and the heap, and are permanently excluded from freestanding".
 * Measurement disagreed on every clause. They do not require exceptions; the
 * gate is `-ffreestanding` itself, enforced by `bits/requires_hosted.h`, and
 * `axl-c++ --hosted` lifts it per translation unit. `std::map<std::string,int>`
 * and a 200-entry `std::unordered_map` were then verified running under UEFI on
 * both arches in a 119 KB image. What the link needs from `libstdc++.a` is two
 * archive members with zero undefined symbols between them — no locale, no
 * iostreams, no unwinder.
 *
 * So there is no `axl::vector` and no `axl::string` either. What this layer
 * supplies is the parts the standard cannot: the error vocabulary below, the
 * runtime hooks in `libaxl-cxx.a` that make the link possible, and
 * #axl::arena_allocator for the one property the standard containers really
 * do cost us — see below.
 *
 * @par The one thing you give up: recoverable OOM
 *
 * A standard container has nowhere to put an allocation failure. Under
 * `-fno-exceptions` it lowers to a halt, and `operator new` may not soften
 * that by returning NULL — libstdc++ hands the result straight to the
 * container without a null check, so NULL buys a `#PF` near address 0 instead
 * of a diagnosable stop.
 *
 * That matters here more than it would elsewhere, because AXL's C side treats
 * OOM as a value: `axl_mem_fail_next_alloc()` is public, the suite carries
 * dozens of OOM assertions, and some are degradation contracts rather than
 * error propagation. On a path that must survive exhaustion, use
 * #axl::arena_allocator — it moves the failure to a fixed-capacity check the
 * caller makes once, up front, where an answer other than "halt" is available.
 *
 * @par Naming
 *
 * Lowercase `axl::`, mirroring the standard: `axl::result`,
 * `axl::arena_allocator`. Files keep the tree's convention
 * (`axl-arena-allocator.hpp`); macros stay `AXL_SCREAMING_CASE`, since
 * namespaces do not contain them.
 *
 * The namespace also dissolves collisions with the C side, which is not
 * hypothetical: `AxlString` is a C type that STAYS, as the streaming builder
 * behind the JSON and XML writer sinks. `axl::` marks which side of the C/C++
 * boundary a reader is on.
 *
 * A deliberate divergence from AGT's namespace-free `AgtButton`. That is right
 * for a FOX-shaped widget toolkit where nothing in C collides; a layer sitting
 * beside `std::` is a different problem.
 *
 * @par Errors are values
 *
 * `-fno-exceptions` is not negotiable here, so a fallible operation returns
 * #axl::result. That matches what the C library already does — errors are
 * QUERIED, never thrown — and it is the standard's own vocabulary rather than
 * an invented one. ETL's answer to this question is no heap at all; EASTL's is
 * an allocator returning null. Ours is a value.
 */

#ifndef AXL_CXX_HPP
#define AXL_CXX_HPP

#ifndef __cplusplus
#error "axl-cxx.hpp is C++ only; C consumers want the axl-*.h headers"
#endif

#include <expected>
#include <utility>

#include <axl/axl-macros.h>

/// Everything the C++ layer provides. See the file docs for the conventions.
namespace axl {

/**
 * The result of a fallible operation: a @a T, or the #AxlStatus that says why
 * not.
 *
 * `AxlStatus` rather than a new enum, so a C++ caller and a C caller
 * distinguish the same outcomes and neither needs a translation table. Its
 * numeric values are part of the C contract and stay that way.
 *
 * @warning `.value()` on an errored result calls `abort()`. The SDK DEFINES
 *     `abort` (see `src/runtime/axl-cxxabi-ops.cpp`) so this is a diagnosable
 *     halt rather than a link failure — but it is still a crash, and
 *     `value_or`, `has_value` or `operator*` after a check are what you want
 *     on any path that can actually fail. This is not specific to
 *     `std::expected`: under `-fno-exceptions` libstdc++ lowers every throw
 *     site the same way, including `std::optional::value()` and `std::get`.
 */
template <class T>
using result = std::expected<T, AxlStatus>;

/**
 * An error result carrying @a s.
 *
 * Spelled out because `std::unexpected` reads as "unexpected" at the call
 * site, where the failure is very much expected and being handled.
 *
 * @return an errored #axl::result carrying @a s.
 */
[[nodiscard]] inline std::unexpected<AxlStatus>
err(
    AxlStatus  s    ///< the failure code; passing #AXL_OK is a caller bug
)
{
    return std::unexpected<AxlStatus>(s);
}

} // namespace axl

#endif // AXL_CXX_HPP
