/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-slist.c
    Singly-linked list. GLib GSList equivalent.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-slist.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("data");

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static AxlSList *
alloc_node(void *data)
{
    AxlSList *node;

    node = axl_malloc(sizeof(AxlSList));
    if (node == NULL) {
        axl_error("failed to allocate slist node");
        return NULL;
    }

    node->data = data;
    node->next = NULL;

    return node;
}

/**
 * @brief Split a list in half using slow/fast pointer technique.
 *
 * After return, the list ending at the slow pointer is terminated
 * (slow->next = NULL), and the second half is returned.
 */
static AxlSList *
split_half(AxlSList *head)
{
    AxlSList *slow;
    AxlSList *fast;

    slow = head;
    fast = head->next;

    while (fast != NULL) {
        fast = fast->next;
        if (fast != NULL) {
            slow = slow->next;
            fast = fast->next;
        }
    }

    AxlSList *second = slow->next;
    slow->next = NULL;

    return second;
}

/**
 * @brief Merge two sorted lists using a context-aware comparator.
 */
static AxlSList *
merge_sorted_data(
    AxlSList           *a,
    AxlSList           *b,
    AxlCompareDataFunc  func,
    void               *user_data)
{
    AxlSList  dummy;
    AxlSList *tail;

    dummy.next = NULL;
    tail = &dummy;

    while (a != NULL && b != NULL) {
        if (func(a->data, b->data, user_data) <= 0) {
            tail->next = a;
            a = a->next;
        } else {
            tail->next = b;
            b = b->next;
        }
        tail = tail->next;
    }

    tail->next = (a != NULL) ? a : b;

    return dummy.next;
}

/**
 * @brief Merge two sorted lists into one sorted list.
 */
static AxlSList *
merge_sorted(
    AxlSList       *a,
    AxlSList       *b,
    AxlCompareFunc  func)
{
    AxlSList  dummy;
    AxlSList *tail;

    dummy.next = NULL;
    tail = &dummy;

    while (a != NULL && b != NULL) {
        if (func(a->data, b->data) <= 0) {
            tail->next = a;
            a = a->next;
        } else {
            tail->next = b;
            b = b->next;
        }
        tail = tail->next;
    }

    tail->next = (a != NULL) ? a : b;

    return dummy.next;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlSList *
axl_slist_append(
    AxlSList *list,
    void     *data)
{
    AxlSList *node;

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    if (list == NULL) {
        return node;
    }

    AxlSList *last = list;
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = node;

    return list;
}

AxlSList *
axl_slist_prepend(
    AxlSList *list,
    void     *data)
{
    AxlSList *node;

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    node->next = list;

    return node;
}

AxlSList *
axl_slist_insert(
    AxlSList *list,
    void     *data,
    int       position)
{
    AxlSList *node;

    if (position < 0) {
        return axl_slist_append(list, data);
    }
    if (list == NULL || position == 0) {
        return axl_slist_prepend(list, data);
    }

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    AxlSList *prev = list;
    int i = 1;
    while (prev->next != NULL && i < position) {
        prev = prev->next;
        i++;
    }

    node->next = prev->next;
    prev->next = node;

    return list;
}

AxlSList *
axl_slist_insert_sorted(
    AxlSList       *list,
    void           *data,
    AxlCompareFunc  func)
{
    AxlSList *node;

    if (func == NULL) {
        return list;
    }

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    if (list == NULL || func(data, list->data) <= 0) {
        node->next = list;
        return node;
    }

    AxlSList *prev = list;
    while (prev->next != NULL && func(data, prev->next->data) > 0) {
        prev = prev->next;
    }

    node->next = prev->next;
    prev->next = node;

    return list;
}

AxlSList *
axl_slist_remove(
    AxlSList   *list,
    const void *data)
{
    if (list == NULL) {
        return NULL;
    }

    if (list->data == data) {
        AxlSList *next = list->next;
        axl_free(list);
        return next;
    }

    AxlSList *prev = list;
    while (prev->next != NULL) {
        if (prev->next->data == data) {
            AxlSList *victim = prev->next;
            prev->next = victim->next;
            axl_free(victim);
            return list;
        }
        prev = prev->next;
    }

    return list;
}

AxlSList *
axl_slist_reverse(
    AxlSList *list)
{
    AxlSList *prev = NULL;
    AxlSList *curr = list;

    while (curr != NULL) {
        AxlSList *next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }

    return prev;
}

AxlSList *
axl_slist_concat(
    AxlSList *list1,
    AxlSList *list2)
{
    if (list1 == NULL) {
        return list2;
    }

    AxlSList *last = list1;
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = list2;

    return list1;
}

AxlSList *
axl_slist_sort(
    AxlSList       *list,
    AxlCompareFunc  func)
{
    AxlSList *second;

    if (list == NULL || list->next == NULL || func == NULL) {
        return list;
    }

    second = split_half(list);

    list   = axl_slist_sort(list, func);
    second = axl_slist_sort(second, func);

    return merge_sorted(list, second, func);
}

AxlSList *
axl_slist_copy(
    AxlSList *list)
{
    AxlSList *new_list = NULL;
    AxlSList *tail = NULL;

    for (AxlSList *l = list; l != NULL; l = l->next) {
        AxlSList *node = alloc_node(l->data);
        if (node == NULL) {
            axl_slist_free(new_list);
            return NULL;
        }

        if (new_list == NULL) {
            new_list = node;
        } else {
            tail->next = node;
        }
        tail = node;
    }

    return new_list;
}

void
axl_slist_free(
    AxlSList *list)
{
    while (list != NULL) {
        AxlSList *next = list->next;
        axl_free(list);
        list = next;
    }
}

void
axl_slist_free_full(
    AxlSList         *list,
    AxlDestroyNotify  free_func)
{
    while (list != NULL) {
        AxlSList *next = list->next;
        if (free_func != NULL) {
            free_func(list->data);
        }
        axl_free(list);
        list = next;
    }
}

size_t
axl_slist_length(
    AxlSList *list)
{
    size_t count = 0;

    for (AxlSList *l = list; l != NULL; l = l->next) {
        count++;
    }

    return count;
}

AxlSList *
axl_slist_nth(
    AxlSList *list,
    size_t    n)
{
    for (size_t i = 0; list != NULL; list = list->next, i++) {
        if (i == n) {
            return list;
        }
    }

    return NULL;
}

void *
axl_slist_nth_data(
    AxlSList *list,
    size_t    n)
{
    AxlSList *node = axl_slist_nth(list, n);

    return (node != NULL) ? node->data : NULL;
}

AxlSList *
axl_slist_last(
    AxlSList *list)
{
    if (list == NULL) {
        return NULL;
    }

    while (list->next != NULL) {
        list = list->next;
    }

    return list;
}

AxlSList *
axl_slist_find(
    AxlSList   *list,
    const void *data)
{
    for (AxlSList *l = list; l != NULL; l = l->next) {
        if (l->data == data) {
            return l;
        }
    }

    return NULL;
}

AxlSList *
axl_slist_find_custom(
    AxlSList       *list,
    const void     *data,
    AxlCompareFunc  func)
{
    if (func == NULL) {
        return NULL;
    }

    for (AxlSList *l = list; l != NULL; l = l->next) {
        if (func(l->data, data) == 0) {
            return l;
        }
    }

    return NULL;
}

void
axl_slist_foreach(
    AxlSList *list,
    AxlFunc   func,
    void     *user_data)
{
    if (func == NULL) {
        return;
    }

    for (AxlSList *l = list; l != NULL; l = l->next) {
        func(l->data, user_data);
    }
}

AxlSList *
axl_slist_insert_before(
    AxlSList *list,
    AxlSList *sibling,
    void     *data)
{
    AxlSList *node;

    if (sibling == NULL) {
        return axl_slist_append(list, data);
    }

    /* Head case: sibling is the first node */
    if (list == sibling) {
        return axl_slist_prepend(list, data);
    }

    node = alloc_node(data);
    if (node == NULL) {
        return list;
    }

    /* Walk to find the predecessor of sibling */
    AxlSList *prev = list;
    while (prev != NULL && prev->next != sibling) {
        prev = prev->next;
    }

    if (prev == NULL) {
        /* sibling not found in list — just append the new node */
        axl_free(node);
        return axl_slist_append(list, data);
    }

    node->next = sibling;
    prev->next = node;

    return list;
}

AxlSList *
axl_slist_remove_all(
    AxlSList   *list,
    const void *data)
{
    AxlSList *prev;
    AxlSList *cur;
    AxlSList *next;

    /* Remove matching head nodes */
    while (list != NULL && list->data == data) {
        next = list->next;
        axl_free(list);
        list = next;
    }

    if (list == NULL) {
        return NULL;
    }

    /* Remove matching interior nodes */
    prev = list;
    cur = list->next;
    while (cur != NULL) {
        next = cur->next;
        if (cur->data == data) {
            prev->next = next;
            axl_free(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }

    return list;
}

AxlSList *
axl_slist_remove_link(
    AxlSList *list,
    AxlSList *link)
{
    if (link == NULL) {
        return list;
    }

    if (list == link) {
        list = link->next;
        link->next = NULL;
        return list;
    }

    AxlSList *prev = list;
    while (prev != NULL && prev->next != link) {
        prev = prev->next;
    }

    if (prev != NULL) {
        prev->next = link->next;
    }

    link->next = NULL;

    return list;
}

AxlSList *
axl_slist_sort_with_data(
    AxlSList           *list,
    AxlCompareDataFunc  func,
    void               *user_data)
{
    AxlSList *second;

    if (list == NULL || list->next == NULL || func == NULL) {
        return list;
    }

    second = split_half(list);

    list   = axl_slist_sort_with_data(list, func, user_data);
    second = axl_slist_sort_with_data(second, func, user_data);

    return merge_sorted_data(list, second, func, user_data);
}

AxlSList *
axl_slist_copy_deep(
    AxlSList    *list,
    AxlCopyFunc  func,
    void        *user_data)
{
    AxlSList *new_list;
    AxlSList *tail;

    if (func == NULL) {
        return NULL;
    }

    new_list = NULL;
    tail = NULL;

    for (AxlSList *l = list; l != NULL; l = l->next) {
        AxlSList *node = alloc_node(func(l->data, user_data));
        if (node == NULL) {
            axl_slist_free(new_list);
            return NULL;
        }

        if (new_list == NULL) {
            new_list = node;
        } else {
            tail->next = node;
        }
        tail = node;
    }

    return new_list;
}
