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
#include <axl/axl-sort.h>
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
    /* Element destructor, or NULL to borrow. GArray and GPtrArray are merged
       into this one type, so the array cannot infer which convention the
       caller wants — clear_is_ptr records which of the two setters supplied
       it. Value mode hands the callback the element's ADDRESS; pointer mode
       hands it the STORED pointer. Guessing wrong in pointer mode would free
       into our own buffer, which is why it is recorded rather than sniffed. */
    AxlDestroyNotify clear_func;
    bool             clear_is_ptr;
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

    /* capacity 0 is reachable after axl_array_steal(), which hands the buffer
       away and leaves the array empty-but-usable. Doubling 0 stays 0. */
    new_cap = (a->capacity != 0) ? a->capacity * 2 : INITIAL_CAPACITY;
    new_buf = axl_calloc(new_cap, a->element_size);   /* two factors: see sized_new */
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

/* Run the element destructor over [start, start+count), if one is set.
   Call BEFORE the slots are moved over or the length is reduced. */
static void
clear_elements(AxlArray *a, size_t start, size_t count)
{
    size_t i;

    if (a->clear_func == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        void *slot = a->buffer + (start + i) * a->element_size;

        if (a->clear_is_ptr) {
            a->clear_func(*(void **)slot);   /* the stored pointer */
        } else {
            a->clear_func(slot);             /* the element itself */
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlArray *
axl_array_new(size_t element_size)
{
    return axl_array_sized_new(element_size, 0);
}

AxlArray *
axl_array_sized_new(size_t element_size, size_t reserved)
{
    AxlArray *a;
    size_t    cap = (reserved != 0) ? reserved : INITIAL_CAPACITY;

    if (element_size == 0) {
        return NULL;
    }

    a = axl_calloc(1, sizeof (AxlArray));
    if (a == NULL) {
        axl_debug("allocation failed");
        return NULL;
    }

    a->element_size = element_size;
    a->capacity = cap;
    a->length = 0;

    /* Two factors, NOT axl_calloc(1, cap * element_size): pre-multiplying here
       would wrap silently and hand back a tiny buffer while a->capacity kept
       the huge value, so the first append would write past the allocation.
       Passing them separately is what lets axl_calloc's own
       `size > SIZE_MAX / count` guard actually fire. `reserved` is
       caller-controlled, so this is reachable in one call. */
    a->buffer = axl_calloc(cap, element_size);
    if (a->buffer == NULL) {
        axl_debug("buffer allocation failed");
        axl_free(a);
        return NULL;
    }

    return a;
}

void
axl_array_set_clear_func(AxlArray *a, AxlDestroyNotify clear_func)
{
    if (a == NULL) {
        return;
    }

    a->clear_func = clear_func;
    a->clear_is_ptr = false;
}

int
axl_array_set_ptr_free_func(AxlArray *a, AxlDestroyNotify free_func)
{
    /* Refuse a value-mode array rather than reinterpret it: dereferencing a
       struct slot as a void* and freeing the result corrupts the heap. */
    if (a == NULL || a->element_size != sizeof (void *)) {
        return AXL_ERR;
    }

    a->clear_func = free_func;
    a->clear_is_ptr = (free_func != NULL);
    return AXL_OK;
}

void *
axl_array_steal(AxlArray *a, size_t *out_len)
{
    void *buf;

    if (a == NULL) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return NULL;
    }

    buf = a->buffer;
    if (out_len != NULL) {
        *out_len = a->length;   /* ELEMENTS, matching axl_array_len */
    }

    /* g_array_steal, not g_array_free(arr, FALSE): the array survives, empty
       and reusable. No clear_func here — ownership TRANSFERS to the caller.
       capacity 0 is why ensure_capacity and set_size both seed from 0. */
    a->buffer = NULL;
    a->length = 0;
    a->capacity = 0;

    return buf;
}

void
axl_array_free(AxlArray *a)
{
    if (a == NULL) {
        return;
    }

    clear_elements(a, 0, a->length);
    axl_free(a->buffer);
    axl_free(a);
}

int
axl_array_append(AxlArray *a, const void *element)
{
    if (a == NULL || element == NULL) {
        return AXL_ERR;
    }

    if (ensure_capacity(a) != 0) {
        return AXL_ERR;
    }

    axl_memcpy(a->buffer + a->length * a->element_size,
             element, a->element_size);
    a->length++;

    return AXL_OK;
}

int
axl_array_insert(AxlArray *a, size_t index, const void *element)
{
    if (a == NULL || element == NULL || index > a->length) {
        return AXL_ERR;
    }

    if (ensure_capacity(a) != 0) {
        return AXL_ERR;
    }

    // Shift [index, length) right by one slot. memmove, not memcpy —
    // the source and destination ranges overlap.
    if (index < a->length) {
        axl_memmove(a->buffer + (index + 1) * a->element_size,
                  a->buffer + index * a->element_size,
                  (a->length - index) * a->element_size);
    }

    axl_memcpy(a->buffer + index * a->element_size, element, a->element_size);
    a->length++;

    return AXL_OK;
}

int
axl_array_prepend(AxlArray *a, const void *element)
{
    return axl_array_insert(a, 0, element);
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

void *
axl_array_data(AxlArray *a)
{
    if (a == NULL) {
        return NULL;
    }

    /* NULL is reachable only after axl_array_steal(), which also zeroes
       length — so the (pointer, length) pair the header promises is safe to
       iterate without a separate NULL test. */
    return a->buffer;
}

size_t
axl_array_element_size(AxlArray *a)
{
    if (a == NULL) {
        return 0;
    }

    return a->element_size;
}

void
axl_array_clear(AxlArray *a)
{
    if (a == NULL) {
        return;
    }

    clear_elements(a, 0, a->length);
    a->length = 0;
}

int
axl_array_append_ptr(AxlArray *a, void *ptr)
{
    return axl_array_append(a, &ptr);
}

int
axl_array_insert_ptr(AxlArray *a, size_t index, void *ptr)
{
    return axl_array_insert(a, index, &ptr);
}

int
axl_array_prepend_ptr(AxlArray *a, void *ptr)
{
    return axl_array_insert(a, 0, &ptr);
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
    if (a == NULL || compare == NULL) {
        return;
    }

    axl_qsort(a->buffer, a->length, a->element_size, compare);
}

int
axl_array_remove_index(AxlArray *a, size_t index)
{
    if (a == NULL || index >= a->length) {
        return AXL_ERR;
    }

    clear_elements(a, index, 1);   /* before the slot is overwritten */

    if (index < a->length - 1) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (index + 1) * a->element_size,
                 (a->length - index - 1) * a->element_size);
    }

    a->length--;
    return AXL_OK;
}

int
axl_array_remove_index_fast(AxlArray *a, size_t index)
{
    if (a == NULL || index >= a->length) {
        return AXL_ERR;
    }

    clear_elements(a, index, 1);   /* before the last element lands on it */

    if (index < a->length - 1) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (a->length - 1) * a->element_size,
                 a->element_size);
    }

    a->length--;
    return AXL_OK;
}

int
axl_array_remove_range(AxlArray *a, size_t index, size_t len)
{
    if (a == NULL || len == 0) {
        return AXL_ERR;
    }

    if (index >= a->length || len > a->length - index) {
        return AXL_ERR;
    }

    clear_elements(a, index, len);   /* before the survivors shift down */

    if (index + len < a->length) {
        axl_memcpy(a->buffer + index * a->element_size,
                 a->buffer + (index + len) * a->element_size,
                 (a->length - index - len) * a->element_size);
    }

    a->length -= len;
    return AXL_OK;
}

int
axl_array_set_size(AxlArray *a, size_t len)
{
    if (a == NULL) {
        return AXL_ERR;
    }

    if (len <= a->length) {
        clear_elements(a, len, a->length - len);   /* the discarded tail */
        a->length = len;
        return AXL_OK;
    }

    /* Grow capacity if needed */
    if (len > a->capacity) {
        size_t    new_cap = a->capacity;
        uint8_t  *new_buf;

        if (new_cap == 0) {
            new_cap = INITIAL_CAPACITY;   /* post-steal; *= 2 would spin forever */
        }
        while (new_cap < len) {
            new_cap *= 2;
        }

        new_buf = axl_calloc(new_cap, a->element_size);   /* two factors: see sized_new */
        if (new_buf == NULL) {
            axl_error("failed to resize array to %zu elements", new_cap);
            return AXL_ERR;
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

    return AXL_OK;
}

void
axl_array_sort_with_data(AxlArray *a, AxlCompareDataFunc compare,
                         void *user_data)
{
    if (a == NULL || compare == NULL) {
        return;
    }

    axl_qsort_with_data(a->buffer, a->length, a->element_size, compare,
                        user_data);
}
