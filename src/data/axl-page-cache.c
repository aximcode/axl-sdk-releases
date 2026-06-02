/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-page-cache.c:
 *
 * Fixed-capacity LRU cache of equal-sized pages with a caller-supplied
 * fill callback. See axl-page-cache.h. The frame count is expected to
 * be small (a working set, not a whole-file index), so resident pages
 * are found by a linear scan over the frames — cheaper and simpler than
 * a hash/tree index, and the scan doubles as the LRU-victim search.
 */

#include <axl/axl-page-cache.h>

#include <axl/axl-mem.h>
#include <axl/axl-str.h>

typedef struct {
    const void *owner;       ///< tenant the resident page belongs to
    size_t      page_index;  ///< resident page (valid only when occupied)
    size_t      valid_len;   ///< bytes the fill function wrote
    uint64_t    lru_seq;     ///< last-access tick
    bool        occupied;    ///< frame holds a resident page
} PcFrame;

struct AxlPageCache {
    size_t            page_size;
    size_t            max_frames;
    AxlPageFillFunc   fill;
    void             *user;
    uint8_t          *pool;      ///< max_frames * page_size bytes
    PcFrame          *frames;    ///< max_frames entries
    uint64_t          clock;
    AxlPageCacheStats stats;
};

static AxlPageCache *
pc_alloc(size_t page_size, size_t max_frames, AxlPageFillFunc fill, void *user)
{
    if (page_size == 0 || max_frames == 0) {
        return NULL;
    }
    /* Guard the frame-pool multiply (axl_malloc, unlike axl_calloc, does
       not). page_size and max_frames are caller-controlled, so an
       overflow here would under-allocate the pool and the first fill
       would write past it. max_frames != 0 above makes the divide safe. */
    if (page_size > SIZE_MAX / max_frames) {
        return NULL;
    }

    AxlPageCache *pc = axl_calloc(1, sizeof(AxlPageCache));
    if (pc == NULL) {
        return NULL;
    }

    pc->page_size = page_size;
    pc->max_frames = max_frames;
    pc->fill = fill;
    pc->user = user;
    pc->pool = axl_malloc(max_frames * page_size);
    pc->frames = axl_calloc(max_frames, sizeof(PcFrame));
    if (pc->pool == NULL || pc->frames == NULL) {
        axl_free(pc->pool);
        axl_free(pc->frames);
        axl_free(pc);
        return NULL;
    }

    return pc;
}

AxlPageCache *
axl_page_cache_new(size_t page_size, size_t max_frames,
                   AxlPageFillFunc fill, void *user)
{
    if (fill == NULL) {
        return NULL;
    }
    return pc_alloc(page_size, max_frames, fill, user);
}

AxlPageCache *
axl_page_cache_new_shared(size_t page_size, size_t max_frames)
{
    return pc_alloc(page_size, max_frames, NULL, NULL);
}

void
axl_page_cache_free(AxlPageCache *pc)
{
    if (pc == NULL) {
        return;
    }
    axl_free(pc->pool);
    axl_free(pc->frames);
    axl_free(pc);
}

const void *
axl_page_cache_fetch(AxlPageCache *pc, const void *owner, size_t page_index,
                     AxlPageFillFunc fill, void *user, size_t *valid_len)
{
    if (pc == NULL || fill == NULL) {
        if (valid_len != NULL) {
            *valid_len = 0;
        }
        return NULL;
    }

    /* One pass: return on a resident hit, else track the LRU victim. The
       hit key is (owner, page_index) so tenants never collide. */
    size_t victim = 0;
    uint64_t oldest = ~(uint64_t)0;
    for (size_t i = 0; i < pc->max_frames; i++) {
        PcFrame *f = &pc->frames[i];
        if (f->occupied && f->owner == owner && f->page_index == page_index) {
            f->lru_seq = ++pc->clock;
            pc->stats.hits++;
            if (valid_len != NULL) {
                *valid_len = f->valid_len;
            }
            return pc->pool + i * pc->page_size;
        }
        if (f->lru_seq < oldest) {
            oldest = f->lru_seq;
            victim = i;
        }
    }

    /* Miss: fault the page into the LRU victim frame. */
    pc->stats.misses++;
    PcFrame *f = &pc->frames[victim];
    if (f->occupied) {
        pc->stats.evictions++;
    }

    int64_t got = fill(user, page_index,
                       pc->pool + victim * pc->page_size, pc->page_size);
    if (got < 0 || (size_t)got > pc->page_size) {
        /* Fill failed — leave the frame empty rather than stale. */
        f->occupied = false;
        f->lru_seq = 0;
        if (valid_len != NULL) {
            *valid_len = 0;
        }
        return NULL;
    }

    f->owner = owner;
    f->page_index = page_index;
    f->valid_len = (size_t)got;
    f->lru_seq = ++pc->clock;
    f->occupied = true;
    pc->stats.fills++;
    if (valid_len != NULL) {
        *valid_len = f->valid_len;
    }
    return pc->pool + victim * pc->page_size;
}

const void *
axl_page_cache_get(AxlPageCache *pc, size_t page_index, size_t *valid_len)
{
    /* Single-tenant convenience: the cache is its own owner, fill/user come
       from construction. NULL fill (a shared cache) yields NULL. */
    if (pc == NULL || pc->fill == NULL) {
        if (valid_len != NULL) {
            *valid_len = 0;
        }
        return NULL;
    }
    return axl_page_cache_fetch(pc, pc, page_index, pc->fill, pc->user, valid_len);
}

void
axl_page_cache_drop_owner(AxlPageCache *pc, const void *owner)
{
    if (pc == NULL) {
        return;
    }
    for (size_t i = 0; i < pc->max_frames; i++) {
        PcFrame *f = &pc->frames[i];
        if (f->occupied && f->owner == owner) {
            f->occupied = false;
            f->lru_seq = 0;
        }
    }
}

size_t
axl_page_cache_page_size(const AxlPageCache *pc)
{
    return (pc != NULL) ? pc->page_size : 0;
}

void
axl_page_cache_clear(AxlPageCache *pc)
{
    if (pc == NULL) {
        return;
    }
    for (size_t i = 0; i < pc->max_frames; i++) {
        pc->frames[i].occupied = false;
        pc->frames[i].lru_seq = 0;
    }
    pc->clock = 0;
    axl_memset(&pc->stats, 0, sizeof(pc->stats));
}

void
axl_page_cache_stats(const AxlPageCache *pc, AxlPageCacheStats *out)
{
    if (out == NULL) {
        return;
    }
    if (pc == NULL) {
        axl_memset(out, 0, sizeof(*out));
        return;
    }
    *out = pc->stats;
}
