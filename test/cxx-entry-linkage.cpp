/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Regression fixture for `make check-cxx-entry`.
 *
 * A C++ translation unit that emits both firmware entry points AXL provides:
 * AXL_APP's `_AxlEntry` and AXL_DRIVER's `DriverEntry`. The check compiles this
 * and asserts (via nm) that both symbols are UNMANGLED. Without AXL_ENTRY_LINKAGE
 * (the `extern "C"` the macros apply under C++), a C++ compile name-mangles them,
 * and the driver link — which resolves the image entry by exact name
 * (`--defsym=_AxlEntry=DriverEntry`) — fails with an undefined `DriverEntry`. C
 * never hit this because C does not mangle, so no C driver in the tree caught it.
 *
 * The two macros emit distinct symbols (_AxlEntry vs DriverEntry) and are safe to
 * combine in one TU; this is a compile+symbol check, never linked or run. */

#include <axl.h>
#include <axl/axl-driver.h>

static int el_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int el_drv_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;
    return 0;
}

static int el_drv_unload(AxlHandle image)
{
    (void)image;
    return 0;
}

AXL_APP(el_app)
AXL_DRIVER(el_drv_entry, el_drv_unload)
