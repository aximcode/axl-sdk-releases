/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cb-noexcept-fixture.cpp
    Fixture for `make check-cb-noexcept`.

    AXL invokes consumer callbacks from its own C frames. Those frames
    are compiled -fno-exceptions and carry no landing pads, so an
    exception unwinding through them runs NO cleanup: every
    AXL_AUTO_FREE in the path leaks, and a RaiseTPL in the path is never
    restored — which wedges the machine on return to the shell, at any
    raised level. AXL_CB_NOEXCEPT makes that a compile error instead of
    a documented hope.

    Compiled TWICE by the gate:

      (default)          the accepting half must compile clean.
      -DEXPECT_REJECT    the rejecting half must FAIL to compile.

    The second build is the whole point. A gate that only checks the
    good case passes just as well for a header where AXL_CB_NOEXCEPT
    expands to nothing.
**/

#include <axl/axl-hash-table.h>
#include <axl/axl-types.h>

/* ---- accepted: a callback that promises not to throw ---------------- */

static void
destroy_ok(void *data) noexcept
{
    (void)data;
}

static int
compare_ok(const void *a, const void *b) noexcept
{
    (void)a;
    (void)b;
    return 0;
}

AxlDestroyNotify g_destroy = destroy_ok;
AxlCompareFunc   g_compare = compare_ok;

/* A lambda is the common consumer spelling and must work too. */
AxlDestroyNotify g_lambda = [](void *) noexcept { };

#ifdef EXPECT_REJECT
/* ---- rejected: a callback that may throw ---------------------------- */

static void
destroy_may_throw(void *data)
{
    (void)data;
}

/* Must not compile: since C++17 noexcept is part of the function type,
   so this conversion is ill-formed. If it ever compiles, the contract
   has silently become advisory again. */
AxlDestroyNotify g_bad = destroy_may_throw;
#endif
