/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-hosted-badalloc.cpp
    A container allocation that fails must not return NULL to the container.

    `__new_allocator::allocate` hands `operator new`'s result straight to
    the container with no null check — the standard guarantees the throwing
    form never returns NULL, and libstdc++ takes that literally. So an
    `operator new` that returns NULL on exhaustion turns an out-of-memory
    into a `#PF` near address 0, somewhere inside a container template,
    with nothing pointing back at the allocation.

    HOW THE FAILURE IS PRODUCED CHANGED AT P4, and the reason is worth
    recording because the old mechanism failed SILENTLY rather than
    breaking. This used to call `axl_mem_fail_next_alloc(1)`, which makes
    the next allocation through AxlMem return NULL. That worked while
    `operator new` was AXL's own and forwarded to `axl_malloc`. P4 deletes
    that operator and takes libstdc++'s, which reaches `malloc` — newlib's
    dlmalloc, a different allocator with a different namespace (§2-DECISION
    of AXL-Libc-Substrate-Design.md). The injection therefore stopped
    reaching the allocation under test, `reserve(64)` simply SUCCEEDED, and
    the fixture printed its own UNREACHABLE line.

    An sbrk-level injection would not fix that either: dlmalloc satisfies a
    256-byte request from its existing top chunk without calling `sbrk` at
    all, so the knob would be bypassed for exactly the small allocations a
    container makes.

    So the request is made genuinely unsatisfiable instead. 2^45 `int`s is
    128 TiB, which no `sbrk` can serve — and it is far BELOW
    `vector::max_size()` (2^61 here, asserted below), which matters: above
    that boundary `reserve` throws `length_error` from a bounds check and
    never reaches `operator new`, so the fixture would exercise a different
    path while still appearing to halt correctly.

    The size comes through a `volatile` so the compiler cannot fold the
    call away at compile time.

    Its sibling `cxx-hosted-throw.cpp` covers the other halt path,
    `vector::at()` out of range.
**/

#include <vector>

#include <axl.h>

int
main(void)
{
    std::vector<int> v;

    /* 2^45 ints = 128 TiB: unsatisfiable, and well below max_size(). Printed
       rather than asserted silently, so a future libstdc++ that changes
       max_size() shows the two numbers side by side instead of turning this
       into a length_error test that still looks like it passes. */
    volatile size_t want = (size_t) 1 << 45;

    axl_printf("cxx-badalloc: max_size=%llu request=%llu\r\n",
               (unsigned long long) v.max_size(),
               (unsigned long long) want);
    axl_print("cxx-badalloc: about to allocate more than any heap can serve\r\n");

    v.reserve((size_t) want);

    axl_printf("cxx-badalloc: UNREACHABLE - reserve returned, data=%p\r\n",
               (void *) v.data());
    return 0;
}
