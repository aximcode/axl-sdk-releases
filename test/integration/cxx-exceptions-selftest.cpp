/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * cxx-exceptions-selftest.cpp — real try/catch under UEFI.
 *
 * Built by test-cxx-exceptions-qemu.sh with `axl-c++ -fexceptions`, which is
 * the whole opt-in: axl-cc sees the flag, selects the exceptions linker script
 * (which KEEPs .eh_frame and defines __eh_frame_start), adds -j .eh_frame -j
 * .gcc_except_table to objcopy, and links the toolchain's libstdc++/libsupc++/
 * libgcc plus AXL's glue objects -- which is what every C++ link carries
 * since P4. What -fexceptions adds is landing pads in this file's own
 * frames, so a throw here is CAUGHT instead of reaching std::terminate.
 *
 * NOTHING HERE CALLS axl_cxxrt_init(). That is deliberate and is half of what
 * this fixture proves: the frame table has to be registered before any throw,
 * AND before global constructors, since a constructor may throw. AXL does it
 * from _axl_cxxabi_run_init_array via a weak reference. A fixture that
 * registered the table itself would pass with that hook broken.
 */
#include <axl.h>

#include <stdexcept>
#include <string>
#include <vector>

static int passed;
static int failed;

static void
check(bool ok, const char *what)
{
    axl_printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

// ---------------------------------------------------------------------------
// A global object whose constructor throws and catches, entirely before main.
// ---------------------------------------------------------------------------
//
// This is the ordering assertion. If the frame table were registered from
// main() -- or from anywhere after the .init_array walk -- this throw would
// find no handler and terminate before a single line printed.
static bool gCtorCaught;

struct ThrowsInCtor {
    ThrowsInCtor()
    {
        try {
            throw std::runtime_error("from a global constructor");
        } catch (const std::exception &) {
            gCtorCaught = true;
        }
    }
};

static ThrowsInCtor gEarly;

// ---------------------------------------------------------------------------
// Unwind depth: three frames, each with a destructor that must run.
// ---------------------------------------------------------------------------

struct Tracer {
    int  *counter;
    explicit Tracer(int *c) : counter(c) {}
    ~Tracer() { (*counter)++; }
};

static void
level3(int *dtors)
{
    Tracer t(dtors);
    throw std::out_of_range("deep");
}

static void
level2(int *dtors)
{
    Tracer t(dtors);
    level3(dtors);
}

static void
level1(int *dtors)
{
    Tracer t(dtors);
    level2(dtors);
}

// A rethrow has to preserve the exception OBJECT, not just its type.
static void
rethrower(void)
{
    try {
        throw std::logic_error("original");
    } catch (const std::exception &) {
        throw;
    }
}

int
main(void)
{
    check(gCtorCaught, "a global constructor threw and caught, before main");

    int dtors = 0;
    bool caught_type = false;
    try {
        level1(&dtors);
    } catch (const std::out_of_range &e) {
        caught_type = axl_strcmp(e.what(), "deep") == 0;
    }
    check(caught_type, "caught by exact type across three frames");
    // Exactly three, not "at least": a unwinder that ran a destructor twice
    // is as broken as one that skipped it, and >= would hide the first.
    axl_printf("  dtors=%d\n", dtors);
    check(dtors == 3, "every destructor on the unwind path ran exactly once");

    bool rethrown = false;
    try {
        rethrower();
    } catch (const std::logic_error &e) {
        rethrown = axl_strcmp(e.what(), "original") == 0;
    }
    check(rethrown, "a rethrow preserves the original exception object");

    bool any = false;
    try {
        throw 42;
    } catch (...) {
        any = true;
    }
    check(any, "catch(...) catches a non-class type");

    // Ordering: an inner handler that does not match must not swallow.
    bool outer = false;
    try {
        try {
            throw std::runtime_error("passes through");
        } catch (const std::bad_alloc &) {
            /* wrong type: must NOT run */
            outer = false;
        }
    } catch (const std::runtime_error &) {
        outer = true;
    }
    check(outer, "a non-matching handler does not intercept");

    // Throwing through a std::vector's element destructors.
    int vec_dtors = 0;
    bool vec_ok = false;
    try {
        std::vector<Tracer> v;
        v.reserve(4);
        for (int i = 0; i < 3; i++) {
            v.emplace_back(&vec_dtors);
        }
        throw std::runtime_error("through a container");
    } catch (const std::exception &) {
        vec_ok = (vec_dtors == 3);
    }
    axl_printf("  vec_dtors=%d\n", vec_dtors);
    check(vec_ok, "container elements destruct during unwind");

    axl_printf("=== %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
