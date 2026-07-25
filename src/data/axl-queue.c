/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-queue.c
    Double-ended queue built on AxlList (GLib GQueue equivalent).
**/

#include "../backend/axl-backend.h"
#include <axl/axl-queue.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("data");

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static AxlList *
alloc_node(void *data)
{
    AxlList *node;

    node = axl_malloc(sizeof(AxlList));
    if (node == NULL) {
        axl_error("failed to allocate queue node");
        return NULL;
    }

    node->data = data;
    node->next = NULL;
    node->prev = NULL;

    return node;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlQueue *
axl_queue_new(void)
{
    AxlQueue *queue;

    queue = axl_calloc(1, sizeof(AxlQueue));
    if (queue == NULL) {
        axl_error("failed to allocate queue");
        return NULL;
    }

    return queue;
}

void
axl_queue_init(
    AxlQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    queue->head = NULL;
    queue->tail = NULL;
    queue->length = 0;
}

void
axl_queue_deinit(
    AxlQueue *queue)
{
    /* Free the nodes and reset to empty, but not the struct itself. Same
       operation as axl_queue_clear; distinct name is the teardown partner
       for axl_queue_init (mirrors axl_ring_buf_init/_deinit). */
    axl_queue_clear(queue);
}

void
axl_queue_deinit_full(
    AxlQueue         *queue,
    AxlDestroyNotify  free_func)
{
    if (queue == NULL) {
        return;
    }

    axl_list_free_full(queue->head, free_func);
    queue->head = NULL;
    queue->tail = NULL;
    queue->length = 0;
}

void
axl_queue_free(
    AxlQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    axl_queue_deinit(queue);   /* free nodes; then the struct (heap only) */
    axl_free(queue);
}

void
axl_queue_free_full(
    AxlQueue         *queue,
    AxlDestroyNotify  free_func)
{
    if (queue == NULL) {
        return;
    }

    axl_queue_deinit_full(queue, free_func);   /* free data + nodes */
    axl_free(queue);
}

void
axl_queue_clear(
    AxlQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    axl_list_free(queue->head);
    queue->head = NULL;
    queue->tail = NULL;
    queue->length = 0;
}

bool
axl_queue_is_empty(
    AxlQueue *queue)
{
    if (queue == NULL) {
        return true;
    }

    return queue->length == 0;
}

size_t
axl_queue_get_length(
    AxlQueue *queue)
{
    if (queue == NULL) {
        return 0;
    }

    return queue->length;
}

int
axl_queue_push_head(
    AxlQueue *queue,
    void     *data)
{
    AxlList *node;

    if (queue == NULL) {
        return AXL_ERR;
    }

    node = alloc_node(data);
    if (node == NULL) {
        return AXL_ERR;
    }

    node->next = queue->head;
    if (queue->head != NULL) {
        queue->head->prev = node;
    } else {
        queue->tail = node;
    }

    queue->head = node;
    queue->length++;
    return AXL_OK;
}

int
axl_queue_push_tail(
    AxlQueue *queue,
    void     *data)
{
    AxlList *node;

    if (queue == NULL) {
        return AXL_ERR;
    }

    node = alloc_node(data);
    if (node == NULL) {
        return AXL_ERR;
    }

    node->prev = queue->tail;
    if (queue->tail != NULL) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }

    queue->tail = node;
    queue->length++;
    return AXL_OK;
}

void *
axl_queue_pop_head(
    AxlQueue *queue)
{
    AxlList *node;
    void    *data;

    if (queue == NULL || queue->head == NULL) {
        return NULL;
    }

    node = queue->head;
    data = node->data;

    queue->head = node->next;
    if (queue->head != NULL) {
        queue->head->prev = NULL;
    } else {
        queue->tail = NULL;
    }

    axl_free(node);
    queue->length--;

    return data;
}

void *
axl_queue_pop_tail(
    AxlQueue *queue)
{
    AxlList *node;
    void    *data;

    if (queue == NULL || queue->tail == NULL) {
        return NULL;
    }

    node = queue->tail;
    data = node->data;

    queue->tail = node->prev;
    if (queue->tail != NULL) {
        queue->tail->next = NULL;
    } else {
        queue->head = NULL;
    }

    axl_free(node);
    queue->length--;

    return data;
}

void *
axl_queue_peek_head(
    AxlQueue *queue)
{
    if (queue == NULL || queue->head == NULL) {
        return NULL;
    }

    return queue->head->data;
}

void *
axl_queue_peek_tail(
    AxlQueue *queue)
{
    if (queue == NULL || queue->tail == NULL) {
        return NULL;
    }

    return queue->tail->data;
}

void *
axl_queue_peek_nth(
    AxlQueue *queue,
    size_t    n)
{
    AxlList *node;

    if (queue == NULL || n >= queue->length) {
        return NULL;
    }

    node = axl_list_nth(queue->head, n);
    if (node == NULL) {
        return NULL;
    }

    return node->data;
}

void
axl_queue_foreach(
    AxlQueue *queue,
    AxlFunc   func,
    void     *user_data)
{
    if (queue == NULL) {
        return;
    }

    axl_list_foreach(queue->head, func, user_data);
}

AxlQueue *
axl_queue_copy(
    AxlQueue *queue)
{
    AxlQueue *copy;
    AxlList  *cur;

    if (queue == NULL) {
        return NULL;
    }

    copy = axl_queue_new();
    if (copy == NULL) {
        return NULL;
    }

    for (cur = queue->head; cur != NULL; cur = cur->next) {
        if (axl_queue_push_tail(copy, cur->data) != AXL_OK) {
            axl_queue_free(copy);
            return NULL;
        }
    }

    return copy;
}

void
axl_queue_reverse(
    AxlQueue *queue)
{
    AxlList *old_head;

    if (queue == NULL || queue->head == NULL) {
        return;
    }

    old_head = queue->head;
    queue->head = axl_list_reverse(queue->head);
    queue->tail = old_head;
}

void
axl_queue_sort(
    AxlQueue       *queue,
    AxlCompareFunc  func)
{
    AxlList *cur;

    if (queue == NULL || func == NULL || queue->length <= 1) {
        return;
    }

    queue->head = axl_list_sort(queue->head, func);

    /* Walk to the end to update tail */
    cur = queue->head;
    while (cur->next != NULL) {
        cur = cur->next;
    }
    queue->tail = cur;
}

AxlList *
axl_queue_find(
    AxlQueue   *queue,
    const void *data)
{
    if (queue == NULL) {
        return NULL;
    }

    return axl_list_find(queue->head, data);
}

AxlList *
axl_queue_find_custom(
    AxlQueue       *queue,
    const void     *data,
    AxlCompareFunc  func)
{
    if (queue == NULL) {
        return NULL;
    }

    return axl_list_find_custom(queue->head, data, func);
}

bool
axl_queue_remove(
    AxlQueue   *queue,
    const void *data)
{
    AxlList *cur;

    if (queue == NULL) {
        return false;
    }

    for (cur = queue->head; cur != NULL; cur = cur->next) {
        if (cur->data == data) {
            if (cur->prev != NULL) {
                cur->prev->next = cur->next;
            } else {
                queue->head = cur->next;
            }

            if (cur->next != NULL) {
                cur->next->prev = cur->prev;
            } else {
                queue->tail = cur->prev;
            }

            axl_free(cur);
            queue->length--;
            return true;
        }
    }

    return false;
}

size_t
axl_queue_remove_all(
    AxlQueue   *queue,
    const void *data)
{
    AxlList *cur;
    AxlList *next;
    size_t   removed;

    if (queue == NULL) {
        return 0;
    }

    removed = 0;
    cur = queue->head;
    while (cur != NULL) {
        next = cur->next;

        if (cur->data == data) {
            if (cur->prev != NULL) {
                cur->prev->next = cur->next;
            } else {
                queue->head = cur->next;
            }

            if (cur->next != NULL) {
                cur->next->prev = cur->prev;
            } else {
                queue->tail = cur->prev;
            }

            axl_free(cur);
            queue->length--;
            removed++;
        }

        cur = next;
    }

    return removed;
}
