/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-handle-selftest.cpp
    #axl::unique_handle owning a real AXL object under UEFI.

    `make check-handle-exclusions` proves the compile-time contract: which
    types get a handle, which are refused, and what the refusal says. It
    cannot see the one thing that matters at run time — that the deleter
    reaches the destroy function the header named, on a real object, and
    that the memory actually comes back.

    Every case is a DELTA against a baseline taken with the handle out of
    scope, and each prints `alive=1` before it prints `freed=1`. The
    `alive` half is not decoration: if the allocation counter were inert
    (a build with no accounting, say) then `freed=1` would hold trivially
    for a deleter that never ran at all, and this file would be a test
    that cannot fail. Watching the counter MOVE first is what makes the
    return to baseline mean something.
**/

#include <utility>

#include <axl.h>
#include <axl/axl-handle.hpp>

static size_t
live(void)
{
    AxlMemStats st;

    axl_mem_get_stats(&st);
    return st.count;
}

/* Returned BY VALUE — the case AXL_AUTOPTR cannot express at all, since a
   cleanup attribute fires when the factory's own scope exits. */
static axl::unique_handle<AxlLoop>
make_loop(void)
{
    return axl::unique_handle<AxlLoop>{axl_loop_new()};
}

/* And held as a MEMBER, the other case the attribute cannot express. */
struct Holder {
    axl::unique_handle<AxlLoop> loop;

    explicit Holder(void)
        : loop(make_loop())
    {
    }
};

int
main(void)
{
    /* 1. Scope exit frees, and the counter moves in both directions. */
    size_t base = live();
    {
        axl::unique_handle<AxlLoop> loop{axl_loop_new()};
        axl_printf("handle: scope alive=%d\r\n", (int)(live() > base));
    }
    axl_printf("handle: scope freed=%d\r\n", (int)(live() == base));

    /* 2. A move transfers ownership; it must not double free, and the
          moved-from handle must destroy cleanly (every bound destroy is
          NULL-safe). */
    base = live();
    {
        axl::unique_handle<AxlLoop> a{axl_loop_new()};
        axl::unique_handle<AxlLoop> b{std::move(a)};
        axl_printf("handle: move alive=%d\r\n", (int)(live() > base));
        axl_printf("handle: move src-empty=%d\r\n", (int)(a.get() == nullptr));
    }
    axl_printf("handle: move freed=%d\r\n", (int)(live() == base));

    /* 3. Returned from a factory and held as a class member. */
    base = live();
    {
        Holder h;
        axl_printf("handle: member alive=%d\r\n", (int)(live() > base));
        axl_printf("handle: member usable=%d\r\n", (int)(h.loop.get() != nullptr));
    }
    axl_printf("handle: member freed=%d\r\n", (int)(live() == base));

    /* 4. release() hands ownership back — nothing is freed until the
          caller does it, which is how a handle escapes into a C API that
          takes ownership. */
    base = live();
    {
        axl::unique_handle<AxlLoop> loop{axl_loop_new()};
        AxlLoop *raw = loop.release();
        axl_printf("handle: release disarmed=%d\r\n", (int)(live() > base));
        axl_loop_free(raw);
    }
    axl_printf("handle: release freed=%d\r\n", (int)(live() == base));

    /* 5. A default-constructed handle destroys cleanly (NULL-safe). */
    {
        axl::unique_handle<AxlLoop> empty;
        axl_printf("handle: empty null=%d\r\n", (int)(empty.get() == nullptr));
    }

    axl_printf("handle: done\r\n");
    return 0;
}
