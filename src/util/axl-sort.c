/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sort.c
    In-place introsort over a raw element buffer (qsort(3) shape).

    Introsort = median-of-three quicksort, with two escape hatches that
    bound the worst case without any heap allocation:
      - small partitions (<= INSERTION_THRESHOLD) finish via insertion
        sort, which beats quicksort's overhead on short runs;
      - when the partition recursion gets deeper than 2*log2(n) (the
        signature of an adversarial / already-pathological input), the
        remaining range falls back to heapsort.
    The result is O(n log n) worst case, O(log n) stack depth, and no
    allocation — the right shape for a freestanding/UEFI library.
**/

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-sort.h>
#include <axl/axl-runtime.h>
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-str.h>

#define INSERTION_THRESHOLD  16

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Unifies the data / no-data comparator behind one call site so the engine
// is written once. Exactly one of func / func_data is set.
typedef struct {
    AxlCompareFunc      func;
    AxlCompareDataFunc  func_data;
    void               *user_data;
    size_t              ops;        // comparison counter for periodic yield
} SortCtx;

// ---------------------------------------------------------------------------
// Function prototypes (static)
// ---------------------------------------------------------------------------

static int      sort_cmp(SortCtx *c, const void *a, const void *b);
static uint8_t *el(uint8_t *base, size_t i, size_t size);
static void     mem_swap(uint8_t *a, uint8_t *b, size_t size);
static void     insertion_sort(uint8_t *base, size_t n, size_t size, SortCtx *c);
static void     sift_down(uint8_t *base, size_t root, size_t n, size_t size, SortCtx *c);
static void     heap_sort(uint8_t *base, size_t n, size_t size, SortCtx *c);
static int      floor_log2(size_t n);
static void     introsort(uint8_t *base, size_t n, size_t size, SortCtx *c,
                          int depth_limit);

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

static int
sort_cmp(SortCtx *c, const void *a, const void *b)
{
    // A big sort can run long; yield occasionally so it stays Ctrl-C
    // responsive. 65536 comparisons between yields is negligible overhead.
    if ((++c->ops & 0xFFFFu) == 0) {
        _axl_poll_break();
    }

    return c->func_data != NULL
        ? c->func_data(a, b, c->user_data)
        : c->func(a, b);
}

static uint8_t *
el(uint8_t *base, size_t i, size_t size)
{
    return base + (i * size);
}

static void
mem_swap(uint8_t *a, uint8_t *b, size_t size)
{
    uint8_t buf[64];

    // Swap in cache-friendly chunks for large elements; the bounded stack
    // buffer keeps us allocation-free regardless of element size.
    while (size >= sizeof(buf)) {
        axl_memcpy(buf, a, sizeof(buf));
        axl_memcpy(a, b, sizeof(buf));
        axl_memcpy(b, buf, sizeof(buf));
        a += sizeof(buf);
        b += sizeof(buf);
        size -= sizeof(buf);
    }

    while (size > 0) {
        uint8_t t = *a;
        *a = *b;
        *b = t;
        a++;
        b++;
        size--;
    }
}

static void
insertion_sort(uint8_t *base, size_t n, size_t size, SortCtx *c)
{
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && sort_cmp(c, el(base, j - 1, size), el(base, j, size)) > 0) {
            mem_swap(el(base, j - 1, size), el(base, j, size), size);
            j--;
        }
    }
}

static void
sift_down(uint8_t *base, size_t root, size_t n, size_t size, SortCtx *c)
{
    for (;;) {
        size_t child = (2 * root) + 1;

        if (child >= n) {
            break;
        }
        if (child + 1 < n &&
            sort_cmp(c, el(base, child, size), el(base, child + 1, size)) < 0) {
            child++;
        }
        if (sort_cmp(c, el(base, root, size), el(base, child, size)) >= 0) {
            break;
        }

        mem_swap(el(base, root, size), el(base, child, size), size);
        root = child;
    }
}

static void
heap_sort(uint8_t *base, size_t n, size_t size, SortCtx *c)
{
    size_t start = n / 2;
    size_t end;

    // Heapify, then repeatedly pull the max to the end and re-sift.
    while (start > 0) {
        start--;
        sift_down(base, start, n, size, c);
    }

    end = n;
    while (end > 1) {
        end--;
        mem_swap(el(base, 0, size), el(base, end, size), size);
        sift_down(base, 0, end, size, c);
    }
}

static int
floor_log2(size_t n)
{
    int r = 0;

    while (n > 1) {
        n >>= 1;
        r++;
    }
    return r;
}

static void
introsort(uint8_t *base, size_t n, size_t size, SortCtx *c, int depth_limit)
{
    // Tail-loop on the larger partition (recurse only into the smaller one)
    // so stack depth stays O(log n) even on a degenerate split.
    while (n > INSERTION_THRESHOLD) {
        size_t   mid;
        uint8_t *lo;
        uint8_t *md;
        uint8_t *hi;
        uint8_t *pivot;
        size_t   store;
        size_t   left_n;
        size_t   right_n;
        uint8_t *right_base;

        if (depth_limit == 0) {
            // Recursion has gone too deep — bail to guaranteed O(n log n).
            heap_sort(base, n, size, c);
            return;
        }
        depth_limit--;

        // Median-of-three: order first/mid/last, then park the median at
        // the end as the Lomuto pivot. This is what defuses the sorted /
        // reverse-sorted inputs that make naive-pivot quicksort O(n^2).
        mid = n / 2;
        lo = el(base, 0, size);
        md = el(base, mid, size);
        hi = el(base, n - 1, size);
        if (sort_cmp(c, lo, md) > 0) { mem_swap(lo, md, size); }
        if (sort_cmp(c, lo, hi) > 0) { mem_swap(lo, hi, size); }
        if (sort_cmp(c, md, hi) > 0) { mem_swap(md, hi, size); }
        mem_swap(md, el(base, n - 1, size), size);

        // Lomuto partition: pivot stays parked at n-1 (never touched by the
        // loop), so its pointer stays valid with no scratch copy.
        pivot = el(base, n - 1, size);
        store = 0;
        for (size_t k = 0; k < n - 1; k++) {
            if (sort_cmp(c, el(base, k, size), pivot) < 0) {
                if (k != store) {
                    mem_swap(el(base, k, size), el(base, store, size), size);
                }
                store++;
            }
        }
        mem_swap(el(base, store, size), el(base, n - 1, size), size);

        left_n = store;
        right_n = n - store - 1;
        right_base = el(base, store + 1, size);
        if (left_n < right_n) {
            introsort(base, left_n, size, c, depth_limit);
            base = right_base;
            n = right_n;
        } else {
            introsort(right_base, right_n, size, c, depth_limit);
            n = left_n;
        }
    }

    insertion_sort(base, n, size, c);
}

void
axl_qsort(void *base, size_t nmemb, size_t size, AxlCompareFunc compare)
{
    SortCtx ctx;

    if (base == NULL || compare == NULL || size == 0 || nmemb <= 1) {
        return;
    }

    ctx.func = compare;
    ctx.func_data = NULL;
    ctx.user_data = NULL;
    ctx.ops = 0;
    introsort(base, nmemb, size, &ctx, 2 * floor_log2(nmemb));
}

void
axl_qsort_with_data(void *base, size_t nmemb, size_t size,
                    AxlCompareDataFunc compare, void *user_data)
{
    SortCtx ctx;

    if (base == NULL || compare == NULL || size == 0 || nmemb <= 1) {
        return;
    }

    ctx.func = NULL;
    ctx.func_data = compare;
    ctx.user_data = user_data;
    ctx.ops = 0;
    introsort(base, nmemb, size, &ctx, 2 * floor_log2(nmemb));
}
