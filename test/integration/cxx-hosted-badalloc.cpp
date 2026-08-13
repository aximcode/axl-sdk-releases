/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-hosted-badalloc.cpp
    A container allocation that fails must HALT, not return NULL.

    `__new_allocator::allocate` hands `operator new`'s result straight
    to the container with no null check — the standard guarantees the
    throwing form never returns NULL, and libstdc++ takes that
    literally. So an `operator new` that returns NULL on exhaustion
    turns an out-of-memory into a `#PF` near address 0, somewhere
    inside a container template, with nothing pointing back at the
    allocation.

    `axl_mem_fail_next_alloc(1)` makes that deterministic: the next
    allocation through AxlMem returns NULL without touching the
    backend, and it works in RELEASE builds too. The vector's first
    allocation is the one that fails.

    Its sibling `cxx-hosted-throw.cpp` covers the other halt path,
    `__throw_out_of_range_fmt`.
**/

#include <vector>

#include <axl.h>

int
main(void)
{
    std::vector<int> v;

    axl_print("cxx-badalloc: about to allocate with OOM injected\r\n");
    axl_mem_fail_next_alloc(1);
    v.reserve(64);
    axl_printf("cxx-badalloc: UNREACHABLE - reserve returned, data=%p\r\n",
               (void *) v.data());
    return 0;
}
