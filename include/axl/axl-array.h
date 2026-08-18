/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-array.h
 *
 * Growable dynamic array. Two modes:
 *   - Value mode: stores copies of fixed-size elements.
 *   - Pointer mode: stores void* pointers (element_size = sizeof(void*)).
 *
 * @section array_glib Relationship to GLib's GArray
 *
 * This container tracks @c GArray closely, so the places it deliberately
 * diverges are recorded here rather than left to look like oversights:
 *
 * - **No ref/unref.** GLib refcounts a @c GArray. AXL does not, because a
 *   refcount shared across CPUs needs atomics and AXL runs @c AxlTaskProc on
 *   application processors. Single-owner is the rule; hand ownership over with
 *   axl_array_steal() instead of sharing it. Do not add refcounting here.
 * - **Element destructors are set, not passed at construction.** Every other
 *   AXL container takes its destructor at construction (@c
 *   axl_hash_table_new_full) or at the terminal call (@c axl_list_free_full).
 *   This one follows @c GArray instead, because GLib's own hooks are setters
 *   and the merged type needs two of them. It is the one place AxlArray
 *   diverges from AXL convention rather than toward it.
 * - **@c GArray and @c GPtrArray are merged into one type**, with @c _ptr
 *   variants (axl_array_append_ptr() and friends) rather than a second type.
 *   Fewer types to learn; the cost is that the array cannot infer which mode
 *   it is in, which is why element cleanup needs the two distinct hooks
 *   described at axl_array_set_clear_func().
 * - **axl_array_get() is a function**, not GLib's typed @c g_array_index
 *   macro, and **axl_array_len() is a function**, not a public @c ->len field.
 *   Both deliberate: the struct stays opaque and there is no macro
 *   type-punning. axl_array_data() does hand out the base pointer — the same
 *   thing @c g_array_index reaches through, and what @c std::vector::data()
 *   is — but it is `void *` and reading it as a typed array is the caller's
 *   explicit cast against axl_array_element_size(), not a macro's silent one.
 * - **One element at a time.** There is no bulk @c append_vals /
 *   @c insert_vals / @c prepend_vals; no caller has needed them.
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
 * @brief Create a new array with capacity reserved up front.
 *
 * Identical to axl_array_new() but pre-sizes the internal buffer, so a
 * caller that already knows roughly how many elements are coming pays no
 * grow-and-copy cost. The array still starts EMPTY — @p reserved is capacity,
 * not length. Matches @c g_array_sized_new.
 *
 * A @p reserved of 0 behaves exactly like axl_array_new().
 *
 * @return new AxlArray, or NULL on failure. Free with axl_array_free().
 */
AxlArray *
axl_array_sized_new(
    size_t element_size,  ///< size of each element in bytes
    size_t reserved       ///< elements to reserve capacity for (0 = default)
);

/**
 * @brief Free a dynamic array and its internal buffer.
 *
 * Calls the clear / free hook on every remaining element if one is set —
 * see axl_array_set_clear_func(). With no hook set, only the buffer and the
 * array itself are released, so anything reached only through a stored
 * pointer LEAKS: either set a hook or free the elements yourself first.
 */
void
axl_array_free(
    AxlArray *a  ///< array (NULL-safe)
);

/**
 * @brief Set the destructor for elements stored by VALUE.
 *
 * The array calls @p clear_func with a pointer to the element SLOT — i.e. the
 * same thing axl_array_get() returns — for every element it discards. Use
 * this when an element is a struct that owns something.
 *
 * @warning In POINTER mode this is almost never what you want, and the
 * mistake is dangerous rather than merely wrong: passing @c axl_free here
 * would free the slot's ADDRESS, which points into the middle of the array's
 * own buffer. Use axl_array_set_ptr_free_func() for pointer mode.
 *
 * The hook runs on every path that discards an element, matching GLib:
 * axl_array_free(), axl_array_clear(), axl_array_remove_index(),
 * axl_array_remove_index_fast(), axl_array_remove_range(), and a SHRINKING
 * axl_array_set_size(). It does NOT run on axl_array_steal() — that transfers
 * ownership to the caller rather than discarding it.
 *
 * Setting either hook replaces any previously set hook of either kind.
 * Passing NULL clears it (elements are then borrowed).
 */
void
axl_array_set_clear_func(
    AxlArray         *a,          ///< array
    AxlDestroyNotify  clear_func  ///< called with a pointer TO the element, or NULL
);

/**
 * @brief Set the destructor for elements stored as POINTERS.
 *
 * The array calls @p free_func with the STORED POINTER — the same thing
 * axl_array_get_ptr() returns — for every element it discards, so
 * @c axl_free_impl is the natural argument. (Pass @c axl_free_impl, not
 * @c axl_free: the latter is a macro and cannot be taken as a function
 * pointer. This matches how every other AXL container is handed a
 * destructor.) This is the @c g_ptr_array_set_free_func
 * half of the pair; see axl_array_set_clear_func() for the value-mode half
 * and for the full list of paths the hook runs on.
 *
 * Requires an element size of exactly @c sizeof(void*); a value-mode array is
 * rejected rather than reinterpreted, because misreading a struct as a
 * pointer and freeing it would corrupt the heap.
 *
 * Setting either hook replaces any previously set hook of either kind.
 * Passing NULL clears it (elements are then borrowed).
 *
 * @return AXL_OK, or AXL_ERR if @p a is NULL or its element size is not
 *     @c sizeof(void*).
 */
int
axl_array_set_ptr_free_func(
    AxlArray         *a,         ///< array (element_size must be sizeof(void*))
    AxlDestroyNotify  free_func  ///< called with the STORED pointer, or NULL
);

/**
 * @brief Hand the internal buffer to the caller and empty the array.
 *
 * Ownership of the element block transfers to the caller, who must release it
 * with axl_free(). The array itself stays VALID and reusable — appending
 * after a steal works and simply allocates a fresh buffer. This is
 * @c g_array_steal, NOT @c g_array_free(arr, FALSE): the array is emptied,
 * not destroyed, so axl_array_free() is still required eventually.
 *
 * @p out_len receives the ELEMENT COUNT, matching axl_array_len() — not a
 * byte count. Multiply by your own element size if you need bytes.
 *
 * No clear/free hook runs: this is a transfer of ownership, not a discard.
 * The block is NOT shrunk to fit, so it may be larger than
 * @p out_len * element_size — free it, do not assume its size.
 *
 * Stealing an array that currently holds no buffer — which happens only after
 * a previous steal, since construction always allocates — returns NULL with
 * @p out_len set to 0. Stealing a merely EMPTY array returns its (valid,
 * unused) block.
 *
 * @return the element buffer, or NULL if @p a is NULL.
 */
void *
axl_array_steal(
    AxlArray *a,       ///< array
    size_t   *out_len  ///< receives the element COUNT (may be NULL)
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
 * @brief The contiguous element buffer, for reading a whole array at once.
 *
 * Elements are stored back to back with no padding between them, so the
 * element at @a i begins at `(uint8_t *)axl_array_data(a) + i *
 * axl_array_element_size(a)` — exactly what axl_array_get() returns, without
 * the per-element call.
 *
 * @par Why this exists when axl_array_get() already does
 *
 * A caller that reads EVERY element pays an out-of-line call and a bounds
 * check per element through axl_array_get(). Measured over a C++ view in
 * AXL-Cxx-Design.md §4.1: indexed traversal ran 4.2x slower and a sort 19.4x
 * slower than the same loop over a base pointer, and adding this accessor
 * recovered ALL of both for 87 bytes of code.
 *
 * That section's §7 item 3 said this was worth adding only if an
 * `AxlArray`-backed C++ CONTAINER were ever wanted, which §5 rejected — the
 * inference being that no other caller had the same need. A borrowed VIEW has
 * exactly the same need and is the shape §4.4 recommended, so the conclusion
 * did not survive the case that motivated it.
 *
 * @par This does not reopen the type
 *
 * The struct stays opaque and there is still no typed indexing macro; this is
 * a read-side base pointer, the same thing `std::vector::data()` is, and it
 * carries `std::vector::data()`'s invalidation rule (below). Appending still
 * copies through axl_array_append()'s memcpy, so §4.1's soundness verdict —
 * a C++ skin over this type never runs `T`'s copy constructor — is untouched.
 *
 * @warning The pointer is INVALIDATED by anything that can grow or move the
 *     buffer: axl_array_append(), axl_array_append_ptr(), axl_array_insert(),
 *     axl_array_insert_ptr(), axl_array_prepend(), axl_array_prepend_ptr(),
 *     axl_array_set_size() and axl_array_steal(). Treat it as a borrow that
 *     lives until the next mutation, and re-fetch after one.
 *
 *     Two more do not move the BUFFER but do move the ELEMENTS, so a pointer
 *     to a particular slot stops meaning what it meant: the removal calls,
 *     and axl_array_sort() / axl_array_sort_with_data(), which permute in
 *     place.
 *
 * @return base of the element buffer, or NULL if @a a is NULL or owns no
 *     buffer (a fresh array always owns one; axl_array_steal() hands it away
 *     and leaves the array empty-but-usable). NULL is always paired with an
 *     axl_array_len() of 0, so a `(pointer, length)` pair from these two is
 *     safe to iterate without a separate NULL test.
 */
void *
axl_array_data(
    AxlArray *a  ///< array
);

/**
 * @brief The per-element stride in bytes, as passed to axl_array_new().
 *
 * The companion axl_array_data() needs to compute an element address, and
 * the check a typed reader must make before casting that buffer to `T *`:
 * a stride that disagrees with `sizeof(T)` means the array holds something
 * else, and reading it as `T` is a wrong answer at best and an out-of-bounds
 * read when `sizeof(T)` is the larger.
 *
 * For a pointer-mode array this is `sizeof(void *)`, which is what
 * axl_array_set_ptr_free_func() already refuses a mismatch on.
 *
 * @return element size in bytes, or 0 if @a a is NULL. Never 0 for a live
 *     array — axl_array_sized_new() refuses a zero @a element_size — so 0 is
 *     unambiguously the NULL case.
 */
size_t
axl_array_element_size(
    AxlArray *a  ///< array
);

/**
 * @brief Remove all elements. Does not free the internal buffer.
 *
 * Runs the clear / free hook on each discarded element if one is set —
 * see axl_array_set_clear_func().
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
 * Runs the clear / free hook on each discarded element if one is set —
 * see axl_array_set_clear_func().
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
 * Runs the clear / free hook on each discarded element if one is set —
 * see axl_array_set_clear_func().
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
 * Runs the clear / free hook on each discarded element if one is set —
 * see axl_array_set_clear_func().
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
 * is reduced without reallocating and the clear / free hook runs on each
 * discarded element if one is set — see axl_array_set_clear_func().
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
