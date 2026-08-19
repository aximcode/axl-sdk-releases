/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Regression fixture for `make check-cxx-entry`.
 *
 * A C++ translation unit that emits the firmware entry points AXL provides.
 * The check compiles this once per DriverEntry-emitting macro and asserts (via
 * nm) that the emitted symbols are UNMANGLED. Without AXL_ENTRY_LINKAGE (the
 * `extern "C"` the macros apply under C++), a C++ compile name-mangles them,
 * and the driver link -- which resolves the image entry by exact name
 * (`--defsym=_AxlEntry=DriverEntry`) -- fails with an undefined `DriverEntry`.
 * C never hit this because C does not mangle, so no C driver in the tree
 * caught it.
 *
 * WHY THREE VARIANTS. Each of AXL_DRIVER / AXL_SHARED_DRIVER /
 * AXL_SERVICE_DRIVER emits the image's single `DriverEntry`, so they cannot
 * share a translation unit -- the gate compiles this file three times, once
 * per AXL_ENTRY_FIXTURE_* define. Covering only AXL_DRIVER is what let
 * AXL_SHARED_DRIVER ship un-compilable from C++ for its whole life: its
 * expansion forward-declares the consumer's `run` WITHOUT AXL_CB_NOEXCEPT and
 * then stores it in a vtable slot that HAS it, which agrees in C and
 * contradicts in C++ (noexcept is part of the function type since C++17).
 *
 * The global below is not decoration. It gives every variant a `.init_array`
 * entry, which is what the runtime half of this contract
 * (test-cxx-driver-ctors-qemu.sh) then proves actually RUNS -- a driver image
 * registered constructors and called none of them, silently, until
 * axl_driver_init learned to walk the array.
 *
 * Compile-and-inspect only: never linked, never run. */

#include <axl.h>
#include <axl/axl-driver.h>

#if defined(AXL_ENTRY_FIXTURE_SHARED)
#  include <axl/axl-shared-driver.h>
#elif defined(AXL_ENTRY_FIXTURE_SERVICE)
#  include <axl/axl-service.h>
#endif

/* A non-trivial global constructor, so every variant's object carries a
 * .init_array entry for the gate to count. */
struct ElSentinel {
    ElSentinel() : value(0x5A) { }
    int value;
};
static ElSentinel el_sentinel;

int el_sentinel_value(void);
int el_sentinel_value(void)
{
    return el_sentinel.value;
}

#if defined(AXL_ENTRY_FIXTURE_SHARED)

/* AXL_SHARED_DRIVER: the three callbacks. `run` MUST be declared
 * AXL_CB_NOEXCEPT-compatible because the vtable slot it lands in is; `init`
 * and `unload` are called directly by the generated stub and are not. */
static int el_sd_init(void);
static int el_sd_run(int, char **) AXL_CB_NOEXCEPT;
static int el_sd_unload(void);

static int el_sd_init(void) { return 0; }
static int el_sd_run(int argc, char **argv) AXL_CB_NOEXCEPT
{
    (void)argc;
    (void)argv;
    return el_sentinel_value();
}
static int el_sd_unload(void) { return 0; }

AXL_SHARED_DRIVER("el-shared", el_sd_init, el_sd_run, el_sd_unload)

#elif defined(AXL_ENTRY_FIXTURE_SERVICE)

/* AXL_SERVICE_DRIVER: the callbacks land in AxlServiceSetup / AxlServiceTeardown,
 * both of which carry AXL_CB_NOEXCEPT, so a C++ consumer declares them noexcept.
 * The designated initializers must appear in declaration order -- C++ does not
 * allow C's out-of-order form. */
static int el_svc_setup(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT;
static int el_svc_teardown(void *user) AXL_CB_NOEXCEPT;

static int el_svc_setup(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT
{
    (void)loop;
    (void)user;
    return el_sentinel_value() == 0x5A ? 0 : 1;
}
static int el_svc_teardown(void *user) AXL_CB_NOEXCEPT
{
    (void)user;
    return 0;
}

static const AxlService el_service = {
    .name           = "el-service",
    .opts_descs     = nullptr,
    .setup          = el_svc_setup,
    .teardown       = el_svc_teardown,
    .user           = nullptr,
    .driver_tick_ms = 50,
};

AXL_SERVICE_DRIVER(el_service)

#else /* AXL_ENTRY_FIXTURE_DRIVER — the default */

/* AXL_APP rides along in this variant only: it emits `_AxlEntry`, a distinct
 * symbol from `DriverEntry`, so the two are safe to combine and the gate gets
 * its app-side coverage without a fourth compile. */
static int el_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return el_sentinel_value();
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

#endif
