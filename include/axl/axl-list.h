/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-list.h
 *
 * Doubly-linked list. GLib GList equivalent.
 * Node struct is exposed for direct traversal:
 *   for (AxlList *l = list; l; l = l->next) { use(l->data); }
 *
 * Functions that modify the head return the new head pointer.
 * Always assign back: list = axl_list_append(list, data);
 *
 * Also defines shared callback types used by AxlSList and AxlQueue.
 */

#ifndef AXL_LIST_H
#define AXL_LIST_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AxlCopyFunc:
 *
 * Deep copy function (GCopyFunc equivalent).
 * Returns a newly allocated copy of @p src.
 */
typedef void *(*AxlCopyFunc)(
    const void *src,       ///< source data to copy
    void       *user_data  ///< caller-provided context
) AXL_CB_NOEXCEPT;

// ---------------------------------------------------------------------------
// AxlList — doubly-linked list node
// ---------------------------------------------------------------------------

typedef struct AxlList {
    void            *data;
    struct AxlList  *next;
    struct AxlList  *prev;
} AxlList;

/**
 * @brief Append data to the end of the list. O(n).
 *
 * @return new head of the list.
 */
AxlList *
axl_list_append(
    AxlList *list,  ///< current head (NULL for empty list)
    void    *data   ///< data pointer to store
);

/**
 * @brief Prepend data to the front of the list. O(1).
 *
 * @return new head of the list.
 */
AxlList *
axl_list_prepend(
    AxlList *list,  ///< current head (NULL for empty list)
    void    *data   ///< data pointer to store
);

/**
 * @brief Insert data at the given position. O(n).
 *
 * Negative or out-of-range position appends to the end.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_insert(
    AxlList *list,      ///< current head
    void    *data,      ///< data pointer to store
    int      position   ///< 0-based insert position
);

/**
 * @brief Insert data in sorted order. O(n).
 *
 * The list must already be sorted by the same comparator.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_insert_sorted(
    AxlList        *list,  ///< current head (sorted)
    void           *data,  ///< data pointer to insert
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Remove the first element matching data. O(n).
 *
 * Compares by pointer equality. Frees the node, not the data.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_remove(
    AxlList    *list,  ///< current head
    const void *data   ///< data pointer to find and remove
);

/**
 * @brief Reverse the list in place. O(n).
 *
 * @return new head (was the tail).
 */
AxlList *
axl_list_reverse(
    AxlList *list  ///< current head
);

/**
 * @brief Concatenate two lists. O(n) in length of list1.
 *
 * list2 is appended to list1. Both must be distinct lists.
 *
 * @return head of the combined list.
 */
AxlList *
axl_list_concat(
    AxlList *list1,  ///< first list
    AxlList *list2   ///< second list (appended)
);

/**
 * @brief Sort the list using merge sort. O(n log n), stable.
 *
 * @return new head of the sorted list.
 */
AxlList *
axl_list_sort(
    AxlList        *list,  ///< current head
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Shallow copy of the list. O(n).
 *
 * Copies node structure; data pointers are shared, not duplicated.
 *
 * @return head of the new list, or NULL on failure.
 */
AxlList *
axl_list_copy(
    AxlList *list  ///< list to copy
);

/**
 * @brief Free all nodes. Does not free element data.
 */
void
axl_list_free(
    AxlList *list  ///< head (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlList, axl_list_free)
#endif

/**
 * @brief Free all nodes, calling free_func on each element's data.
 */
void
axl_list_free_full(
    AxlList          *list,       ///< head (NULL-safe)
    AxlDestroyNotify  free_func   ///< called on each node's data
);

/**
 * @brief Count elements. O(n).
 *
 * @return number of elements.
 */
size_t
axl_list_length(
    AxlList *list  ///< head
);

/**
 * @brief Get the nth node. O(n).
 *
 * @return node at position n, or NULL if out of range.
 */
AxlList *
axl_list_nth(
    AxlList *list,  ///< head
    size_t   n      ///< 0-based index
);

/**
 * @brief Get data from the nth node. O(n).
 *
 * @return data pointer, or NULL if out of range.
 */
void *
axl_list_nth_data(
    AxlList *list,  ///< head
    size_t   n      ///< 0-based index
);

/**
 * @brief Get the first node (rewind to head). O(n).
 *
 * Useful when you have a pointer to a middle node.
 *
 * @return the first node, or NULL if list is empty.
 */
AxlList *
axl_list_first(
    AxlList *list  ///< any node in the list
);

/**
 * @brief Get the last node. O(n).
 *
 * @return the last node, or NULL if list is empty.
 */
AxlList *
axl_list_last(
    AxlList *list  ///< head
);

/**
 * @brief Find the first node with matching data (pointer equality). O(n).
 *
 * @return matching node, or NULL.
 */
AxlList *
axl_list_find(
    AxlList    *list,  ///< head
    const void *data   ///< data pointer to find
);

/**
 * @brief Find using a custom comparator. O(n).
 *
 * Calls func(node->data, data) for each node. Returns the first
 * node where func returns 0.
 *
 * @return matching node, or NULL.
 */
AxlList *
axl_list_find_custom(
    AxlList        *list,  ///< head
    const void     *data,  ///< data to compare against
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Call func for each element. O(n).
 */
void
axl_list_foreach(
    AxlList *list,       ///< head
    AxlFunc  func,       ///< callback
    void    *user_data   ///< passed to callback
);

/**
 * @brief Insert data before the given sibling node. O(1).
 *
 * If sibling is NULL, appends to the end.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_insert_before(
    AxlList *list,     ///< current head
    AxlList *sibling,  ///< node to insert before (NULL = append)
    void    *data      ///< data pointer to store
);

/**
 * @brief Insert data after the given sibling node. O(1).
 *
 * If sibling is NULL, prepends to the front.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_insert_after(
    AxlList *list,     ///< current head
    AxlList *sibling,  ///< node to insert after (NULL = prepend)
    void    *data      ///< data pointer to store
);

/**
 * @brief Remove ALL nodes matching data (pointer equality). O(n).
 *
 * Frees the nodes, not the data.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_remove_all(
    AxlList    *list,  ///< current head
    const void *data   ///< data pointer to match
);

/**
 * @brief Unlink a specific node without freeing it. O(1).
 *
 * The caller is responsible for freeing the unlinked node.
 * The node's prev/next pointers are set to NULL after unlinking.
 *
 * @return new head of the list.
 */
AxlList *
axl_list_remove_link(
    AxlList *list,  ///< current head
    AxlList *link   ///< node to unlink
);

/**
 * @brief Sort with context-aware comparator. O(n log n), stable.
 *
 * @return new head of the sorted list.
 */
AxlList *
axl_list_sort_with_data(
    AxlList            *list,       ///< current head
    AxlCompareDataFunc  func,       ///< comparison function with user_data
    void               *user_data   ///< passed to every compare call
);

/**
 * @brief Deep copy using a copy function. O(n).
 *
 * Calls func(node->data, user_data) for each node to produce
 * the data pointer for the new list.
 *
 * @return head of the new list, or NULL on failure.
 */
AxlList *
axl_list_copy_deep(
    AxlList     *list,       ///< list to copy
    AxlCopyFunc  func,       ///< copy function for each element
    void        *user_data   ///< passed to func
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_LIST_H */
