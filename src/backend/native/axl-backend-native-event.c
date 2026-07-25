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
#include <axl/axl-mem.h>

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

// ---------------------------------------------------------------------------
// Periodic notify-signal timer — driver-mode dispatch primitive.
//
// Used by axl_loop_attach_driver. Allocates a small NotifyTimerCtx
// that bridges the firmware's (EFI_EVENT, void *) notify signature
// to AXL's (void *ctx) callback shape, and tracks the allocation in
// a fixed table so axl_backend_event_close can free it without a
// separate close primitive.
// ---------------------------------------------------------------------------

typedef struct {
    void  (*notify)(void *);
    void   *ctx;
} NotifyTimerCtx;

#define NOTIFY_TIMER_TABLE_SIZE  16

typedef struct {
    EFI_EVENT       handle;
    NotifyTimerCtx *ctx;
    bool            active;
} NotifyTimerEntry;

static NotifyTimerEntry mNotifyTimerTable[NOTIFY_TIMER_TABLE_SIZE];

static VOID EFIAPI
notify_timer_trampoline(
    EFI_EVENT  event,
    VOID      *ctx_v
    )
{
    (void)event;
    NotifyTimerCtx *nc = (NotifyTimerCtx *)ctx_v;
    if (nc != NULL && nc->notify != NULL) {
        nc->notify(nc->ctx);
    }
}

int
axl_backend_event_create_notify_timer(
    void   (*notify)(void *ctx),
    void    *ctx,
    uint64_t interval_100ns,
    AxlEventHandle *event
    )
{
    EFI_STATUS       status;
    EFI_EVENT        ev = NULL;
    NotifyTimerCtx  *nc = NULL;
    size_t           slot = NOTIFY_TIMER_TABLE_SIZE;

    if (notify == NULL || event == NULL || interval_100ns == 0) {
        return AXL_ERR;
    }

    /* Reserve a tracking slot first — fail before allocating if the
       table is full so we don't have to roll back the alloc. */
    for (size_t i = 0; i < NOTIFY_TIMER_TABLE_SIZE; i++) {
        if (!mNotifyTimerTable[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == NOTIFY_TIMER_TABLE_SIZE) {
        axl_warning("notify-timer table full (%d slots) - "
                    "increase NOTIFY_TIMER_TABLE_SIZE if you hit this",
                    NOTIFY_TIMER_TABLE_SIZE);
        return AXL_ERR;
    }

    nc = (NotifyTimerCtx *)axl_malloc(sizeof(NotifyTimerCtx));
    if (nc == NULL) {
        return AXL_ERR;
    }
    nc->notify = notify;
    nc->ctx    = ctx;

    /* TPL_CALLBACK is the lowest TPL legal for an EVT_NOTIFY_SIGNAL
       event per UEFI 2.11 §7.1 (TPL_APPLICATION rejects with
       EFI_INVALID_PARAMETER — there is no signal queue at the main-
       thread level). TPL_CALLBACK alternates fairly with co-located
       firmware drivers' notifies (TCP4/MNP/SNP run at the same
       level) so a fast consumer notify doesn't starve them. The
       caller's notify MUST be short — see axl_loop_attach_driver
       doxygen and src/loop/README.md. */
    status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              notify_timer_trampoline, nc, &ev);
    if (EFI_ERROR(status)) {
        axl_free(nc);
        return AXL_ERR;
    }

    status = gBS->SetTimer(ev, TimerPeriodic, interval_100ns);
    if (EFI_ERROR(status)) {
        gBS->CloseEvent(ev);
        axl_free(nc);
        return AXL_ERR;
    }

    mNotifyTimerTable[slot].handle = ev;
    mNotifyTimerTable[slot].ctx    = nc;
    mNotifyTimerTable[slot].active = true;

    event_close_ring_record_create((void *)ev);
    *event = (AxlEventHandle)ev;
    return AXL_OK;
}

/* Look up and release a notify-timer's tracking slot. Returns true
   if the handle was a tracked notify-timer (caller has already
   closed the EFI event); the close path in event_close_dbg uses
   this to free the bridging context. */
static bool
notify_timer_release(EFI_EVENT handle)
{
    if (handle == NULL) {
        return false;
    }
    for (size_t i = 0; i < NOTIFY_TIMER_TABLE_SIZE; i++) {
        if (mNotifyTimerTable[i].active &&
            mNotifyTimerTable[i].handle == handle) {
            axl_free(mNotifyTimerTable[i].ctx);
            mNotifyTimerTable[i].handle = NULL;
            mNotifyTimerTable[i].ctx    = NULL;
            mNotifyTimerTable[i].active = false;
            return true;
        }
    }
    return false;
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

    /* If this was a notify-timer created by
       axl_backend_event_create_notify_timer, cancel the timer first
       so no NEW notifies queue, then CloseEvent (which the UEFI
       spec requires to drain any in-flight notify before
       returning), then free the bridging context. The cancel +
       close + free ordering ensures the trampoline never reads
       freed `nc` memory: by the time we hit notify_timer_release,
       CloseEvent has guaranteed no notify is mid-execution. */
    bool is_notify_timer = false;
    for (size_t i = 0; i < NOTIFY_TIMER_TABLE_SIZE; i++) {
        if (mNotifyTimerTable[i].active &&
            mNotifyTimerTable[i].handle == (EFI_EVENT)event) {
            is_notify_timer = true;
            gBS->SetTimer((EFI_EVENT)event, TimerCancel, 0);
            break;
        }
    }

    gBS->CloseEvent((EFI_EVENT)event);

    if (is_notify_timer) {
        notify_timer_release((EFI_EVENT)event);
    }
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

/* Spin cadence for the raised-TPL CheckEvent fallback in event_wait.
   Short enough to keep latency low for an event the firmware signals
   directly (a network tx/rx completion), while still yielding the CPU
   between passes. */
#define EVENT_WAIT_RAISED_TPL_SPIN_US  200ULL

bool
axl_backend_at_raised_tpl(void)
{
    /* Canonical read-current-TPL idiom: raise to the ceiling (always legal
       from any TPL) and immediately restore — RaiseTPL returns the entry
       level. gBS->WaitForEvent returns EFI_UNSUPPORTED above
       TPL_APPLICATION, so event_wait uses this to pick its CheckEvent
       fallback, and the sync TCP wrappers use it to install a Poll() tick
       only when one is actually needed. */
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(tpl);
    return tpl > TPL_APPLICATION;
}

uintptr_t
axl_backend_enter_critical(void)
{
    /* TPL_NOTIFY sits above TPL_CALLBACK (the driver-pump dispatch level) and
       above TPL_APPLICATION (a foreground console writer), so raising here makes
       a short buffer update atomic against both. AllocatePool is still legal at
       TPL_NOTIFY, so a guarded append that grows its buffer stays within the
       rules. RaiseTPL returns the entry level for a strict LIFO restore. */
    return (uintptr_t)gBS->RaiseTPL(TPL_NOTIFY);
}

void
axl_backend_leave_critical(uintptr_t token)
{
    gBS->RestoreTPL((EFI_TPL)token);
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

    /* gBS->WaitForEvent is unavailable above TPL_APPLICATION (it returns
       EFI_UNSUPPORTED) — the case for any nested wait reached from a
       driver-pump notify dispatched at TPL_CALLBACK. A caller looping on a
       plain AXL_ERR there would spin forever and hard-wedge. Fall back to a
       non-blocking CheckEvent sweep with a brief Stall between passes:
       timer/tick events still signal (the timer DPC runs at TPL_HIGH_LEVEL,
       preempting the spin), so a waiter's protocol-Poll() tick keeps
       advancing and the awaited completion still lands. At
       TPL_APPLICATION the code below runs a plain WaitForEvent. */
    if (axl_backend_at_raised_tpl()) {
        for (;;) {
            for (size_t i = 0; i < count; i++) {
                int rc = axl_backend_event_check(events[i]);
                if (rc == 0) {
                    *fired_index = i;
                    return AXL_OK;
                }
                /* CheckEvent error (bad/closed handle) — mirror WaitForEvent,
                   which aborts on a bad event in the set rather than looping. */
                if (rc < 0) {
                    return AXL_ERR;
                }
            }
            axl_backend_stall(EVENT_WAIT_RAISED_TPL_SPIN_US);
        }
    }

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

int
axl_backend_install_protocol(
    void           **handle,
    const void      *guid,
    void            *iface
    )
{
    /* InstallProtocolInterface allocates a fresh handle when *handle is
       NULL and writes it back — the in/out contract maps directly. */
    EFI_STATUS status = gBS->InstallProtocolInterface(
        (EFI_HANDLE *)handle,
        (EFI_GUID *)guid,
        EFI_NATIVE_INTERFACE,
        iface);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}

int
axl_backend_uninstall_protocol(
    void            *handle,
    const void      *guid,
    void            *iface
    )
{
    EFI_STATUS status = gBS->UninstallProtocolInterface(
        (EFI_HANDLE)handle,
        (EFI_GUID *)guid,
        iface);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}
