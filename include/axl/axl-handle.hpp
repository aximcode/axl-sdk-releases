/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-handle.hpp
 *
 * Unique ownership of an AXL C handle, for C++ consumers.
 *
 * `AXL_AUTOPTR` already gives C scope-exit cleanup, and it stays exactly
 * as it is. What it cannot do is be a class member, be moved, or be
 * returned from a factory — it is a GCC `cleanup` attribute on a local,
 * so the long-lived cases (a widget's backing buffer, a window's
 * compositor, an editor's piece tree) fall back to a raw pointer and a
 * hand-written destructor. #axl::unique_handle is the same ownership
 * idea for the language where it can be all three.
 *
 * @code
 * #include <axl/axl-handle.hpp>
 * #include <axl/axl-loop.h>          // AND the header declaring your type
 *
 * axl::unique_handle<AxlLoop> loop{axl_loop_new()};
 * axl_loop_run(loop.get());          // the C API, unchanged
 * @endcode
 *
 * @par Include this header AND the one that declares your type
 *
 * This header pulls in `<memory>` and `<axl/axl-macros.h>` and nothing
 * else. The trait for a type is emitted where that TYPE is declared, so
 * `axl::unique_handle<AxlVterm>` needs `<axl/axl-vterm.h>` as well —
 * which a consumer usually already includes to call the C API.
 *
 * The deliberate part is what this header does NOT do: pulling every
 * subsystem header in so that `unique_handle<T>` always resolves would
 * make one `#include` here drag the whole SDK surface into any header
 * that mentions a handle. A consumer that tried the transitive shape
 * measured its public-header include weight growing about 13x, because
 * the handle header became reachable from effectively everything.
 *
 * Missing it is a compile error, not a silent failure — `'AxlVterm' was
 * not declared in this scope` — but that message names the symptom
 * rather than the rule, which is why the rule is written here.
 *
 * The handle stays the C handle: `get()` hands it to any `axl_*`
 * function, `release()` gives ownership back, and the deleter is
 * stateless, so `sizeof(axl::unique_handle<T>) == sizeof(T *)` and the
 * destroy call inlines to the one a hand-written destructor would emit.
 * There is deliberately no wrapper class per handle and no C++ API over
 * the C one — see AXL-Cxx-Design.md §6, which asks the C++ layer to earn
 * its place only where C++ gives something C cannot.
 *
 * @par Opt-in per type, because owning some of them is a bug
 *
 * The deleter resolves through `axl::handle_traits<T>`, which a header
 * emits by invoking `AXL_DEFINE_AUTOPTR_CLEANUP`. So the ~60 types with
 * an autoptr binding get a handle automatically and in the same line
 * that gives C its cleanup attribute, while a type with no binding is a
 * compile error rather than a plausible-looking double free.
 *
 * That is the design, and two types state their reason via
 * #AXL_DEFINE_NO_HANDLE rather than leaving a bare `incomplete type`:
 *
 * - `AxlSurface` — `axl_surface_new` returns a BORROWED node in a tree
 *   the compositor owns, and `axl_compositor_free` destroys every
 *   surface in it. Holding both as members makes correct teardown
 *   depend on declaration order.
 * - `AxlJsonReader` — a caller-owned value struct. `axl_json_free`
 *   releases what the reader holds; it does not free the struct.
 *
 * The same care applies to a pointer the SDK lends you: a buffer from
 * `axl_surface_buffer()` is owned by the surface and has the identical
 * type as one from `axl_gfx_buffer_new()`, so no template can tell them
 * apart. Own what a `_new` / `_open` gave you, and nothing else.
 *
 * @par No shared form
 *
 * There is no `shared_handle`. AXL's ownership is a tree, and a
 * refcounted handle would invite exactly the shared ownership the
 * design avoids. The one refcounted type, `AxlBytes`, is bound to
 * `axl_bytes_unref`, so a #axl::unique_handle over it owns ONE
 * reference — which is what unique ownership of a reference means.
 */

#ifndef AXL_HANDLE_HPP
#define AXL_HANDLE_HPP

#include <memory>

#include <axl/axl-macros.h>

namespace axl {

/**
 * Deleter for #axl::unique_handle: forwards to the destroy function the
 * type's header named. Stateless, so it costs nothing.
 */
template <class T>
struct handle_deleter {
    void
    operator()(T *p) const noexcept
    {
        handle_traits<T>::destroy(p);
    }
};

/**
 * A `std::unique_ptr` over an AXL handle, released by the same function
 * `AXL_AUTOPTR` would call.
 *
 * Movable, returnable from a factory, and usable as a class member.
 * Every `axl_*_free` AXL binds is NULL-safe, so a moved-from or default
 * handle destroys cleanly.
 */
template <class T>
using unique_handle = std::unique_ptr<T, handle_deleter<T>>;

} // namespace axl

#endif /* AXL_HANDLE_HPP */
