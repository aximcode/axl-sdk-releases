/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-list.cpp
    The out-of-line helpers `std::list` and `std::shared_ptr` call, so
    that neither needs `libstdc++.a` -- and, more to the point, so that
    neither drags in the unwinder.

    CLEAN-ROOM under Apache-2.0, written against the layouts and
    prototypes `<bits/stl_list.h>` and `<bits/shared_ptr_base.h>`
    DECLARE.  No GPL source was consulted.  Same footing as
    `axl-cxx-rbtree.cpp` and `axl-cxx-hash.cpp` beside it.

    @par Why this is not "the unwinder project"

    `AXL-Cxx-Stdlib-Surface.md` listed `std::list::sort` and `shared_ptr`
    under tier 2, the ~13 `_Unwind_*` symbols.  Measured, a translation
    unit using both under the SDK's own `-fno-exceptions -fno-rtti`
    references **zero** `_Unwind_*` symbols.  The cascade appeared only
    because the link sourced five functions from libstdc++'s `list.o`
    (4816 bytes), whose undefined set is `__gxx_personality_v0`,
    `_Unwind_Resume`, `__cxa_call_unexpected` and
    `__glibcxx_assert_fail` -- landing pads it carries because IT was
    compiled with exceptions, not because a linked list needs unwinding.

    `shared_ptr.o` (17120 bytes) is worse and matters more: its undefined
    set is `__cxa_throw`, `__cxa_allocate_exception`,
    `__cxa_guard_acquire`/`release`, `__gxx_personality_v0`,
    **`pthread_mutex_lock`/`unlock`**, `typeinfo for std::exception` and
    `vtable for __cxxabiv1::__si_class_type_info`.  Dropping it is what
    keeps pthread and the `__cxa` exception machinery out of a firmware
    image entirely.

    Supplying these seven symbols removes both members, and the cascade
    with them.  See `AXL-Cxx-Unwinder-Design.md`.

    @par A list is a RING

    `std::list` keeps a sentinel header node whose `_M_next` is the first
    element and whose `_M_prev` is the last; an empty list points both at
    itself.  So there are no null links to check, and every operation is
    pointer surgery that must leave the ring consistent in BOTH
    directions.  That is why the tests walk it forwards and backwards: a
    broken `_M_transfer` can leave a list that iterates forward correctly
    and is corrupt in reverse.
**/

#include <stddef.h>

/* Declarations only, so a signature that drifts from libstdc++'s is a
   compile error here rather than a link error in a consumer. Both are
   header-only and pull no libstdc++ objects. */
#include <list>
#include <memory>

namespace std {
_GLIBCXX_BEGIN_NAMESPACE_VERSION

namespace __detail {

/* Insert this node immediately BEFORE @a pos. */
void
_List_node_base::_M_hook(_List_node_base *const pos) noexcept
{
    _M_next             = pos;
    _M_prev             = pos->_M_prev;
    pos->_M_prev->_M_next = this;
    pos->_M_prev        = this;
}

/* Unlink this node. Its own pointers are left dangling -- the caller is
   about to destroy or relink it, which is what libstdc++'s callers do. */
void
_List_node_base::_M_unhook() noexcept
{
    _M_next->_M_prev = _M_prev;
    _M_prev->_M_next = _M_next;
}

/**
 * Move `[first, last)` to just before this node.
 *
 * The engine behind `splice`, and behind `sort`, which is a merge sort
 * that splices runs rather than moving values. Six writes, in an order
 * that never reads a pointer it has already overwritten:
 *
 *   1-3 close the hole the range leaves behind and point the destination's
 *       predecessor at the range;
 *   4-6 rebuild the three `_M_prev` links, staging the destination's old
 *       predecessor in a temporary first because step 5 overwrites it.
 *
 * `this == last` is a self-splice and must be a no-op; without the guard
 * the writes below corrupt the ring.
 *
 * @warning PRECONDITION: `first != last`. An empty range is not merely a
 *     no-op here, it CORRUPTS -- libstdc++'s own version asserts it rather
 *     than handling it. Every in-header caller already guards it
 *     (`splice` checks `__first != __last`, `_M_put_all` checks
 *     `!empty()`, `_M_take_one` passes a one-node range), so this is a
 *     contract to preserve, not a live bug. It is exactly the kind of
 *     detail a clean-room reimplementation loses silently.
 */
void
_List_node_base::_M_transfer(_List_node_base *const first,
                             _List_node_base *const last) noexcept
{
    if (this == last) {
        return;
    }

    last->_M_prev->_M_next  = this;
    first->_M_prev->_M_next = last;
    _M_prev->_M_next        = first;

    _List_node_base *const tmp = _M_prev;
    _M_prev                    = last->_M_prev;
    last->_M_prev              = first->_M_prev;
    first->_M_prev             = tmp;
}

/* Reverse the ring in place by swapping every node's two links, including
   the sentinel's -- which is what makes begin() and rbegin() trade places
   without touching any element. */
void
_List_node_base::_M_reverse() noexcept
{
    _List_node_base *cur = this;

    do {
        _List_node_base *const next = cur->_M_next;
        cur->_M_next = cur->_M_prev;
        cur->_M_prev = next;
        cur          = next;
    } while (cur != this);
}

/**
 * Exchange the contents of two lists by exchanging their sentinels.
 *
 * The sentinel is EMBEDDED in the list object, so its address cannot
 * move: after swapping the link fields, the first and last elements of
 * each ring must be re-pointed at their new sentinel. An empty ring
 * points at itself and has no elements to re-point, which is why the
 * three cases below are not one -- swapping a populated list with an
 * empty one is the branch a same-size swap never takes.
 */
void
_List_node_base::swap(_List_node_base &x, _List_node_base &y) noexcept
{
    if (x._M_next != &x) {
        if (y._M_next != &y) {
            /* Both populated: exchange, then fix up both rings. */
            _List_node_base *const xn = x._M_next;
            _List_node_base *const xp = x._M_prev;
            x._M_next = y._M_next;
            x._M_prev = y._M_prev;
            y._M_next = xn;
            y._M_prev = xp;
            x._M_next->_M_prev = x._M_prev->_M_next = &x;
            y._M_next->_M_prev = y._M_prev->_M_next = &y;
        } else {
            /* Only x populated: y adopts the ring, x becomes empty. */
            y._M_next = x._M_next;
            y._M_prev = x._M_prev;
            y._M_next->_M_prev = y._M_prev->_M_next = &y;
            x._M_next = x._M_prev = &x;
        }
    } else if (y._M_next != &y) {
        /* Only y populated. */
        x._M_next = y._M_next;
        x._M_prev = y._M_prev;
        x._M_next->_M_prev = x._M_prev->_M_next = &x;
        y._M_next = y._M_prev = &y;
    }
    /* Both empty: nothing to do -- each already points at itself. */
}

} // namespace __detail

/**
 * Is @a ti the type_info identifying `_Sp_make_shared_tag`?
 *
 * Only reachable with RTTI OFF, which is this SDK's default.
 * `<bits/shared_ptr_base.h>` reads:
 *
 * @code
 * if (&__ti == &_Sp_make_shared_tag::_S_ti()
 *     || (__cpp_rtti ? __ti == typeid(_Sp_make_shared_tag)
 *                    : _Sp_make_shared_tag::_S_eq(__ti)))
 * @endcode
 *
 * With RTTI off there is no `typeid` to compare, so libstdc++ invents an
 * identity token instead: `_S_ti()` returns a reference to a zero-filled
 * static that is never used AS a `type_info`, only as a unique address.
 * The comparison is therefore an address comparison, and needs no RTTI
 * here either -- which is what keeps this out of the consumer-side
 * `-frtti` bucket `AXL-Cxx-Unwinder-Design.md` U0.2 expected it to land in.
 *
 * @note This is REFERENCED but never CALLED, and the reason is narrower
 *     than "the address comparison wins". In GCC 14 the only leaf caller of
 *     `_M_get_deleter` is `std::get_deleter<D>`, which is itself
 *     `#if __cpp_rtti ... #else return 0;` -- so with RTTI off
 *     `_M_get_deleter` is never ENTERED at all, and neither disjunct runs.
 *     The symbol is emitted only because
 *     `_Sp_counted_ptr_inplace::_M_get_deleter` is a virtual override and
 *     therefore lands in the vtable. Nothing in the tree calls
 *     `_M_get_deleter(_S_ti())`; the header says that path is "no longer
 *     used".
 *
 *     So sabotaging this to `return false` -- or to `return true` --
 *     changes nothing observable, confirmed both ways. It exists to
 *     resolve the link. `_Sp_make_shared_tag` is private with only
 *     `_Sp_counted_ptr_inplace` as friend, so a consumer cannot reach it
 *     either, and a uniformly `-frtti` build references no `_S_eq` at all.
 */
bool
_Sp_make_shared_tag::_S_eq(const type_info &ti) noexcept
{
    return &ti == &_S_ti();
}

_GLIBCXX_END_NAMESPACE_VERSION
} // namespace std

/**
 * glibc's "this process never started a thread" flag.
 *
 * libstdc++ reads it to take the NON-ATOMIC path in `shared_ptr`'s
 * reference counting. Under UEFI that is not an optimisation guess, it is
 * simply true: boot services are single-threaded, and AXL's MP work
 * dispatches to APs rather than sharing C++ objects across them.
 *
 * C linkage, and deliberately here rather than in `axl-cxxabi.c` beside
 * the other C-linkage runtime bits: its only consumer is libstdc++, which
 * only a C++ translation unit includes, so a pure-C image has no reason
 * to carry it.
 *
 * @warning The VALUE is a deliberate POLICY, not merely a speed knob, and
 *     an earlier revision of this comment got that wrong. `1` downgrades
 *     `shared_ptr`'s `lock xadd` to a plain load/add/store, which is
 *     correct for the single-flow case AXL targets and NOT safe in two
 *     situations AXL supports: a `shared_ptr` copied on an application
 *     processor under MP services, and -- even on one CPU -- a
 *     `shared_ptr` copied inside a UEFI event notify function, since a
 *     non-atomic `++` is interruptible between load and store where
 *     `lock add` is not.
 *
 *     `weak`, so a consumer on either of those paths can override it with
 *     their own `char __libc_single_threaded = 0;` and get the atomic
 *     path back. Without `weak` the strong definition here collides at
 *     link time and the policy is unoverridable.
 *
 * @note No test discriminates the VALUE, deliberately -- both settings are
 *     memory-safe in the single-flow case the suite exercises. What is
 *     asserted is that the symbol EXISTS: without it `shared_ptr` does not
 *     link at all.
 */
extern "C" {
__attribute__((weak)) char __libc_single_threaded = 1;
}
