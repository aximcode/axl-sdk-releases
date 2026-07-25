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

#include "axl-file-gen.h"

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
    char         *path;        ///< own copy — needed to re-stat and re-open
    size_t        file_size;
    size_t        page_size;   ///< power of two
    size_t        page_mask;   ///< page_size - 1
    unsigned      page_shift;  ///< log2(page_size)
    AxlPageCache *cache;
    bool          owns_cache;  ///< true: free cache on close; false: borrowed (drop owner)
    uint32_t      gen_key;     ///< this path's slot in the write registry
    uint32_t      gen_seen;    ///< generation this view is in step with
    bool          pinned;      ///< frozen: ignore writes (see the header)
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
fv_fill(size_t page_index, void *dst, size_t cap, void *user)
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
    /* Sample the generation BEFORE the stat, never after. A write that
       lands between the two then leaves gen_seen behind the registry and
       costs one redundant re-stat on the first read; sampling after would
       instead record the post-write generation against the pre-write
       size, and that write would be lost for the life of the view. */
    uint32_t gen_key  = axl_file_gen_key(path);
    uint32_t gen_seen = axl_file_gen_read(gen_key);

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

    v->path = axl_strdup(path);
    if (v->path == NULL) {
        axl_free(v);
        if (owns) {
            axl_page_cache_free(cache);
        }
        return NULL;
    }

    v->stream = axl_fopen(path, "r");
    if (v->stream == NULL) {
        axl_warning("file_view: cannot open '%s'", path);
        axl_free(v->path);
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
    v->gen_key = gen_key;
    v->gen_seen = gen_seen;
    return v;
}

/* Bring @v into line with the file if any in-image write path has bumped
   its generation since the last sync. This is the whole of the BEST-EFFORT
   half of the consistency model on the read side (the guaranteed half is
   close-to-open, and fv_make is where that is delivered): a hit costs one
   load and one compare, so the overwhelmingly common "nothing wrote it"
   case never touches the firmware.

   On a move, EVERYTHING the view remembers about the file is suspect —
   its cached pages, its length, and the open handle itself. The handle
   is reopened rather than reused because the file may have been replaced
   wholesale (axl_file_write_atomic renames a different file over this
   path), in which case the old handle still refers to the file that was
   moved aside.

   Returns AXL_ERR when the file can no longer be opened. The view is not
   destroyed in that case: it reports size 0, reads nothing, and keeps
   answering AXL_ERR until the path exists again. That is what lets a
   caller give the SAME answer to every read after the file vanished
   rather than one answer on the first and another on the next. */
static int
fv_sync(AxlFileView *v)
{
    if (v->pinned) {
        return AXL_OK;
    }
    uint32_t gen = axl_file_gen_read(v->gen_key);
    if (gen == v->gen_seen && v->stream != NULL) {
        return AXL_OK;
    }
    v->gen_seen = gen;

    axl_page_cache_drop_owner(v->cache, v);
    if (v->stream != NULL) {
        axl_fclose(v->stream);
        v->stream = NULL;
    }

    AxlFsEntry entry;
    if (axl_file_info(v->path, &entry) != AXL_OK) {
        v->file_size = 0;
        return AXL_ERR;
    }
    v->stream = axl_fopen(v->path, "r");
    if (v->stream == NULL) {
        v->file_size = 0;
        return AXL_ERR;
    }
    v->file_size = (size_t)entry.size;
    return AXL_OK;
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
    axl_free(v->path);
    axl_free(v);
}

int
axl_file_view_refresh(AxlFileView *v)
{
    if (v == NULL) {
        return AXL_ERR;
    }
    return fv_sync(v);
}

void
axl_file_view_set_pinned(AxlFileView *v, bool pin)
{
    if (v == NULL) {
        return;
    }
    v->pinned = pin;
}

size_t
axl_file_view_size(AxlFileView *v)
{
    if (v == NULL) {
        return 0;
    }
    /* Deliberately not const: the length is a property of the FILE, and a
       view that answered from a length another writer has already
       invalidated would hand its caller a clamp bound that is simply
       wrong. fv_sync reports failure by zeroing file_size, which is the
       honest answer for a file that is gone. */
    if (fv_sync(v) != AXL_OK) {
        return 0;
    }
    return v->file_size;
}

size_t
axl_file_view_read(AxlFileView *v, size_t offset, void *out, size_t len)
{
    if (v == NULL || out == NULL || fv_sync(v) != AXL_OK
        || offset >= v->file_size) {
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
    if (v == NULL || fv_sync(v) != AXL_OK || offset >= v->file_size) {
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
