/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-backend-native-mp.c
    Native UEFI backend — MP Services protocol wrapper.

    Direct calls into EFI_MP_SERVICES_PROTOCOL. Owns the
    AxlMpContext struct (BSP/AP enumeration cache + the
    `ap_numbers` index→processor-number table) so axl-backend-native.c
    doesn't carry the typedef.

    Split out of axl-backend-native.c per docs/Style-Cleanup-Plan.md
    Pass C — MP services have their own data structures (AxlMpContext)
    and are conceptually independent of the rest of the backend.
**/

#include "axl-backend.h"
#include <axl/axl-cpu.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("backend");

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

/* Timeout handed to the blocking-dispatch fallback, microseconds.

   The AP is launched before the firmware enters its wait loop, so this
   does not gate the worker starting -- it only bounds how long the BSP
   is parked before the firmware hands control back with EFI_TIMEOUT.
   AxlTaskPool runs its own readiness handshake, so the smallest sane
   value wins: ArmPsciMpServicesDxe stalls min(timeout, POLL_INTERVAL_US)
   per iteration, making this the exact per-AP cost of the fallback. */
#define MP_BLOCKING_DISPATCH_TIMEOUT_US  1000u

/* Bound on waiting for the firmware to retire a stopped worker before
   its completion event may be closed. MpInitLib signals from a periodic
   timer (PcdCpuApStatusCheckIntervalInMicroSeconds, 100 ms by default),
   so allow several intervals. Drained concurrently, so this bounds
   teardown as a whole, not per AP. */
#define MP_EVENT_DRAIN_TIMEOUT_US        1000000u
#define MP_EVENT_DRAIN_STEP_US           10000u

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct AxlMpContext {
    EFI_MP_SERVICES_PROTOCOL  *mp;
    UINTN                     *ap_numbers;  ///< maps index to processor number
    EFI_EVENT                 *ap_events;   ///< per-AP completion event, or NULL
    size_t                     count;
};

// ---------------------------------------------------------------------------
// Function prototypes (static)
// ---------------------------------------------------------------------------

static void mp_release_events(EFI_EVENT *events, size_t count);

// ---------------------------------------------------------------------------
// AxlBackend public surface — MP services
// ---------------------------------------------------------------------------

AxlMpContext *
axl_backend_mp_init(
    size_t  *worker_count
    )
{
    EFI_STATUS                  status;
    EFI_MP_SERVICES_PROTOCOL   *mp;
    UINTN                       num_proc;
    UINTN                       num_enabled;
    UINTN                       bsp_number;
    UINTN                       i;
    size_t                      slot;
    AxlMpContext               *ctx;
    EFI_GUID                    mp_guid = gEfiMpServicesProtocolGuid;

    if (worker_count != NULL) {
        *worker_count = 0;
    }

    status = gBS->LocateProtocol(&mp_guid, NULL, (void **)&mp);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    status = mp->GetNumberOfProcessors(mp, &num_proc, &num_enabled);
    if (EFI_ERROR(status) || num_enabled <= 1) {
        return NULL;
    }

    status = mp->WhoAmI(mp, &bsp_number);
    if (EFI_ERROR(status)) {
        return NULL;
    }

    ctx = axl_backend_alloc_zero(sizeof(AxlMpContext));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->mp = mp;
    ctx->ap_numbers = axl_backend_alloc_zero(
                          (num_enabled - 1) * sizeof(UINTN));
    if (ctx->ap_numbers == NULL) {
        axl_backend_free(ctx);
        return NULL;
    }

    ctx->ap_events = axl_backend_alloc_zero(
                         (num_enabled - 1) * sizeof(EFI_EVENT));
    if (ctx->ap_events == NULL) {
        axl_backend_free(ctx->ap_numbers);
        axl_backend_free(ctx);
        return NULL;
    }

    /* Enumerate enabled APs (skip BSP) */
    slot = 0;
    for (i = 0; i < num_proc && slot < num_enabled - 1; i++) {
        EFI_PROCESSOR_INFORMATION  proc_info;

        if (i == bsp_number) {
            continue;
        }

        status = mp->GetProcessorInfo(mp, i, &proc_info);
        if (EFI_ERROR(status) ||
            !(proc_info.StatusFlag & PROCESSOR_ENABLED_BIT)) {
            continue;
        }

        ctx->ap_numbers[slot] = i;
        slot++;
    }

    ctx->count = slot;
    if (ctx->count == 0) {
        axl_backend_free(ctx->ap_events);
        axl_backend_free(ctx->ap_numbers);
        axl_backend_free(ctx);
        return NULL;
    }

    if (worker_count != NULL) {
        *worker_count = ctx->count;
    }
    return ctx;
}

int
axl_backend_mp_start_ap(
    AxlMpContext  *ctx,
    size_t         ap_index,
    AxlApProc      proc,
    void          *arg
    )
{
    EFI_STATUS  status;
    EFI_EVENT   ap_event;

    if (ctx == NULL || ap_index >= ctx->count || proc == NULL) {
        return AXL_ERR;
    }

    /* From here a second core may execute AXL code, so the SIMD-tier memo
       -- which is only sound while the BSP is the only runner -- has to be
       retired BEFORE the AP can observe it. Retiring after the dispatch
       would leave a window in which the AP reads the BSP's verdict, which
       on a hybrid part is a #UD rather than a wrong number. */
    axl_cpu_simd_memo_invalidate();

    /* Preferred shape: non-blocking with an infinite timeout. A worker
       that never returns can never "complete", so blocking mode would
       never return either -- and MpInitLib's blocking path calls
       ResetProcessorToIdleState on a straggler, which would kill the
       worker outright. */
    status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &ap_event);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    status = ctx->mp->StartupThisAP(
                 ctx->mp,
                 (EFI_AP_PROCEDURE)proc,
                 ctx->ap_numbers[ap_index],
                 ap_event,
                 0,
                 arg,
                 NULL);

    if (!EFI_ERROR(status)) {
        /* The firmware retains this handle until it observes the worker
           return, and signals it then. It therefore has to outlive the
           dispatch: closing it here leaves MpInitLib signalling freed
           pool memory once the worker retires. mp_cleanup closes it,
           after the signal has landed. */
        ctx->ap_events[ap_index] = ap_event;
        return AXL_OK;
    }

    gBS->CloseEvent(ap_event);

    if (status != EFI_UNSUPPORTED) {
        return AXL_ERR;
    }

    /* Non-blocking refused. ArmPsciMpServicesDxe answers EFI_UNSUPPORTED
       to every non-blocking request once EFI_EVENT_GROUP_READY_TO_BOOT
       has been signalled -- which, for anything launched from the Shell,
       is always. Blocking with a short timeout is the way through: PSCI
       cannot preempt a running AP, so the firmware simply hands control
       back and leaves the worker running.

       Keyed off the firmware's own answer rather than an #ifdef, so any
       platform that refuses non-blocking gets the same treatment and x86
       keeps exercising the branch above. */
    status = ctx->mp->StartupThisAP(
                 ctx->mp,
                 (EFI_AP_PROCEDURE)proc,
                 ctx->ap_numbers[ap_index],
                 NULL,
                 MP_BLOCKING_DISPATCH_TIMEOUT_US,
                 arg,
                 NULL);

    /* EFI_TIMEOUT is the success case here: the worker is running and
       did not return. EFI_SUCCESS means it DID return, so it is not a
       usable persistent worker and the caller must not count it. */
    if (status == EFI_TIMEOUT) {
        return AXL_OK;
    }

    if (!EFI_ERROR(status)) {
        axl_warning("AP #%llu returned from its worker procedure at "
                    "dispatch; not a usable persistent worker",
                    (unsigned long long)ctx->ap_numbers[ap_index]);
    }
    return AXL_ERR;
}

size_t
axl_backend_mp_get_ap_number(
    AxlMpContext  *ctx,
    size_t         ap_index
    )
{
    if (ctx == NULL || ap_index >= ctx->count) {
        return 0;
    }
    return (size_t)ctx->ap_numbers[ap_index];
}

/* Close the completion events of workers that have already stopped.

   The caller must have stopped the workers first. Even then the firmware
   still holds each handle until its periodic AP-status check notices the
   worker returned and signals it (MpInitLib: CheckAndUpdateApsStatus,
   driven by a timer at PcdCpuApStatusCheckIntervalInMicroSeconds, 100 ms
   by default). Closing before that point frees the event out from under
   the firmware, which then signals freed pool memory -- the very bug the
   retained handle exists to avoid. So wait for the signal, then close.

   All events are drained concurrently, so the bound covers teardown as a
   whole rather than accumulating per AP.

   An event that is never signalled is deliberately LEAKED rather than
   closed: a leaked handle costs one firmware pool allocation for the life
   of the image, whereas a premature close is a use-after-free inside the
   firmware. Neither is good; only one is dangerous. */
static
void
mp_release_events(
    EFI_EVENT  *events,
    size_t      count
    )
{
    EFI_STATUS  status;
    uint64_t    waited;
    size_t      remaining;
    size_t      i;

    remaining = 0;
    for (i = 0; i < count; i++) {
        if (events[i] != NULL) {
            remaining++;
        }
    }

    for (waited = 0; remaining > 0 && waited < MP_EVENT_DRAIN_TIMEOUT_US;
         waited += MP_EVENT_DRAIN_STEP_US) {
        for (i = 0; i < count; i++) {
            if (events[i] == NULL) {
                continue;
            }
            status = gBS->CheckEvent(events[i]);
            if (status == EFI_NOT_READY) {
                continue;          /* firmware has not retired it yet */
            }
            if (status == EFI_SUCCESS) {
                gBS->CloseEvent(events[i]);
            }
            /* Any other status means the handle is not ours to close. */
            events[i] = NULL;
            remaining--;
        }
        if (remaining > 0) {
            axl_backend_stall(MP_EVENT_DRAIN_STEP_US);
        }
    }

    if (remaining > 0) {
        axl_warning("%llu AP completion event(s) never signalled; leaking "
                    "the handles rather than freeing them under the firmware",
                    (unsigned long long)remaining);
    }
}

void
axl_backend_mp_cleanup(
    AxlMpContext  *ctx
    )
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->ap_events != NULL) {
        mp_release_events(ctx->ap_events, ctx->count);
        axl_backend_free(ctx->ap_events);
    }

    axl_backend_free(ctx->ap_numbers);
    axl_backend_free(ctx);
}
