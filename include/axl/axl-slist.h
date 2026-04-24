/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-slist.h:
 *
 * Singly-linked list. GLib GSList equivalent.
 * Node struct is exposed for direct traversal:
 *   for (AxlSList *l = list; l; l = l->next) { use(l->data); }
 *
 * Functions that modify the head return the new head pointer.
 * Always assign back: list = axl_slist_prepend(list, data);
 *
 * Note: axl_slist_append is O(n). For frequent appends, use
 * axl_slist_prepend + axl_slist_reverse, or use AxlList instead.
 */

#ifndef AXL_SLIST_H
#define AXL_SLIST_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-list.h>  /* AxlFunc, AxlDestroyNotify, AxlCompareFunc */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlSList {
    void             *data;
    struct AxlSList  *next;
} AxlSList;

/**
 * @brief Append data to the end. O(n).
 *
 * @return new head.
 */
AxlSList *
axl_slist_append(
    AxlSList *list,  ///< current head (NULL for empty)
    void     *data   ///< data pointer
);

/**
 * @brief Prepend data to the front. O(1).
 *
 * @return new head.
 */
AxlSList *
axl_slist_prepend(
    AxlSList *list,  ///< current head (NULL for empty)
    void     *data   ///< data pointer
);

/**
 * @brief Insert at position. O(n).
 *
 * @return new head.
 */
AxlSList *
axl_slist_insert(
    AxlSList *list,      ///< current head
    void     *data,      ///< data pointer
    int       position   ///< 0-based position
);

/**
 * @brief Insert in sorted order. O(n).
 *
 * @return new head.
 */
AxlSList *
axl_slist_insert_sorted(
    AxlSList       *list,  ///< current head (sorted)
    void           *data,  ///< data to insert
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Remove first match by pointer equality. O(n).
 *
 * @return new head.
 */
AxlSList *
axl_slist_remove(
    AxlSList   *list,  ///< current head
    const void *data   ///< data to find and remove
);

/**
 * @brief Reverse in place. O(n).
 *
 * @return new head (was tail).
 */
AxlSList *
axl_slist_reverse(
    AxlSList *list  ///< current head
);

/**
 * @brief Concatenate two lists. O(n) in list1.
 *
 * @return head of combined list.
 */
AxlSList *
axl_slist_concat(
    AxlSList *list1,  ///< first list
    AxlSList *list2   ///< appended to list1
);

/**
 * @brief Sort using merge sort. O(n log n), stable.
 *
 * @return new head.
 */
AxlSList *
axl_slist_sort(
    AxlSList       *list,  ///< current head
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Shallow copy. O(n).
 *
 * @return new list head, or NULL on failure.
 */
AxlSList *
axl_slist_copy(
    AxlSList *list  ///< list to copy
);

/**
 * @brief Free all nodes. Does not free element data.
 */
void
axl_slist_free(
    AxlSList *list  ///< head (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSList, axl_slist_free)
#endif

/**
 * @brief Free all nodes, calling free_func on each element's data.
 */
void
axl_slist_free_full(
    AxlSList         *list,       ///< head (NULL-safe)
    AxlDestroyNotify  free_func   ///< called on each data
);

/**
 * @brief Count elements. O(n).
 *
 * @return number of elements.
 */
size_t
axl_slist_length(
    AxlSList *list  ///< head
);

/**
 * @brief Get nth node. O(n).
 *
 * @return node, or NULL if out of range.
 */
AxlSList *
axl_slist_nth(
    AxlSList *list,  ///< head
    size_t    n      ///< 0-based index
);

/**
 * @brief Get data from nth node. O(n).
 *
 * @return data pointer, or NULL if out of range.
 */
void *
axl_slist_nth_data(
    AxlSList *list,  ///< head
    size_t    n      ///< 0-based index
);

/**
 * @brief Get last node. O(n).
 *
 * @return last node, or NULL.
 */
AxlSList *
axl_slist_last(
    AxlSList *list  ///< head
);

/**
 * @brief Find by pointer equality. O(n).
 *
 * @return matching node, or NULL.
 */
AxlSList *
axl_slist_find(
    AxlSList   *list,  ///< head
    const void *data   ///< data to find
);

/**
 * @brief Find with custom comparator. O(n).
 *
 * @return first node where func(node->data, data) == 0, or NULL.
 */
AxlSList *
axl_slist_find_custom(
    AxlSList       *list,  ///< head
    const void     *data,  ///< data to compare against
    AxlCompareFunc  func   ///< comparison function
);

/**
 * @brief Call func for each element. O(n).
 */
void
axl_slist_foreach(
    AxlSList *list,       ///< head
    AxlFunc   func,       ///< callback
    void     *user_data   ///< passed to callback
);

/**
 * @brief Insert data before the given sibling node. O(n).
 *
 * If sibling is NULL, appends to the end.
 *
 * @return new head of the list.
 */
AxlSList *
axl_slist_insert_before(
    AxlSList *list,     ///< current head
    AxlSList *sibling,  ///< node to insert before (NULL = append)
    void     *data      ///< data pointer to store
);

/**
 * @brief Remove ALL nodes matching data (pointer equality). O(n).
 *
 * Frees the nodes, not the data.
 *
 * @return new head of the list.
 */
AxlSList *
axl_slist_remove_all(
    AxlSList   *list,  ///< current head
    const void *data   ///< data pointer to match
);

/**
 * @brief Unlink a specific node without freeing it. O(n).
 *
 * The caller is responsible for freeing the unlinked node.
 * The node's next pointer is set to NULL after unlinking.
 *
 * @return new head of the list.
 */
AxlSList *
axl_slist_remove_link(
    AxlSList *list,  ///< current head
    AxlSList *link   ///< node to unlink
);

/**
 * @brief Sort with context-aware comparator. O(n log n), stable.
 *
 * @return new head of the sorted list.
 */
AxlSList *
axl_slist_sort_with_data(
    AxlSList           *list,       ///< current head
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
AxlSList *
axl_slist_copy_deep(
    AxlSList    *list,       ///< list to copy
    AxlCopyFunc  func,       ///< copy function for each element
    void        *user_data   ///< passed to func
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SLIST_H */
