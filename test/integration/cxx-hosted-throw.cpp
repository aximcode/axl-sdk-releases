/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-hosted-throw.cpp
    Reach a `-fno-exceptions` throw site on purpose.

    `std::vector::at()` out of range calls `std::__throw_out_of_range_fmt`.
    With exceptions off that call is still EMITTED — only the throw inside it
    is not — so the symbol has to be satisfied by somebody. This fixture
    proves it is satisfied by AXL's stub: the image halts, names the function,
    and does NOT return into the code after the call, which the optimizer has
    already proved unreachable.

    The index comes through a `volatile` so the compiler cannot fold the
    bounds check away at compile time and turn this into a fixture that
    exercises nothing.
**/

#include <vector>

#include <axl.h>

int
main(void)
{
    std::vector<int> v{1, 2, 3};
    volatile int idx = 99;

    axl_print("cxx-throw: about to call vector::at(99) on a 3-element vector\r\n");
    int got = v.at((size_t) idx);
    axl_printf("cxx-throw: UNREACHABLE - at() returned %d\r\n", got);
    return 0;
}
