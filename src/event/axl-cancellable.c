/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-cancellable.c
    Generic cancellation token. Composes an AxlEvent — the
    underlying one-shot latch — and adds a magic-number sentinel
    that catches use-after-free on the cancellable itself: on free
    we overwrite the magic with AXL_CANCELLABLE_DEAD (and axl_free
    then scribbles 0xAF over the whole buffer under AXL_MEM_DEBUG).
    Any subsequent public op sees a non-live magic and logs an
    error instead of silently corrupting state or crashing deep
    inside UEFI.

    Semantic layering: axl_cancellable_cancel/is_cancelled/reset
    delegate to axl_event_signal/is_set/reset. What makes a
    cancellable distinct is its contract — async ops receiving one
    interpret the signalled state as "abort" and fire their
    callbacks with AXL_CANCELLED.
**/

#include "axl-cancellable-internal.h"
#include "../runtime/axl-registry-internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-event.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("cancellable");

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/* "AxLC" in little-endian ASCII. Distinctive enough to avoid chance
   collisions; not a secret. */
#define AXL_CANCELLABLE_MAGIC  0x434C7841U
#define AXL_CANCELLABLE_DEAD   0xDEADCA11U

struct AxlCancellable {
    uint32_t  magic;
    uint32_t  _registry_handle;
    AxlEvent *event;
};

static bool
cancellable_is_live(const AxlCancellable *c, const char *op)
{
    if (c == NULL) {
        return false;
    }
    if (c->magic == AXL_CANCELLABLE_MAGIC) {
        return true;
    }
    if (c->magic == AXL_CANCELLABLE_DEAD) {
        axl_error("%s: use-after-free on AxlCancellable", op);
    } else {
        axl_error("%s: AxlCancellable corrupted (magic=0x%08x)",
                  op, (unsigned)c->magic);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlCancellable *
axl_cancellable_new_impl(const char *file, int line)
{
    AxlCancellable *c;

    c = axl_calloc(1, sizeof(AxlCancellable));
    if (c == NULL) {
        return NULL;
    }

    /* The inner event's registry entry records THIS file/line too --
     * i.e., axl-cancellable.c. That's intentional: the event is
     * library-allocated on behalf of the cancellable, so if it ever
     * leaks that points to this wrapper, not the caller. The
     * cancellable itself gets the caller's file/line below. */
    c->event = axl_event_new();
    if (c->event == NULL) {
        axl_free(c);
        return NULL;
    }

    c->magic = AXL_CANCELLABLE_MAGIC;
    c->_registry_handle = _axl_registry_add(AXL_RES_CANCELLABLE, c, file, line);
    return c;
}

void
axl_cancellable_free(AxlCancellable *c)
{
    if (c == NULL) {
        return;
    }
    if (c->magic != AXL_CANCELLABLE_MAGIC) {
        axl_error("axl_cancellable_free: double-free or corrupted "
                  "AxlCancellable (magic=0x%08x)", (unsigned)c->magic);
        return;
    }
    _axl_registry_remove(c->_registry_handle);
    axl_event_free(c->event);
    c->event = NULL;
    c->magic = AXL_CANCELLABLE_DEAD;
    axl_free(c);
}

// ---------------------------------------------------------------------------
// Signal / observe
// ---------------------------------------------------------------------------

void
axl_cancellable_cancel(AxlCancellable *c)
{
    if (!cancellable_is_live(c, "axl_cancellable_cancel")) {
        return;
    }
    axl_event_signal(c->event);
}

bool
axl_cancellable_is_cancelled(const AxlCancellable *c)
{
    if (!cancellable_is_live(c, "axl_cancellable_is_cancelled")) {
        return false;
    }
    return axl_event_is_set(c->event);
}

void
axl_cancellable_reset(AxlCancellable *c)
{
    if (!cancellable_is_live(c, "axl_cancellable_reset")) {
        return;
    }
    axl_event_reset(c->event);
}

// ---------------------------------------------------------------------------
// Internal bridge for async ops / wait primitives
// ---------------------------------------------------------------------------

AxlEventHandle
_axl_cancellable_event(AxlCancellable *c)
{
    if (!cancellable_is_live(c, "_axl_cancellable_event")) {
        return NULL;
    }
    return axl_event_handle(c->event);
}
