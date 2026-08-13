/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-buf-pool.h
 *
 * Preallocated buffer pool with zero-copy get/put.
 *
 * All buffers are allocated up front in a single contiguous block.
 * Get and put are O(1) free-stack operations — no allocation, no
 * memcpy.  Designed for network receive buffers, file transfer
 * chunks, and VNC tile buffers.
 *
 * @code
 * AxlBufPool *pool = axl_buf_pool_new(4, 64 * 1024);  // 4 x 64KB
 * void *buf = axl_buf_pool_get(pool);   // grab a buffer
 * // ... use buf ...
 * axl_buf_pool_put(pool, buf);          // return it
 * axl_buf_pool_free(pool);
 * @endcode
 */

#ifndef AXL_BUF_POOL_H
#define AXL_BUF_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlBufPool AxlBufPool;

/**
 * @brief Create a new buffer pool.
 *
 * Allocates @a count buffers of @a buf_size bytes each in a single
 * contiguous block.  All buffers are initially available.
 *
 * @return pool handle, or NULL on failure.
 */
AxlBufPool *
axl_buf_pool_new(
    size_t count,    ///< number of buffers
    size_t buf_size  ///< size of each buffer in bytes
);

/**
 * @brief Get a buffer from the pool.
 *
 * @return pointer to a buffer, or NULL if the pool is exhausted.
 */
void *
axl_buf_pool_get(
    AxlBufPool *pool  ///< buffer pool
);

/**
 * @brief Return a buffer to the pool.
 *
 * The buffer must have been obtained from this pool via
 * axl_buf_pool_get.  NULL-safe (no-op if pool or buf is NULL).
 */
void
axl_buf_pool_put(
    AxlBufPool *pool, ///< buffer pool
    void       *buf   ///< buffer to return
);

/**
 * @brief Get the number of available (free) buffers.
 *
 * @return number of buffers available for axl_buf_pool_get.
 */
size_t
axl_buf_pool_available(
    AxlBufPool *pool  ///< buffer pool (NULL returns 0)
);

/**
 * @brief Get the size of each buffer in the pool.
 *
 * @return buffer size in bytes (0 if pool is NULL).
 */
size_t
axl_buf_pool_buf_size(
    AxlBufPool *pool  ///< buffer pool
);

/**
 * @brief Free the pool and all backing memory. NULL-safe.
 */
void
axl_buf_pool_free(
    AxlBufPool *pool  ///< buffer pool to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlBufPool, axl_buf_pool_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_BUF_POOL_H */
