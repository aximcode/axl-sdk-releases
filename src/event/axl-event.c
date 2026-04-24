/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-event.c
    Foundational one-shot latch. Wraps a UEFI event plus an is_set
    flag that tracks the *current* signalled state: signal sets it,
    reset clears it, and a successful wait clears it (because the
    loop's SOURCE_EVENT dispatch consumed the signal via CheckEvent).
    The flag lets synchronous code fast-check without a loop
    round-trip.

    Struct carries a magic-number sentinel so use-after-free on the
    event itself is detected: on free we overwrite the magic with
    AXL_EVENT_DEAD (and axl_free then scribbles 0xAF over the whole
    buffer under AXL_MEM_DEBUG). Any subsequent public op sees a
    non-live magic and logs an error instead of silently corrupting
    state or crashing deep inside UEFI.

    Ordering: AXL targets UEFI BSP, which is single-threaded and
    cooperative. is_set is declared volatile so nested callbacks
    (protocol notifications, event notifies) observe the latest
    value, but the library does not guarantee cross-core ordering
    between signal and is_set on platforms with preemptive /
    multi-core parallelism. Safe from any context a single BSP
    thread naturally reaches.
**/

#include "axl-event-internal.h"
#include "axl-wait-internal.h"
#include "../backend/axl-backend.h"
#include "../runtime/axl-registry-internal.h"

#include <stddef.h>
#include <stdint.h>

#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("event");

// ---------------------------------------------------------------------------
// Types / sentinels
// ---------------------------------------------------------------------------

/* "AxLE" in little-endian ASCII -- distinctive enough to avoid
   chance collisions; not a secret. Mirrors the AxlCancellable
   sentinel scheme. */
#define AXL_EVENT_MAGIC  0x454C7841U
#define AXL_EVENT_DEAD   0xDEAD0E7EU

static bool
event_is_live(const AxlEvent *e, const char *op)
{
    if (e == NULL) {
        return false;
    }
    if (e->magic == AXL_EVENT_MAGIC) {
        return true;
    }
    if (e->magic == AXL_EVENT_DEAD) {
        axl_error("%s: use-after-free on AxlEvent", op);
    } else {
        axl_error("%s: AxlEvent corrupted (magic=0x%08x)",
                  op, (unsigned)e->magic);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlEvent *
axl_event_new_impl(const char *file, int line)
{
    AxlEvent *e;

    e = axl_calloc(1, sizeof(AxlEvent));
    if (e == NULL) {
        return NULL;
    }

    if (axl_backend_event_create(&e->handle) != 0) {
        axl_error("failed to create event");
        axl_free(e);
        return NULL;
    }
    e->is_set = false;
    e->magic = AXL_EVENT_MAGIC;
    e->_registry_handle = _axl_registry_add(AXL_RES_EVENT, e, file, line);
    return e;
}

void
axl_event_free(AxlEvent *e)
{
    if (e == NULL) {
        return;
    }
    if (e->magic != AXL_EVENT_MAGIC) {
        axl_error("axl_event_free: double-free or corrupted "
                  "AxlEvent (magic=0x%08x)", (unsigned)e->magic);
        return;
    }
    _axl_registry_remove(e->_registry_handle);
    axl_backend_event_close(e->handle);
    e->handle = NULL;
    e->magic = AXL_EVENT_DEAD;
    axl_free(e);
}

// ---------------------------------------------------------------------------
// Signal / reset / observe
// ---------------------------------------------------------------------------

void
axl_event_signal(AxlEvent *e)
{
    if (!event_is_live(e, "axl_event_signal")) {
        return;
    }
    if (e->handle == NULL) {
        return;
    }
    e->is_set = true;
    axl_backend_event_signal(e->handle);
}

void
axl_event_reset(AxlEvent *e)
{
    if (!event_is_live(e, "axl_event_reset")) {
        return;
    }
    if (e->handle == NULL) {
        return;
    }
    e->is_set = false;
    /* CheckEvent returns EFI_SUCCESS and clears the signalled state
       when the event is signalled, or EFI_NOT_READY when not --
       either way the event is unsignalled afterwards. */
    (void)axl_backend_event_check(e->handle);
}

bool
axl_event_is_set(const AxlEvent *e)
{
    if (!event_is_live(e, "axl_event_is_set")) {
        return false;
    }
    return e->is_set;
}

AxlEventHandle
axl_event_handle(const AxlEvent *e)
{
    if (!event_is_live(e, "axl_event_handle")) {
        return NULL;
    }
    return e->handle;
}

// ---------------------------------------------------------------------------
// Wait
// ---------------------------------------------------------------------------

int
axl_event_wait_timeout(
    AxlEvent       *e,
    AxlCancellable *cancel,
    uint64_t        timeout_us
    )
{
    int rc;

    if (!event_is_live(e, "axl_event_wait_timeout")) {
        return -1;
    }
    if (e->handle == NULL) {
        return -1;
    }
    rc = _axl_event_wait_timeout_with_tick(e->handle, NULL, NULL,
                                           NULL, NULL, 0,
                                           cancel, timeout_us);
    if (rc == 0) {
        /* The loop's SOURCE_EVENT dispatch consumed the signal via
           CheckEvent. Mirror that on the is_set fast-check so
           subsequent axl_event_is_set(e) reflects current state
           rather than a sticky "signal was called at some point".
           Cancel / timeout / Ctrl-C don't consume the event, so
           is_set stays true in those cases. */
        e->is_set = false;
    }
    return rc;
}

int
axl_event_wait(
    AxlEvent       *e,
    AxlCancellable *cancel
    )
{
    return axl_event_wait_timeout(e, cancel, 0);
}
