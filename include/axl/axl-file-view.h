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
 *
 * ## Consistency model: CLOSE-TO-OPEN
 *
 * The same guarantee NFS gives by default, and it is worth naming so you
 * have a known model to reason with rather than a bespoke hedge:
 *
 *   - GUARANTEED — a freshly opened view sees the file's CURRENT
 *     contents. Unconditionally: across EFI images, against a non-AXL
 *     writer, against the UEFI Shell, against the firmware itself. This
 *     needs no bookkeeping and nothing can defeat it — the open stats the
 *     file itself, and a view's cached pages are keyed on its own
 *     identity and dropped when it closes.
 *
 *     To see current data, RE-OPEN the view. That is the contract.
 *
 *   - NOT GUARANTEED — that a view ALREADY OPEN when a write happens will
 *     notice. As a best effort it usually will, if the write went through
 *     AXL in the same PE image: every AXL write path records that it
 *     touched a file, and a view compares one integer per access, dropping
 *     its pages and re-stat'ing only when the file it reads actually
 *     moved. That costs a memory compare, not a firmware round trip, so it
 *     is worth having. But it is an optimisation, not a promise.
 *
 * Do not build on the best-effort half. It does not fire for a writer in
 * another PE image (libaxl is statically linked, so an application and a
 * driver it loads have separate bookkeeping and neither sees the other's),
 * nor for a non-AXL writer, nor for the Shell — and no amount of reading
 * will reveal such a write. It is also only evaluated when the view is
 * ASKED for data; a view nobody reads notices nothing. Re-open, or accept
 * that what you hold may be stale.
 *
 * A consumer that wants the opposite — a length fixed at open, immune to
 * later writes — pins the view with axl_file_view_set_pinned(). An editor
 * buffer holding byte offsets into the original text, or an HTTP response
 * body whose Content-Length was already sent, wants exactly that.
 */

#ifndef AXL_FILE_VIEW_H
#define AXL_FILE_VIEW_H

#include <stdbool.h>
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
 * @brief Re-check the file, and report whether it can still be read.
 *
 * Runs the same best-effort check every read runs, so it does NOT make a
 * view see a write it could not otherwise see — a writer in another PE
 * image, a non-AXL writer or the Shell stays invisible either way. Only
 * re-opening the view guarantees current contents (see the consistency
 * model above).
 *
 * The reason to call it is the return value: an empty read cannot
 * otherwise be told apart from a file that was deleted or renamed away
 * under the view.
 *
 * On a pinned view this is a no-op that reports AXL_OK: a pin is a
 * request not to move, and an explicit refresh does not override it.
 *
 * @return AXL_OK if the view can serve reads (whether or not anything
 *     changed); AXL_ERR if the file could no longer be opened, in which
 *     case the view reports size 0 and every read yields 0 bytes until
 *     the path exists again.
 */
AXL_WARN_UNUSED int
axl_file_view_refresh(
    AxlFileView *v  ///< view (NULL → AXL_ERR)
);

/**
 * @brief Freeze the length the view reports, or unfreeze it.
 *
 * Pin a view whose consumer has already committed to a length or to byte
 * offsets — an HTTP body streamed under an already-sent Content-Length,
 * an editor buffer indexing the original text — where picking up a
 * concurrent write would corrupt the consumer rather than refresh it.
 *
 * A pin turns OFF the best-effort half of the consistency model for this
 * view: it stops following writes it would otherwise have noticed. It
 * does not, and cannot, add any guarantee about the file's bytes — under
 * close-to-open there was never one to strengthen. So be precise, because
 * a pin is NOT a snapshot. It bounds what the view REPORTS, not what it
 * can DELIVER:
 *
 *   - GUARANTEED by the pin: the LENGTH stops moving. axl_file_view_size keeps
 *     answering with what the view last observed, so a Content-Length
 *     already on the wire stays honest and offset arithmetic derived from
 *     that length stays self-consistent. This is what the consumers above
 *     actually depend on.
 *   - NOT guaranteed: the BYTES. Current file contents can still show
 *     through, two ways:
 *       - A page that is not resident yet is read from the file as it
 *         stands when it is first touched.
 *       - A page that IS resident can be evicted at any time.
 *         AxlPageCache chooses its LRU victim across ALL frames with no
 *         regard for owner or pin — evicting is what the module is for.
 *         Reading past max_frames pages evicts the view's own earlier
 *         pages, and in a shared cache another tenant can evict them
 *         sooner. Whatever re-faults afterwards is filled from the file
 *         as it stands then.
 *
 * So a pinned view CAN hand back post-write bytes for a region it
 * revisits — consistent with close-to-open, not an exception to it. There
 * is no cheap way to close that: UEFI has no file-snapshot primitive, so
 * the only complete answer is to hold the whole file in memory — exactly
 * what a windowed view exists to avoid. A consumer that genuinely cannot
 * tolerate it must copy the file first.
 *
 * Unpinning does not immediately re-read; the next access does.
 */
void
axl_file_view_set_pinned(
    AxlFileView *v,   ///< view (NULL-safe)
    bool         pin  ///< true: freeze the length; false: resume best-effort tracking
);

/**
 * @brief Byte length of the file as this view currently understands it.
 *
 * The length observed when the view opened, which the best-effort check
 * may have updated since (see the consistency model above) — so this may
 * re-stat the file, and a write this view cannot see leaves it reporting
 * the length from its own open. 0 if the file was deleted or renamed
 * away.
 */
size_t
axl_file_view_size(
    AxlFileView *v  ///< view
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
 * The pointer is valid only until the next call on this view — ANY of
 * them, not only the ones that read. axl_file_view_size and
 * axl_file_view_refresh run the coherence check too, and that drops every
 * frame this view holds when the file has been written. Do not free it.
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
