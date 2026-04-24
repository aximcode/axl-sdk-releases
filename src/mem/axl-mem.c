/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file AxlMem.c
    Memory allocation wrappers with debug features(dmalloc-inspired).

    POSIX-style API: size-tracking header enables realloc without old_size.
    DEBUG builds add fence-post guards, alloc/free fill patterns,
    file/line tracking, and leak reporting.
**/

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("mem");

// ---------------------------------------------------------------------------
// Local helpers (can't use axl-str.c — circular dependency with AxlDataLib)
// ---------------------------------------------------------------------------

static size_t
mem_strlen(const char *s)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

static void *
mem_cpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s2 = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s2[i];
    }
    return dst;
}

static void *
mem_set(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char val = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = val;
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define AXL_FENCE_HEAD  ((size_t)0xC0C0AB1B)
#define AXL_FENCE_TAIL  ((size_t)0xFACADE69)
#define AXL_ALLOC_FILL  0xDA
#define AXL_FREE_FILL   0xDF

// ---------------------------------------------------------------------------
// Internal header — prepended to every POSIX-API allocation
// ---------------------------------------------------------------------------

#ifdef AXL_MEM_DEBUG

typedef struct AXL_MEM_HEADER_ {
    struct AXL_MEM_HEADER_  *next;
    struct AXL_MEM_HEADER_  *prev;
    size_t                   fence_head;
    size_t                   size;
    const char              *file;
    size_t                   line;
    size_t                   fence_mid;
} AXL_MEM_HEADER;

#else

typedef struct {
    size_t  size;
} AXL_MEM_HEADER;

#endif

// ---------------------------------------------------------------------------
// Global stats
// ---------------------------------------------------------------------------

static size_t  mAllocCount;
static size_t  mAllocBytes;
static size_t  mTotalCount;
static size_t  mTotalBytes;

#ifdef AXL_MEM_DEBUG
static AXL_MEM_HEADER  *mAllocList;

/* Fault-injection counter for OOM testing. 0 = disabled; N > 0
   causes the Nth subsequent allocation through axl_malloc_impl to
   return NULL without touching the backend. Set via
   axl_mem_fail_next_alloc(). */
static size_t  mFailNextAlloc;
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/** Round up to size_t alignment. */
static
size_t
align_up(
    size_t  value
    )
{
    return (value + sizeof (size_t) - 1) & ~(sizeof (size_t) - 1);
}

/** Total bytes to allocate for a user request of @size bytes. */
static
size_t
total_size(
    size_t  size
    )
{
    size_t  aligned = align_up(size);

#ifdef AXL_MEM_DEBUG
    return sizeof (AXL_MEM_HEADER) + aligned + sizeof (size_t);
#else
    return sizeof (AXL_MEM_HEADER) + aligned;
#endif
}

/** Get user pointer from header. */
static
void *
user_ptr(
    AXL_MEM_HEADER  *hdr
    )
{
    return (uint8_t *)hdr + sizeof (AXL_MEM_HEADER);
}

/** Get header from user pointer. */
static
AXL_MEM_HEADER *
get_header(
    const void  *ptr
    )
{
    return (AXL_MEM_HEADER *)((uint8_t *)ptr - sizeof (AXL_MEM_HEADER));
}

#ifdef AXL_MEM_DEBUG

/** Get pointer to tail fence value. */
static
size_t *
tail_fence(
    AXL_MEM_HEADER  *hdr
    )
{
    return (size_t *)((uint8_t *)user_ptr(hdr) + align_up(hdr->size));
}

/** Link header into the allocation list. */
static
void
list_insert(
    AXL_MEM_HEADER  *hdr
    )
{
    hdr->prev = NULL;
    hdr->next = mAllocList;
    if (mAllocList != NULL) {
        mAllocList->prev = hdr;
    }
    mAllocList = hdr;
}

/** Unlink header from the allocation list. */
static
void
list_remove(
    AXL_MEM_HEADER  *hdr
    )
{
    if (hdr->prev != NULL) {
        hdr->prev->next = hdr->next;
    } else {
        mAllocList = hdr->next;
    }
    if (hdr->next != NULL) {
        hdr->next->prev = hdr->prev;
    }
}

/** Validate fence-posts around an allocation.  Returns true if clean. */
static
bool
validate_fences(
    AXL_MEM_HEADER  *hdr,
    const char      *caller
    )
{
    bool  ok = true;

    if (hdr->fence_head != AXL_FENCE_HEAD) {
        axl_error(
            "%s: head fence corrupt at %p(alloc %s:%llu, %llu bytes)",
            caller, user_ptr(hdr), hdr->file, (unsigned long long)hdr->line, (unsigned long long)hdr->size
            );
        ok = false;
    }
    if (hdr->fence_mid != AXL_FENCE_HEAD) {
        axl_error(
            "%s: mid fence corrupt at %p(alloc %s:%llu, %llu bytes)",
            caller, user_ptr(hdr), hdr->file, (unsigned long long)hdr->line, (unsigned long long)hdr->size
            );
        ok = false;
    }
    if (*tail_fence(hdr) != AXL_FENCE_TAIL) {
        axl_error(
            "%s: tail fence corrupt at %p(alloc %s:%llu, %llu bytes) — overflow",
            caller, user_ptr(hdr), hdr->file, (unsigned long long)hdr->line, (unsigned long long)hdr->size
            );
        ok = false;
    }

    return ok;
}

#endif // AXL_MEM_DEBUG

// ---------------------------------------------------------------------------
// POSIX-style API — implementation
// ---------------------------------------------------------------------------

void *
axl_malloc_impl(
    size_t      size,
    const char  *file,
    size_t      line
    )
{
    AXL_MEM_HEADER  *hdr;
    size_t           total;

    if (size == 0) {
        size = 1;
    }

#ifdef AXL_MEM_DEBUG
    /* Fault injection for OOM path testing. Fires without touching
       the backend so callers exercise their real error-handling
       path, and logs at debug level so the real-OOM error signal
       stays clean. See axl_mem_fail_next_alloc in axl-mem.h. */
    if (mFailNextAlloc > 0) {
        mFailNextAlloc--;
        if (mFailNextAlloc == 0) {
            axl_debug(
                "axl_malloc(%zu) injected OOM at %s:%llu",
                size, file ? file : "?", (unsigned long long)line
                );
            return NULL;
        }
    }
#endif

    total = total_size(size);
    hdr = (AXL_MEM_HEADER *)axl_backend_alloc(total);
    if (hdr == NULL) {
        axl_error(
            "axl_malloc(%zu) failed at %s:%llu",
            size, file ? file : "?", (unsigned long long)line
            );
        return NULL;
    }

    hdr->size = size;

#ifdef AXL_MEM_DEBUG
    hdr->fence_head = AXL_FENCE_HEAD;
    hdr->file       = file;
    hdr->line       = line;
    hdr->fence_mid  = AXL_FENCE_HEAD;
    mem_set(user_ptr(hdr), AXL_ALLOC_FILL, align_up(size));
    *tail_fence(hdr) = AXL_FENCE_TAIL;
    list_insert(hdr);
#endif

    mAllocCount++;
    mAllocBytes += size;
    mTotalCount++;
    mTotalBytes += size;

    return user_ptr(hdr);
}

void *
axl_calloc_impl(
    size_t      count,
    size_t      size,
    const char  *file,
    size_t      line
    )
{
    size_t  total;
    void   *ptr;

    if (count != 0 && size > SIZE_MAX / count) {
        axl_error(
            "axl_calloc(%zu, %zu) overflow at %s:%llu",
            count, size, file ? file : "?", (unsigned long long)line
            );
        return NULL;
    }
    total = count * size;

    ptr = axl_malloc_impl(total, file, line);
    if (ptr != NULL) {
        mem_set(ptr, 0, total);
    }
    return ptr;
}

void *
axl_realloc_impl(
    void        *ptr,
    size_t      size,
    const char  *file,
    size_t      line
    )
{
    AXL_MEM_HEADER  *old_hdr;
    size_t           old_size;
    void            *new_ptr;
    size_t           copy_size;

    if (ptr == NULL) {
        return axl_malloc_impl(size, file, line);
    }
    if (size == 0) {
        axl_free_impl(ptr);
        return NULL;
    }

    old_hdr  = get_header(ptr);
    old_size = old_hdr->size;

#ifdef AXL_MEM_DEBUG
    validate_fences(old_hdr, "axl_realloc");
#endif

    new_ptr = axl_malloc_impl(size, file, line);
    if (new_ptr == NULL) {
        axl_error(
            "axl_realloc(%p, %zu) failed at %s:%llu",
            ptr, size, file ? file : "?", (unsigned long long)line
            );
        return NULL;
    }

    copy_size = (old_size < size) ? old_size : size;
    mem_cpy(new_ptr, ptr, copy_size);
    axl_free_impl(ptr);

    return new_ptr;
}

void
axl_free_impl(
    void  *ptr
    )
{
    AXL_MEM_HEADER  *hdr;

    if (ptr == NULL) {
        return;
    }

    hdr = get_header(ptr);

#ifdef AXL_MEM_DEBUG
    validate_fences(hdr, "axl_free");
    list_remove(hdr);
    mem_set(user_ptr(hdr), AXL_FREE_FILL, align_up(hdr->size));
#endif

    mAllocCount--;
    mAllocBytes -= hdr->size;

    axl_backend_free(hdr);
}

char *
axl_strdup_impl(
    const char  *str,
    const char  *file,
    size_t      line
    )
{
    size_t  len;
    char    *dup;

    if (str == NULL) {
        return NULL;
    }

    len = mem_strlen(str);
    dup = (char *)axl_malloc_impl(len + 1, file, line);
    if (dup != NULL) {
        mem_cpy(dup, str, len + 1);
    }
    return dup;
}

void *
axl_memdup_impl(
    const void  *src,
    size_t      size,
    const char  *file,
    size_t      line
    )
{
    void  *dup;

    if (src == NULL) {
        return NULL;
    }

    dup = axl_malloc_impl(size, file, line);
    if (dup != NULL) {
        mem_cpy(dup, src, size);
    }
    return dup;
}

// ---------------------------------------------------------------------------
// Statistics and debug
// ---------------------------------------------------------------------------

void
axl_mem_get_stats(
    AxlMemStats  *stats
    )
{
    if (stats == NULL) {
        return;
    }
    stats->count       = (size_t)mAllocCount;
    stats->bytes       = (size_t)mAllocBytes;
    stats->total_count = (size_t)mTotalCount;
    stats->total_bytes = (size_t)mTotalBytes;
}

void
axl_mem_dump_leaks(
    void
    )
{
#ifdef AXL_MEM_DEBUG
    AXL_MEM_HEADER  *cur;
    size_t           idx;

    if (mAllocList == NULL) {
        axl_info("no leaks detected");
        return;
    }

    axl_warning("=== AxlMem leak report: %llu allocations, %llu bytes ===",
               (unsigned long long)mAllocCount, (unsigned long long)mAllocBytes);

    idx = 0;
    for (cur = mAllocList; cur != NULL; cur = cur->next) {
        axl_warning("  [%llu] %llu bytes at %p — %s:%llu",
                   (unsigned long long)idx, (unsigned long long)cur->size, user_ptr(cur), cur->file, (unsigned long long)cur->line);
        idx++;
    }

    axl_warning("=== end leak report ===");
#endif
}

bool
axl_mem_check(
    const void  *ptr
    )
{
#ifdef AXL_MEM_DEBUG
    AXL_MEM_HEADER  *hdr;

    if (ptr == NULL) {
        return false;
    }

    hdr = get_header(ptr);
    return validate_fences(hdr, "axl_mem_check") ? true : false;
#else
    return true;
#endif
}

void
axl_mem_fail_next_alloc(
    size_t  n
    )
{
#ifdef AXL_MEM_DEBUG
    mFailNextAlloc = n;
#else
    (void)n;
#endif
}

// ---------------------------------------------------------------------------
// Page-aligned allocation
// ---------------------------------------------------------------------------

int
axl_alloc_pages(
    size_t    count,
    uint64_t *phys_addr
    )
{
    EFI_STATUS          status;
    EFI_PHYSICAL_ADDRESS addr = 0;

    if (count == 0 || phys_addr == NULL) {
        return -1;
    }

    status = axl_bs()->AllocatePages(
        AllocateAnyPages,
        EfiBootServicesData,
        (size_t)count,
        &addr);

    if (EFI_ERROR(status)) {
        return -1;
    }

    *phys_addr = (uint64_t)addr;
    return 0;
}

void
axl_free_pages(
    uint64_t  phys_addr,
    size_t    count
    )
{
    if (count == 0) {
        return;
    }

    axl_bs()->FreePages((EFI_PHYSICAL_ADDRESS)phys_addr, (size_t)count);
}
