/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-ntree.h:
 *
 * Generic n-ary tree. GLib GNode equivalent.
 *
 * The node IS the subtree handle — there is no separate container
 * object; you hold a root node and the struct fields are public, so
 * traversal is a plain pointer walk:
 *   for (AxlNTree *c = node->children; c; c = c->next) { use(c->data); }
 *
 * Intrusive sibling/child links (no per-link allocation beyond the node
 * itself). Single-threaded, no locking. The tree owns its node objects;
 * the caller owns each node's @a data unless a free function is passed to
 * axl_ntree_free_full.
 *
 * Distinct from axl-radix-tree (a string-prefix lookup tree) and
 * axl-tree (a balanced sorted key->value map): this is the general
 * parent->children hierarchy (UI/device/DOM trees).
 */

#ifndef AXL_NTREE_H
#define AXL_NTREE_H

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// AxlNTree — n-ary tree node (public fields, GNode-style)
// ---------------------------------------------------------------------------

typedef struct AxlNTree AxlNTree;
struct AxlNTree {
    void      *data;       ///< caller payload (borrowed unless freed via _free_full)
    AxlNTree  *parent;     ///< parent node (NULL at the root)
    AxlNTree  *prev;       ///< previous sibling (NULL = first child)
    AxlNTree  *next;       ///< next sibling (NULL = last child)
    AxlNTree  *children;   ///< first child (NULL = leaf)
};

/**
 * AxlNTreeTraverseType:
 *
 * Visiting order for axl_ntree_traverse. For an n-ary node, IN_ORDER
 * visits the first child, then the node, then the remaining children
 * (GLib G_IN_ORDER semantics).
 */
typedef enum {
    AXL_NTREE_PRE_ORDER,    ///< node, then children left-to-right
    AXL_NTREE_POST_ORDER,   ///< children left-to-right, then node
    AXL_NTREE_IN_ORDER,     ///< first child, node, then remaining children
    AXL_NTREE_LEVEL_ORDER,  ///< breadth-first (root, then depth 2, ...)
} AxlNTreeTraverseType;

/**
 * AxlNTreeTraverseFlags:
 *
 * Which nodes a traversal / count visits. Combine LEAVES and NON_LEAVES,
 * or use ALL.
 */
typedef enum {
    AXL_NTREE_LEAVES     = 1 << 0,  ///< only leaf nodes (no children)
    AXL_NTREE_NON_LEAVES = 1 << 1,  ///< only internal nodes (>= 1 child)
    AXL_NTREE_ALL        = (1 << 0) | (1 << 1),  ///< every node
} AxlNTreeTraverseFlags;

/**
 * AxlNTreeTraverseFunc:
 *
 * Per-node callback for axl_ntree_traverse. Return true to STOP the
 * walk early (GLib semantics), false to continue.
 */
typedef bool (*AxlNTreeTraverseFunc)(
    AxlNTree *node,  ///< the visited node
    void     *user   ///< caller-provided context
);

/**
 * AxlNTreeForeachFunc:
 *
 * Per-node callback for axl_ntree_children_foreach (cannot stop early).
 */
typedef void (*AxlNTreeForeachFunc)(
    AxlNTree *node,  ///< the visited child
    void     *user   ///< caller-provided context
);

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

/**
 * @brief Allocate a new, parentless leaf node holding @a data.
 *
 * @return the node, or NULL on allocation failure.
 */
AxlNTree *
axl_ntree_new(
    void *data  ///< payload pointer to store (borrowed)
);

/**
 * @brief Free @a node and its entire subtree. Data pointers are left
 *        untouched (borrowed). NULL-safe.
 *
 * @a node must be a root (unlink it first if it has a parent), otherwise
 * its parent is left with a dangling child pointer.
 */
void
axl_ntree_free(
    AxlNTree *node  ///< subtree root to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlNTree, axl_ntree_free)
#endif

/**
 * @brief Free @a node and its entire subtree, calling @a free_func on every
 *        node's @a data first. NULL-safe; @a free_func may be NULL (same as
 *        axl_ntree_free).
 */
void
axl_ntree_free_full(
    AxlNTree         *node,      ///< subtree root to free (NULL-safe)
    AxlDestroyNotify  free_func  ///< called on each node's data, or NULL
);

/**
 * @brief Detach @a node from its parent and siblings; the subtree rooted
 *        at @a node stays intact and becomes its own root.
 *
 * No-op if @a node is NULL or already a root.
 */
void
axl_ntree_unlink(
    AxlNTree *node  ///< node to detach
);

// ---------------------------------------------------------------------------
// Insert (all return the inserted child, or NULL on bad arguments)
// ---------------------------------------------------------------------------

/**
 * @brief Append @a child as the last child of @a parent. O(n) in the child
 *        count (walks to the tail).
 *
 * @a child must be a root (no existing parent).
 *
 * @return @a child, or NULL if @a parent or @a child is NULL or @a child already
 *         has a parent.
 */
AxlNTree *
axl_ntree_append_child(
    AxlNTree *parent,  ///< parent to receive the child
    AxlNTree *child    ///< root node to attach
);

/**
 * @brief Prepend @a child as the first child of @a parent. O(1).
 *
 * @return @a child, or NULL on bad arguments (see axl_ntree_append_child).
 */
AxlNTree *
axl_ntree_prepend_child(
    AxlNTree *parent,  ///< parent to receive the child
    AxlNTree *child    ///< root node to attach
);

/**
 * @brief Insert @a child after @a sibling among @a parent's children. O(1).
 *
 * If @a sibling is NULL, @a child becomes the first child (prepend).
 *
 * @return @a child, or NULL on bad arguments (NULL @a parent/@a child, @a child
 *         already attached, or @a sibling not a child of @a parent).
 */
AxlNTree *
axl_ntree_insert_after(
    AxlNTree *parent,   ///< parent owning @a sibling
    AxlNTree *sibling,  ///< existing child to insert after (NULL = prepend)
    AxlNTree *child     ///< root node to attach
);

/**
 * @brief Insert @a child before @a sibling among @a parent's children. O(1).
 *
 * If @a sibling is NULL, @a child becomes the last child (append, O(n)).
 *
 * @return @a child, or NULL on bad arguments (NULL @a parent/@a child, @a child
 *         already attached, or @a sibling not a child of @a parent).
 */
AxlNTree *
axl_ntree_insert_before(
    AxlNTree *parent,   ///< parent owning @a sibling
    AxlNTree *sibling,  ///< existing child to insert before (NULL = append)
    AxlNTree *child     ///< root node to attach
);

/**
 * @brief Convenience: allocate a node for @a data and append it to
 *        @a parent.
 *
 * @return the new child, or NULL on allocation failure / NULL @a parent.
 */
AxlNTree *
axl_ntree_append_data(
    AxlNTree *parent,  ///< parent to receive the new child
    void     *data     ///< payload for the new child
);

// ---------------------------------------------------------------------------
// Reorder / move — reposition an ALREADY-ATTACHED node (or root)
// ---------------------------------------------------------------------------
//
// The insert_* calls above attach a *root* node; these reposition a node
// that may already be in a tree, detaching it from its current spot first.
// Same @a sibling == NULL semantics as the matching insert_* (after NULL =
// first; before NULL = last), so the four common moves are:
//   raise to top    : axl_ntree_move_before(parent, NULL, node)  /* last  */
//   lower to bottom : axl_ntree_move_after (parent, NULL, node)  /* first */
//   place above sib : axl_ntree_move_after (parent, sib,  node)
//   place below sib : axl_ntree_move_before(parent, sib,  node)
// Passing a @a parent different from @a node's current parent reparents it.
// A move that would put @a node inside its own subtree (a cycle) is rejected.

/**
 * @brief Reposition @a node to sit immediately after @a sibling among
 *        @a parent's children, detaching it from its current position first.
 *
 * @a sibling == NULL moves @a node to the first position.  No-op-safe if the
 * node is already there.
 *
 * @return @a node, or NULL on bad arguments (NULL @a parent/@a node,
 *         @a sibling not a child of @a parent, @a node == @a sibling,
 *         or the move would create a cycle — @a node is @a parent or an
 *         ancestor of @a parent).
 */
AxlNTree *
axl_ntree_move_after(
    AxlNTree *parent,   ///< destination parent
    AxlNTree *sibling,  ///< child to position after (NULL = first)
    AxlNTree *node      ///< node to move (may already be attached)
);

/**
 * @brief Reposition @a node to sit immediately before @a sibling among
 *        @a parent's children, detaching it from its current position first.
 *
 * @a sibling == NULL moves @a node to the last position.
 *
 * @return @a node, or NULL on bad arguments (see axl_ntree_move_after).
 */
AxlNTree *
axl_ntree_move_before(
    AxlNTree *parent,   ///< destination parent
    AxlNTree *sibling,  ///< child to position before (NULL = last)
    AxlNTree *node      ///< node to move (may already be attached)
);

// ---------------------------------------------------------------------------
// Navigate / query
// ---------------------------------------------------------------------------

/**
 * @brief First child of @a node, or NULL if @a node is a leaf / NULL.
 */
AxlNTree *
axl_ntree_first_child(
    const AxlNTree *node  ///< node to inspect
);

/**
 * @brief Last child of @a node. O(n). NULL if a leaf / NULL.
 */
AxlNTree *
axl_ntree_last_child(
    const AxlNTree *node  ///< node to inspect
);

/**
 * @brief Nth (0-based) child of @a node. O(n). NULL if out of range / NULL.
 */
AxlNTree *
axl_ntree_nth_child(
    const AxlNTree *node,  ///< parent node
    uint32_t        n      ///< 0-based child index
);

/**
 * @brief Number of immediate children of @a node. O(n).
 */
uint32_t
axl_ntree_n_children(
    const AxlNTree *node  ///< node to inspect
);

/**
 * @brief Walk up to the root of @a node's tree. O(depth).
 *
 * @return the root (== @a node if it has no parent), or NULL if @a node is
 *         NULL.
 */
AxlNTree *
axl_ntree_get_root(
    AxlNTree *node  ///< any node in the tree
);

/**
 * @brief True iff @a node is an ancestor of @a descendant (strict — a node
 *        is not its own ancestor). O(depth).
 */
bool
axl_ntree_is_ancestor(
    const AxlNTree *node,        ///< candidate ancestor
    const AxlNTree *descendant   ///< candidate descendant
);

/**
 * @brief Depth of @a node: the root is 1, its children 2, ... (GLib
 *        convention). O(depth). 0 if @a node is NULL.
 */
uint32_t
axl_ntree_depth(
    const AxlNTree *node  ///< node to measure
);

/**
 * @brief Count the nodes in the subtree rooted at @a node that match
 *        @a flags. O(n).
 */
uint32_t
axl_ntree_n_nodes(
    const AxlNTree        *node,  ///< subtree root
    AxlNTreeTraverseFlags  flags  ///< ALL / LEAVES / NON_LEAVES
);

/**
 * @brief Height of the subtree rooted at @a node: a single node is 1, a
 *        node with children is 1 + max child height. O(n). 0 if NULL.
 */
uint32_t
axl_ntree_max_height(
    const AxlNTree *node  ///< subtree root
);

// ---------------------------------------------------------------------------
// Traverse
// ---------------------------------------------------------------------------

/**
 * @brief Call @a func for each immediate child of @a node matching @a flags.
 *        O(n). Cannot stop early.
 */
void
axl_ntree_children_foreach(
    AxlNTree              *node,   ///< parent whose children are visited
    AxlNTreeTraverseFlags  flags,  ///< ALL / LEAVES / NON_LEAVES
    AxlNTreeForeachFunc    func,   ///< per-child callback
    void                  *user    ///< passed to @a func
);

/**
 * @brief Traverse the subtree rooted at @a root in @a order, visiting nodes
 *        that match @a flags, to a maximum relative depth of @a max_depth.
 *
 * @a func returns true to stop the walk early (its node still counts as
 * visited). @a max_depth <= 0 means unlimited; 1 visits only @a root.
 */
void
axl_ntree_traverse(
    AxlNTree              *root,       ///< subtree root
    AxlNTreeTraverseType   order,      ///< PRE / POST / IN / LEVEL order
    AxlNTreeTraverseFlags  flags,      ///< ALL / LEAVES / NON_LEAVES
    int                    max_depth,  ///< max depth (<= 0 = unlimited)
    AxlNTreeTraverseFunc   func,       ///< per-node callback (true = stop)
    void                  *user        ///< passed to @a func
);

// ---------------------------------------------------------------------------
// Iterator — pull-style, no callback
// ---------------------------------------------------------------------------

/**
 * AxlNTreeIter:
 *
 * Stack-allocated, pull-style cursor over a subtree in **pre-order**
 * (node before children) — the no-callback alternative to
 * axl_ntree_traverse. No internal stack: it walks via the parent/sibling
 * links, so it is O(1) per step and any depth.
 *
 *   AxlNTreeIter it;
 *   axl_ntree_iter_init(&it, root, AXL_NTREE_ALL);
 *   for (AxlNTree *n; (n = axl_ntree_iter_next(&it)) != NULL; ) { ... }
 *
 * `axl_ntree_iter_init_reverse` walks the SAME nodes in the exact reverse
 * of pre-order (deepest-last child first, a node after all its
 * descendants) — i.e. topmost-first for a paint-order tree, the
 * hit-test idiom: pull until the first node whose region contains the
 * point, then stop.
 *
 * Treat the fields as opaque. Restructuring the subtree (unlink / insert /
 * free) during iteration invalidates the iterator. For post/in/level
 * order, use axl_ntree_traverse.
 */
typedef struct {
    AxlNTree              *root;     ///< private: subtree boundary
    AxlNTree              *current;  ///< private: last visited node
    AxlNTreeTraverseFlags  flags;    ///< private: node filter
    bool                   started;  ///< private: has iteration begun
    bool                   reverse;  ///< private: reverse pre-order
} AxlNTreeIter;

/**
 * @brief Position @a iter before the first matching node of the subtree
 *        rooted at @a root, visiting in pre-order, filtered by @a flags.
 *        NULL @a root yields an immediately-exhausted iterator.
 */
void
axl_ntree_iter_init(
    AxlNTreeIter          *iter,   ///< [out] iterator to initialize
    AxlNTree              *root,   ///< subtree root (boundary; not its siblings)
    AxlNTreeTraverseFlags  flags   ///< ALL / LEAVES / NON_LEAVES
);

/**
 * @brief Position @a iter to walk the subtree rooted at @a root in
 *        **reverse pre-order** (the exact reverse of axl_ntree_iter_init) —
 *        topmost-first for a paint-order tree.  Advance with
 *        axl_ntree_iter_next, same as the forward iterator.
 *        NULL @a root yields an immediately-exhausted iterator.
 */
void
axl_ntree_iter_init_reverse(
    AxlNTreeIter          *iter,   ///< [out] iterator to initialize
    AxlNTree              *root,   ///< subtree root (boundary; not its siblings)
    AxlNTreeTraverseFlags  flags   ///< ALL / LEAVES / NON_LEAVES
);

/**
 * @brief Advance to and return the next node in pre-order matching the
 *        iterator's flags.
 *
 * @return the next node, or NULL when the subtree is exhausted.
 */
AxlNTree *
axl_ntree_iter_next(
    AxlNTreeIter *iter  ///< iterator
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NTREE_H */
