/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-new-forms.cpp
    Every `new` / `delete` form a consumer can write, as a LINK check.

    Deliberately uses no container, so it compiles both freestanding
    and hosted and the harness can prove the operators resolve in
    BOTH modes. Two of these had no definition anywhere until this
    fixture existed, and both failed only at link — nothing about the
    source looks unusual:

      - `new (std::nothrow) T` needs the `std::nothrow` OBJECT, not
        just the overloads that take it. The object lives in
        libsupc++, which a firmware image does not link.
      - an over-aligned `new` calls a DIFFERENT operator
        (`operator new(size_t, align_val_t)`), chosen by the compiler
        whenever `alignof(T)` exceeds 16. `alignas(64)` on a
        cache-line-sized struct is all it takes.

    Runtime behaviour of the aligned path — that the returned pointer
    is actually aligned and that `delete` recovers the right block —
    is asserted in cxx-hosted-selftest.cpp, which runs.
**/

#include <new>

#include <axl.h>

struct Thing {
    virtual ~Thing() = default;
    int v = 7;
};

struct alignas(64) Cache {
    double d[8];
};

int
main(void)
{
    Thing *t  = new Thing();          /* operator new                     */
    int   *a  = new int[16];          /* operator new[]                   */
    int   *n  = new (std::nothrow) int;   /* + the std::nothrow object    */
    Cache *c  = new Cache();          /* operator new(size_t, align_val_t) */
    Cache *ca = new Cache[2];         /* operator new[](., align_val_t)   */

    char   buf[sizeof(Thing)];
    Thing *p = new (buf) Thing();     /* placement new, from <new>        */

    axl_printf("cxx-new: %d %d %d\r\n", t->v, p->v, n != nullptr);

    p->~Thing();
    delete[] ca;
    delete c;
    delete n;
    delete[] a;
    delete t;
    return 0;
}
