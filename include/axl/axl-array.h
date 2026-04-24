/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-array.h:
 *
 * Growable dynamic array. Two modes:
 *   - Value mode: stores copies of fixed-size elements.
 *   - Pointer mode: stores void* pointers (element_size = sizeof(void*)).
 */

#ifndef AXL_ARRAY_H
#define AXL_ARRAY_H

#include <stddef.h>
#include <stdbool.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlArray AxlArray;

/**
 * @brief Create a new dynamic array for value-mode storage.
 *
 * @return new AxlArray, or NULL on failure.
 *     Free with axl_array_free().
 */
AxlArray *
axl_array_new(
    size_t element_size  ///< size of each element in bytes
);

/**
 * @brief Free a dynamic array and its internal buffer.
 */
void
axl_array_free(
    AxlArray *a  ///< array (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlArray, axl_array_free)
#endif

/**
 * @brief Append an element (value mode).
 *
 * @return 0 on success, -1 on allocation failure.
 */
int
axl_array_append(
    AxlArray   *a,      ///< array
    const void *element ///< pointer to element data to copy (element_size bytes)
);

/**
 * @brief Get a pointer to the element at @p index (value mode).
 *
 * @return pointer into internal buffer, or NULL if out of range.
 */
void *
axl_array_get(
    AxlArray *a,    ///< array
    size_t    index ///< element index
);

/**
 * @brief Get the number of elements in the array.
 *
 * @return number of elements.
 */
size_t
axl_array_len(
    AxlArray *a  ///< array
);

/**
 * @brief Remove all elements. Does not free the internal buffer.
 */
void
axl_array_clear(
    AxlArray *a  ///< array
);

/**
 * @brief Append a pointer (pointer mode).
 *
 * @return 0 on success, -1 on allocation failure.
 */
int
axl_array_append_ptr(
    AxlArray *a,  ///< array
    void     *ptr ///< pointer to store
);

/**
 * @brief Get stored pointer at @p index (pointer mode).
 *
 * @return stored pointer, or NULL if out of range.
 */
void *
axl_array_get_ptr(
    AxlArray *a,    ///< array
    size_t    index ///< element index
);

/**
 * @brief Sort array elements in place using insertion sort.
 */
void
axl_array_sort(
    AxlArray       *a,      ///< array
    AxlCompareFunc  compare ///< comparison function (qsort-compatible)
);

/**
 * @brief Remove the element at @p index, shifting remaining elements left.
 *
 * @return 0 on success, -1 if index is out of range.
 */
int
axl_array_remove_index(
    AxlArray *a,    ///< array
    size_t    index ///< element index to remove
);

/**
 * @brief Remove the element at @p index by swapping with the last element.
 *
 * O(1) but does not preserve order.
 *
 * @return 0 on success, -1 if index is out of range.
 */
int
axl_array_remove_index_fast(
    AxlArray *a,    ///< array
    size_t    index ///< element index to remove
);

/**
 * @brief Remove @p len elements starting at @p index, shifting remaining left.
 *
 * @return 0 on success, -1 if the range is out of bounds.
 */
int
axl_array_remove_range(
    AxlArray *a,    ///< array
    size_t    index, ///< first element to remove
    size_t    len   ///< number of elements to remove
);

/**
 * @brief Resize the array to exactly @p len elements.
 *
 * If growing, new elements are zero-initialized. If growing beyond
 * capacity, the internal buffer is reallocated. If shrinking, length
 * is reduced without reallocating.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int
axl_array_set_size(
    AxlArray *a,  ///< array
    size_t    len ///< desired number of elements
);

/**
 * @brief Sort array elements in place with a context-aware comparator.
 */
void
axl_array_sort_with_data(
    AxlArray            *a,         ///< array
    AxlCompareDataFunc   compare,   ///< comparison function with user_data
    void                *user_data  ///< passed to every compare call
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_ARRAY_H */
