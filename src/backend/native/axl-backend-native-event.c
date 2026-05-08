/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-event.c
    Native UEFI backend — events, timers, and the double-close debug
    ring.

    The double-close ring (mEventCloseRing) records recent
    CloseEvent calls keyed by handle pointer + caller file:line.
    It also tracks creates so a slot reused by UEFI's allocator
    after a previous close doesn't trip the detector on what is
    actually a fresh event. On a real double-close we log both
    sites and SKIP the second close so the test can proceed and
    surface additional info.

    Split out of axl-backend-native.c per docs/Style-Cleanup-Plan.md
    Pass C — events have their own state (the close ring) that
    deserves its own file.
**/

#include "axl-backend.h"
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("backend");

// ---------------------------------------------------------------------------
// Event close debug ring — DIAG 2026-04-27
// ---------------------------------------------------------------------------

#define EVENT_CLOSE_RING_SIZE  256

typedef struct {
    void        *handle;
    const char  *file;
    int          line;
    bool         closed;   /* true after a close; cleared by a fresh create */
} EventCloseRecord;

static EventCloseRecord  mEventCloseRing[EVENT_CLOSE_RING_SIZE];
static size_t            mEventCloseHead;

/* Called by every gBS->CreateEvent wrapper on success. Clears any
 * stale "closed" record for the returned handle so the next close
 * doesn't trip the double-close detector on what's actually a fresh
 * event reusing a recycled slot. */
static void
event_close_ring_record_create(void *handle)
{
    if (handle == NULL) {
        return;
    }
    for (size_t i = 0; i < EVENT_CLOSE_RING_SIZE; i++) {
        if (mEventCloseRing[i].handle == handle) {
            mEventCloseRing[i].handle = NULL;
            mEventCloseRing[i].closed = false;
        }
    }
}

// ---------------------------------------------------------------------------
// AxlBackend public surface — events and timers
// ---------------------------------------------------------------------------

int
axl_backend_event_create_timer(
    AxlEventHandle  *event
    )
{
    EFI_STATUS  status;

    if (event == NULL) {
        return AXL_ERR;
    }

    status = gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION,
                              NULL, NULL, (EFI_EVENT *)event);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    event_close_ring_record_create((void *)*event);
    return AXL_OK;
}

int
axl_backend_event_create(
    AxlEventHandle  *event
    )
{
    EFI_STATUS  status;

    if (event == NULL) {
        return AXL_ERR;
    }

    status = gBS->CreateEvent(0, TPL_APPLICATION,
                              NULL, NULL, (EFI_EVENT *)event);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    event_close_ring_record_create((void *)*event);
    return AXL_OK;
}

void
axl_backend_event_close_dbg(
    AxlEventHandle  event,
    const char     *file,
    int             line
    )
{
    if (event == NULL) {
        return;
    }

    /* Scan ring for a prior close of the same handle. */
    for (size_t i = 0; i < EVENT_CLOSE_RING_SIZE; i++) {
        EventCloseRecord *rec = &mEventCloseRing[i];
        if (rec->closed && rec->handle == (void *)event) {
            axl_warning("DOUBLE-CLOSE: event=%p first-closed-at=%s:%d "
                        "now-being-closed-at=%s:%d -- skipping to avoid "
                        "DxeCore CoreCloseEvent #GP",
                        (void *)event,
                        rec->file ? rec->file : "?",
                        rec->line,
                        file ? file : "?",
                        line);
            return;
        }
    }

    /* Record this close before performing it. */
    EventCloseRecord *rec = &mEventCloseRing[mEventCloseHead];
    rec->handle = (void *)event;
    rec->file   = file;
    rec->line   = line;
    rec->closed = true;
    mEventCloseHead = (mEventCloseHead + 1) % EVENT_CLOSE_RING_SIZE;

    gBS->CloseEvent((EFI_EVENT)event);
}

int
axl_backend_event_set_timer(
    AxlEventHandle  event,
    int             type,
    uint64_t        interval_100ns
    )
{
    EFI_STATUS  status;

    status = gBS->SetTimer((EFI_EVENT)event, (EFI_TIMER_DELAY)type,
                           interval_100ns);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_backend_event_wait(
    size_t          count,
    AxlEventHandle  *events,
    size_t          *fired_index
    )
{
    EFI_STATUS  status;
    UINTN       index;

    status = gBS->WaitForEvent((UINTN)count, (EFI_EVENT *)events, &index);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }
    *fired_index = (size_t)index;
    return AXL_OK;
}

int
axl_backend_event_check(
    AxlEventHandle  event
    )
{
    EFI_STATUS  status;

    status = gBS->CheckEvent((EFI_EVENT)event);
    if (status == EFI_SUCCESS) {
        return 0;
    }
    if (status == EFI_NOT_READY) {
        return 1;
    }
    return -1;
}

int
axl_backend_event_register_protocol_notify(
    void            *guid,
    AxlEventHandle   event,
    void           **registration
    )
{
    EFI_STATUS  status;

    status = gBS->RegisterProtocolNotify((EFI_GUID *)guid,
                                         (EFI_EVENT)event,
                                         registration);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}
