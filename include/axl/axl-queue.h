/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-queue.h
 *
 * Double-ended queue built on AxlList. GLib GQueue equivalent.
 * O(1) push/pop at both ends. Struct is exposed for direct access.
 *
 * Can be heap-allocated (axl_queue_new / axl_queue_free) or embedded
 * in another struct / on the stack (axl_queue_init or AXL_QUEUE_INIT,
 * torn down with axl_queue_deinit):
 *   AxlQueue q = AXL_QUEUE_INIT;
 *   axl_queue_push_tail(&q, data);
 *   axl_queue_deinit(&q);   // NOT axl_queue_free — see below
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
 * @brief Initialize a stack-allocated or embedded queue.
 *
 * Pair with axl_queue_deinit (NOT axl_queue_free — that frees the struct
 * pointer, which corrupts a stack/embedded queue).
 */
void
axl_queue_init(
    AxlQueue *queue  ///< queue to initialize
);

/**
 * @brief Tear down a stack-allocated or embedded queue: free all nodes,
 *     reset to empty. Does NOT free the AxlQueue struct itself or the
 *     element data. NULL-safe.
 *
 * The teardown partner for axl_queue_init / AXL_QUEUE_INIT (mirrors
 * axl_ring_buf_deinit). For a heap queue from axl_queue_new, use
 * axl_queue_free instead. To also free element data, use
 * axl_queue_deinit_full.
 */
void
axl_queue_deinit(
    AxlQueue *queue  ///< queue (NULL-safe)
);

/**
 * @brief Like axl_queue_deinit but calls free_func on each element's data
 *     first. Does NOT free the AxlQueue struct itself. NULL-safe.
 *
 * The embedded-queue counterpart of axl_queue_free_full.
 */
void
axl_queue_deinit_full(
    AxlQueue         *queue,      ///< queue (NULL-safe)
    AxlDestroyNotify  free_func   ///< called on each data
);

/**
 * @brief Free a heap queue (from axl_queue_new) and all its nodes. Does
 *     not free element data. NULL-safe.
 *
 * ONLY for heap queues. For a stack/embedded queue (axl_queue_init /
 * AXL_QUEUE_INIT), use axl_queue_deinit — calling axl_queue_free on a
 * non-heap queue frees the struct pointer and corrupts the stack.
 */
void
axl_queue_free(
    AxlQueue *queue  ///< heap queue (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlQueue, axl_queue_free)
#endif

/**
 * @brief Free a heap queue (from axl_queue_new), calling free_func on each
 *     element's data. NULL-safe.
 *
 * ONLY for heap queues; for a stack/embedded queue use
 * axl_queue_deinit_full.
 */
void
axl_queue_free_full(
    AxlQueue         *queue,      ///< heap queue (NULL-safe)
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
