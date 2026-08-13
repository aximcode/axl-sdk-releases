/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file cxx-rtti-selftest.cpp
    `typeid` and `dynamic_cast`, running under UEFI.

    RTTI is OFF by default — the `type_info` objects cost image size
    for every polymorphic class — but `-frtti` is a supported opt-in
    with `axl-c++ --hosted`, and this proves it rather than assuming
    it. `libstdc++.a` carries the `__cxxabiv1::__class_type_info`
    vtables and `__dynamic_cast`; the only thing that had ever blocked
    them was `__stack_chk_fail`, which `libaxl.a` now defines.

    Freestanding `-frtti` COMPILES and does not LINK, because those
    vtables live in `libsupc++`. The harness asserts that failure too,
    so "RTTI works" cannot quietly come to mean "in whichever mode you
    happened to try".

    Both the positive and the NEGATIVE cast are checked: a
    `dynamic_cast` that always succeeded would satisfy the first and
    is exactly what a broken type_info comparison produces.
**/

#include <typeinfo>

#include <axl.h>

struct Base {
    virtual ~Base() = default;
    virtual int id(void) const { return 1; }
};

struct Derived : Base {
    int v = 42;
    int id(void) const override { return 2; }
};

int
main(void)
{
    Derived  d;
    Base    *b = &d;
    Derived *p = dynamic_cast<Derived *>(b);

    /* A Base that is NOT a Derived: the cast must fail. */
    Base  plain;
    Base *nb = dynamic_cast<Derived *>(&plain) ? (Base *) 1 : nullptr;

    axl_printf("rtti: name=%s cast=%d neg=%d\r\n",
               typeid(*b).name(), p ? p->v : -1, nb == nullptr);
    return 0;
}
