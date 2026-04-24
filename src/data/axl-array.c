/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-array.c
    Growable dynamic array with value and pointer modes.
**/

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-log.h>
#include <axl/axl-array.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("array");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define INITIAL_CAPACITY  16

// ---------------------------------------------------------------------------
// Internal struct
// ---------------------------------------------------------------------------

struct AxlArray {
    uint8_t  *buffer;
    size_t    element_size;
    size_t    length;
    size_t    capacity;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int
ensure_capacity(AxlArray *a)
{
    size_t    new_cap;
    uint8_t  *new_buf;

    if (a->length < a->capacity) {
        return 0;
    }

    new_cap = a->capacity * 2;
    new_buf = axl_calloc(1, new_cap * a->element_size);
    if (new_buf == NULL) {
        axl_error("failed to resize array to %zu elements", new_cap);
        return -1;
    }

    if (a->buffer != NULL) {
        axl_memcpy(new_buf, a->buffer, a->length * a->element_size);
        axl_free(a->buffer);
    }

    a->buffer = new_buf;
    a->capacity = new_cap;

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlArray *
axl_array_new(size_t element_size)
{
    AxlArray *a;

    if (element_size == 0) {
        return NULL;
    }

    a = axl_calloc(1, sizeof (AxlArray));
    if (a == NULL) {
        axl_warning("allocation failed");
        return NULL;
    }

    a->element_size = element_size;
    a->capacity = INITIAL_CAPACITY;
    a->length = 0;

    a->buffer = axl_calloc(1, INITIAL_CAPACITY * element_size);
    if (a->buffer == NULL) {
        axl_warning("buffer allocation failed");
        axl_free(a);
        return NULL;
    }

    return a;
}

void
axl_array_free(AxlArray *a)
{
    if (a == NULL) {
        return;
    }

    axl_free(a->buffer);
    axl_free(a);
}

int
axl_array_append(AxlArray *a, const void *element)
{
    if (a == NULL || element == NULL) {
        return -1;
    }

    if (ensure_capacity(a) != 0) {
        return -1;
    }

    axl_memcpy(a->buffer + a->length * a->element_size,
             element, a->element_size);
    a->length++;

    return 0;
}

void *
axl_array_get(AxlArray *a, size_t index)
{
    if (a == NULL || index >= a->length) {
        return NULL;
    }

    return a->buffer + index * a->element_size;
}

size_t
axl_array_len(AxlArray *a)
{
    if (a == NULL) {
        return 0;
    }

    return a->length;
}

void
axl_array_clear(AxlArray *a)
{
    if (a == NULL) {
        return;
    }

    a->length = 0;
}

int
axl_array_append_ptr(AxlArray *a, void *ptr)
{
    return axl_array_append(a, &ptr);
}

void *
axl_array_get_ptr(AxlArray *a, size_t index)
{
    void **slot;

    slot = axl_array_get(a, index);
    if (slot == NULL) {
        return NULL;
    }

    return *slot;
}

void
axl_array_sort(AxlArray *a, AxlCompareFunc compare)
{
    size_t    i;
    size_t    j;
    uint8_t  *tmp;
    uint8_t  *ai;
    uint8_t  *aj;

    if (a == NULL || compare == NULL || a->length <= 1) {
        return;
    }

    /* Insertion sort — adequate for typical UEFI array sizes */
    tmp = axl_malloc(a->element_size);
    if (tmp == NULL) {
        axl_warning("sort allocation failed");
        return;
    }

    for (i = 1; i < a->length; i++) {
        /* Sort is O(n^2); yield every 1024 outer iters so a large
           sort stays Ctrl-C responsive. No-op on small arrays. */
        if ((i & 0x3FF) == 0) {
            axl_yield();
        }
        ai = a->buffer + i * a->element_size;
        axl_memcpy(tmp, ai, a->element_size);

        j = i;
        while (j > 0) {
            aj = a->buffer + (j - 1) * a->element_size;
            if (compare(aj, tmp) <= 0) {
                break;
            }
            axl_memcpy(aj + a->element_size, aj, a->element_size);
            j--;
        }

        axl_memcpy(a->buffer + j * a->element_size, tmp, a->element_size);
    }

    axl_free(tmp);
}

int
axl_array_remove_index(AxlArray *a, size_t index)
{
    if (a == NULL || index >= a->length) {
        return -1;
    }

    if (index < a->length - 1) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (index + 1) * a->element_size,
                 (a->length - index - 1) * a->element_size);
    }

    a->length--;
    return 0;
}

int
axl_array_remove_index_fast(AxlArray *a, size_t index)
{
    if (a == NULL || index >= a->length) {
        return -1;
    }

    if (index < a->length - 1) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (a->length - 1) * a->element_size,
                 a->element_size);
    }

    a->length--;
    return 0;
}

int
axl_array_remove_range(AxlArray *a, size_t index, size_t len)
{
    if (a == NULL || len == 0) {
        return -1;
    }

    if (index >= a->length || len > a->length - index) {
        return -1;
    }

    if (index + len < a->length) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (index + len) * a->element_size,
                 (a->length - index - len) * a->element_size);
    }

    a->length -= len;
    return 0;
}

int
axl_array_set_size(AxlArray *a, size_t len)
{
    if (a == NULL) {
        return -1;
    }

    if (len <= a->length) {
        a->length = len;
        return 0;
    }

    /* Grow capacity if needed */
    if (len > a->capacity) {
        size_t    new_cap = a->capacity;
        uint8_t  *new_buf;

        while (new_cap < len) {
            new_cap *= 2;
        }

        new_buf = axl_calloc(1, new_cap * a->element_size);
        if (new_buf == NULL) {
            axl_error("failed to resize array to %zu elements", new_cap);
            return -1;
        }

        if (a->buffer != NULL) {
            axl_memcpy(new_buf, a->buffer, a->length * a->element_size);
            axl_free(a->buffer);
        }

        a->buffer = new_buf;
        a->capacity = new_cap;
    }

    /* Zero-initialize new elements */
    axl_memset(a->buffer + a->length * a->element_size, 0,
            (len - a->length) * a->element_size);
    a->length = len;

    return 0;
}

void
axl_array_sort_with_data(AxlArray *a, AxlCompareDataFunc compare,
                         void *user_data)
{
    size_t    i;
    size_t    j;
    uint8_t  *tmp;
    uint8_t  *ai;
    uint8_t  *aj;

    if (a == NULL || compare == NULL || a->length <= 1) {
        return;
    }

    /* Insertion sort — adequate for typical UEFI array sizes */
    tmp = axl_malloc(a->element_size);
    if (tmp == NULL) {
        axl_warning("sort allocation failed");
        return;
    }

    for (i = 1; i < a->length; i++) {
        /* Sort is O(n^2); yield every 1024 outer iters so a large
           sort stays Ctrl-C responsive. No-op on small arrays. */
        if ((i & 0x3FF) == 0) {
            axl_yield();
        }
        ai = a->buffer + i * a->element_size;
        axl_memcpy(tmp, ai, a->element_size);

        j = i;
        while (j > 0) {
            aj = a->buffer + (j - 1) * a->element_size;
            if (compare(aj, tmp, user_data) <= 0) {
                break;
            }
            axl_memcpy(aj + a->element_size, aj, a->element_size);
            j--;
        }

        axl_memcpy(a->buffer + j * a->element_size, tmp, a->element_size);
    }

    axl_free(tmp);
}
