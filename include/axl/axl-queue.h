/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-queue.h:
 *
 * Double-ended queue built on AxlList. GLib GQueue equivalent.
 * O(1) push/pop at both ends. Struct is exposed for direct access.
 *
 * Can be heap-allocated (axl_queue_new) or stack-allocated:
 *   AxlQueue q = AXL_QUEUE_INIT;
 *   axl_queue_push_tail(&q, data);
 */

#ifndef AXL_QUEUE_H
#define AXL_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-macros.h>
#include <axl/axl-list.h>  /* AxlList, AxlFunc, AxlDestroyNotify, AxlCompareFunc */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    AxlList *head;
    AxlList *tail;
    size_t   length;
} AxlQueue;

/// Static initializer for stack-allocated queues.
#define AXL_QUEUE_INIT { NULL, NULL, 0 }

/**
 * @brief Allocate a new empty queue.
 *
 * @return new queue, or NULL on failure. Free with axl_queue_free().
 */
AxlQueue *
axl_queue_new(void);

/**
 * @brief Initialize a stack-allocated queue.
 */
void
axl_queue_init(
    AxlQueue *queue  ///< queue to initialize
);

/**
 * @brief Free queue and all nodes. Does not free element data.
 */
void
axl_queue_free(
    AxlQueue *queue  ///< queue (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlQueue, axl_queue_free)
#endif

/**
 * @brief Free queue, calling free_func on each element's data.
 */
void
axl_queue_free_full(
    AxlQueue         *queue,      ///< queue (NULL-safe)
    AxlDestroyNotify  free_func   ///< called on each data
);

/**
 * @brief Remove all elements. Queue itself is not freed.
 */
void
axl_queue_clear(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Check if the queue is empty.
 *
 * @return true if empty.
 */
bool
axl_queue_is_empty(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Get the number of elements. O(1).
 *
 * @return element count.
 */
size_t
axl_queue_get_length(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Push data to the front. O(1).
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_queue_push_head(
    AxlQueue *queue,  ///< queue
    void     *data    ///< data pointer
);

/**
 * @brief Push data to the back. O(1).
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_queue_push_tail(
    AxlQueue *queue,  ///< queue
    void     *data    ///< data pointer
);

/**
 * @brief Remove and return the front element. O(1).
 *
 * @return data pointer, or NULL if empty.
 */
void *
axl_queue_pop_head(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Remove and return the back element. O(1).
 *
 * @return data pointer, or NULL if empty.
 */
void *
axl_queue_pop_tail(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Peek at the front element without removing. O(1).
 *
 * @return data pointer, or NULL if empty.
 */
void *
axl_queue_peek_head(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Peek at the back element without removing. O(1).
 *
 * @return data pointer, or NULL if empty.
 */
void *
axl_queue_peek_tail(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Peek at the nth element. O(n).
 *
 * @return data pointer, or NULL if out of range.
 */
void *
axl_queue_peek_nth(
    AxlQueue *queue,  ///< queue
    size_t    n       ///< 0-based index from head
);

/**
 * @brief Call func for each element, head to tail. O(n).
 */
void
axl_queue_foreach(
    AxlQueue *queue,      ///< queue
    AxlFunc   func,       ///< callback
    void     *user_data   ///< passed to callback
);

/**
 * @brief Shallow copy. O(n).
 *
 * @return new queue, or NULL on failure.
 */
AxlQueue *
axl_queue_copy(
    AxlQueue *queue  ///< queue to copy
);

/**
 * @brief Reverse the queue in place. O(n).
 */
void
axl_queue_reverse(
    AxlQueue *queue  ///< queue
);

/**
 * @brief Sort the queue using merge sort. O(n log n).
 */
void
axl_queue_sort(
    AxlQueue       *queue,  ///< queue
    AxlCompareFunc  func    ///< comparison function
);

/**
 * @brief Find the first node matching data (pointer equality). O(n).
 *
 * @return matching AxlList node, or NULL.
 */
AxlList *
axl_queue_find(
    AxlQueue   *queue,  ///< queue
    const void *data    ///< data pointer to find
);

/**
 * @brief Find using a custom comparator. O(n).
 *
 * Returns the first node where func(node->data, data) == 0.
 *
 * @return matching AxlList node, or NULL.
 */
AxlList *
axl_queue_find_custom(
    AxlQueue       *queue,  ///< queue
    const void     *data,   ///< data to compare against
    AxlCompareFunc  func    ///< comparison function
);

/**
 * @brief Remove the first node matching data (pointer equality). O(n).
 *
 * Frees the node, not the data. Updates head/tail/length.
 *
 * @return true if a match was found and removed.
 */
bool
axl_queue_remove(
    AxlQueue   *queue,  ///< queue
    const void *data    ///< data pointer to find and remove
);

/**
 * @brief Remove ALL nodes matching data (pointer equality). O(n).
 *
 * Frees the nodes, not the data. Updates head/tail/length.
 *
 * @return number of nodes removed.
 */
size_t
axl_queue_remove_all(
    AxlQueue   *queue,  ///< queue
    const void *data    ///< data pointer to match
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_QUEUE_H */
