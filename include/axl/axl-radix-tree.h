/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-radix-tree.h
 *
 * Radix tree (compact prefix tree) with string keys. Supports exact
 * lookup, longest-prefix lookup, insert, remove, and iteration.
 *
 * Ideal for URL routing, path matching, and any scenario where
 * longest-prefix matching is needed. Lookup is O(k) where k is the
 * key length, independent of the number of entries.
 *
 * API mirrors GLib/AXL conventions (axl_radix_tree_*).
 */

#ifndef AXL_RADIX_TREE_H
#define AXL_RADIX_TREE_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-hash-table.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlRadixTree AxlRadixTree;

// ---------------------------------------------------------------------------
// Construction and destruction
// ---------------------------------------------------------------------------

/**
 * @brief Create a new radix tree.
 *
 * Values are not freed on removal or tree destruction.
 *
 * @return new AxlRadixTree, or NULL on allocation failure.
 */
AxlRadixTree *
axl_radix_tree_new(void);

/**
 * @brief Create a new radix tree with a value destructor.
 *
 * @return new AxlRadixTree, or NULL on allocation failure.
 */
AxlRadixTree *
axl_radix_tree_new_full(
    AxlDestroyNotify value_free  ///< value destructor, or NULL
);

/**
 * @brief Free a radix tree and all entries.
 *
 * Calls value_free on each entry's value (if set).
 */
void
axl_radix_tree_free(
    AxlRadixTree *tree  ///< radix tree (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlRadixTree, axl_radix_tree_free)
#endif

// ---------------------------------------------------------------------------
// Core operations
// ---------------------------------------------------------------------------

/**
 * @brief Insert or replace a key-value pair.
 *
 * If the key already exists, the old value is freed via value_free
 * (if set) and replaced with the new value.
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_radix_tree_insert(
    AxlRadixTree *tree,   ///< radix tree
    const char   *key,    ///< string key (copied internally)
    void         *value   ///< value pointer
);

/**
 * @brief Exact lookup of a key.
 *
 * @return value pointer, or NULL if not found.
 */
void *
axl_radix_tree_lookup(
    AxlRadixTree *tree,  ///< radix tree
    const char   *key    ///< key to look up
);

/**
 * @brief Longest-prefix lookup.
 *
 * Finds the longest inserted key that is a prefix of @p key and
 * returns its value. Sets @p *suffix to point into @p key at the
 * first character after the matched prefix. The caller can compute
 * the prefix length as @p *suffix - @p key.
 *
 * On no match, @p *suffix is not modified. Pass NULL for @p suffix
 * if the suffix pointer is not needed.
 *
 * @return value pointer, or NULL if no prefix matches.
 */
void *
axl_radix_tree_lookup_prefix(
    AxlRadixTree  *tree,    ///< radix tree
    const char    *key,     ///< key to match against
    const char   **suffix   ///< receives pointer into key after matched prefix
);

/**
 * @brief Remove a key.
 *
 * Calls value_free on the value (if set). Collapses intermediate
 * nodes with a single child.
 *
 * @return true if removed, false if not found.
 */
bool
axl_radix_tree_remove(
    AxlRadixTree *tree,  ///< radix tree
    const char   *key    ///< key to remove
);

/**
 * @brief Get the number of entries.
 *
 * @return entry count.
 */
size_t
axl_radix_tree_size(
    AxlRadixTree *tree  ///< radix tree
);

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

/**
 * @brief Iterate all entries in depth-first order.
 *
 * The key passed to @p func is a reconstructed NUL-terminated string
 * valid only for the duration of the callback.
 */
void
axl_radix_tree_foreach(
    AxlRadixTree             *tree,  ///< radix tree
    AxlHashTableForeachFunc   func,  ///< callback(key, value, data)
    void                     *data   ///< opaque data passed to callback
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_RADIX_TREE_H */
