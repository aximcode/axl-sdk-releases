/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-file-view.h:
 *
 * An mmap-like windowed view over a file.
 *
 * The file is NEVER loaded whole. Reads are served from a small, fixed
 * set of page frames (an AxlPageCache) backed by positional reads
 * (axl_pread); only the hot pages stay resident. This gives the
 * out-of-core property real mmap would — without MMU demand paging,
 * which is unworkable in UEFI boot services (servicing a page fault
 * means blocking filesystem I/O from an exception handler, outside the TPL
 * model — and a FAT file has no physical backing to zero-copy map, so
 * even "real" mmap would copy each page in through the FS driver
 * anyway).
 *
 * Two access modes:
 *   - axl_file_view_read  — copy a [offset, offset+len) range out
 *     (spans pages transparently). Always safe.
 *   - axl_file_view_page  — borrow a pointer into the resident frame
 *     for the page containing an offset (zero-copy, single page only;
 *     valid until the next view call that may evict).
 *
 * Read-only: the view never writes back. Intended for windowing large
 * files (logs, the original text of an out-of-core editor buffer) where
 * loading the whole file is undesirable.
 */

#ifndef AXL_FILE_VIEW_H
#define AXL_FILE_VIEW_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlFileView AxlFileView;
typedef struct AxlPageCache AxlPageCache;   /* <axl/axl-page-cache.h> */

/// Cache counters, for spike validation (hit rate, that we never
/// pread more than necessary, eviction actually happens).
typedef struct {
    uint64_t hits;        ///< page lookups served from a resident frame
    uint64_t misses;      ///< page lookups that had to read from the file
    uint64_t evictions;   ///< resident pages discarded to make room
    uint64_t preads;      ///< underlying axl_pread calls (one per successful fill; == misses when no read errors)
} AxlFileViewStats;

/**
 * @brief Open a windowed, page-cached view over a file.
 *
 * The file is opened read-only; its size is queried once. No file
 * content is read until the first access.
 *
 * @return new view, or NULL on open failure / OOM.
 *     Free with axl_file_view_close().
 */
AxlFileView *
axl_file_view_open(
    const char *path,        ///< file path (UTF-8)
    size_t      page_size,   ///< frame size in bytes (0 = 64 KiB default; rounded up to a power of two)
    size_t      max_frames   ///< number of resident frames (LRU capacity; min 1)
);

/**
 * @brief Open a view that shares an existing page cache.
 *
 * Like axl_file_view_open but instead of allocating its own frame pool,
 * the view borrows @p cache (created with axl_page_cache_new_shared) and
 * keys its pages by this view's identity, so one bounded frame budget is
 * shared across many open files. The view adopts the cache's page size
 * (which must be a power of two). The cache is NOT freed on close — the
 * view drops only its own frames (axl_page_cache_drop_owner); the caller
 * owns the cache and frees it after all sharing views are closed.
 *
 * @return new view, or NULL on open failure / OOM / a non-power-of-two
 *     cache page size. Free with axl_file_view_close().
 */
AxlFileView *
axl_file_view_open_cached(
    const char   *path,   ///< file path (UTF-8)
    AxlPageCache *cache   ///< shared cache to borrow (caller-owned)
);

/**
 * @brief Close a view and release its frames. NULL-safe.
 */
void
axl_file_view_close(
    AxlFileView *v  ///< view (NULL-safe)
);

/**
 * @brief Total byte length of the underlying file.
 */
size_t
axl_file_view_size(
    const AxlFileView *v  ///< view
);

/**
 * @brief Copy a byte range out of the view.
 *
 * Copies up to @p len bytes starting at @p offset into @p out,
 * faulting in (and caching) whichever pages the range touches. The
 * range may span any number of pages. Clamped to the file size: a
 * range starting at or past EOF copies 0 bytes.
 *
 * @return number of bytes actually copied (< @p len near EOF).
 */
size_t
axl_file_view_read(
    AxlFileView *v,       ///< view
    size_t       offset,  ///< byte offset to read from
    void        *out,     ///< destination buffer
    size_t       len      ///< bytes requested
);

/**
 * @brief Borrow a zero-copy pointer into the page containing @p offset.
 *
 * Ensures the page is resident and returns a pointer to @p offset
 * within that frame, setting @p *avail to the number of contiguous
 * valid bytes available from @p offset to the end of that page's data.
 * To cross a page boundary, call again at @p offset + @p *avail.
 *
 * The pointer is valid only until the next call that may evict
 * (any read/page call). Do not free it.
 *
 * @return pointer into a resident frame, or NULL if @p offset is at or
 *     past EOF (in which case @p *avail is set to 0).
 */
const void *
axl_file_view_page(
    AxlFileView *v,       ///< view
    size_t       offset,  ///< byte offset
    size_t      *avail    ///< [out] contiguous valid bytes from @p offset in this page
);

/**
 * @brief Snapshot the cache counters (spike instrumentation).
 */
void
axl_file_view_stats(
    const AxlFileView *v,    ///< view
    AxlFileViewStats  *out   ///< [out] counters
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlFileView, axl_file_view_close)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_FILE_VIEW_H */
