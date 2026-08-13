/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-rb-tree.h
 *
 * A generic, intrusive, augmentable red-black tree.
 *
 * Intrusive: the caller embeds an AxlRBNode in its own struct and
 * recovers the struct with AXL_RB_ENTRY; the tree never allocates or
 * frees nodes. Generic: no key type or comparator is baked in — the
 * caller descends to the insertion point itself (by key, by position,
 * by weighted sum), so the same tree serves ordered maps,
 * order-statistic trees, and the byte/newline-weighted piece tree.
 *
 * Augmentation: an optional recompute callback recomputes a node's
 * cached aggregate(s) (subtree size, subtree byte/newline sums, …) from
 * its own payload plus its children's aggregates. The tree invokes it
 * bottom-up after every structural change, so the aggregates stay exact
 * with O(log n) work per edit.
 *
 * This is the substrate behind AxlPieceTree. It is distinct from
 * AxlTree, which is a non-intrusive, opaque, key->value AVL map.
 *
 * Reimplemented under Apache-2.0 from the textbook red-black algorithm
 * (CLRS) and the well-known intrusive/augment API pattern. No GPL
 * (e.g. Linux kernel rbtree) source is used.
 *
 * Single-threaded (UEFI).
 */

#ifndef AXL_RB_TREE_H
#define AXL_RB_TREE_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AXL_RB_RED   = 0,
    AXL_RB_BLACK = 1,
} AxlRBColor;

/**
 * AxlRBNode:
 *
 * Intrusive tree link. Embed one in your struct and recover the struct
 * with AXL_RB_ENTRY. Fields are managed by the tree — treat as opaque.
 */
typedef struct AxlRBNode AxlRBNode;
struct AxlRBNode {
    AxlRBNode *parent;
    AxlRBNode *left;
    AxlRBNode *right;
    AxlRBColor color;
};

/**
 * AxlRBRecompute:
 *
 * Recompute @p node's cached aggregate(s) from its own payload and its
 * children's aggregates (read the children via AXL_RB_ENTRY on
 * node->left / node->right; a NULL child contributes the identity).
 * Invoked bottom-up after structural changes — when called, both
 * children already hold correct aggregates. May be NULL (a plain
 * balanced tree with no augmentation).
 */
typedef void (*AxlRBRecompute)(AxlRBNode *node, void *user) AXL_CB_NOEXCEPT;

/**
 * AxlRBTree:
 *
 * Tree handle — a caller-embedded value (no allocation; the tree owns
 * nothing). Initialize with axl_rb_tree_init.
 */
typedef struct {
    AxlRBNode      *root;
    AxlRBRecompute  recompute;   ///< augmentation hook (NULL = none)
    void           *user;        ///< cookie passed to @c recompute
} AxlRBTree;

/**
 * @brief Recover the embedding struct from a node pointer.
 *
 * @param ptr     AxlRBNode pointer
 * @param type    embedding struct type
 * @param member  name of the AxlRBNode field in @p type
 */
#define AXL_RB_ENTRY(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @brief Initialize an empty tree.
 */
void
axl_rb_tree_init(
    AxlRBTree      *t,          ///< tree
    AxlRBRecompute  recompute,  ///< augmentation hook, or NULL
    void           *user        ///< cookie for @p recompute
);

/**
 * @brief Whether the tree has no nodes.
 */
bool
axl_rb_tree_is_empty(
    const AxlRBTree *t  ///< tree
);

/**
 * @brief Link a new node into a found slot as a red leaf.
 *
 * Step 1 of insertion: the caller descends to the insertion point,
 * tracking the @p parent and the address of the child slot @p link
 * (`&parent->left` or `&parent->right`, or `&tree->root` for the first
 * node). This links @p node there; call axl_rb_insert() next to
 * rebalance.
 */
void
axl_rb_link_node(
    AxlRBNode  *node,    ///< node to link (payload already set)
    AxlRBNode  *parent,  ///< parent node (NULL for the root)
    AxlRBNode **link     ///< child slot to write @p node into
);

/**
 * @brief Rebalance after axl_rb_link_node and propagate augmentation.
 *
 * Step 2 of insertion. Restores the red-black invariants (rotations as
 * needed) and brings every affected node's aggregate up to date.
 */
void
axl_rb_insert(
    AxlRBTree *t,    ///< tree
    AxlRBNode *node  ///< the just-linked node
);

/**
 * @brief Remove a node and rebalance.
 *
 * Restores the red-black invariants and updates augmentation. The
 * node's memory is the caller's to reuse or free afterward.
 */
void
axl_rb_erase(
    AxlRBTree *t,    ///< tree
    AxlRBNode *node  ///< node to remove (must be in @p t)
);

/**
 * @brief Re-propagate augmentation after an in-place payload change.
 *
 * Call when a node's payload changed in a way that affects its
 * aggregate but its tree position did not (e.g. trimming a piece's
 * length). Recomputes from @p node to the root.
 */
void
axl_rb_update_augment(
    AxlRBTree *t,    ///< tree
    AxlRBNode *node  ///< node whose payload changed
);

/// @brief First (in-order minimum) node, or NULL if empty.
AxlRBNode *
axl_rb_first(
    const AxlRBTree *t  ///< tree
);

/// @brief Last (in-order maximum) node, or NULL if empty.
AxlRBNode *
axl_rb_last(
    const AxlRBTree *t  ///< tree
);

/// @brief In-order successor of @p node, or NULL.
AxlRBNode *
axl_rb_next(
    const AxlRBNode *node  ///< node
);

/// @brief In-order predecessor of @p node, or NULL.
AxlRBNode *
axl_rb_prev(
    const AxlRBNode *node  ///< node
);

/**
 * @brief Verify every red-black invariant, for tests and debugging.
 *
 * An in-order walk proves the tree is SORTED; it says nothing about
 * whether it is BALANCED. A tree that has degenerated into a linked
 * list still yields correct answers to every query, just in O(n) — so
 * a traversal-based test passes while the structure the tree exists
 * for is gone. This is the check that notices.
 *
 * Verified, in order:
 *   1. the root is black, and has no parent;
 *   2. every node's `left`/`right` child names it as `parent`;
 *   3. no red node has a red child;
 *   4. every root-to-leaf path has the same black height;
 *   5. the node count matches an in-order walk (no cycles, nothing
 *      orphaned).
 *
 * O(n) and allocation-free, so it is usable inside a loop that mutates
 * the tree — which is exactly how a rebalancing bug gets localised to
 * the operation that caused it rather than discovered later.
 *
 * @return true if every invariant holds (an empty tree trivially
 *     passes); false otherwise, with the first failure logged at
 *     warning level.
 */
bool
axl_rb_check_invariants(
    const AxlRBTree *t  ///< tree
);

/**
 * @brief Black height of the tree, or -1 if it is not uniform.
 *
 * The count of black NODES on any root-to-NULL path, not counting the
 * NULL leaf itself -- so a single black root is 1, and an empty tree is 0.
 * The tree's own balance metric. Exposed separately from axl_rb_check_invariants()
 * because a test that ASSERTS a bound ("stays under 2*log2(n+1)")
 * catches a tree drifting toward degenerate while every invariant
 * still technically holds.
 *
 * @return black height (0 for an empty tree), or -1 if paths disagree.
 */
int
axl_rb_black_height(
    const AxlRBTree *t  ///< tree
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RB_TREE_H */
