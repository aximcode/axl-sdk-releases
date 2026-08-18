/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * cxx-terminate-selftest.cpp — reaching std::terminate on purpose, three ways.
 *
 * The sibling fixture (cxx-exceptions-selftest.cpp) proves every throw finds
 * its handler. This one proves what happens when none does, which is the path
 * a consumer meets by accident and the only one that has to explain itself.
 *
 * libstdc++'s own answer is __gnu_cxx::__verbose_terminate_handler, and under
 * UEFI it prints NOTHING: its output goes to a newlib stderr no UEFI image
 * wires up. It still costs ~112 KB in every -fexceptions image, because it
 * drags __cxa_demangle and newlib's stdio. AXL preempts it
 * (src/cxxrt/axl-cxxrt-terminate.cpp), so this fixture asserts the replacement
 * both SPEAKS and names the exception.
 *
 * ONE FIXTURE, THREE BUILDS, selected by -DTERM_CASE=n — because each case
 * ends the image, so they cannot share a run. One file rather than three keeps
 * the three expected shapes side by side where a reader compares them.
 *
 *   1  a std::exception  -> type name AND what()
 *   2  a bare int        -> type name, and the handler SAYS there is no what()
 *   3  nothing in flight -> neither; a different bug, reported differently
 *
 * Case 2 is not a curiosity: it is the branch that keeps an uncaught non-std
 * throw diagnosable instead of escaping the handler into libsupc++'s abort.
 *
 * Every string the test matches is byte-for-byte, including the MANGLED type
 * names (St13runtime_error, i) — we do not link a demangler, and that is the
 * point of the whole change.
 *
 * There is no `return` after any case and none is missing: control cannot
 * reach the end of main.
 */
#include <axl.h>

#include <exception>
#include <stdexcept>

#ifndef TERM_CASE
#define TERM_CASE 1
#endif

int
main(void)
{
#if TERM_CASE == 1
    axl_printf("cxx-terminate: throwing a std::runtime_error with no handler\n");
    throw std::runtime_error("a deliberate uncaught error");
#elif TERM_CASE == 2
    /* Through a volatile so the value cannot be folded into something the
       optimizer proves unreachable, the same guard cxx-hosted-throw.cpp uses
       on its index. */
    volatile int code = 42;

    axl_printf("cxx-terminate: throwing a bare int with no handler\n");
    throw (int) code;
#elif TERM_CASE == 3
    axl_printf("cxx-terminate: calling std::terminate with nothing in flight\n");
    std::terminate();
#else
#error "TERM_CASE must be 1, 2 or 3"
#endif
}
