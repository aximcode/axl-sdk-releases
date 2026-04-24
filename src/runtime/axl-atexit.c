/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-atexit.c
    POSIX-flavored cleanup registry. Storage is AxlArray + monotonic
    seq, mirroring the tier-1 resource registry's layout — same
    pattern, different payload. See docs/AXL-Runtime.md §4.3.
**/

#include <axl/axl-atexit.h>
#include <axl/axl-array.h>
#include <axl/axl-log.h>

#include "axl-atexit-internal.h"

AXL_LOG_DOMAIN("atexit");

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    AxlAtexitFn  fn;
    void        *data;
    uint64_t     seq;    /* 0 = dead slot (reusable); else insertion order */
} AtexitSlot;

static AxlArray *mAtexit;
static uint64_t  mNextSeq = 1;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void
_axl_atexit_init(void)
{
    if (mAtexit != NULL) {
        return;
    }
    mAtexit = axl_array_new(sizeof (AtexitSlot));
    mNextSeq = 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

uint32_t
axl_atexit(AxlAtexitFn fn, void *data)
{
    AtexitSlot  slot;
    size_t      len;

    if (fn == NULL || mAtexit == NULL) {
        return 0;
    }

    /* Reuse a dead slot if available. */
    len = axl_array_len(mAtexit);
    for (size_t i = 0; i < len; i++) {
        AtexitSlot *s = axl_array_get(mAtexit, i);
        if (s->seq == 0) {
            s->fn   = fn;
            s->data = data;
            s->seq  = mNextSeq++;
            return (uint32_t)(i + 1);
        }
    }

    /* Append a fresh slot. */
    slot.fn   = fn;
    slot.data = data;
    slot.seq  = mNextSeq++;
    axl_array_append(mAtexit, &slot);
    return (uint32_t)axl_array_len(mAtexit);
}

void
axl_atexit_remove(uint32_t handle)
{
    AtexitSlot *s;
    size_t      idx;

    if (mAtexit == NULL || handle == 0) {
        return;
    }
    idx = handle - 1;
    if (idx >= axl_array_len(mAtexit)) {
        return;
    }
    s = axl_array_get(mAtexit, idx);
    s->seq  = 0;
    s->fn   = NULL;
    s->data = NULL;
}

// ---------------------------------------------------------------------------
// Drain (called by _axl_cleanup)
// ---------------------------------------------------------------------------

void
_axl_atexit_run_all(void)
{
    if (mAtexit == NULL) {
        return;
    }

    /* Highest-seq-first walk = LIFO. Quadratic in the worst case, but
     * the table is tiny and this runs once at exit. */
    for (;;) {
        AtexitSlot *newest = NULL;
        size_t      len    = axl_array_len(mAtexit);
        for (size_t i = 0; i < len; i++) {
            AtexitSlot *s = axl_array_get(mAtexit, i);
            if (s->seq != 0 && (newest == NULL || s->seq > newest->seq)) {
                newest = s;
            }
        }
        if (newest == NULL) {
            break;
        }

        AxlAtexitFn  fn   = newest->fn;
        void        *data = newest->data;
        /* Mark dead before invoking so a callback that calls
         * axl_atexit_remove on its own handle is a safe no-op. */
        newest->seq  = 0;
        newest->fn   = NULL;
        newest->data = NULL;

        fn(data);
    }

    axl_array_free(mAtexit);
    mAtexit  = NULL;
    mNextSeq = 1;
}
