/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file handle-exclusion-fixture.cpp
    Fixture for `make check-handle-exclusions`.

    #axl::unique_handle is opt-in per type: it resolves its deleter
    through `axl::handle_traits<T>`, which only exists where a header
    invoked AXL_DEFINE_AUTOPTR_CLEANUP. A type whose ownership does not
    fit a unique owner therefore does not compile — that is the design,
    not a gap, and this fixture is what keeps it true.

    Compiled THREE ways by the gate:

      (default)                the bound half must compile clean.
      -DEXPECT_REJECT_SURFACE  AxlSurface must FAIL, naming its reason.
      -DEXPECT_REJECT_JSON     AxlJsonReader must FAIL, naming its reason.

    The rejecting builds are the point, and the gate matches the
    static_assert TEXT rather than just the exit status: a fixture typo
    fails to compile exactly like a working exclusion does, so an
    exit-status-only gate would pass while proving nothing.

    The accepting build is equally load-bearing. Without it the gate
    passes just as well for a header where `handle_traits` was never
    specialized for anything at all — every type would "correctly"
    reject and the feature would be entirely absent.
**/

#include <axl/axl-compositor.h>
#include <axl/axl-handle.hpp>
#include <axl/axl-json.h>
#include <axl/axl-loop.h>
#include <axl/axl-tcp.h>
#include <axl/axl-vterm.h>

/* ---- accepted: types that own themselves ---------------------------- */

/* Plain binding — AXL_DEFINE_AUTOPTR_CLEANUP. */
axl::unique_handle<AxlLoop> g_loop;

/* Argument binding — AXL_DEFINE_AUTOPTR_CLEANUP_ARG. A `template <auto
   Free>` deleter cannot express these at all: axl_tcp_close takes a
   teardown mode, so its arity is 2. Scope exit is graceful, matching
   what the C cleanup attribute passes. */
axl::unique_handle<AxlTcp> g_tcp;

/* The two bindings added with this feature. */
axl::unique_handle<AxlCompositor> g_compositor;
axl::unique_handle<AxlVterm>      g_vterm;

/* No size or indirection cost over the raw pointer: the deleter is
   stateless, so the empty-base optimization applies. A consumer holding
   one as a class member pays exactly what a raw pointer costs. */
static_assert(sizeof(axl::unique_handle<AxlLoop>) == sizeof(AxlLoop *),
              "unique_handle must be pointer-sized");
static_assert(sizeof(axl::unique_handle<AxlTcp>) == sizeof(AxlTcp *),
              "unique_handle must be pointer-sized (arg-bound deleter)");

/* Movable, and returnable from a factory — the whole reason this exists
   alongside AXL_AUTOPTR, which is a scope-exit attribute and can be
   neither. */
static_assert(!std::is_copy_constructible_v<axl::unique_handle<AxlLoop>>,
              "unique ownership must not be copyable");
static_assert(std::is_move_constructible_v<axl::unique_handle<AxlLoop>>,
              "unique_handle must be movable");

static axl::unique_handle<AxlLoop>
make_loop(void)
{
    return axl::unique_handle<AxlLoop>{axl_loop_new()};
}

/* And usable as a class member, which AXL_AUTOPTR cannot be. */
struct Holder {
    axl::unique_handle<AxlLoop> loop{make_loop()};
};

#ifdef EXPECT_REJECT_SURFACE
/* ---- rejected: owned by the compositor's surface tree ---------------- */

/* axl_surface_new returns a BORROWED node in a tree the compositor owns;
   axl_compositor_free destroys every surface in it. Holding both as
   members makes teardown depend on declaration order. Must not compile. */
axl::unique_handle<AxlSurface> g_surface;
#endif

#ifdef EXPECT_REJECT_JSON
/* ---- rejected: a value type, not a handle --------------------------- */

/* axl_json_free takes a caller-owned (normally stack) reader and releases
   its CONTENTS; it does not free the struct. Must not compile. */
axl::unique_handle<AxlJsonReader> g_reader;
#endif
