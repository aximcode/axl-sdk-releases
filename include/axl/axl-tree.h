/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tree.h
 *
 * Balanced sorted map. GLib GTree equivalent, AVL-backed.
 *
 * An ordered key->value store with O(log n) insert / lookup / remove
 * and, unlike a hash table, in-order (sorted) iteration plus
 * range / nearest-key queries (lower_bound / upper_bound). Keys are
 * ordered by a caller-supplied AxlCompareDataFunc.
 *
 * Opaque container (the node internals stay private). Single-threaded,
 * no locking. Choose this over:
 *   - axl-hash-table  when you need ordered iteration or range queries
 *                     (the hash table is unordered, O(1));
 *   - axl-radix-tree  when keys are not strings or you need general
 *                     ordering rather than longest-prefix lookup;
 *   - axl-ntree       when you need a parent/child hierarchy, not a map.
 */

#ifndef AXL_TREE_H
#define AXL_TREE_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlTree AxlTree;  ///< opaque AVL sorted map

/**
 * AxlTreeForeachFunc:
 *
 * Per-entry callback for axl_tree_foreach, called in ascending key
 * order. Return true to STOP iteration early, false to continue.
 */
typedef bool (*AxlTreeForeachFunc)(
    void *key,    ///< entry key
    void *value,  ///< entry value
    void *user    ///< caller-provided context
) AXL_CB_NOEXCEPT;

// ---------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------

/**
 * @brief Create an empty tree ordered by @a cmp. Keys and values are
 *        borrowed (not freed by the tree).
 *
 * @return the tree, or NULL on allocation failure / NULL @a cmp.
 */
AxlTree *
axl_tree_new(
    AxlCompareDataFunc  cmp,      ///< key comparator (required)
    void               *cmp_user  ///< context passed to @a cmp
);

/**
 * @brief Create an empty tree that OWNS keys and/or values: the matching
 *        destroy callback (if non-NULL) is called when an entry is
 *        removed/replaced or the tree is freed.
 *
 * @return the tree, or NULL on allocation failure / NULL @a cmp.
 */
AxlTree *
axl_tree_new_full(
    AxlCompareDataFunc  cmp,            ///< key comparator (required)
    void               *cmp_user,       ///< context passed to @a cmp
    AxlDestroyNotify    key_destroy,    ///< key destructor, or NULL to borrow
    AxlDestroyNotify    value_destroy   ///< value destructor, or NULL to borrow
);

/**
 * @brief Free the tree and every node, running the key/value destroy
 *        callbacks (if set) on each entry. NULL-safe.
 */
void
axl_tree_free(
    AxlTree *tree  ///< tree to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTree, axl_tree_free)
#endif

// ---------------------------------------------------------------------------
// Mutate
// ---------------------------------------------------------------------------

/**
 * @brief Insert @a key -> @a value, keeping the EXISTING key on a collision
 *        (GTree semantics).
 *
 * If an equal key is already present: the stored key is kept and the new
 * @a key is destroyed (if a key destructor is set); the old value is
 * destroyed (if a value destructor is set) and replaced by @a value.
 * Otherwise a new node is created.
 */
void
axl_tree_insert(
    AxlTree *tree,   ///< target tree
    void    *key,    ///< key (ownership taken iff new + key_destroy set)
    void    *value   ///< value (ownership taken iff value_destroy set)
);

/**
 * @brief Insert @a key -> @a value, replacing BOTH key and value on a
 *        collision (GTree semantics).
 *
 * If an equal key is already present: the OLD key and value are both
 * destroyed (if destructors are set) and replaced by @a key / @a value.
 */
void
axl_tree_replace(
    AxlTree *tree,   ///< target tree
    void    *key,    ///< key (replaces the stored key)
    void    *value   ///< value (replaces the stored value)
);

/**
 * @brief Remove the entry equal to @a key, running the key/value destroy
 *        callbacks (if set).
 *
 * @return true if an entry was removed, false if @a key was absent.
 */
bool
axl_tree_remove(
    AxlTree    *tree,  ///< target tree
    const void *key    ///< key to remove
);

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

/**
 * @brief Look up the value stored for @a key.
 *
 * @return the value, or NULL if @a key is absent (NULL is also a valid
 *         stored value — use axl_tree_lookup_extended to disambiguate).
 */
void *
axl_tree_lookup(
    AxlTree    *tree,  ///< tree to search
    const void *key    ///< key to find
);

/**
 * @brief Look up @a key, reporting the stored key and value separately.
 *
 * @return true if found (then *@a found_key / *@a value are filled, if
 *         non-NULL); false otherwise (outputs untouched).
 */
bool
axl_tree_lookup_extended(
    AxlTree     *tree,       ///< tree to search
    const void  *key,        ///< key to find
    void       **found_key,  ///< [out] stored key, or NULL to ignore
    void       **value       ///< [out] stored value, or NULL to ignore
);

/**
 * @brief Number of entries. O(1).
 */
uint32_t
axl_tree_nnodes(
    const AxlTree *tree  ///< tree to measure (NULL-safe → 0)
);

/**
 * @brief Height of the tree (0 if empty, 1 for a single node). O(1).
 */
uint32_t
axl_tree_height(
    const AxlTree *tree  ///< tree to measure (NULL-safe → 0)
);

// ---------------------------------------------------------------------------
// Ordered iteration / range queries (the reason to pick this over a hash)
// ---------------------------------------------------------------------------

/**
 * @brief Call @a func for every entry in ascending key order; stops early
 *        if @a func returns true.
 */
void
axl_tree_foreach(
    AxlTree            *tree,  ///< tree to iterate
    AxlTreeForeachFunc  func,  ///< per-entry callback (true = stop)
    void               *user   ///< passed to @a func
);

/// Max tree height the stack-based iterator handles without heap — the
/// AVL height bound (~1.44·log2 n) stays under this for any tree that
/// fits in addressable memory (n up to ~10^9).
#define AXL_TREE_ITER_MAX_DEPTH  48

/**
 * AxlTreeIter:
 *
 * Stack-allocated, pull-style cursor over a tree's entries in ascending
 * key order — the no-callback alternative to axl_tree_foreach. Declare
 * one on the stack, axl_tree_iter_init it, then loop on
 * axl_tree_iter_next:
 *
 *   AxlTreeIter it;
 *   void *k, *v;
 *   axl_tree_iter_init(&it, tree);
 *   while (axl_tree_iter_next(&it, &k, &v)) { ... }
 *
 * Treat the fields as opaque. The iterator holds raw pointers to the
 * tree's nodes, so any insert/remove/free on the tree during iteration
 * invalidates it — a later axl_tree_iter_next would then read a moved or
 * freed node (use-after-free). Don't mutate the tree while iterating.
 */
typedef struct {
    void *stack[AXL_TREE_ITER_MAX_DEPTH];  ///< private: path of pending nodes
    int   top;                             ///< private: stack depth
} AxlTreeIter;

/**
 * @brief Position @a iter before the first (smallest-key) entry of @a tree.
 *        NULL @a tree yields an immediately-exhausted iterator.
 */
void
axl_tree_iter_init(
    AxlTreeIter *iter,  ///< [out] iterator to initialize
    AxlTree     *tree   ///< tree to iterate (borrowed for the iterator's life)
);

/**
 * @brief Advance to the next entry in ascending key order.
 *
 * Writes the entry's key/value to @a key / @a value (each may be NULL to
 * ignore) and returns true; returns false when the iteration is
 * exhausted (outputs untouched).
 */
bool
axl_tree_iter_next(
    AxlTreeIter  *iter,   ///< iterator
    void        **key,    ///< [out] entry key, or NULL to ignore
    void        **value   ///< [out] entry value, or NULL to ignore
);

/**
 * @brief Value of the first entry whose key is >= @a key (the lower
 *        bound).
 *
 * @return the value, or NULL if every key is < @a key (or the tree is
 *         empty).
 */
void *
axl_tree_lower_bound(
    AxlTree    *tree,  ///< tree to search
    const void *key    ///< bound key
);

/**
 * @brief Value of the first entry whose key is > @a key (the upper
 *        bound — strictly greater).
 *
 * @return the value, or NULL if every key is <= @a key (or the tree is
 *         empty).
 */
void *
axl_tree_upper_bound(
    AxlTree    *tree,  ///< tree to search
    const void *key    ///< bound key
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TREE_H */
