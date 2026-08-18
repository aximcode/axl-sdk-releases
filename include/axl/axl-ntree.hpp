/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ntree.hpp
 *
 * `AxlNTree` walks as a range, so a tree traversal is a `for` loop over
 * something with a name rather than a hand-written pointer chase.
 *
 * @code
 * #include <axl/axl-ntree.hpp>
 *
 * for (AxlNTree *c : axl::children(node)) { ... }              // was: c = c->children; c; c = c->next
 * for (const AxlNTree *p : axl::ancestors(node)) { depth++; }  // was: p = p->parent
 *
 * for (AxlNTree *n : axl::preorder(root)) {                    // was: axl_ntree_traverse + a callback
 *     draw(axl::data_of<Item>(n));
 * }
 *
 * auto open = axl::children(node)
 *           | std::views::filter([](AxlNTree *c) { return axl::data_of<Item>(c)->expanded; });
 * @endcode
 *
 * @par What this is for, and it is not shorter code
 *
 * `axl_ntree_traverse()` already visits every node, and it takes a function
 * pointer plus a `void *`. That boundary is a wall: the visitor cannot be
 * inlined into the walk, cannot capture, cannot `break`, and cannot compose
 * with anything in `<ranges>`. A range has none of those limits, and
 * AXL-Cxx-Design.md §5 singles this type out as the one where the C structure
 * gives it away for free — `AxlNTree`'s `parent`, `next` and `children` links
 * are PUBLIC, so every walk here is a plain pointer chase the compiler can see
 * through, with no accessor call to inline and no change to the C side. That
 * is the thing `AxlArray` needed axl_array_data() before it could offer.
 *
 * The shape is `llvm::ilist`'s: an iterator over intrusive links, with a
 * default-constructed iterator as the end sentinel.
 *
 * @par Four walks, all lazy and none of them allocating
 *
 * | | yields | order |
 * |---|---|---|
 * | #axl::children | the direct children of a node | first to last |
 * | #axl::ancestors | `parent`, then ITS parent, … | inner to outer; excludes the node |
 * | #axl::preorder | a node and its whole subtree | node, then each child's subtree |
 * | #axl::postorder | a node and its whole subtree | each child's subtree, then node |
 *
 * `preorder` and `postorder` INCLUDE the node they are given, so
 * `preorder(root)` is the whole tree. Both climb back out through the public
 * `parent` link rather than keeping a stack, which is why they need no memory
 * and cannot fail.
 *
 * **There is no `level_order` and no `in_order` here**, and that is a cost
 * decision rather than an oversight: breadth-first needs a queue, so a range
 * for it would allocate silently inside a `for` loop that reads like the four
 * above. `axl_ntree_traverse()` implements all four orders and stays the
 * answer for those two; it just cannot do it lazily either.
 *
 * @par Borrowed, like every other view here
 *
 * These ranges hold node pointers and nothing else. Unlinking, moving or
 * freeing a node invalidates any iterator positioned on it or below it — so
 * the destructive walk `while (AxlNTree *c = n->children) { axl_ntree_unlink(c);
 * … }` stays a `while` loop and must not become a range-for.
 *
 * Each factory has a `const` overload yielding `const AxlNTree *`, so a
 * `const`-qualified member function walks without casting.
 */

#ifndef AXL_NTREE_HPP
#define AXL_NTREE_HPP

#ifndef __cplusplus
#error "axl-ntree.hpp is C++ only; C consumers want axl-ntree.h"
#endif

#include <cstddef>
#include <iterator>
#include <ranges>

#include <axl/axl-ntree.h>

namespace axl {

/// Implementation detail of the range factories below; not a public surface.
namespace detail {

/**
 * Iterator over one intrusive link, followed until it is NULL.
 *
 * @tparam Node `AxlNTree` or `const AxlNTree`; the qualifier rides through to
 *     what `operator*` yields.
 * @tparam Link the member to follow — `&AxlNTree::next` for siblings,
 *     `&AxlNTree::parent` for ancestors. Naming the link as a parameter is
 *     what keeps these two walks one piece of code.
 */
template <class Node, AxlNTree *AxlNTree::*Link>
class link_iterator {
public:
    using iterator_concept  = std::forward_iterator_tag;
    /* The C++17 tag, and deliberately WEAKER than the concept above.
       Cpp17ForwardIterator requires `reference` to be `value_type &`, and
       operator* here returns a prvalue `Node *`. std::ranges reads
       iterator_concept and gets the full forward guarantee; a C++17-era
       algorithm reads this one and is told only what is true.
       iota_view::iterator and transform_view::iterator do the same. */
    using iterator_category = std::input_iterator_tag;
    using value_type        = Node *;
    using difference_type   = std::ptrdiff_t;

    /// Default-constructed IS the end sentinel. Required as well as
    /// convenient: `std::input_or_output_iterator` refines
    /// `std::semiregular`, and an iterator without this is accepted by
    /// `std::sort` and then rejected by `views::filter`.
    link_iterator() noexcept = default;

    explicit link_iterator(Node *n) noexcept : m_node(n) {}

    Node *operator*() const noexcept { return m_node; }

    link_iterator &
    operator++() noexcept
    {
        m_node = m_node->*Link;
        return *this;
    }

    link_iterator
    operator++(int) noexcept
    {
        link_iterator prev = *this;
        ++*this;
        return prev;
    }

    bool operator==(const link_iterator &) const noexcept = default;

private:
    Node *m_node = nullptr;
};

/**
 * Iterator over a whole subtree, depth-first, climbing back out through
 * `parent` so no stack is needed.
 *
 * @tparam Post `false` for pre-order, `true` for post-order. One class because
 *     the two differ only in where the node is visited relative to its
 *     children; the bounds checking against @a m_root, which is the part that
 *     is easy to get wrong, is then written once.
 */
template <class Node, bool Post>
class subtree_iterator {
public:
    using iterator_concept  = std::forward_iterator_tag;
    /// @copydoc link_iterator::iterator_category
    using iterator_category = std::input_iterator_tag;
    using value_type        = Node *;
    using difference_type   = std::ptrdiff_t;

    subtree_iterator() noexcept = default;

    explicit subtree_iterator(Node *root) noexcept
        : m_node(Post ? deepest_first(root) : root)
        , m_root(root)
    {}

    Node *operator*() const noexcept { return m_node; }

    subtree_iterator &
    operator++() noexcept
    {
        if constexpr (Post) {
            /* The root is visited last, so reaching it ends the walk before
               any climb — without this the loop would step to the root's own
               sibling and escape the subtree. */
            if (m_node == m_root) {
                m_node = nullptr;
            } else if (m_node->next != nullptr) {
                m_node = deepest_first(m_node->next);
            } else {
                m_node = m_node->parent;
            }
        } else {
            if (m_node->children != nullptr) {
                m_node = m_node->children;
                return *this;
            }
            /* No children: take the nearest following sibling at or above this
               level, climbing no higher than the root. `n != nullptr` guards a
               node whose parent chain does not actually reach m_root, which is
               a caller error rather than a shape to walk off the end of. */
            Node *n = m_node;
            while (n != m_root && n != nullptr) {
                if (n->next != nullptr) {
                    m_node = n->next;
                    return *this;
                }
                n = n->parent;
            }
            m_node = nullptr;
        }
        return *this;
    }

    subtree_iterator
    operator++(int) noexcept
    {
        subtree_iterator prev = *this;
        ++*this;
        return prev;
    }

    /// Compares position only. Two iterators over DIFFERENT roots that happen
    /// to sit on the same node are equal, which is the same latitude every
    /// standard iterator takes outside its own range.
    bool
    operator==(const subtree_iterator &other) const noexcept
    {
        return m_node == other.m_node;
    }

private:
    /// The first node post-order visits within @a n's subtree: descend the
    /// first-child chain to a leaf.
    static Node *
    deepest_first(Node *n) noexcept
    {
        while (n != nullptr && n->children != nullptr) {
            n = n->children;
        }
        return n;
    }

    Node *m_node = nullptr;
    Node *m_root = nullptr;
};

/**
 * Pre-order iterator that asks before descending.
 *
 * Identical to `subtree_iterator<Node, false>` except for one condition on
 * the descent, which is the whole point: composing `views::filter` over
 * `preorder` cannot express this, because filter removes a node from the
 * OUTPUT after the walk has already descended into it.
 *
 * @tparam Descend predicate `bool(const Node *)` — true to walk the node's
 *     children. The node itself is visited either way.
 */
template <class Node, class Descend>
class pruned_iterator {
public:
    /* INPUT, not forward, and deliberately. A forward iterator must be
       `std::semiregular`, which requires default-construction; this one
       stores the predicate BY VALUE, and a capturing lambda is not
       default-constructible. Holding a pointer to a predicate owned by the
       range would buy the stronger concept at the cost of a dangling hazard
       the other ranges here do not have. A pruned walk is consumed once in
       practice, so the weaker concept costs nothing real. */
    using iterator_concept  = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type        = Node *;
    using difference_type   = std::ptrdiff_t;

    pruned_iterator(Node *root, Descend descend)
        : m_node(root)
        , m_root(root)
        , m_descend(static_cast<Descend &&>(descend))
    {}

    Node *operator*() const noexcept { return m_node; }

    pruned_iterator &
    operator++()
    {
        /* THE one condition that differs from subtree_iterator. */
        if (m_node->children != nullptr && m_descend(m_node)) {
            m_node = m_node->children;
            return *this;
        }
        /* Pruned or childless: the same climb, bounded by m_root, that the
           unpruned walk does. */
        Node *n = m_node;
        while (n != m_root && n != nullptr) {
            if (n->next != nullptr) {
                m_node = n->next;
                return *this;
            }
            n = n->parent;
        }
        m_node = nullptr;
        return *this;
    }

    /// Post-increment returns void: an input iterator may, and returning a
    /// copy would silently require the predicate to be copy-assignable.
    void operator++(int) { ++*this; }

    bool
    operator==(std::default_sentinel_t) const noexcept
    {
        return m_node == nullptr;
    }

private:
    Node   *m_node = nullptr;
    Node   *m_root = nullptr;
    Descend m_descend;
};

/// A range over a `pruned_iterator`, ending at `std::default_sentinel`.
/// Separate from `node_range` because that one's `end()` default-constructs
/// its iterator, which a predicate-carrying iterator cannot do.
template <class It>
class pruned_range : public std::ranges::view_interface<pruned_range<It>> {
public:
    explicit pruned_range(It first) : m_first(static_cast<It &&>(first)) {}

    It                      begin() const { return m_first; }
    std::default_sentinel_t end() const noexcept { return std::default_sentinel; }

private:
    It m_first;
};

/// A range from one iterator, ending at the default-constructed sentinel.
template <class It>
class node_range : public std::ranges::view_interface<node_range<It>> {
public:
    node_range() noexcept = default;
    explicit node_range(It first) noexcept : m_first(first) {}

    It begin() const noexcept { return m_first; }
    It end() const noexcept { return It{}; }

private:
    It m_first {};
};

} // namespace detail

/// The direct children of @a n, first to last. Empty for a leaf or a NULL node.
[[nodiscard]] inline auto
children(
    AxlNTree *n    ///< node whose children to walk, or NULL
) noexcept
{
    using It = detail::link_iterator<AxlNTree, &AxlNTree::next>;
    return detail::node_range<It>(It(n != nullptr ? n->children : nullptr));
}

/// @copydoc children(AxlNTree *)
[[nodiscard]] inline auto
children(
    const AxlNTree *n    ///< node whose children to walk, or NULL
) noexcept
{
    using It = detail::link_iterator<const AxlNTree, &AxlNTree::next>;
    return detail::node_range<It>(It(n != nullptr ? n->children : nullptr));
}

/**
 * Disambiguate a literal `nullptr`.
 *
 * The `AxlNTree *` / `const AxlNTree *` overload pair is ambiguous for
 * `nullptr`, which converts equally to both — and each is documented "or
 * NULL". Rather than weaken the docs or make the caller write a cast, the
 * literal gets its own overload, resolving as a NULL `AxlNTree *` variable
 * would have.
 *
 * @return an empty range.
 */
[[nodiscard]] inline auto
children(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    using It = detail::link_iterator<AxlNTree, &AxlNTree::next>;
    return detail::node_range<It>(It{});
}

/**
 * @a n's parent, then its parent, up to and including the tree's root.
 *
 * EXCLUDES @a n itself, so the count is the number of EDGES above @a n —
 * which is `axl_ntree_depth(n) - 1`, not `axl_ntree_depth(n)`. That function
 * counts NODES and is 1-based (a root is depth 1), so `ancestors(root)` is
 * empty while `axl_ntree_depth(root)` is 1. An earlier revision of this line
 * claimed the two were equal and that depth counted edges; both were wrong.
 *
 * To stop early at a known ancestor (a hidden super-root, say), compose
 * `std::views::take_while`.
 */
[[nodiscard]] inline auto
ancestors(
    AxlNTree *n    ///< node to walk up from, or NULL
) noexcept
{
    using It = detail::link_iterator<AxlNTree, &AxlNTree::parent>;
    return detail::node_range<It>(It(n != nullptr ? n->parent : nullptr));
}

/// @copydoc ancestors(AxlNTree *)
[[nodiscard]] inline auto
ancestors(
    const AxlNTree *n    ///< node to walk up from, or NULL
) noexcept
{
    using It = detail::link_iterator<const AxlNTree, &AxlNTree::parent>;
    return detail::node_range<It>(It(n != nullptr ? n->parent : nullptr));
}

/**
 * Disambiguate a literal `nullptr`.
 *
 * The `AxlNTree *` / `const AxlNTree *` overload pair is ambiguous for
 * `nullptr`, which converts equally to both — and each is documented "or
 * NULL". Rather than weaken the docs or make the caller write a cast, the
 * literal gets its own overload, resolving as a NULL `AxlNTree *` variable
 * would have.
 *
 * @return an empty range.
 */
[[nodiscard]] inline auto
ancestors(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    using It = detail::link_iterator<AxlNTree, &AxlNTree::parent>;
    return detail::node_range<It>(It{});
}

/**
 * @a n and its whole subtree, depth-first: each node before its children.
 *
 * `AXL_NTREE_PRE_ORDER` as a range. Yields @a n first, so `preorder(root)`
 * visits the entire tree and `std::ranges::distance` over it is
 * `axl_ntree_n_nodes(root, AXL_NTREE_ALL)` — that call FILTERS, and with
 * `AXL_NTREE_LEAVES` or `AXL_NTREE_NON_LEAVES` it counts something else.
 */
[[nodiscard]] inline auto
preorder(
    AxlNTree *n    ///< subtree root, or NULL
) noexcept
{
    using It = detail::subtree_iterator<AxlNTree, false>;
    return detail::node_range<It>(It(n));
}

/// @copydoc preorder(AxlNTree *)
[[nodiscard]] inline auto
preorder(
    const AxlNTree *n    ///< subtree root, or NULL
) noexcept
{
    using It = detail::subtree_iterator<const AxlNTree, false>;
    return detail::node_range<It>(It(n));
}

/**
 * Disambiguate a literal `nullptr`.
 *
 * The `AxlNTree *` / `const AxlNTree *` overload pair is ambiguous for
 * `nullptr`, which converts equally to both — and each is documented "or
 * NULL". Rather than weaken the docs or make the caller write a cast, the
 * literal gets its own overload, resolving as a NULL `AxlNTree *` variable
 * would have.
 *
 * @return an empty range.
 */
[[nodiscard]] inline auto
preorder(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    using It = detail::subtree_iterator<AxlNTree, false>;
    return detail::node_range<It>(It{});
}

/**
 * @brief Pre-order over @a n and its subtree, skipping the children of any
 *     node @a descend rejects.
 *
 * The rejected node is still VISITED; only its subtree is skipped. That
 * distinction is the point: a collapsed row in a tree view is still drawn,
 * while everything beneath it is not.
 *
 * `preorder(n) | std::views::filter(pred)` does NOT do this. `filter` drops a
 * node from the OUTPUT after the walk has already descended into it, so a
 * hidden subtree is still traversed and every node in it still tested. Here
 * the predicate governs the DESCENT.
 *
 * ```cpp
 * for (AxlNTree *row : axl::preorder_pruned(root, [](const AxlNTree *n) {
 *         return axl::data_of<Row>(n)->expanded;
 *     })) {
 *     draw(axl::data_of<Row>(row));
 * }
 * ```
 *
 * Allocation-free and stack-free like the other ranges here — it climbs back
 * out through `parent`. The predicate is stored by value in the iterator, so
 * this yields an `input_range` rather than the `forward_range` #axl::preorder
 * gives; see `detail::pruned_iterator` for why. A NULL @a n is an empty range.
 *
 * @return a range over `AxlNTree *`.
 */
template <class Descend>
[[nodiscard]] inline auto
preorder_pruned(
    AxlNTree *n,      ///< subtree root, or NULL for an empty range
    Descend   descend ///< `bool(const AxlNTree *)` — true to walk n's children
    )
{
    using It = detail::pruned_iterator<AxlNTree, Descend>;
    return detail::pruned_range<It>(It(n, static_cast<Descend &&>(descend)));
}

/// @copydoc preorder_pruned(AxlNTree *, Descend)
template <class Descend>
[[nodiscard]] inline auto
preorder_pruned(
    const AxlNTree *n,      ///< subtree root, or NULL for an empty range
    Descend         descend ///< `bool(const AxlNTree *)` — true to walk children
    )
{
    using It = detail::pruned_iterator<const AxlNTree, Descend>;
    return detail::pruned_range<It>(It(n, static_cast<Descend &&>(descend)));
}

/**
 * @a n and its whole subtree, depth-first: each node after its children.
 *
 * `AXL_NTREE_POST_ORDER` as a range, and the order to walk when what you do to
 * a node depends on its children having been handled already — summing sizes,
 * or laying out a tree bottom-up. @a n is yielded LAST.
 */
[[nodiscard]] inline auto
postorder(
    AxlNTree *n    ///< subtree root, or NULL
) noexcept
{
    using It = detail::subtree_iterator<AxlNTree, true>;
    return detail::node_range<It>(It(n));
}

/// @copydoc postorder(AxlNTree *)
[[nodiscard]] inline auto
postorder(
    const AxlNTree *n    ///< subtree root, or NULL
) noexcept
{
    using It = detail::subtree_iterator<const AxlNTree, true>;
    return detail::node_range<It>(It(n));
}

/**
 * Disambiguate a literal `nullptr`.
 *
 * The `AxlNTree *` / `const AxlNTree *` overload pair is ambiguous for
 * `nullptr`, which converts equally to both — and each is documented "or
 * NULL". Rather than weaken the docs or make the caller write a cast, the
 * literal gets its own overload, resolving as a NULL `AxlNTree *` variable
 * would have.
 *
 * @return an empty range.
 */
[[nodiscard]] inline auto
postorder(
    std::nullptr_t    ///< the NULL literal
) noexcept
{
    using It = detail::subtree_iterator<AxlNTree, true>;
    return detail::node_range<It>(It{});
}

/**
 * A node's payload, typed.
 *
 * `AxlNTree::data` is a `void *` the tree only borrows, so every consumer
 * writes the same `static_cast` at every use. This is that cast, named, and
 * NULL-safe on the node so it composes with the ranges above without a guard.
 *
 * Takes a `const AxlNTree *` and returns a MUTABLE `T *`, deliberately: node
 * constness is about the tree's shape — the links — and says nothing about
 * the payload, which the node merely borrows. A `const`-qualified walk that
 * wants to edit what it finds is the normal case, and one that does not says
 * so with `data_of<const Item>(n)`.
 *
 * @tparam T what the payload actually is. Unchecked — the tree stores no type
 *     and cannot check it for you.
 *
 * @return @a n's payload as `T *`, or NULL if @a n is NULL or carries none.
 */
template <class T>
[[nodiscard]] inline T *
data_of(
    const AxlNTree *n    ///< node, or NULL
) noexcept
{
    return n != nullptr ? static_cast<T *>(n->data) : nullptr;
}

} // namespace axl

/* The ranges above own nothing -- a node_range is one iterator holding node
   pointers -- so an iterator into one stays valid after the range itself dies.

   Without this opt-in every std::ranges:: algorithm taking the range BY VALUE
   hands back std::ranges::dangling instead of an iterator, and
   std::ranges::find_if(axl::children(n), pred) fails to compile at the use
   site. axl::array_span has no such problem because std::span already declares
   itself borrowed; this is what keeps the two range surfaces in this phase
   composing the same way.

   Hidden from Doxygen with @cond, and the comment below it is a plain block
   rather than a doc block. Both are needed: Doxygen cannot resolve a
   variable-template specialization of a std:: entity at all, and reports
   "documented symbol was not declared or defined" whether or not a doc
   comment precedes it -- which fails the zero-warning docs gate. The prose
   lives in docs/sphinx/modules/cxx-ntree-ranges.rst instead. */
/// @cond DOXYGEN_CANNOT_PARSE_STD_VARIABLE_TEMPLATE_SPECIALIZATION
template <class It>
inline constexpr bool
    std::ranges::enable_borrowed_range<axl::detail::node_range<It>> = true;
/// @endcond

#endif /* AXL_NTREE_HPP */
