/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-list.c
    Doubly-linked list (GLib GList equivalent).
**/

#include "../backend/axl-backend.h"
#include <axl/axl-list.h>
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
        axl_error("failed to allocate list node");
        return NULL;
    }

    node->data = data;
    node->next = NULL;
    node->prev = NULL;

    return node;
}

/**
 * Split the list at its midpoint.  Returns the second half.
 * Uses slow/fast pointer technique to find the middle.
 */
static AxlList *
split_at_mid(AxlList *list)
{
    AxlList *slow;
    AxlList *fast;
    AxlList *second;

    slow = list;
    fast = list->next;

    while (fast != NULL) {
        fast = fast->next;
        if (fast != NULL) {
            slow = slow->next;
            fast = fast->next;
        }
    }

    second = slow->next;
    slow->next = NULL;
    if (second != NULL) {
        second->prev = NULL;
    }

    return second;
}

/**
 * Merge two sorted lists using a context-aware comparator.
 */
static AxlList *
merge_sorted_data(
    AxlList            *a,
    AxlList            *b,
    AxlCompareDataFunc  func,
    void               *user_data)
{
    AxlList  head;
    AxlList *tail;

    head.next = NULL;
    head.prev = NULL;
    head.data = NULL;
    tail = &head;

    while (a != NULL && b != NULL) {
        if (func(a->data, b->data, user_data) <= 0) {
            tail->next = a;
            a->prev = tail;
            a = a->next;
        } else {
            tail->next = b;
            b->prev = tail;
            b = b->next;
        }
        tail = tail->next;
    }

    if (a != NULL) {
        tail->next = a;
        a->prev = tail;
    } else {
        tail->next = b;
        if (b != NULL) {
            b->prev = tail;
        }
    }

    /* Detach from the sentinel. head.next can still be NULL if both
       a and b entered as NULL — merge_sort's recursion never calls
       us that way today, but guard anyway so the function is safe
       to call from elsewhere. */
    if (head.next != NULL) {
        head.next->prev = NULL;
    }

    return head.next;
}

/**
 * Recursive merge sort with context-aware comparator.
 */
static AxlList *
merge_sort_data(
    AxlList            *list,
    AxlCompareDataFunc  func,
    void               *user_data)
{
    AxlList *second;

    if (list == NULL || list->next == NULL) {
        return list;
    }

    second = split_at_mid(list);
    list   = merge_sort_data(list, func, user_data);
    second = merge_sort_data(second, func, user_data);

    return merge_sorted_data(list, second, func, user_data);
}

/**
 * Merge two sorted lists into one sorted list.
 */
static AxlList *
merge_sorted(
    AxlList        *a,
    AxlList        *b,
    AxlCompareFunc  func)
{
    AxlList  head;
    AxlList *tail;

    head.next = NULL;
    head.prev = NULL;
    head.data = NULL;
    tail = &head;

    while (a != NULL && b != NULL) {
        if (func(a->data, b->data) <= 0) {
            tail->next = a;
            a->prev = tail;
            a = a->next;
        } else {
            tail->next = b;
            b->prev = tail;
            b = b->next;
        }
        tail = tail->next;
    }

    if (a != NULL) {
        tail->next = a;
        a->prev = tail;
    } else {
        tail->next = b;
        if (b != NULL) {
            b->prev = tail;
        }
    }

    /* Detach from the sentinel — see merge_sorted_data above. */
    if (head.next != NULL) {
        head.next->prev = NULL;
    }

    return head.next;
}

/**
 * Recursive merge sort for a doubly-linked list.
 */
static AxlList *
merge_sort(
    AxlList        *list,
    AxlCompareFunc  func)
{
    AxlList *second;

    if (list == NULL || list->next == NULL) {
        return list;
    }

    second = split_at_mid(list);
    list   = merge_sort(list, func);
    second = merge_sort(second, func);

    return merge_sorted(list, second, func);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlList *
axl_list_append(
    AxlList *list,
    void    *data)
{
    AxlList *node;
    AxlList *last;

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    if (list == NULL) {
        return node;
    }

    last = list;
    while (last->next != NULL) {
        last = last->next;
    }

    last->next = node;
    node->prev = last;

    return list;
}

AxlList *
axl_list_prepend(
    AxlList *list,
    void    *data)
{
    AxlList *node;

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    node->next = list;
    if (list != NULL) {
        list->prev = node;
    }

    return node;
}

AxlList *
axl_list_insert(
    AxlList *list,
    void    *data,
    int      position)
{
    AxlList *node;
    AxlList *cur;
    int      i;

    if (list == NULL || position == 0) {
        return axl_list_prepend(list, data);
    }

    if (position < 0) {
        return axl_list_append(list, data);
    }

    cur = list;
    for (i = 0; i < position && cur != NULL; i++) {
        cur = cur->next;
    }

    /* Out of range -- append */
    if (cur == NULL) {
        return axl_list_append(list, data);
    }

    /* Insert before cur */
    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    node->prev = cur->prev;
    node->next = cur;
    cur->prev = node;
    if (node->prev != NULL) {
        node->prev->next = node;
    }

    return list;
}

AxlList *
axl_list_insert_sorted(
    AxlList        *list,
    void           *data,
    AxlCompareFunc  func)
{
    AxlList *node;
    AxlList *cur;

    if (func == NULL) {
        return list;
    }

    if (list == NULL) {
        return alloc_node(data);
    }

    /* Insert before the first node where data < node->data */
    for (cur = list; cur != NULL; cur = cur->next) {
        if (func(data, cur->data) <= 0) {
            node = alloc_node(data);
            if (node == NULL) {
                return list;
            }

            node->prev = cur->prev;
            node->next = cur;
            cur->prev = node;

            if (node->prev != NULL) {
                node->prev->next = node;
                return list;
            }

            /* New head */
            return node;
        }
    }

    /* data is >= all elements -- append */
    return axl_list_append(list, data);
}

AxlList *
axl_list_remove(
    AxlList    *list,
    const void *data)
{
    AxlList *cur;

    for (cur = list; cur != NULL; cur = cur->next) {
        if (cur->data == data) {
            if (cur->prev != NULL) {
                cur->prev->next = cur->next;
            } else {
                list = cur->next;
            }

            if (cur->next != NULL) {
                cur->next->prev = cur->prev;
            }

            axl_free(cur);
            return list;
        }
    }

    return list;
}

AxlList *
axl_list_reverse(
    AxlList *list)
{
    AxlList *cur;
    AxlList *tmp;
    AxlList *last;

    last = NULL;
    cur = list;

    while (cur != NULL) {
        tmp = cur->prev;
        cur->prev = cur->next;
        cur->next = tmp;
        last = cur;
        cur = cur->prev;
    }

    return last;
}

AxlList *
axl_list_concat(
    AxlList *list1,
    AxlList *list2)
{
    AxlList *last;

    if (list1 == NULL) {
        return list2;
    }

    if (list2 == NULL) {
        return list1;
    }

    last = list1;
    while (last->next != NULL) {
        last = last->next;
    }

    last->next = list2;
    list2->prev = last;

    return list1;
}

AxlList *
axl_list_sort(
    AxlList        *list,
    AxlCompareFunc  func)
{
    if (list == NULL || func == NULL) {
        return list;
    }

    return merge_sort(list, func);
}

AxlList *
axl_list_copy(
    AxlList *list)
{
    AxlList *new_list;
    AxlList *tail;
    AxlList *cur;
    AxlList *node;

    new_list = NULL;
    tail = NULL;
    for (cur = list; cur != NULL; cur = cur->next) {
        node = alloc_node(cur->data);
        if (node == NULL) {
            axl_list_free(new_list);
            return NULL;
        }
        node->prev = tail;
        if (tail != NULL) {
            tail->next = node;
        } else {
            new_list = node;
        }
        tail = node;
    }

    return new_list;
}

void
axl_list_free(
    AxlList *list)
{
    AxlList *cur;
    AxlList *next;

    cur = list;
    while (cur != NULL) {
        next = cur->next;
        axl_free(cur);
        cur = next;
    }
}

void
axl_list_free_full(
    AxlList          *list,
    AxlDestroyNotify  free_func)
{
    AxlList *cur;
    AxlList *next;

    cur = list;
    while (cur != NULL) {
        next = cur->next;
        if (free_func != NULL) {
            free_func(cur->data);
        }
        axl_free(cur);
        cur = next;
    }
}

size_t
axl_list_length(
    AxlList *list)
{
    size_t   n;
    AxlList *cur;

    n = 0;
    for (cur = list; cur != NULL; cur = cur->next) {
        n++;
    }

    return n;
}

AxlList *
axl_list_nth(
    AxlList *list,
    size_t   n)
{
    AxlList *cur;
    size_t   i;

    cur = list;
    for (i = 0; cur != NULL && i < n; i++) {
        cur = cur->next;
    }

    return cur;
}

void *
axl_list_nth_data(
    AxlList *list,
    size_t   n)
{
    AxlList *node;

    node = axl_list_nth(list, n);
    if (node == NULL) {
        return NULL;
    }

    return node->data;
}

AxlList *
axl_list_first(
    AxlList *list)
{
    if (list == NULL) {
        return NULL;
    }

    while (list->prev != NULL) {
        list = list->prev;
    }

    return list;
}

AxlList *
axl_list_last(
    AxlList *list)
{
    if (list == NULL) {
        return NULL;
    }

    while (list->next != NULL) {
        list = list->next;
    }

    return list;
}

AxlList *
axl_list_find(
    AxlList    *list,
    const void *data)
{
    AxlList *cur;

    for (cur = list; cur != NULL; cur = cur->next) {
        if (cur->data == data) {
            return cur;
        }
    }

    return NULL;
}

AxlList *
axl_list_find_custom(
    AxlList        *list,
    const void     *data,
    AxlCompareFunc  func)
{
    AxlList *cur;

    if (func == NULL) {
        return NULL;
    }

    for (cur = list; cur != NULL; cur = cur->next) {
        if (func(cur->data, data) == 0) {
            return cur;
        }
    }

    return NULL;
}

void
axl_list_foreach(
    AxlList *list,
    AxlFunc  func,
    void    *user_data)
{
    AxlList *cur;

    if (func == NULL) {
        return;
    }

    for (cur = list; cur != NULL; cur = cur->next) {
        func(cur->data, user_data);
    }
}

AxlList *
axl_list_insert_before(
    AxlList *list,
    AxlList *sibling,
    void    *data)
{
    AxlList *node;

    if (sibling == NULL) {
        return axl_list_append(list, data);
    }

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    node->prev = sibling->prev;
    node->next = sibling;
    sibling->prev = node;

    if (node->prev != NULL) {
        node->prev->next = node;
        return list;
    }

    /* New head */
    return node;
}

AxlList *
axl_list_insert_after(
    AxlList *list,
    AxlList *sibling,
    void    *data)
{
    AxlList *node;

    if (sibling == NULL) {
        return axl_list_prepend(list, data);
    }

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    node->prev = sibling;
    node->next = sibling->next;

    if (sibling->next != NULL) {
        sibling->next->prev = node;
    }
    sibling->next = node;

    return list;
}

AxlList *
axl_list_remove_all(
    AxlList    *list,
    const void *data)
{
    AxlList *cur;
    AxlList *next;

    cur = list;
    while (cur != NULL) {
        next = cur->next;

        if (cur->data == data) {
            if (cur->prev != NULL) {
                cur->prev->next = cur->next;
            } else {
                list = cur->next;
            }

            if (cur->next != NULL) {
                cur->next->prev = cur->prev;
            }

            axl_free(cur);
        }

        cur = next;
    }

    return list;
}

AxlList *
axl_list_remove_link(
    AxlList *list,
    AxlList *link)
{
    if (link == NULL) {
        return list;
    }

    if (link->prev != NULL) {
        link->prev->next = link->next;
    } else {
        list = link->next;
    }

    if (link->next != NULL) {
        link->next->prev = link->prev;
    }

    link->prev = NULL;
    link->next = NULL;

    return list;
}

AxlList *
axl_list_sort_with_data(
    AxlList            *list,
    AxlCompareDataFunc  func,
    void               *user_data)
{
    if (list == NULL || func == NULL) {
        return list;
    }

    return merge_sort_data(list, func, user_data);
}

AxlList *
axl_list_copy_deep(
    AxlList     *list,
    AxlCopyFunc  func,
    void        *user_data)
{
    AxlList *new_list;
    AxlList *tail;
    AxlList *cur;
    AxlList *node;

    if (func == NULL) {
        return NULL;
    }

    new_list = NULL;
    tail = NULL;
    for (cur = list; cur != NULL; cur = cur->next) {
        node = alloc_node(func(cur->data, user_data));
        if (node == NULL) {
            axl_list_free(new_list);
            return NULL;
        }
        node->prev = tail;
        if (tail != NULL) {
            tail->next = node;
        } else {
            new_list = node;
        }
        tail = node;
    }

    return new_list;
}
