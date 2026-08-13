/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-page-cache.h
 *
 * A fixed-capacity LRU cache of equal-sized pages, backed by a
 * caller-supplied fill function. The cache owns a pool of page frames;
 * on a miss it evicts the least-recently-used frame and calls the fill
 * function to populate it. Lookups return a borrowed pointer into the
 * resident frame — zero-copy, no per-access value copy.
 *
 * It is the mechanism behind AxlFileView (pages faulted in from a file
 * via positional reads), but knows nothing about files: the fill
 * function decides where bytes come from, so the same cache windows any
 * large, randomly-addressed backing store (block device, decompressed
 * stream, generated data) where only the hot pages should stay
 * resident.
 *
 * Distinct from AxlCache: that is a TTL, string-keyed, copy-in/copy-out
 * value cache; this is a capacity-only, integer-indexed, zero-copy page
 * cache whose frames are recycled in place on eviction.
 *
 * Two usage modes share one frame pool and LRU:
 *   - Single-tenant — axl_page_cache_new(... fill ...) binds one fill
 *     function; axl_page_cache_get keys frames by page index alone.
 *   - Multi-tenant — axl_page_cache_new_shared() makes a fill-less cache
 *     that several owners share; axl_page_cache_fetch keys frames by
 *     (owner, page index) and takes the fill per call, so one budget of
 *     resident frames serves many backing stores (e.g. every open file in
 *     an editor). axl_page_cache_drop_owner reclaims a closing owner's
 *     frames. (Single-tenant get is the multi-tenant fetch with the cache
 *     itself as the owner.)
 *
 * Single-threaded (UEFI). Borrowed page pointers are valid only until
 * the next axl_page_cache_get / _fetch / _clear that may evict.
 */

#ifndef AXL_PAGE_CACHE_H
#define AXL_PAGE_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlPageCache AxlPageCache;

/**
 * AxlPageFillFunc:
 *
 * Populate the frame for @a page_index. @a dst has @a cap bytes (the
 * cache's page size); write up to @a cap bytes of that page's content
 * and return how many were written. A short count (< @a cap) marks a
 * partial trailing page — the cache records it as the page's valid
 * length. Return -1 on error; the cache then leaves the frame empty and
 * axl_page_cache_get returns NULL.
 *
 * @return bytes written (0..cap), or -1 on error.
 */
typedef int64_t (*AxlPageFillFunc)(
    size_t  page_index,  ///< page to fill
    void   *dst,         ///< frame buffer (cap bytes)
    size_t  cap,         ///< frame capacity == page size
    void   *user         ///< opaque cookie from axl_page_cache_new
) AXL_CB_NOEXCEPT;

/// Cache counters (monotonic; reset by axl_page_cache_clear).
typedef struct {
    uint64_t hits;        ///< lookups served from a resident frame
    uint64_t misses;      ///< lookups that required a fill
    uint64_t evictions;   ///< resident pages discarded to make room
    uint64_t fills;       ///< successful fill calls (== misses minus fill errors)
} AxlPageCacheStats;

/**
 * @brief Create a page cache.
 *
 * Allocates @a max_frames frames of @a page_size bytes up front.
 *
 * @return new cache, or NULL on OOM / invalid args (@a page_size or
 *     @a max_frames zero, or @a fill NULL). Free with
 *     axl_page_cache_free().
 */
AxlPageCache *
axl_page_cache_new(
    size_t          page_size,   ///< bytes per page (frame capacity)
    size_t          max_frames,  ///< resident frame count (LRU capacity)
    AxlPageFillFunc fill,        ///< page-fill callback (required)
    void           *user         ///< opaque cookie passed to @p fill
);

/**
 * @brief Create a shared (multi-tenant) page cache.
 *
 * Like axl_page_cache_new but with no bound fill function: several owners
 * share the one frame pool and LRU via axl_page_cache_fetch, each
 * supplying its own fill per call and its own owner token. Use it to cap
 * the total resident pages across many backing stores (e.g. all open
 * files in an editor). axl_page_cache_get is not available on a shared
 * cache (it has no default fill) — use fetch.
 *
 * @return new cache, or NULL on OOM / invalid args (@p page_size or
 *     @p max_frames zero). Free with axl_page_cache_free().
 */
AxlPageCache *
axl_page_cache_new_shared(
    size_t page_size,   ///< bytes per page (frame capacity)
    size_t max_frames   ///< resident frame count shared across all owners
);

/**
 * @brief Free a page cache and its frame pool. NULL-safe.
 */
void
axl_page_cache_free(
    AxlPageCache *pc  ///< cache (NULL-safe)
);

/**
 * @brief Get a borrowed pointer to a resident page.
 *
 * Returns a pointer to the start of @a page_index's frame, faulting it
 * in via the fill function on a miss (evicting the LRU frame first).
 * @p *valid_len receives the page's valid byte count (what the fill
 * function returned).
 *
 * The pointer is valid only until the next axl_page_cache_get / _fetch /
 * _clear call, which may evict this frame. Do not free it.
 *
 * @return frame pointer, or NULL if the fill function reported an error
 *     (or the cache has no bound fill — i.e. a shared cache; use _fetch).
 */
const void *
axl_page_cache_get(
    AxlPageCache *pc,         ///< cache
    size_t        page_index,  ///< page to fetch
    size_t       *valid_len    ///< [out, optional] valid bytes in the page
);

/**
 * @brief Get a borrowed page for a specific owner (multi-tenant).
 *
 * Like axl_page_cache_get but keys the frame by (@p owner, @p page_index)
 * and takes the @p fill / @p user from the call rather than the cache's
 * constructor, so distinct owners never collide on the same page index
 * and one cache serves many backing stores. @p owner is any stable,
 * unique pointer identifying the tenant (typically the owning object).
 * Eviction is global LRU across all owners.
 *
 * Works on both shared and single-tenant caches. The borrowed pointer is
 * valid only until the next get / fetch / clear that may evict.
 *
 * @return frame pointer, or NULL if @p fill is NULL or reported an error.
 */
const void *
axl_page_cache_fetch(
    AxlPageCache   *pc,          ///< cache
    const void     *owner,       ///< tenant identity (stable, unique pointer)
    size_t          page_index,  ///< page to fetch within the owner
    AxlPageFillFunc fill,        ///< fill callback for a miss (required)
    void           *user,        ///< opaque cookie passed to @p fill
    size_t         *valid_len    ///< [out, optional] valid bytes in the page
);

/**
 * @brief Evict every resident frame belonging to @p owner.
 *
 * A closing tenant (e.g. an AxlFileView sharing the cache) calls this to
 * return its frames to the pool. Frames of other owners are untouched.
 */
void
axl_page_cache_drop_owner(
    AxlPageCache *pc,    ///< cache
    const void   *owner  ///< tenant whose frames to drop
);

/**
 * @brief Page size (frame capacity) in bytes.
 */
size_t
axl_page_cache_page_size(
    const AxlPageCache *pc  ///< cache
);

/**
 * @brief Drop all resident pages (e.g. the backing store changed).
 *
 * Frames become empty; the next get of any page is a miss. Stats are
 * reset to zero.
 */
void
axl_page_cache_clear(
    AxlPageCache *pc  ///< cache
);

/**
 * @brief Snapshot the cache counters.
 */
void
axl_page_cache_stats(
    const AxlPageCache *pc,    ///< cache
    AxlPageCacheStats  *out    ///< [out] counters (zeroed if @p pc is NULL)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlPageCache, axl_page_cache_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_PAGE_CACHE_H */
