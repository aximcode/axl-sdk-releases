/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-sort.h:
 *
 * In-place sort over a raw element buffer — the qsort(3) shape, but
 * with a fixed worst case. The engine is an introsort (median-of-three
 * quicksort, insertion sort for small runs, heapsort fallback when the
 * recursion depth exceeds 2*log2(n)). That guarantees O(n log n) worst
 * case while doing no heap allocation and keeping stack depth bounded —
 * the right trade for a freestanding/UEFI environment where malloc can
 * fail and stacks are small.
 *
 * The sort is NOT stable: equal elements may be reordered. Comparators
 * use the shared AxlCompareFunc / AxlCompareDataFunc typedefs and follow
 * the standard < 0 / 0 / > 0 convention.
 */

#ifndef AXL_SORT_H
#define AXL_SORT_H

#include <stddef.h>
#include <axl/axl-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sort @p nmemb elements of @p size bytes in place (introsort).
 *
 * Drop-in for qsort(3): same argument shape and comparator convention,
 * but with a guaranteed O(n log n) worst case and no allocation. Not
 * stable. No-op if @p base is NULL, @p compare is NULL, @p size is 0,
 * or @p nmemb is less than 2.
 */
void
axl_qsort(
    void           *base,    ///< start of the element buffer
    size_t          nmemb,   ///< number of elements
    size_t          size,    ///< size of each element in bytes
    AxlCompareFunc  compare  ///< comparison function (qsort-compatible)
);

/**
 * @brief Sort in place with a context-aware comparator (introsort).
 *
 * Like axl_qsort() but threads @p user_data through every comparison —
 * the qsort_r(3) shape. Not stable. Same no-op guards as axl_qsort().
 */
void
axl_qsort_with_data(
    void                *base,      ///< start of the element buffer
    size_t               nmemb,     ///< number of elements
    size_t               size,      ///< size of each element in bytes
    AxlCompareDataFunc   compare,   ///< comparison function with user_data
    void                *user_data  ///< passed to every compare call
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SORT_H */
