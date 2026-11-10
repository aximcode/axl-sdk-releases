/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-cxx.cpp — the C++ child for test-heap-release-qemu.sh, built
 * twice by the staged axl-c++: default (exceptions script) and --no-eh-frame.
 *
 * It calls nothing from the C library itself. libsupc++'s emergency-pool
 * constructor used to malloc before main in EVERY C++ image, so even this one
 * took the C heap's arena, and must give it back on exit.
 *
 * It also reports, first thing in main, whether a C heap arena exists yet:
 *
 *   HEAPARENA <tag> none|taken
 *
 * <tag> is HEAP_REL_TAG from the build. A --no-eh-frame image must say
 * `none`: the pool can never be used there, and axl-cxxrt-nothrow.o now keeps
 * libsupc++'s eh_alloc.o -- pool, constructor and all -- off the link. The
 * default build says `taken` (its frame registration mallocs), which is the
 * control that the probe sees an arena at all.
 */

#include <stdint.h>

#include <axl.h>

#ifndef HEAP_REL_TAG
#define HEAP_REL_TAG "untagged"
#endif

/* AXL's own (src/cxxrt/axl-cxxrt-alloc.c). sbrk(0) reports the break without
   taking anything: NULL until the first allocation commits the arena. */
extern "C" void *_sbrk(intptr_t increment);

namespace {

struct Global {
    int v;
    Global() : v(42) {}
};

Global g;

} // namespace

int
main()
{
    axl_printf("HEAPARENA %s %s\n", HEAP_REL_TAG,
               _sbrk(0) == nullptr ? "none" : "taken");
    return g.v == 42 ? 0 : 1;
}
