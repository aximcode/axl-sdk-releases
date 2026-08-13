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
#endif

/* Heap corruptions detected: fence violations, use-after-free writes
   caught on quarantine eviction, and refused double frees. Monotonic.
   Always present (not gated) so axl_mem_corruption_count() is one
   definition in both build modes — it simply never increments in
   RELEASE, where there are no fences to check. */
static size_t  mCorruptionCount;

/* Fault-injection counter for OOM testing. 0 = disabled; N > 0
   causes the Nth subsequent allocation through axl_malloc_impl to
   return NULL without touching the backend. Set via
   axl_mem_fail_next_alloc(). Always present (not gated by
   AXL_MEM_DEBUG) so consumers can exercise their real error-
   handling paths in either build mode — the cost is one
   well-predicted branch per allocation. */
static size_t  mFailNextAlloc;

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
            "%s: tail fence corrupt at %p(alloc %s:%llu, %llu bytes) - overflow",
            caller, user_ptr(hdr), hdr->file, (unsigned long long)hdr->line, (unsigned long long)hdr->size
            );
        ok = false;
    }

    /* Deliberately does NOT touch mCorruptionCount: a quarantined block
       is validated twice (once at free, once at eviction), so counting
       here would score one overflow as two depending on how full the
       ring happened to be. Callers count, once per block. */
    return ok;
}

// ---------------------------------------------------------------------------
// Free quarantine
//
// The leak report can only see memory that was never freed; it is blind
// by construction to memory freed too EARLY, which is the more dangerous
// defect. Holding freed blocks for a while makes both halves of that
// defect observable: the 0xDF fill is re-checked on eviction (catching a
// use-after-free WRITE), and a block still held is refused a second free
// (catching a double free) instead of releasing the pool twice.
// ---------------------------------------------------------------------------

/** Ring capacity. Blocks held beyond this evict the oldest. */
#define AXL_MEM_QUARANTINE_MAX    32u

/** Total bytes the ring may pin. One big block must not starve the heap. */
#define AXL_MEM_QUARANTINE_BYTES  (64u * 1024u)

static AXL_MEM_HEADER  *mQuarantine[AXL_MEM_QUARANTINE_MAX];
static size_t           mQuarantineCount;
static size_t           mQuarantineHead;
static size_t           mQuarantineBytes;
static size_t           mQuarantineLimit = 16;

/* Eviction logs, and a consumer AxlLogHandler may allocate — which can
   re-enter axl_free_impl and mutate the ring underneath a push that has
   already chosen its slot. AXL's own log path allocates nothing, so this
   only fires for a consumer handler, but the failure (aliasing the
   oldest slot, dropping a block, running the count past the ring) is
   silent. A re-entrant free goes straight to the backend instead. */
static bool             mQuarantineBusy;

/** Whether a free of @hdr will land in the ring rather than go straight
 *  back to the backend. Decides which side owns the corruption count:
 *  a held block is re-validated on eviction, so counting at free time
 *  too would score one defect twice. Must agree with the guard at the
 *  top of @ref quarantine_push. */
static
bool
quarantine_will_hold(
    const AXL_MEM_HEADER  *hdr
    )
{
    return mQuarantineLimit > 0
        && !mQuarantineBusy
        && hdr->size <= AXL_MEM_QUARANTINE_BYTES;
}

/** True if @hdr is currently held — i.e. this free is a double free. */
static
bool
quarantine_contains(
    const AXL_MEM_HEADER  *hdr
    )
{
    size_t  i;

    for (i = 0; i < mQuarantineCount; i++) {
        size_t idx = (mQuarantineHead + i) % AXL_MEM_QUARANTINE_MAX;
        if (mQuarantine[idx] == hdr) {
            return true;
        }
    }
    return false;
}

/** Re-check a block we actually HELD, then hand it back for real.
 *
 * Only for blocks that spent time in the ring: a block released without
 * being held was filled microseconds ago and never handed out, so
 * scanning it cannot fire and would double the cost of every free — for
 * a multi-MiB block, expensively. Those go straight to
 * @ref axl_backend_free. */
static
void
quarantine_release(
    AXL_MEM_HEADER  *hdr
    )
{
    uint8_t  *body = (uint8_t *)user_ptr(hdr);
    size_t    n    = align_up(hdr->size);
    bool      bad  = false;
    size_t    i;

    /* A write past the end of a freed block lands on the tail fence
       rather than in the body, so both checks are needed — but they
       describe ONE corrupt block and are counted once. */
    if (!validate_fences(hdr, "axl_free (quarantined)")) {
        bad = true;
    }

    for (i = 0; i < n; i++) {
        if (body[i] != AXL_FREE_FILL) {
            axl_error(
                "use-after-free write at %p+%llu (alloc %s:%llu, %llu bytes): "
                "0x%02x, expected 0x%02x",
                user_ptr(hdr), (unsigned long long)i, hdr->file,
                (unsigned long long)hdr->line, (unsigned long long)hdr->size,
                body[i], AXL_FREE_FILL
                );
            bad = true;
            break;      /* one report per block, not one per byte */
        }
    }

    if (bad) {
        mCorruptionCount++;
    }

    axl_backend_free(hdr);
}

/** Release the oldest held block. */
static
void
quarantine_evict(
    void
    )
{
    AXL_MEM_HEADER  *hdr = mQuarantine[mQuarantineHead];

    mQuarantineHead   = (mQuarantineHead + 1) % AXL_MEM_QUARANTINE_MAX;
    mQuarantineCount -= 1;
    mQuarantineBytes -= hdr->size;
    quarantine_release(hdr);
}

/** Take ownership of a freed block, evicting older ones to make room. */
static
void
quarantine_push(
    AXL_MEM_HEADER  *hdr
    )
{
    size_t  cap = mQuarantineLimit;
    size_t  idx;

    if (cap > AXL_MEM_QUARANTINE_MAX) {
        cap = AXL_MEM_QUARANTINE_MAX;
    }

    /* A block bigger than the whole budget is released immediately —
       holding it would evict every other block and pin the heap for no
       extra coverage. Straight to the backend, NOT via
       quarantine_release: the block was never held, so its fill cannot
       have changed and scanning it is pure cost.

       mQuarantineBusy covers the re-entrant case: an eviction below logs,
       a consumer log handler may allocate and free, and that free must
       not renumber the ring while this push is mid-flight. */
    if (cap == 0 || mQuarantineBusy || hdr->size > AXL_MEM_QUARANTINE_BYTES) {
        axl_backend_free(hdr);
        return;
    }

    mQuarantineBusy = true;
    while (mQuarantineCount > 0
           && (mQuarantineCount >= cap
               || mQuarantineBytes + hdr->size > AXL_MEM_QUARANTINE_BYTES)) {
        quarantine_evict();
    }

    idx = (mQuarantineHead + mQuarantineCount) % AXL_MEM_QUARANTINE_MAX;
    mQuarantine[idx]  = hdr;
    mQuarantineCount += 1;
    mQuarantineBytes += hdr->size;
    mQuarantineBusy = false;
}

/** Release everything held, running the checks on each. */
static
void
quarantine_drain(
    void
    )
{
    while (mQuarantineCount > 0) {
        quarantine_evict();
    }
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

    /* Fault injection for OOM path testing. Fires without touching
       the backend so callers exercise their real error-handling
       path, and logs at debug level so the real-OOM error signal
       stays clean. See axl_mem_fail_next_alloc in axl-mem.h.
       Always-on (not gated by AXL_MEM_DEBUG) so the documented
       public-API contract holds in both DEBUG and RELEASE. */
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
    /* Refuse a second free of a block we are still holding. Without the
       quarantine this is undetectable: the header has already gone back
       to the firmware, so re-reading its fences is itself undefined and
       the double release corrupts the pool allocator rather than
       reporting anything. Checked BEFORE the fences for that reason. */
    if (quarantine_contains(hdr)) {
        mCorruptionCount++;
        axl_error(
            "axl_free: double free of %p (alloc %s:%llu, %llu bytes) - refused",
            ptr, hdr->file, (unsigned long long)hdr->line,
            (unsigned long long)hdr->size
            );
        return;
    }

    /* Counted here ONLY when the block will not be quarantined. A held
       block is validated again on eviction, and counting in both places
       scores one overflow as two — with the delta depending on whether
       the ring happened to be enabled, which makes the counter useless
       to assert on. quarantine_release owns the count for held blocks. */
    if (!validate_fences(hdr, "axl_free") && !quarantine_will_hold(hdr)) {
        mCorruptionCount++;
    }
    list_remove(hdr);
    mem_set(user_ptr(hdr), AXL_FREE_FILL, align_up(hdr->size));
#endif

    mAllocCount--;
    mAllocBytes -= hdr->size;

#ifdef AXL_MEM_DEBUG
    /* The block leaves the live list above, so holding it here never
       shows up as a leak — it is simply released later, after the
       use-after-free check. */
    quarantine_push(hdr);
    return;
#else
    axl_backend_free(hdr);
#endif
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

/* Shared body of the two leak reports.
 *
 * @a at_exit selects the ONE token that separates a verdict from a
 * diagnostic, and the QEMU harness gates on that distinction:
 *
 *   at_exit  -> "=== AxlMem leak report: ..."
 *               Printed from _axl_cleanup / the minimal CRT0, after
 *               atexit + the tier-1 sweep have run. Nothing further
 *               will free anything, so every block listed IS leaked.
 *               test_check_leaks (test/integration/common-test.sh)
 *               greps for exactly this and fails the run.
 *
 *   !at_exit -> "=== AxlMem leak report (live allocations): ..."
 *               axl_mem_dump_leaks() called by a running program. The
 *               listed blocks are merely alive at the call site and
 *               may well be freed later, so this must NOT fail a test
 *               run — hence the infix, which keeps the harness's
 *               "leak report:" anchor from matching.
 *
 * One format string, one infix: the two spellings cannot drift apart.
 * test_leak_dump (test/unit/axl-test-mem.c) pins the live-allocations
 * spelling exactly, so collapsing the split back into one report is a
 * unit-test failure rather than a gate that silently stops firing.
 */
static void
dump_leaks(
    bool  at_exit
    )
{
#ifdef AXL_MEM_DEBUG
    AXL_MEM_HEADER  *cur;
    size_t           idx;

    if (mAllocList == NULL) {
        axl_info("no leaks detected");
        return;
    }

    axl_warning("=== AxlMem leak report%s: %llu allocations, %llu bytes ===",
               at_exit ? "" : " (live allocations)",
               (unsigned long long)mAllocCount, (unsigned long long)mAllocBytes);

    idx = 0;
    for (cur = mAllocList; cur != NULL; cur = cur->next) {
        axl_warning("  [%llu] %llu bytes at %p - %s:%llu",
                   (unsigned long long)idx, (unsigned long long)cur->size, user_ptr(cur), cur->file, (unsigned long long)cur->line);
        idx++;
    }

    axl_warning("=== end leak report ===");
#else
    (void)at_exit;
#endif
}

void
axl_mem_dump_leaks(
    void
    )
{
#ifdef AXL_MEM_DEBUG
    /* Drain first. A resident driver has no _axl_cleanup (see
       axl-driver.h), so this diagnostic form is the ONLY report it ever
       gets — and a driver that never drains would pin the ring's worth
       of freed pool for its whole residency. Draining here also means
       the use-after-free checks actually run for driver code. */
    quarantine_drain();
#endif
    dump_leaks(false);
}

void
_axl_mem_dump_leaks_at_exit(
    void
    )
{
#ifdef AXL_MEM_DEBUG
    /* Drain BEFORE the verdict: a held block is not a leak, but the
       use-after-free check that runs on its way out may still have
       something to say, and it should be said before the report a
       reader treats as the summary. */
    quarantine_drain();
#endif
    dump_leaks(true);
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
    mFailNextAlloc = n;
}

void
axl_mem_set_quarantine(
    size_t  blocks
    )
{
#ifdef AXL_MEM_DEBUG
    mQuarantineLimit = (blocks > AXL_MEM_QUARANTINE_MAX)
                     ? AXL_MEM_QUARANTINE_MAX
                     : blocks;

    /* Shrinking releases the excess now, so 0 is also the "run the
       pending checks immediately" control a test needs. */
    while (mQuarantineCount > mQuarantineLimit) {
        quarantine_evict();
    }
#else
    (void)blocks;
#endif
}

size_t
axl_mem_corruption_count(
    void
    )
{
    return mCorruptionCount;
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
        return AXL_ERR;
    }

    status = axl_bs()->AllocatePages(
        AllocateAnyPages,
        EfiBootServicesData,
        (size_t)count,
        &addr);

    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    *phys_addr = (uint64_t)addr;
    return AXL_OK;
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
