/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-registry.c
    Tier-1 firmware-resource registry. Stores one entry per live
    AxlEvent / AxlLoop / AxlCancellable / AxlArena, keyed by a
    monotonic seq number so the exit-time sweep processes them in
    true LIFO order (last-registered-first-freed), matching atexit
    semantics and letting containers tear down before their contents.
    See docs/AXL-Lifecycle.md §4.2.
**/

#include "axl-registry-internal.h"

#include <axl/axl-array.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("registry");

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    AxlResKind   kind;
    void        *resource;
    void       (*dtor)(void *resource);  /* per-entry free; see _axl_registry_add */
    const char  *file;
    int          line;
    uint64_t     seq;       /* 0 = dead slot (reusable); else insertion order */
} RegistryEntry;

static AxlArray *mRegistry;
static uint64_t  mNextSeq = 1;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void
_axl_registry_init(void)
{
    if (mRegistry != NULL) {
        return;
    }
    mRegistry = axl_array_new(sizeof (RegistryEntry));
    mNextSeq = 1;
}

// ---------------------------------------------------------------------------
// Add / remove
// ---------------------------------------------------------------------------

uint32_t
_axl_registry_add(
    AxlResKind   kind,
    void        *resource,
    void       (*dtor)(void *resource),
    const char  *file,
    int          line
    )
{
    RegistryEntry  entry;
    size_t         len;

    if (mRegistry == NULL) {
        return 0;
    }

    /* Reuse a dead slot if one exists (keeps the array compact across
     * add/remove churn). */
    len = axl_array_len(mRegistry);
    for (size_t i = 0; i < len; i++) {
        RegistryEntry *e = axl_array_get(mRegistry, i);
        if (e->seq == 0) {
            e->kind     = kind;
            e->resource = resource;
            e->dtor     = dtor;
            e->file     = file;
            e->line     = line;
            e->seq      = mNextSeq++;
            return (uint32_t)(i + 1);
        }
    }

    /* Append a fresh slot. */
    entry.kind     = kind;
    entry.resource = resource;
    entry.dtor     = dtor;
    entry.file     = file;
    entry.line     = line;
    entry.seq      = mNextSeq++;
    axl_array_append(mRegistry, &entry);
    return (uint32_t)axl_array_len(mRegistry);
}

void
_axl_registry_remove(uint32_t handle)
{
    RegistryEntry *e;
    size_t         idx;

    if (mRegistry == NULL || handle == 0) {
        return;
    }
    idx = handle - 1;
    if (idx >= axl_array_len(mRegistry)) {
        return;
    }
    e = axl_array_get(mRegistry, idx);
    e->seq      = 0;
    e->resource = NULL;
}

size_t
_axl_registry_size(void)
{
    size_t count = 0;
    size_t len;

    if (mRegistry == NULL) {
        return 0;
    }
    len = axl_array_len(mRegistry);
    for (size_t i = 0; i < len; i++) {
        RegistryEntry *e = axl_array_get(mRegistry, i);
        if (e->seq != 0) {
            count++;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

static const char *
kind_name(AxlResKind kind)
{
    switch (kind) {
    case AXL_RES_EVENT:       return "AxlEvent";
    case AXL_RES_LOOP:        return "AxlLoop";
    case AXL_RES_CANCELLABLE: return "AxlCancellable";
    case AXL_RES_ARENA:       return "AxlArena";
    }
    return "?";
}

void
_axl_registry_sweep(void)
{
    if (mRegistry == NULL) {
        return;
    }

    /* Pick the highest-seq live entry each iteration, process it,
     * mark its slot dead, and repeat until empty. Quadratic in the
     * worst case but the table is tiny and this only runs once at
     * exit. */
    for (;;) {
        RegistryEntry *newest     = NULL;
        size_t         len        = axl_array_len(mRegistry);
        for (size_t i = 0; i < len; i++) {
            RegistryEntry *e = axl_array_get(mRegistry, i);
            if (e->seq != 0 && (newest == NULL || e->seq > newest->seq)) {
                newest = e;
            }
        }
        if (newest == NULL) {
            break;
        }

        axl_warning("sweep: %s leaked at %s:%d -- closing",
                    kind_name(newest->kind), newest->file, newest->line);

        /* Mark dead before calling the destructor — the _free path calls
         * back into _axl_registry_remove, which is then a safe no-op. */
        void       (*dtor)(void *) = newest->dtor;
        void        *resource      = newest->resource;
        newest->seq                = 0;
        newest->resource           = NULL;

        /* Indirect call (see _axl_registry_add): the registry holds no
         * static reference to axl_loop_free / axl_event_free / etc. */
        if (dtor != NULL) {
            dtor(resource);
        }
    }

    axl_array_free(mRegistry);
    mRegistry = NULL;
    mNextSeq  = 1;
}
