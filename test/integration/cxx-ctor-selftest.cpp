/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-ctor-selftest.cpp
    A C++ global object's constructor must run before `main`.

    It did not, and nothing noticed for as long as the feature has
    existed. `--gc-sections` collected `.init_array` because the
    linker scripts placed it WITHOUT `KEEP()`, and nothing references
    an `.init_array` entry -- that is the entire point of one. The
    crt0 walker then found `__init_array_start == __init_array_end`
    and ran nothing, silently. A global's constructor never fired and
    its members stayed zero, with no diagnostic anywhere.

    `sdk/examples/hello.cpp` carried a comment claiming "Static
    initializer -- proves .init_array runs before main()" over
    `const char *const kDefaultName = "world";`, which is a CONSTANT
    initializer: no constructor, no `.init_array` entry, nothing
    proved. A claim of coverage is not coverage.

    This fixture uses initializers that cannot be folded to constants:

      - a constructor with a SIDE EFFECT (it prints), so a skipped
        run is visible even if the value were somehow right;
      - a value computed from a `volatile`, so the compiler cannot
        constant-fold it into `.data` and pass the test without ever
        emitting an `.init_array` entry at all;
      - ORDER between two objects in the same TU, which is the part a
        walker running the array backwards would still get wrong.
**/

#include <axl.h>

namespace {

volatile int seed = 0x1000;   /* defeats constant folding */

int order_counter = 0;

struct First {
    int magic;
    int order;
    First() : magic(seed + 0xBCD), order(++order_counter)
    {
        axl_print("ctor: First ran\r\n");
    }
};

struct Second {
    int order;
    Second() : order(++order_counter)
    {
        axl_print("ctor: Second ran\r\n");
    }
};

First  first_obj;
Second second_obj;

} // namespace

int
main(void)
{
    axl_printf("ctor: magic=0x%X order=%d,%d count=%d\r\n",
               first_obj.magic, first_obj.order, second_obj.order,
               order_counter);
    axl_print("ctor: done\r\n");
    return 0;
}
