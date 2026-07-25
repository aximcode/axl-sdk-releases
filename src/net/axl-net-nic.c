/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-nic.c
    The canonical per-physical-NIC registry. SimpleNetwork child handles
    deduped by MAC, each NIC correlated to its IP4Config2 handle by MAC, in
    stable firmware-enumeration order. See axl-net-internal.h for why.
**/

#include "../backend/axl-backend.h"
#include "axl-net-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>

AXL_LOG_DOMAIN("net");

/* See axl-net-internal.h for why this guard exists and why it's shared. */
bool
_axl_net_snp_mac(EFI_SIMPLE_NETWORK_PROTOCOL *snp, uint8_t out_mac[6])
{
    if (snp == NULL || snp->Mode == NULL || snp->Mode->HwAddressSize < 6) {
        return false;
    }
    axl_memcpy(out_mac, &snp->Mode->CurrentAddress, 6);
    return true;
}

EFI_SIMPLE_NETWORK_PROTOCOL *
_axl_net_snp_on(EFI_HANDLE h)
{
    EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, h,
        &gEfiSimpleNetworkProtocolGuid, (void **)&snp);
    return snp;
}

/* Fill nic->has_ipv4/ipv4/netmask/gateway from its already-resolved
   ip4cfg_handle. No-op when the NIC has no IP4Config2. */
static void
fill_ipv4(AxlNic *nic)
{
    if (nic->ip4cfg_handle == NULL) {
        return;
    }
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, nic->ip4cfg_handle,
        &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
    if (cfg == NULL) {
        return;
    }

    /* Two-phase: the InterfaceInfo buffer carries an optional trailing route
       table whose size we don't know up front. */
    size_t info_size = 0;
    EFI_STATUS st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL
        || info_size < sizeof(EFI_IP4_CONFIG2_INTERFACE_INFO)) {
        return;
    }
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_malloc(info_size);
    if (info == NULL) {
        return;
    }
    st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, info);
    if (EFI_ERROR(st)) {
        axl_free(info);
        return;
    }
    EFI_IPv4_ADDRESS *sa = &info->StationAddress;
    if (sa->Addr[0] != 0 || sa->Addr[1] != 0
        || sa->Addr[2] != 0 || sa->Addr[3] != 0) {
        nic->has_ipv4 = true;
        axl_memcpy(nic->ipv4,    &info->StationAddress, 4);
        axl_memcpy(nic->netmask, &info->SubnetMask,     4);
    }
    axl_free(info);

    if (nic->has_ipv4) {
        EFI_IPv4_ADDRESS gw;
        size_t gw_size = sizeof(gw);
        st = axl_efi_call(cfg->GetData, 4, cfg,
            Ip4Config2DataTypeGateway, &gw_size, &gw);
        if (!EFI_ERROR(st)) {
            axl_memcpy(nic->gateway, &gw, 4);
        }
    }
}

void
_axl_net_nics_free(AxlNic *nics)
{
    axl_free(nics);
}

int
_axl_net_nics_build(AxlNic **out, size_t *count)
{
    if (out == NULL || count == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    *count = 0;

    EFI_HANDLE *snp_handles = NULL;
    size_t      nsnp = 0;
    EFI_STATUS  st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &nsnp, &snp_handles);
    if (EFI_ERROR(st) || nsnp == 0 || snp_handles == NULL) {
        return AXL_OK;   /* no NICs is success with count 0 */
    }

    /* IP4Config2 handles fetched ONCE here, not per NIC. The pre-registry code
       re-enumerated them inside a per-NIC loop, so a 5-NIC bring-up did ~10
       full sweeps. */
    EFI_HANDLE *cfg_handles = NULL;
    size_t      ncfg = 0;
    st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL, &ncfg, &cfg_handles);
    if (EFI_ERROR(st)) {
        cfg_handles = NULL;
        ncfg = 0;
    }

    AxlNic *nics = axl_calloc(nsnp, sizeof *nics);   /* nsnp is the upper bound */
    if (nics == NULL) {
        axl_backend_free(snp_handles);
        if (cfg_handles != NULL) {
            axl_backend_free(cfg_handles);
        }
        return AXL_ERR;
    }

    size_t nnic = 0;
    for (size_t i = 0; i < nsnp; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = _axl_net_snp_on(snp_handles[i]);
        uint8_t mac[6];
        if (!_axl_net_snp_mac(snp, mac)) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < nnic; j++) {
            if (axl_memcmp(nics[j].mac, mac, 6) == 0) {
                dup = true;   /* first handle for this MAC wins */
                break;
            }
        }
        if (dup) {
            continue;
        }

        AxlNic *nic = &nics[nnic];
        axl_memcpy(nic->mac, mac, 6);
        axl_snprintf(nic->name, sizeof(nic->name), "eth%zu", nnic);
        /* A NIC whose firmware doesn't implement media detection reports
           MediaPresent=FALSE meaninglessly; treating that as down would hide
           a working NIC. One rule, registry-wide. */
        nic->link_up = (!snp->Mode->MediaPresentSupported)
                       || snp->Mode->MediaPresent;
        nic->mtu = snp->Mode->MaxPacketSize;
        nic->snp_handle = snp_handles[i];

        /* Correlate to IP4Config2 by MAC: it lives on a child handle separate
           from the bare-NIC SNP handle on some OEM firmware, so we can't just
           HandleProtocol the SNP handle. */
        for (size_t k = 0; k < ncfg; k++) {
            uint8_t cfg_mac[6];
            if (!_axl_net_snp_mac(_axl_net_snp_on(cfg_handles[k]), cfg_mac)) {
                continue;
            }
            if (axl_memcmp(cfg_mac, mac, 6) == 0) {
                nic->ip4cfg_handle = cfg_handles[k];
                break;
            }
        }
        fill_ipv4(nic);
        nnic++;
    }

    axl_backend_free(snp_handles);
    if (cfg_handles != NULL) {
        axl_backend_free(cfg_handles);
    }

    if (nnic == 0) {
        axl_free(nics);
        return AXL_OK;
    }
    *out = nics;
    *count = nnic;
    return AXL_OK;
}

size_t
_axl_net_nic_resolve_index(const AxlNic *nics, size_t count, size_t nic_index)
{
    if (nics == NULL || count == 0) {
        return count;
    }

    if (nic_index == (size_t)AXL_NET_NIC_AUTO) {
        /* Prefer link-up AND configurable; else the first configurable. The
           old AUTO was a side effect of the clamp this replaces, and it
           happily picked a link-down NIC that would never lease. */
        size_t first_cfg = count;
        size_t idx = count;
        for (size_t i = 0; i < count; i++) {
            if (nics[i].ip4cfg_handle == NULL) {
                continue;
            }
            if (first_cfg == count) {
                first_cfg = i;
            }
            if (nics[i].link_up) {
                idx = i;
                break;
            }
        }
        if (idx == count) {
            idx = first_cfg;
        }
        if (idx == count) {
            /* No NIC correlates to an IP4Config2. nics[0] is the placeholder
               ordinal the handle-resolution fallback in
               _axl_net_nic_resolve_ip4cfg tries next -- see its doc comment
               for why that positional guess is safe only there (count == 1
               && exactly one IP4Config2 handle). count >= 1 is guaranteed by
               the guard above, so nics[0] always exists. */
            axl_debug("nic auto-select: no NIC correlates to an IP4Config2 - "
                      "trying the single-NIC positional fallback");
            idx = 0;
        }
        return idx;
    }

    if (nic_index >= count) {
        /* NOT a clamp to 0 -- that clamp is what sent DHCP to the wrong NIC. */
        axl_warning("nic index %zu out of range (%zu NIC%s)",
                    nic_index, count, (count == 1) ? "" : "s");
        return count;
    }
    return nic_index;
}

EFI_IP4_CONFIG2_PROTOCOL *
_axl_net_nic_resolve_ip4cfg(const AxlNic *nics, size_t count, size_t nic_index)
{
    size_t idx = _axl_net_nic_resolve_index(nics, count, nic_index);
    if (idx >= count) {
        return NULL;
    }

    EFI_HANDLE handle = nics[idx].ip4cfg_handle;
    if (handle == NULL) {
        /* Uncorrelatable MAC (firmware where SNP isn't reachable from the
           IP4Config2 handle). Guess positionally ONLY when there is exactly
           one NIC and exactly one IP4Config2 handle -- the only configuration
           where the guess cannot be wrong. One IP4Config2 handle can coexist
           with several NICs (only one has the IP4 stack bound), and there we
           still can't tell which NIC it serves. */
        EFI_HANDLE *cfg_handles = NULL;
        size_t      ncfg = 0;
        EFI_STATUS  st = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
            ByProtocol, &gEfiIp4Config2ProtocolGuid, NULL, &ncfg, &cfg_handles);
        if (EFI_ERROR(st) || cfg_handles == NULL) {
            return NULL;
        }
        if (count == 1 && ncfg == 1) {
            handle = cfg_handles[0];
        } else {
            char macbuf[18];
            axl_mac_format(nics[idx].mac, macbuf, sizeof macbuf);
            axl_warning("nic %zu (%s): no IP4Config2 "
                        "correlates and a positional guess is unsafe "
                        "(%zu NICs, %zu IP4Config2)",
                        idx, macbuf, count, ncfg);
        }
        axl_backend_free(cfg_handles);
        if (handle == NULL) {
            return NULL;
        }
    }

    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    axl_efi_call(axl_bs()->HandleProtocol, 3, handle,
        &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
    return cfg;
}
