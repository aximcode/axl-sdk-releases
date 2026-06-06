/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-bytes.h
    Immutable, reference-counted byte buffer.

    Mirrors GLib's GBytes: a read-only `(data, size)` blob with a
    reference count, so the same bytes can be shared across owners
    without copying and without anyone having to track "who frees
    this." Ideal as the currency for data that flows between
    subsystems — an HTTP body handed to a parser, file contents passed
    to a hasher, a shared-memory segment exposed to several readers.

    The bytes never change after construction, which is what makes
    sharing safe: `axl_bytes_ref` is O(1) and hands back the same
    object; `axl_bytes_new_from_bytes` carves out a sub-range that
    shares the parent's storage (zero copy) and keeps the parent alive
    for as long as the slice lives.

    Reference counting is not thread-safe (single-threaded UEFI BSP).

    @code
    AxlBytes *b = axl_bytes_new(buf, len);     // copies buf
    size_t n;
    const uint8_t *p = axl_bytes_get_data(b, &n);
    AxlBytes *head = axl_bytes_new_from_bytes(b, 0, 16);  // zero-copy slice
    axl_bytes_unref(b);     // 'head' still keeps the storage alive
    // ... use head ...
    axl_bytes_unref(head);  // now the storage is freed
    @endcode
**/

#ifndef AXL_BYTES_H
#define AXL_BYTES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlBytes AxlBytes;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Create a byte buffer by COPYING @p size bytes from @p data.
 *
 * The caller's buffer is independent afterwards. @p size may be 0
 * (@p data may then be NULL), yielding a valid empty buffer.
 *
 * @return a new AxlBytes with refcount 1, or NULL on allocation
 *     failure. Release with axl_bytes_unref().
 */
AxlBytes *
axl_bytes_new(
    const void *data,  ///< source bytes (may be NULL iff @p size is 0)
    size_t      size   ///< number of bytes
);

/**
 * @brief Create a byte buffer that TAKES OWNERSHIP of @p data.
 *
 * No copy: @p data must be a heap block from axl_malloc/axl_calloc/
 * axl_realloc, and is freed with axl_free() when the last reference is
 * dropped. @p data may be NULL only when @p size is 0.
 *
 * @return a new AxlBytes with refcount 1, or NULL on allocation
 *     failure (in which case @p data is NOT freed) or if @p data is
 *     NULL with @p size > 0.
 */
AxlBytes *
axl_bytes_new_take(
    void   *data,  ///< heap buffer to take ownership of
    size_t  size   ///< number of bytes
);

/**
 * @brief Create a byte buffer over STATIC @p data (never freed).
 *
 * No copy and no ownership: @p data must outlive every reference
 * (string literals, embedded blobs, .rodata). Nothing is freed when
 * the last reference is dropped.
 *
 * @return a new AxlBytes with refcount 1, or NULL on allocation
 *     failure, or if @p data is NULL with @p size > 0.
 */
AxlBytes *
axl_bytes_new_static(
    const void *data,  ///< static bytes outliving all references
    size_t      size   ///< number of bytes
);

/**
 * @brief Create a sub-range that SHARES the parent's storage (no copy).
 *
 * The slice covers @p length bytes of @p parent starting at @p offset
 * and holds a reference to @p parent, so the parent's storage stays
 * alive for as long as the slice does. @p offset + @p length must not
 * exceed the parent's size. As an optimization, a slice that spans the
 * whole parent returns a new reference to @p parent itself.
 *
 * @return a new AxlBytes with refcount 1 (or a ref to @p parent), or
 *     NULL if @p parent is NULL or the range is out of bounds.
 */
AxlBytes *
axl_bytes_new_from_bytes(
    AxlBytes *parent,  ///< buffer to slice
    size_t    offset,  ///< start offset into @p parent
    size_t    length   ///< slice length
);

// ---------------------------------------------------------------------------
// Reference counting
// ---------------------------------------------------------------------------

/**
 * @brief Add a reference. O(1), returns the same object.
 *
 * @return @p b (or NULL if @p b is NULL).
 */
AxlBytes *
axl_bytes_ref(
    AxlBytes *b  ///< buffer
);

/**
 * @brief Drop a reference; free the buffer when the last one goes.
 *
 * NULL-safe. Dropping the final reference frees owned storage (for
 * axl_bytes_new / _new_take) and releases the parent reference (for a
 * slice); static buffers free only the AxlBytes wrapper.
 */
void
axl_bytes_unref(
    AxlBytes *b  ///< buffer (may be NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlBytes, axl_bytes_unref)
#endif

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

/**
 * @brief Borrow the bytes and (optionally) the size.
 *
 * The returned pointer is owned by @p b and valid until the reference
 * that returned it is dropped. Do not write through it or free it.
 *
 * @return pointer to the bytes (NULL for an empty buffer), and the
 *     length via @p size if non-NULL.
 */
const void *
axl_bytes_get_data(
    const AxlBytes *b,    ///< buffer
    size_t         *size  ///< receives the size, or NULL to ignore
);

/**
 * @brief Get the size in bytes.
 *
 * @return the buffer length (0 if @p b is NULL).
 */
size_t
axl_bytes_get_size(
    const AxlBytes *b  ///< buffer
);

// ---------------------------------------------------------------------------
// Comparison (usable as AxlHashTable key callbacks)
// ---------------------------------------------------------------------------

/**
 * @brief Hash the contents. (GLib: g_bytes_hash)
 *
 * Signature matches AxlHashFunc so AxlBytes can key a hash table;
 * @p b is an `const AxlBytes *` and must be non-NULL (unlike
 * axl_bytes_equal, which tolerates NULL).
 *
 * @return a content-derived hash.
 */
size_t
axl_bytes_hash(
    const void *b  ///< const AxlBytes *
);

/**
 * @brief Content equality. (GLib: g_bytes_equal)
 *
 * Signature matches AxlEqualFunc; @p a and @p b are `const AxlBytes *`.
 *
 * @return true if both have the same size and identical bytes.
 */
bool
axl_bytes_equal(
    const void *a,  ///< const AxlBytes *
    const void *b   ///< const AxlBytes *
);

/**
 * @brief Lexicographic ordering by content. (GLib: g_bytes_compare)
 *
 * Compares the shared prefix byte-by-byte; if one is a prefix of the
 * other, the shorter sorts first. @p a and @p b are `const AxlBytes *`
 * and must be non-NULL (unlike axl_bytes_equal). The magnitude is not
 * normalized to ±1 — only the sign is meaningful.
 *
 * @return <0, 0, or >0.
 */
int
axl_bytes_compare(
    const void *a,  ///< const AxlBytes *
    const void *b   ///< const AxlBytes *
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_BYTES_H */
