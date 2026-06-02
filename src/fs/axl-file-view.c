/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-file-view.c:
 *
 * mmap-like windowed view over a file. See axl-file-view.h for the
 * rationale (why a software page cache rather than MMU demand paging in
 * UEFI). AxlFileView is the file-specific policy layer: it owns the
 * open stream and file size, converts byte offsets to page indices, and
 * supplies the page-fill callback (a positional read). All frame
 * management — residency, LRU eviction, the frame pool — lives in the
 * generic AxlPageCache.
 */

#include <axl/axl-file-view.h>

#include <axl/axl-page-cache.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("file-view");

#define AXL_FILE_VIEW_DEFAULT_PAGE  (64u * 1024u)

struct AxlFileView {
    AxlStream    *stream;
    size_t        file_size;
    size_t        page_size;   ///< power of two
    size_t        page_mask;   ///< page_size - 1
    unsigned      page_shift;  ///< log2(page_size)
    AxlPageCache *cache;
    bool          owns_cache;  ///< true: free cache on close; false: borrowed (drop owner)
};

static size_t
round_up_pow2(size_t v)
{
    size_t p = 1;
    while (p < v) {
        size_t next = p << 1;
        if (next < p) {            /* overflow guard */
            return p;
        }
        p = next;
    }
    return p;
}

/* Page-fill callback for the cache: read page @p page_index of the file
   into @p dst. The trailing page is short; pages never extend past EOF
   because the view clamps every offset before asking for a page. */
static int64_t
fv_fill(void *user, size_t page_index, void *dst, size_t cap)
{
    AxlFileView *v = (AxlFileView *)user;
    size_t base = page_index << v->page_shift;
    if (base >= v->file_size) {
        return 0;
    }
    size_t want = v->file_size - base;
    if (want > cap) {
        want = cap;
    }
    axl_ssize_t got = axl_pread(v->stream, dst, want, base);
    if (got < 0 || (size_t)got != want) {
        axl_warning("file_view: pread page %zu (base %zu, want %zu) got %lld",
                    page_index, base, want, (long long)got);
        return -1;
    }
    return (int64_t)got;
}

/* Open the stream + compute page geometry for a view over @path backed by
   @cache (page size already a power of two), owned iff @owns. Frees the
   cache on failure only when owned. */
static AxlFileView *
fv_make(const char *path, AxlPageCache *cache, bool owns)
{
    AxlFsEntry entry;
    if (axl_file_info(path, &entry) != AXL_OK) {
        axl_warning("file_view: cannot stat '%s'", path);
        if (owns) {
            axl_page_cache_free(cache);
        }
        return NULL;
    }

    AxlFileView *v = axl_calloc(1, sizeof(AxlFileView));
    if (v == NULL) {
        if (owns) {
            axl_page_cache_free(cache);
        }
        return NULL;
    }

    v->stream = axl_fopen(path, "r");
    if (v->stream == NULL) {
        axl_warning("file_view: cannot open '%s'", path);
        axl_free(v);
        if (owns) {
            axl_page_cache_free(cache);
        }
        return NULL;
    }

    size_t page_size = axl_page_cache_page_size(cache);
    v->file_size = (size_t)entry.size;
    v->page_size = page_size;
    v->page_mask = page_size - 1;
    v->page_shift = 0;
    for (size_t p = page_size; p > 1; p >>= 1) {
        v->page_shift++;
    }
    v->cache = cache;
    v->owns_cache = owns;
    return v;
}

AxlFileView *
axl_file_view_open(const char *path, size_t page_size, size_t max_frames)
{
    if (path == NULL) {
        return NULL;
    }
    if (page_size == 0) {
        page_size = AXL_FILE_VIEW_DEFAULT_PAGE;
    }
    page_size = round_up_pow2(page_size);
    if (max_frames < 1) {
        max_frames = 1;
    }

    AxlPageCache *cache = axl_page_cache_new_shared(page_size, max_frames);
    if (cache == NULL) {
        return NULL;
    }
    return fv_make(path, cache, true);
}

AxlFileView *
axl_file_view_open_cached(const char *path, AxlPageCache *cache)
{
    if (path == NULL || cache == NULL) {
        return NULL;
    }
    /* The view's offset->page math needs a power-of-two page size. */
    size_t ps = axl_page_cache_page_size(cache);
    if (ps == 0 || (ps & (ps - 1)) != 0) {
        axl_warning("file_view: shared cache page size %zu is not a power of two", ps);
        return NULL;
    }
    return fv_make(path, cache, false);
}

void
axl_file_view_close(AxlFileView *v)
{
    if (v == NULL) {
        return;
    }
    if (v->owns_cache) {
        axl_page_cache_free(v->cache);
    } else {
        axl_page_cache_drop_owner(v->cache, v);   /* return our frames to the pool */
    }
    if (v->stream != NULL) {
        axl_fclose(v->stream);
    }
    axl_free(v);
}

size_t
axl_file_view_size(const AxlFileView *v)
{
    return (v != NULL) ? v->file_size : 0;
}

size_t
axl_file_view_read(AxlFileView *v, size_t offset, void *out, size_t len)
{
    if (v == NULL || out == NULL || offset >= v->file_size) {
        return 0;
    }

    size_t avail = v->file_size - offset;
    if (len > avail) {
        len = avail;
    }

    uint8_t *dst = (uint8_t *)out;
    size_t copied = 0;
    while (copied < len) {
        size_t cur = offset + copied;
        size_t valid_len = 0;
        const uint8_t *page = axl_page_cache_fetch(v->cache, v, cur >> v->page_shift,
                                                   fv_fill, v, &valid_len);
        if (page == NULL) {
            break;
        }
        size_t intra = cur & v->page_mask;
        size_t in_page = valid_len - intra;       /* intra < valid_len since cur < file_size */
        size_t n = len - copied;
        if (n > in_page) {
            n = in_page;
        }
        if (n == 0) {
            break;
        }
        axl_memcpy(dst + copied, page + intra, n);
        copied += n;
    }
    return copied;
}

const void *
axl_file_view_page(AxlFileView *v, size_t offset, size_t *avail)
{
    if (avail != NULL) {
        *avail = 0;
    }
    if (v == NULL || offset >= v->file_size) {
        return NULL;
    }
    size_t valid_len = 0;
    const uint8_t *page = axl_page_cache_fetch(v->cache, v, offset >> v->page_shift,
                                               fv_fill, v, &valid_len);
    if (page == NULL) {
        return NULL;
    }
    size_t intra = offset & v->page_mask;
    if (avail != NULL) {
        *avail = valid_len - intra;
    }
    return page + intra;
}

void
axl_file_view_stats(const AxlFileView *v, AxlFileViewStats *out)
{
    if (out == NULL) {
        return;
    }
    if (v == NULL) {
        axl_memset(out, 0, sizeof(*out));
        return;
    }
    AxlPageCacheStats cs;
    axl_page_cache_stats(v->cache, &cs);
    out->hits = cs.hits;
    out->misses = cs.misses;
    out->evictions = cs.evictions;
    out->preads = cs.fills;     /* one pread per successful fill */
}
