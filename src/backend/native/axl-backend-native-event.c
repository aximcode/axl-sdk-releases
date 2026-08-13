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

/* Shared by axl_loop_attach_driver's dispatch timers AND by every live
   AxlTaskPool's before-ExitBootServices hook. Exhaustion is not just a
   lost timer: a pool that cannot register its hook silently loses its
   boot-hang protection on exactly the platforms that need it. */
#define NOTIFY_TIMER_TABLE_SIZE  64

/* Tracks every notify-bridged event, not only timers: the event-group
   registration below shares the bridging context and the close path.
   `is_timer` keeps close from issuing SetTimer(TimerCancel) against an
   event that has no timer to cancel. */
typedef struct {
    EFI_EVENT       handle;
    NotifyTimerCtx *ctx;
    bool            is_timer;
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

/* Reserve a tracking slot and build the bridging context.

   Split out because the two creators below differ only in which
   CreateEvent flavour they call: everything around it -- reserving the
   slot, bridging the firmware's (EFI_EVENT, void *) notify signature to
   AXL's (void *ctx) shape, publishing into the table, and recording the
   create in the double-close ring -- is identical, and a second copy is
   a second place for the close path to go wrong.

   The slot is reserved but NOT marked active, so a caller whose event
   creation fails just calls notify_bridge_abort.

   @return the bridging context, or NULL (table full / out of memory). */
static NotifyTimerCtx *
notify_bridge_reserve(
    void  (*notify)(void *ctx),
    void   *ctx,
    size_t *slot_out
    )
{
    NotifyTimerCtx *nc;
    size_t          slot = NOTIFY_TIMER_TABLE_SIZE;

    for (size_t i = 0; i < NOTIFY_TIMER_TABLE_SIZE; i++) {
        if (!mNotifyTimerTable[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == NOTIFY_TIMER_TABLE_SIZE) {
        axl_warning("notify table full (%d slots) - "
                    "increase NOTIFY_TIMER_TABLE_SIZE if you hit this",
                    NOTIFY_TIMER_TABLE_SIZE);
        return NULL;
    }

    nc = (NotifyTimerCtx *)axl_malloc(sizeof(NotifyTimerCtx));
    if (nc == NULL) {
        return NULL;
    }
    nc->notify = notify;
    nc->ctx    = ctx;
    *slot_out  = slot;
    return nc;
}

/* Discard a reservation whose event creation failed. The slot was never
   marked active, so only the context needs undoing. */
static void
notify_bridge_abort(
    NotifyTimerCtx *nc
    )
{
    axl_free(nc);
}

/* Publish a created event into its reserved slot. */
static void
notify_bridge_publish(
    size_t           slot,
    EFI_EVENT        ev,
    NotifyTimerCtx  *nc,
    bool             is_timer,
    AxlEventHandle  *event
    )
{
    mNotifyTimerTable[slot].handle   = ev;
    mNotifyTimerTable[slot].ctx      = nc;
    mNotifyTimerTable[slot].is_timer = is_timer;
    mNotifyTimerTable[slot].active   = true;

    event_close_ring_record_create((void *)ev);
    *event = (AxlEventHandle)ev;
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
    NotifyTimerCtx  *nc;
    size_t           slot;

    if (notify == NULL || event == NULL || interval_100ns == 0) {
        return AXL_ERR;
    }

    nc = notify_bridge_reserve(notify, ctx, &slot);
    if (nc == NULL) {
        return AXL_ERR;
    }

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
        notify_bridge_abort(nc);
        return AXL_ERR;
    }

    status = gBS->SetTimer(ev, TimerPeriodic, interval_100ns);
    if (EFI_ERROR(status)) {
        gBS->CloseEvent(ev);
        notify_bridge_abort(nc);
        return AXL_ERR;
    }

    notify_bridge_publish(slot, ev, nc, true, event);
    return AXL_OK;
}

int
axl_backend_event_create_before_exit_boot(
    void (*notify)(void *ctx),
    void  *ctx,
    AxlEventHandle *event
    )
{
    EFI_STATUS       status;
    EFI_EVENT        ev = NULL;
    NotifyTimerCtx  *nc;
    size_t           slot;
    EFI_GUID         group = gEfiEventBeforeExitBootServicesGuid;

    if (notify == NULL || event == NULL) {
        return AXL_ERR;
    }

    nc = notify_bridge_reserve(notify, ctx, &slot);
    if (nc == NULL) {
        return AXL_ERR;
    }

    /* TPL_CALLBACK, not TPL_NOTIFY. There is nothing to outrank: the
       firmware's own ExitBootServices-group handlers are signalled after
       this whole group regardless of TPL, which is the entire reason for
       using the before-EBS group. Raising to TPL_NOTIFY would buy nothing
       and cost I/O legality -- a handler that logs reaches the FAT driver,
       whose lock asserts TPL <= TPL_CALLBACK, so the handler written to
       prevent a hang at handoff could hang there itself. */
    status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                notify_timer_trampoline, nc, &group, &ev);
    if (EFI_ERROR(status)) {
        notify_bridge_abort(nc);
        return AXL_ERR;
    }

    notify_bridge_publish(slot, ev, nc, false, event);
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
            if (mNotifyTimerTable[i].is_timer) {
                gBS->SetTimer((EFI_EVENT)event, TimerCancel, 0);
            }
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

uintptr_t
axl_backend_tpl_current(void)
{
    /* Canonical read-current-TPL idiom: raise to the ceiling (always legal
       from any TPL) and immediately restore — RaiseTPL returns the entry
       level. UEFI offers no direct read, so this pair IS the accessor.
       Safe at TPL_HIGH_LEVEL: the raise is a no-op there and RestoreTPL
       back to the same level touches neither the allocator nor the event
       queue, which are the two things that hang at that level. */
    EFI_TPL tpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(tpl);
    return (uintptr_t)tpl;
}

bool
axl_backend_at_raised_tpl(void)
{
    /* gBS->WaitForEvent returns EFI_UNSUPPORTED above TPL_APPLICATION, so
       event_wait uses this to pick its CheckEvent fallback, and the sync
       TCP wrappers use it to install a Poll() tick only when one is
       actually needed. */
    return axl_backend_tpl_current() > TPL_APPLICATION;
}

uintptr_t
axl_backend_tpl_raise(uintptr_t level)
{
    /* RaiseTPL rejects a request BELOW the current level (it is a raise,
       not a set), so clamp: a caller asking for less than it already has
       gets the current level back and its paired restore stays a no-op.
       Without this the pair would try to RESTORE UPWARD, which the
       firmware treats as fatal misuse rather than an error return.

       Read and raise inside ONE raise-to-ceiling window rather than
       calling axl_backend_tpl_current() first. The obvious spelling —
       read the level, then raise — costs two RestoreTPL transitions per
       entry instead of one, and RestoreTPL is where the firmware
       dispatches queued event notifications. axl_backend_enter_critical
       runs on the console emit path, so handing it an extra dispatch
       point (and the re-entrancy that comes with one) for every short
       critical section is not a cost worth paying for a clamp. */
    EFI_TPL prev   = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    EFI_TPL target = ((EFI_TPL)level > prev) ? (EFI_TPL)level : prev;

    gBS->RestoreTPL(target);
    return (uintptr_t)prev;
}

void
axl_backend_tpl_restore(uintptr_t level)
{
    gBS->RestoreTPL((EFI_TPL)level);
}

uintptr_t
axl_backend_enter_critical(void)
{
    /* TPL_NOTIFY sits above TPL_CALLBACK (the driver-pump dispatch level) and
       above TPL_APPLICATION (a foreground console writer), so raising here makes
       a short buffer update atomic against both. AllocatePool is still legal at
       TPL_NOTIFY, so a guarded append that grows its buffer stays within the
       rules.

       Via axl_backend_tpl_raise, not gBS->RaiseTPL directly: a caller
       already above TPL_NOTIFY would otherwise ask the firmware to raise
       DOWNWARD, which is fatal misuse rather than an error return. The
       clamp lives in one place so both entry points get it. */
    return axl_backend_tpl_raise(TPL_NOTIFY);
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
