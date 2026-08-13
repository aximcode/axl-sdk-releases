/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cache.h
 *
 * TTL cache with LRU eviction. Fixed-size slots, string keys,
 * opaque fixed-size values. Thread-safe: no (single-threaded UEFI).
 */

#ifndef AXL_CACHE_H
#define AXL_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlCache AxlCache;

/**
 * @brief Create a new TTL cache.
 *
 * @return new cache, or NULL on allocation failure.
 *     Free with axl_cache_free().
 */
AxlCache *
axl_cache_new(
    size_t   max_slots,   ///< maximum number of cached entries
    size_t   entry_size,  ///< size of each value in bytes
    uint64_t ttl_ms       ///< time-to-live per entry in milliseconds
);

/**
 * @brief Store a value in the cache.
 *
 * Copies @p value into the cache under @p key. If @p key already
 * exists, the entry is refreshed. If the cache is full, the oldest
 * (LRU) entry is evicted.
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_cache_put(
    AxlCache   *c,      ///< cache
    const char *key,     ///< string key (copied internally)
    const void *value    ///< value to store (entry_size bytes copied)
);

/**
 * @brief Look up a value by key.
 *
 * Copies the cached value into @p value if found and not expired.
 * Expired entries are treated as misses and invalidated.
 *
 * @return AXL_OK on hit, AXL_ERR on miss or error.
 */
int
axl_cache_get(
    AxlCache   *c,      ///< cache
    const char *key,     ///< string key
    void       *value    ///< [out] receives entry_size bytes
);

/**
 * @brief Invalidate a specific key. No-op if not found.
 */
void
axl_cache_invalidate(
    AxlCache   *c,      ///< cache
    const char *key      ///< key to remove
);

/**
 * @brief Free a cache and all its entries. NULL-safe.
 */
void
axl_cache_free(
    AxlCache *c  ///< cache to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlCache, axl_cache_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_CACHE_H */
