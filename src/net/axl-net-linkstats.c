/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-linkstats.c
    axl_net_get_link_stats — physical-link state via EFI_SIMPLE_NETWORK_PROTOCOL.

    UEFI has no portable link-speed surface, so this reports the media/link
    state (the one reliably-available field) and leaves speed/duplex/autoneg
    at "unknown". See the header note.
**/

#include "axl-net-internal.h"
#include "../backend/axl-backend.h"
#include <axl/axl-net.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("net");

int
axl_net_get_link_stats(size_t nic, AxlNetLinkStats *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    EFI_HANDLE *handles = NULL;
    size_t      hc      = 0;
    EFI_STATUS  st      = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                                       ByProtocol,
                                       &gEfiSimpleNetworkProtocolGuid,
                                       NULL, &hc, &handles);
    if (EFI_ERROR(st) || hc == 0 || handles == NULL) {
        return AXL_ERR;
    }
    if (nic >= hc) {
        axl_backend_free(handles);
        return AXL_ERR;
    }

    EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
    st = axl_efi_call(axl_bs()->HandleProtocol, 3, handles[nic],
                      &gEfiSimpleNetworkProtocolGuid, (void **)&snp);
    axl_backend_free(handles);
    if (EFI_ERROR(st) || snp == NULL || snp->Mode == NULL) {
        return AXL_ERR;
    }

    /* Link state is authoritative; speed/duplex/autoneg have no portable
       SimpleNetwork source and stay 0 (see the header note). */
    out->link_up = (!snp->Mode->MediaPresentSupported) || snp->Mode->MediaPresent;
    return AXL_OK;
}
