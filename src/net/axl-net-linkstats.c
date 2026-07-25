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

    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK || nnic == 0) {
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }
    if (nic >= nnic) {
        /* Out of range errors -- it does NOT clamp to NIC 0. */
        _axl_net_nics_free(nics);
        return AXL_ERR;
    }

    /* Link state is authoritative; speed/duplex/autoneg have no portable
       SimpleNetwork source and stay 0 (see the header note). */
    out->link_up = nics[nic].link_up;
    _axl_net_nics_free(nics);
    return AXL_OK;
}
