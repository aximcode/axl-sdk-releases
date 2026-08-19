/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * cxx-ctor-driver.cpp — a C++ driver image whose global constructor must run.
 *
 * ONE SOURCE, THREE IMAGES. Each of AXL_DRIVER / AXL_SHARED_DRIVER /
 * AXL_SERVICE_DRIVER emits the image's single `DriverEntry`, so they cannot
 * share a translation unit; the build compiles this file once per
 * AXL_CTOR_FIXTURE_* define. Driven by test-cxx-driver-ctors-qemu.sh, which
 * loads all three from cxx-ctor-test.efi.
 *
 * WHAT IT PINS. Before the fix, a driver image had `__init_array_start` /
 * `__init_array_end` bracketing real constructors and NO code that ever walked
 * them: `AXL_APP` reaches the walker through `_axl_init`, `AXL_DRIVER` reaches
 * `axl_driver_init`, and only the first called `_axl_cxxabi_run_init_array`.
 * No link error, no warning, no diagnostic -- every global with a non-trivial
 * constructor was simply unconstructed.
 *
 * WHY THE ASSERTIONS LOOK OVER-BUILT. crt0 zeroes .bss, so an unconstructed
 * global reads all-zero rather than as garbage: the symptom is a plausible
 * wrong value far from the cause, which is the same shape as the
 * bss-not-zeroed crt0 regression (f7886537). So the fixture prints three
 * things, not one:
 *
 *   magic  written ONLY by the constructor -- 0 means it never ran
 *   runs   incremented ONLY by the constructor -- a count, not a flag, which
 *          is what makes the RELOAD case assertable (a second load must
 *          report 1 again, never 2)
 *   bss    written by NOTHING, and declared `volatile` so the compiler cannot
 *          fold the read to a constant -- must stay 0, which is what proves
 *          .bss really was zeroed and `magic` is therefore a real assertion
 *          rather than a coin flip on the day someone picks 0 as the magic
 *          value. WITHOUT `volatile` this field is eliminated and prints an
 *          immediate 0 that cannot fail; see its declaration below.
 *
 * The destructor's line is the other half of the contract: static destructors
 * register through __cxa_atexit -> axl_atexit (measured on both arches -- a
 * C++ static destructor emits NO .fini_array entry, it registers at run time),
 * and `axl_atexit` refuses registration while its table is uninitialised.
 * Walking .init_array without also initialising that table would have left
 * every destructor silently unregistered -- a new instance of the very bug
 * being fixed.
 */

#include <axl.h>

#if defined(AXL_CTOR_FIXTURE_SHARED)
#  include <axl/axl-shared-driver.h>
#  define CTOR_TAG "CTORSD"
#elif defined(AXL_CTOR_FIXTURE_SERVICE)
#  include <axl/axl-service.h>
#  define CTOR_TAG "CTORSVC"
#elif defined(AXL_CTOR_FIXTURE_FAILENTRY)
#  include <axl/axl-driver.h>
#  define CTOR_TAG "CTORFE"
#elif defined(AXL_CTOR_FIXTURE_FAILUNLOAD)
#  include <axl/axl-driver.h>
#  define CTOR_TAG "CTORFU"
#else
#  include <axl/axl-driver.h>
#  define CTOR_TAG "CTORDRV"
#endif

/* Not 0, not 1, and not a value .bss zeroing or a stray memset produces. */
#define CTOR_MAGIC 0x5A5AC0DEu

/* Incremented by the constructor below and by nothing else. */
static unsigned g_ctor_runs;

/* Written by NOTHING -- and `volatile` is what makes that observable rather
 * than a lie. Without it the compiler proves nothing writes the object,
 * eliminates it, and folds the read below to an immediate 0: measured, the
 * symbol is absent from the object entirely and the argument compiles to
 * `mov $0x0,%r8d` with no memory operand, at -Og and every level above it.
 * The field would then print 0 whatever .bss actually held, which is the
 * `test_check(true, ...)` shape this project forbids -- and it was worse than
 * an ordinary tautology, because three comment blocks described it as the
 * load-bearing evidence. `volatile` forces the real load back. */
static volatile unsigned g_never_written;

struct CtorSentinel {
    CtorSentinel()
        : magic(CTOR_MAGIC)
    {
        g_ctor_runs++;
    }

    ~CtorSentinel()
    {
        axl_printf(CTOR_TAG ": DTOR\n");
    }

    unsigned magic;
};

static CtorSentinel g_sentinel;

/* Report from wherever the macro under test hands control to consumer code.
 * @a where names that site so one serial log distinguishes the three images'
 * entry points. */
static void
ctor_report(const char *where)
{
    axl_printf(CTOR_TAG ": %s magic=0x%08x runs=%u bss=%u\n",
               where, g_sentinel.magic, g_ctor_runs, g_never_written);
}

#if defined(AXL_CTOR_FIXTURE_SHARED)

// ---------------------------------------------------------------------------
// AXL_SHARED_DRIVER — init_fn runs inside DriverEntry, before the publish
// ---------------------------------------------------------------------------

static int ctor_sd_init(void);
static int ctor_sd_run(int, char **) AXL_CB_NOEXCEPT;
static int ctor_sd_unload(void);

static int
ctor_sd_init(void)
{
    ctor_report("init");
    return 0;
}

static int
ctor_sd_run(int argc, char **argv) AXL_CB_NOEXCEPT
{
    (void)argc;
    (void)argv;
    ctor_report("run");
    return 0;
}

static int
ctor_sd_unload(void)
{
    axl_printf(CTOR_TAG ": unload\n");   /* ordering vs the destructor */
    return 0;
}

AXL_SHARED_DRIVER("axl/cxx-ctor", ctor_sd_init, ctor_sd_run, ctor_sd_unload)

#elif defined(AXL_CTOR_FIXTURE_SERVICE)

// ---------------------------------------------------------------------------
// AXL_SERVICE_DRIVER — setup runs from _axl_service_driver_init, which reaches
// axl_driver_init the same way AXL_DRIVER does
// ---------------------------------------------------------------------------

static int ctor_svc_setup(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT;
static int ctor_svc_teardown(void *user) AXL_CB_NOEXCEPT;

static int
ctor_svc_setup(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT
{
    (void)loop;
    (void)user;
    ctor_report("setup");
    return 0;
}

static int
ctor_svc_teardown(void *user) AXL_CB_NOEXCEPT
{
    (void)user;
    /* Printed so the ORDER against the destructor is assertable.
     * AXL_SERVICE_DRIVER has its own copy of the cleanup call (it does not
     * expand to AXL_DRIVER), so the ordering the other fixtures prove does
     * not cover it -- moving that call above the teardown would otherwise
     * pass every assertion. */
    axl_printf(CTOR_TAG ": teardown\n");
    return 0;
}

/* Designated initializers in DECLARATION order — C++ does not allow C's
 * out-of-order form, which is a nuisance a C++ consumer meets here first. */
static const AxlService ctor_service = {
    .name           = "axl/cxx-ctor-svc",
    .opts_descs     = nullptr,
    .setup          = ctor_svc_setup,
    .teardown       = ctor_svc_teardown,
    .user           = nullptr,
    .driver_tick_ms = 50,
};

AXL_SERVICE_DRIVER(ctor_service)

#elif defined(AXL_CTOR_FIXTURE_FAILENTRY)

// ---------------------------------------------------------------------------
// AXL_DRIVER whose ENTRY FAILS — the other teardown path, and the one with no
// second chance. EDK2's CoreStartImage unloads a failed image through
// CoreUnloadAndCloseImage, which never invokes Image->Info.Unload, so the
// unload stub does NOT run. Constructors have already run by then, so without
// an explicit drain on this path a failed load leaks every destructor, the
// atexit table, and the registered .eh_frame table.
//
// The test asserts the DTOR line appears anyway. Before the fix it did not.
// ---------------------------------------------------------------------------

static int
ctor_fe_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;
    ctor_report("entry");
    return AXL_ERR;          /* refuse the load */
}

static int
ctor_fe_unload(AxlHandle image)
{
    (void)image;
    /* Must never be reached — the firmware does not call Unload for an image
     * whose entry failed. Printing here would make that visible rather than
     * letting a wrong assumption hide. */
    axl_printf(CTOR_TAG ": UNLOAD-STUB-RAN\n");
    return AXL_OK;
}

AXL_DRIVER(ctor_fe_entry, ctor_fe_unload)

#elif defined(AXL_CTOR_FIXTURE_FAILUNLOAD)

// ---------------------------------------------------------------------------
// AXL_DRIVER whose FIRST unload FAILS, second succeeds. Pins two claims the
// docstring makes and nothing else exercised:
//
//   1. destructors run ONLY on a successful unload -- a failing unload leaves
//      the image RESIDENT, where destructed globals would be worse than
//      undestructed ones. Dropping the `_rc == 0` guard passes every other
//      assertion in this file, because every other fixture's unload returns 0.
//   2. nothing is lost by that: a RETRIED unload re-enters the stub and
//      drains, which is why success-only is the right trade rather than a
//      leak.
// ---------------------------------------------------------------------------

static unsigned g_unload_attempts;

static int
ctor_fu_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;
    ctor_report("entry");
    return AXL_OK;
}

static int
ctor_fu_unload(AxlHandle image)
{
    (void)image;
    g_unload_attempts++;
    axl_printf(CTOR_TAG ": unload attempt=%u\n", g_unload_attempts);
    return (g_unload_attempts == 1) ? AXL_ERR : AXL_OK;
}

AXL_DRIVER(ctor_fu_entry, ctor_fu_unload)

#else

// ---------------------------------------------------------------------------
// AXL_DRIVER — the bare macro
// ---------------------------------------------------------------------------

static int
ctor_drv_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;
    ctor_report("entry");
    return AXL_OK;
}

static int
ctor_drv_unload(AxlHandle image)
{
    (void)image;
    /* Deliberately BEFORE the destructors: the unload stub runs the consumer's
     * unload first and global destructors after, mirroring an app's
     * main-returns-then-_axl_cleanup order. The test asserts that sequence. */
    axl_printf(CTOR_TAG ": unload\n");
    return AXL_OK;
}

AXL_DRIVER(ctor_drv_entry, ctor_drv_unload)

#endif
