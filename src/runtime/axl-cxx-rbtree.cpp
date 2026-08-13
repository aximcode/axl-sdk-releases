/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cxx-rbtree.cpp
    The nine out-of-line red-black tree helpers `std::map`, `std::set`,
    `std::multimap` and `std::multiset` call, so that a hosted C++ image
    needs nothing from `libstdc++.a` at all.

    CLEAN-ROOM: written under Apache-2.0 from the textbook red-black
    algorithm (CLRS, "Introduction to Algorithms", ch. 13) against the
    node layout and header-node convention that `<bits/stl_tree.h>`
    DECLARES.  No GPL source was consulted -- not libstdc++'s `tree.cc`,
    not the Linux kernel's rbtree.  This is the same footing as
    `src/data/axl-rb-tree.c` (AXL's own public tree) and as
    `axl-cxx-rehash.cpp` next door, which supplies
    `_Prime_rehash_policy` from its declared contract with its own prime
    table.

    @par Why this exists

    Everything else the standard containers need is already ours:
    `operator new`/`delete`, the five `std::__throw_*` entry points,
    `ceil`, `_Prime_rehash_policy`.  Measured on this tree, the ONLY
    remaining pulls from `libstdc++.a` were two archive members --
    `tree.o` (these nine functions) and `hash_bytes.o` (see
    `axl-cxx-hash.cpp`).  Supplying both means `axl-c++ --hosted` stops
    naming `libstdc++.a` on the link line, which makes the SDK
    self-contained AND removes the one act the GCC Runtime Library
    Exception does not cover: redistributing the runtime library itself.
    See `AXL-Cxx-Design.md` section 8.

    @par The layout this must match

    `<bits/stl_tree.h>` declares, and its INLINE code manipulates
    directly:

        struct _Rb_tree_node_base {
            _Rb_tree_color  _M_color;    // _S_red = 0, _S_black = 1
            _Base_ptr       _M_parent;
            _Base_ptr       _M_left;
            _Base_ptr       _M_right;
        };

    Note the colour comes FIRST, where AXL's own `AxlRBNode` puts it
    last -- so these cannot be a cast-and-delegate over
    `axl_rb_*`, and are not.

    @par The header node

    `_Rb_tree` keeps a sentinel `_M_header` that is NOT part of the tree:

      - `_M_header._M_parent` is the ROOT (and the root's `_M_parent`
        points back at the header -- a two-way link the increment walk
        depends on);
      - `_M_header._M_left`  is the LEFTMOST node;
      - `_M_header._M_right` is the RIGHTMOST node;
      - the header is coloured RED, which is what lets
        `_Rb_tree_increment` distinguish "walked past the end" from
        "reached the root" in one comparison.

    An empty tree has `_M_parent == nullptr` and both ends pointing at
    the header itself.  Maintaining the leftmost/rightmost cache is part
    of the contract: `begin()` is `_M_header._M_left`, so losing it does
    not fail loudly, it silently returns the wrong iterator.
**/

#include <stddef.h>

/* The declarations only -- <bits/stl_tree.h> is where the node layout and
   these prototypes live, and including it is what makes a signature that
   drifts from the standard's a COMPILE error here rather than a link error in
   a consumer. It is header-only and pulls no libstdc++ objects.
   That protection covers SEVEN of the nine: _Rb_tree_rotate_left and
   _rotate_right are declared by no header under /usr/include/c++, so those
   two get none. They are called only from this file. */
#include <bits/stl_tree.h>

namespace {

using base_ptr  = std::_Rb_tree_node_base *;
using cbase_ptr = const std::_Rb_tree_node_base *;

constexpr std::_Rb_tree_color RED   = std::_S_red;
constexpr std::_Rb_tree_color BLACK = std::_S_black;

/* A NULL child counts as black, which is what makes the black-height
   invariant well defined at the leaves. */
inline bool
is_red(cbase_ptr n)
{
    return n != nullptr && n->_M_color == RED;
}

/* PRECONDITION for both: @a n is a real node, never null. Every call site
   guards it (a non-null child, or the root of a non-empty tree). Stated with
   __builtin_unreachable so the contract is visible to a reader AND to clang's
   analyzer, which cannot see those guards from inside the helper and
   otherwise reports the loop as a null dereference. */
inline base_ptr
minimum(base_ptr n)
{
    if (n == nullptr) {
        __builtin_unreachable();
    }
    while (n->_M_left != nullptr) {
        n = n->_M_left;
    }
    return n;
}

inline base_ptr
maximum(base_ptr n)
{
    if (n == nullptr) {
        __builtin_unreachable();
    }
    while (n->_M_right != nullptr) {
        n = n->_M_right;
    }
    return n;
}

} // namespace

namespace std {

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------
//
// The end() iterator IS the header, so incrementing the rightmost node has to
// land exactly there. That falls out of the two-way header/root link: walking
// up from the rightmost node reaches the root, whose _M_parent is the header,
// and the loop's "came from the right child" test stops there.

_Rb_tree_node_base *
_Rb_tree_increment(_Rb_tree_node_base *x) noexcept
{
    if (x->_M_right != nullptr) {
        return minimum(x->_M_right);
    }

    base_ptr y = x->_M_parent;
    while (x == y->_M_right) {
        x = y;
        y = y->_M_parent;
    }
    /* Guards the one case the walk cannot: incrementing the header of a
       one-node tree, where root->_M_right is null and y is already the
       header. Without it the result would be the root, not end(). */
    if (x->_M_right != y) {
        x = y;
    }
    return x;
}

const _Rb_tree_node_base *
_Rb_tree_increment(const _Rb_tree_node_base *x) noexcept
{
    return _Rb_tree_increment(const_cast<base_ptr>(x));
}

_Rb_tree_node_base *
_Rb_tree_decrement(_Rb_tree_node_base *x) noexcept
{
    /* Decrementing end() must give the rightmost node. end() is the header,
       which is RED and whose grandparent is itself -- a combination no real
       node can have, and the reason the header is coloured at all. */
    if (x->_M_color == RED && x->_M_parent->_M_parent == x) {
        return x->_M_right;
    }

    if (x->_M_left != nullptr) {
        return maximum(x->_M_left);
    }

    base_ptr y = x->_M_parent;
    while (x == y->_M_left) {
        x = y;
        y = y->_M_parent;
    }
    return y;
}

const _Rb_tree_node_base *
_Rb_tree_decrement(const _Rb_tree_node_base *x) noexcept
{
    return _Rb_tree_decrement(const_cast<base_ptr>(x));
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------
//
// @a root is taken by REFERENCE because a rotation at the root changes which
// node the header points to; the caller passes _M_header._M_parent.

void
_Rb_tree_rotate_left(_Rb_tree_node_base *x, _Rb_tree_node_base *&root)
{
    base_ptr y = x->_M_right;

    x->_M_right = y->_M_left;
    if (y->_M_left != nullptr) {
        y->_M_left->_M_parent = x;
    }
    y->_M_parent = x->_M_parent;

    if (x == root) {
        root = y;
    } else if (x == x->_M_parent->_M_left) {
        x->_M_parent->_M_left = y;
    } else {
        x->_M_parent->_M_right = y;
    }
    y->_M_left   = x;
    x->_M_parent = y;
}

void
_Rb_tree_rotate_right(_Rb_tree_node_base *x, _Rb_tree_node_base *&root)
{
    base_ptr y = x->_M_left;

    x->_M_left = y->_M_right;
    if (y->_M_right != nullptr) {
        y->_M_right->_M_parent = x;
    }
    y->_M_parent = x->_M_parent;

    if (x == root) {
        root = y;
    } else if (x == x->_M_parent->_M_right) {
        x->_M_parent->_M_right = y;
    } else {
        x->_M_parent->_M_left = y;
    }
    y->_M_right  = x;
    x->_M_parent = y;
}

// ---------------------------------------------------------------------------
// Insertion
// ---------------------------------------------------------------------------

void
_Rb_tree_insert_and_rebalance(const bool          insert_left,
                              _Rb_tree_node_base *x,
                              _Rb_tree_node_base *p,
                              _Rb_tree_node_base &header) noexcept
{
    base_ptr &root = header._M_parent;

    x->_M_parent = p;
    x->_M_left   = nullptr;
    x->_M_right  = nullptr;
    x->_M_color  = RED;

    /* insert_left is also true for the very first insert, where p IS the
       header -- which is how the three header fields all end up at x. */
    if (insert_left) {
        p->_M_left = x;
        if (p == &header) {
            header._M_parent = x;
            header._M_right  = x;
        } else if (p == header._M_left) {
            header._M_left = x;         /* maintain leftmost */
        }
    } else {
        p->_M_right = x;
        if (p == header._M_right) {
            header._M_right = x;        /* maintain rightmost */
        }
    }

    /* CLRS insert-fixup. The loop condition cannot run off the top: the root
       is black on entry to every iteration, so the walk stops there. */
    while (x != root && is_red(x->_M_parent)) {
        base_ptr gp = x->_M_parent->_M_parent;

        if (x->_M_parent == gp->_M_left) {
            base_ptr uncle = gp->_M_right;
            if (is_red(uncle)) {
                x->_M_parent->_M_color = BLACK;
                uncle->_M_color        = BLACK;
                gp->_M_color           = RED;
                x = gp;
            } else {
                if (x == x->_M_parent->_M_right) {
                    x = x->_M_parent;
                    _Rb_tree_rotate_left(x, root);
                }
                x->_M_parent->_M_color = BLACK;
                x->_M_parent->_M_parent->_M_color = RED;
                _Rb_tree_rotate_right(x->_M_parent->_M_parent, root);
            }
        } else {
            base_ptr uncle = gp->_M_left;
            if (is_red(uncle)) {
                x->_M_parent->_M_color = BLACK;
                uncle->_M_color        = BLACK;
                gp->_M_color           = RED;
                x = gp;
            } else {
                if (x == x->_M_parent->_M_left) {
                    x = x->_M_parent;
                    _Rb_tree_rotate_right(x, root);
                }
                x->_M_parent->_M_color = BLACK;
                x->_M_parent->_M_parent->_M_color = RED;
                _Rb_tree_rotate_left(x->_M_parent->_M_parent, root);
            }
        }
    }
    root->_M_color = BLACK;
}

// ---------------------------------------------------------------------------
// Erase
// ---------------------------------------------------------------------------
//
// The fiddly one. Two things make it harder than the textbook version:
//
//   1. There is no sentinel NIL leaf, so the "double black" node the fixup
//      walks from may be NULL -- and then its PARENT has to be carried
//      separately, because a NULL has none.
//   2. A two-child erase SPLICES the successor into the erased node's place
//      rather than copying its value. libstdc++ hands out iterators that are
//      node pointers, so moving a value would invalidate an iterator the
//      standard says stays valid.

_Rb_tree_node_base *
_Rb_tree_rebalance_for_erase(_Rb_tree_node_base *const z,
                             _Rb_tree_node_base &header) noexcept
{
    base_ptr &root      = header._M_parent;
    base_ptr &leftmost  = header._M_left;
    base_ptr &rightmost = header._M_right;

    base_ptr y      = z;          /* node actually removed from its slot */
    base_ptr x      = nullptr;    /* y's only child, which takes its place */
    base_ptr xparent = nullptr;   /* x's parent AFTER the relink (x may be NULL) */

    if (y->_M_left == nullptr) {
        x = y->_M_right;
    } else if (y->_M_right == nullptr) {
        x = y->_M_left;
    } else {
        y = minimum(y->_M_right);   /* in-order successor */
        x = y->_M_right;
    }

    if (y != z) {
        /* Splice y into z's position. z's left subtree is non-empty by
           construction, so it always gets a new parent. */
        z->_M_left->_M_parent = y;
        y->_M_left            = z->_M_left;

        if (y != z->_M_right) {
            xparent = y->_M_parent;
            if (x != nullptr) {
                x->_M_parent = y->_M_parent;
            }
            y->_M_parent->_M_left = x;
            y->_M_right           = z->_M_right;
            z->_M_right->_M_parent = y;
        } else {
            /* y IS z's right child: x stays under y, so y is x's parent. */
            xparent = y;
        }

        if (root == z) {
            root = y;
        } else if (z->_M_parent->_M_left == z) {
            z->_M_parent->_M_left = y;
        } else {
            z->_M_parent->_M_right = y;
        }
        y->_M_parent = z->_M_parent;

        /* Swap colours, NOT values: y now occupies z's place in the tree, so
           it must wear z's colour, and the fixup below is driven by the
           colour y ORIGINALLY had. */
        _Rb_tree_color tmp = y->_M_color;
        y->_M_color = z->_M_color;
        z->_M_color = tmp;
        y = z;                       /* y now names the node to fix up from */
    } else {
        xparent = y->_M_parent;
        if (x != nullptr) {
            x->_M_parent = y->_M_parent;
        }
        if (root == z) {
            root = x;
        } else if (z->_M_parent->_M_left == z) {
            z->_M_parent->_M_left = x;
        } else {
            z->_M_parent->_M_right = x;
        }

        /* The cached ends. Only reachable when z had at most one child, which
           is always true of the leftmost and rightmost nodes. */
        if (leftmost == z) {
            leftmost = (z->_M_right == nullptr) ? z->_M_parent : minimum(x);
        }
        if (rightmost == z) {
            rightmost = (z->_M_left == nullptr) ? z->_M_parent : maximum(x);
        }
    }

    /* Removing a RED node cannot change any black height. */
    if (y->_M_color == BLACK) {
        while (x != root && !is_red(x)) {
            if (x == xparent->_M_left) {
                base_ptr w = xparent->_M_right;
                if (is_red(w)) {
                    w->_M_color       = BLACK;
                    xparent->_M_color = RED;
                    _Rb_tree_rotate_left(xparent, root);
                    w = xparent->_M_right;
                }
                if (!is_red(w->_M_left) && !is_red(w->_M_right)) {
                    w->_M_color = RED;
                    x = xparent;
                    xparent = xparent->_M_parent;
                } else {
                    if (!is_red(w->_M_right)) {
                        if (w->_M_left != nullptr) {
                            w->_M_left->_M_color = BLACK;
                        }
                        w->_M_color = RED;
                        _Rb_tree_rotate_right(w, root);
                        w = xparent->_M_right;
                    }
                    w->_M_color       = xparent->_M_color;
                    xparent->_M_color = BLACK;
                    if (w->_M_right != nullptr) {
                        w->_M_right->_M_color = BLACK;
                    }
                    _Rb_tree_rotate_left(xparent, root);
                    break;
                }
            } else {
                base_ptr w = xparent->_M_left;
                if (is_red(w)) {
                    w->_M_color       = BLACK;
                    xparent->_M_color = RED;
                    _Rb_tree_rotate_right(xparent, root);
                    w = xparent->_M_left;
                }
                if (!is_red(w->_M_right) && !is_red(w->_M_left)) {
                    w->_M_color = RED;
                    x = xparent;
                    xparent = xparent->_M_parent;
                } else {
                    if (!is_red(w->_M_left)) {
                        if (w->_M_right != nullptr) {
                            w->_M_right->_M_color = BLACK;
                        }
                        w->_M_color = RED;
                        _Rb_tree_rotate_left(w, root);
                        w = xparent->_M_left;
                    }
                    w->_M_color       = xparent->_M_color;
                    xparent->_M_color = BLACK;
                    if (w->_M_left != nullptr) {
                        w->_M_left->_M_color = BLACK;
                    }
                    _Rb_tree_rotate_right(xparent, root);
                    break;
                }
            }
        }
        if (x != nullptr) {
            x->_M_color = BLACK;
        }
    }
    return y;
}

// ---------------------------------------------------------------------------
// Debug helper
// ---------------------------------------------------------------------------

/* Black nodes on the path (node, root]. libstdc++'s own _M_rb_verify calls
   this; nothing in a release build does. */
unsigned int
_Rb_tree_black_count(const _Rb_tree_node_base *node,
                     const _Rb_tree_node_base *root) noexcept
{
    if (node == nullptr) {
        return 0;
    }
    unsigned int sum = 0;
    do {
        if (node->_M_color == BLACK) {
            ++sum;
        }
        if (node == root) {
            break;
        }
        node = node->_M_parent;
    } while (true);
    return sum;
}

} // namespace std
