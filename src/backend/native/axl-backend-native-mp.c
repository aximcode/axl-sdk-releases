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
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("backend");

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct AxlMpContext {
    EFI_MP_SERVICES_PROTOCOL  *mp;
    UINTN                     *ap_numbers;  ///< maps index to processor number
    size_t                     count;
};

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

    /* Create temp event for non-blocking StartupThisAP */
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

    gBS->CloseEvent(ap_event);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
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

void
axl_backend_mp_cleanup(
    AxlMpContext  *ctx
    )
{
    if (ctx == NULL) {
        return;
    }
    axl_backend_free(ctx->ap_numbers);
    axl_backend_free(ctx);
}
