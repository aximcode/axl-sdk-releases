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
#include <axl/axl-macros.h>
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
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_array_append(
    AxlArray   *a,      ///< array
    const void *element ///< pointer to element data to copy (element_size bytes)
);

/**
 * @brief Insert an element at @p index (value mode), shifting the rest right.
 *
 * @p index may equal the current length (equivalent to append).
 * Matches g_array_insert_val.
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure or if
 *     @p index is past the end (> length).
 */
int
axl_array_insert(
    AxlArray   *a,       ///< array
    size_t      index,   ///< position to insert at (0..length)
    const void *element  ///< pointer to element data to copy (element_size bytes)
);

/**
 * @brief Prepend an element (value mode) — insert at the front.
 *
 * Equivalent to axl_array_insert(a, 0, element). Matches
 * g_array_prepend_val. O(n) (shifts all elements).
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_array_prepend(
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
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_array_append_ptr(
    AxlArray *a,  ///< array
    void     *ptr ///< pointer to store
);

/**
 * @brief Insert a pointer at @p index (pointer mode), shifting the rest right.
 *
 * @p index may equal the current length (equivalent to append).
 * Matches g_ptr_array_insert.
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure or if
 *     @p index is past the end (> length).
 */
int
axl_array_insert_ptr(
    AxlArray *a,      ///< array
    size_t    index,  ///< position to insert at (0..length)
    void     *ptr     ///< pointer to store
);

/**
 * @brief Prepend a pointer (pointer mode) — insert at the front.
 *
 * Equivalent to axl_array_insert_ptr(a, 0, ptr). O(n).
 *
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_array_prepend_ptr(
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
 * @brief Sort array elements in place (introsort, O(n log n)).
 *
 * Delegates to axl_qsort(). Not stable: equal elements may be reordered.
 */
void
axl_array_sort(
    AxlArray       *a,      ///< array
    AxlCompareFunc  compare ///< comparison function (qsort-compatible)
);

/**
 * @brief Remove the element at @p index, shifting remaining elements left.
 *
 * @return AXL_OK on success, AXL_ERR if index is out of range.
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
 * @return AXL_OK on success, AXL_ERR if index is out of range.
 */
int
axl_array_remove_index_fast(
    AxlArray *a,    ///< array
    size_t    index ///< element index to remove
);

/**
 * @brief Remove @p len elements starting at @p index, shifting remaining left.
 *
 * @return AXL_OK on success, AXL_ERR if the range is out of bounds.
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
 * @return AXL_OK on success, AXL_ERR on allocation failure.
 */
int
axl_array_set_size(
    AxlArray *a,  ///< array
    size_t    len ///< desired number of elements
);

/**
 * @brief Sort in place with a context-aware comparator (introsort).
 *
 * Delegates to axl_qsort_with_data(). Not stable: equal elements may be
 * reordered.
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
