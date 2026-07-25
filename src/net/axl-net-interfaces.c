/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-interfaces.c
    Network interface enumeration and IPv4 query:
    axl_net_get_ip_address, axl_net_is_available, axl_net_list_interfaces.
    Also hosts axl_net_locate_sb — the per-NIC service-binding picker
    shared by TCP and UDP for source-IP / subnet-match selection.
**/

#include "../backend/axl-backend.h"
#include "axl-net-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>

AXL_LOG_DOMAIN("net");

// ---------------------------------------------------------------------------
// axl_net_locate_sb — per-NIC service-binding picker shared by TCP +
// UDP open / listen / _via paths. See header doc for the priority
// ladder; impl walks all handles publishing @p sb_guid, queries each
// underlying interface's IP4Config2 InterfaceInfo for StationAddress
// + SubnetMask, picks the best candidate.
// ---------------------------------------------------------------------------

/// Try to read InterfaceInfo from the IP4Config2 protocol on @p handle.
/// On success copies StationAddress + SubnetMask into the outputs.
static EFI_STATUS
get_iface_info(
    EFI_HANDLE         handle,
    EFI_IPv4_ADDRESS  *station_out,
    EFI_IPv4_ADDRESS  *mask_out
    )
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    EFI_STATUS status = axl_efi_call(axl_bs()->HandleProtocol, 3,
        handle, &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
    if (EFI_ERROR(status) || cfg == NULL) {
        return EFI_NOT_FOUND;
    }

    /* Two-phase: ask for size, alloc, then read. The InterfaceInfo
       buffer carries an OPTIONAL trailing route table whose size we
       don't know up front. */
    UINTN data_size = 0;
    status = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &data_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL ||
        data_size < sizeof(EFI_IP4_CONFIG2_INTERFACE_INFO))
    {
        return EFI_NOT_FOUND;
    }
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_malloc(data_size);
    if (info == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    status = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &data_size, info);
    if (EFI_ERROR(status)) {
        axl_free(info);
        return status;
    }
    *station_out = info->StationAddress;
    *mask_out    = info->SubnetMask;
    axl_free(info);
    return EFI_SUCCESS;
}

EFI_STATUS
axl_net_locate_sb(
    const EFI_GUID                 *sb_guid,
    const EFI_IPv4_ADDRESS         *dest,
    const EFI_IPv4_ADDRESS         *forced_source,
    EFI_SERVICE_BINDING_PROTOCOL  **sb,
    EFI_HANDLE                     *out_handle
    )
{
    EFI_STATUS  status;
    EFI_HANDLE *handles      = NULL;
    size_t      handle_count = 0;

    if (sb_guid == NULL || sb == NULL || out_handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    (EFI_GUID *)sb_guid,   /* drop const: callee only reads it */
                    NULL,
                    &handle_count,
                    &handles
                    );
    if (EFI_ERROR(status) || handle_count == 0) {
        return EFI_NOT_FOUND;
    }

    EFI_HANDLE chosen      = NULL;
    int        chosen_rank = 0;

    for (size_t i = 0; i < handle_count; i++) {
        EFI_IPv4_ADDRESS station = { 0 };
        EFI_IPv4_ADDRESS mask    = { 0 };
        if (get_iface_info(handles[i], &station, &mask) != EFI_SUCCESS) {
            continue;
        }

        if (forced_source != NULL) {
            if (axl_ipv4_equals(station.Addr, forced_source->Addr)) {
                chosen = handles[i];
                break;
            }
            continue;
        }

        /* Auto mode: skip 0.0.0.0 entirely. */
        static const uint8_t zero4[4] = { 0, 0, 0, 0 };
        if (axl_ipv4_equals(station.Addr, zero4)) {
            continue;
        }

        if (dest != NULL &&
            axl_ipv4_in_subnet(dest->Addr, station.Addr, mask.Addr))
        {
            chosen = handles[i];
            break;  /* subnet-match is the strongest auto signal */
        }

        if (chosen_rank < 1) {
            chosen      = handles[i];
            chosen_rank = 1;
        }
    }

    if (chosen == NULL) {
        axl_backend_free(handles);
        return EFI_NOT_FOUND;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    chosen,
                    (EFI_GUID *)sb_guid,   /* drop const: callee only reads it */
                    (void **)sb
                    );
    if (!EFI_ERROR(status)) {
        *out_handle = chosen;
    }

    axl_backend_free(handles);
    return status;
}

// ---------------------------------------------------------------------------
// axl_net_get_ip_address
// ---------------------------------------------------------------------------

int
axl_net_get_ip_address(AxlIPv4Address *addr)
{
    EFI_STATUS                      status;
    EFI_HANDLE                     *handles;
    size_t                          handle_count;
    size_t                          i;
    EFI_IP4_CONFIG2_PROTOCOL       *ip4_config2;
    EFI_IP4_CONFIG2_INTERFACE_INFO *if_info;
    size_t                          data_size;

    if (addr == NULL) {
        return AXL_ERR;
    }

    axl_memset(addr, 0, sizeof(*addr));

    //
    // Find handles with IP4Config2 protocol
    //
    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiIp4Config2ProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles);
    if (EFI_ERROR(status)) {
        axl_debug("no IP4Config2 protocol found");
        return AXL_ERR;
    }

    //
    // Try each handle until we find a configured NIC
    //
    for (i = 0; i < handle_count; i++) {
        status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                        handles[i],
                        &gEfiIp4Config2ProtocolGuid,
                        (void **)&ip4_config2);
        if (EFI_ERROR(status)) {
            continue;
        }

        //
        // Query interface info size
        //
        data_size = 0;
        status = axl_efi_call(ip4_config2->GetData, 4,
                               ip4_config2,
                               Ip4Config2DataTypeInterfaceInfo,
                               &data_size,
                               NULL);
        if (status != EFI_BUFFER_TOO_SMALL || data_size == 0) {
            continue;
        }

        if_info = axl_backend_alloc(data_size);
        if (if_info == NULL) {
            continue;
        }

        status = axl_efi_call(ip4_config2->GetData, 4,
                               ip4_config2,
                               Ip4Config2DataTypeInterfaceInfo,
                               &data_size,
                               if_info);
        if (EFI_ERROR(status)) {
            axl_backend_free(if_info);
            continue;
        }

        //
        // Skip unconfigured NICs (0.0.0.0)
        //
        EFI_IPv4_ADDRESS *station_addr = &if_info->StationAddress;
        if (station_addr->Addr[0] == 0 && station_addr->Addr[1] == 0 &&
            station_addr->Addr[2] == 0 && station_addr->Addr[3] == 0)
        {
            axl_backend_free(if_info);
            continue;
        }

        axl_memcpy(addr, station_addr, sizeof(EFI_IPv4_ADDRESS));
        axl_backend_free(if_info);
        axl_backend_free(handles);
        return AXL_OK;
    }

    axl_backend_free(handles);

    /* No IP4Config2-configured NIC. On firmware that lacks IP4Config2, a
       Dhcp4-SB / PXE bring-up caches its lease — surface that address here so
       this reader works on IP4Config2-free firmware too. */
    AxlDhcpLease fb;
    if (_axl_net_fallback_lease(&fb)) {
        axl_memcpy(addr, fb.address, sizeof(EFI_IPv4_ADDRESS));
        return AXL_OK;
    }

    /* Quiet by design — this function is polled by axl_net_auto_init
       while waiting for DHCP, so a warning here would spam every second.
       Callers that care log their own message on final timeout. */
    axl_debug("no configured NIC found");
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// axl_net_is_available
// ---------------------------------------------------------------------------

bool
axl_net_is_available(void)
{
    AxlIPv4Address addr;

    if (axl_net_get_ip_address(&addr) == AXL_OK) {
        return true;
    }

    /* Fallback: some firmware (e.g., AAVMF/AARCH64) lacks IP4Config2
       but still has a working TCP4 stack. Check for TCP4ServiceBinding. */
    EFI_STATUS  status;
    EFI_HANDLE *handles;
    size_t      handle_count;

    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiTcp4ServiceBindingProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles);
    if (!EFI_ERROR(status) && handle_count > 0) {
        axl_backend_free(handles);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// axl_net_list_interfaces
// ---------------------------------------------------------------------------

int
axl_net_list_interfaces(AxlNetInterface *out, size_t *count)
{
    if (count == NULL) {
        return AXL_ERR;
    }
    size_t capacity = (out != NULL) ? *count : 0;

    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK) {
        *count = 0;
        return AXL_ERR;
    }

    if (out == NULL) {
        *count = nnic;   /* count query: physical NICs, not SNP child handles */
        _axl_net_nics_free(nics);
        return AXL_OK;
    }

    size_t filled = 0;
    for (size_t i = 0; i < nnic && filled < capacity; i++) {
        AxlNetInterface *iface = &out[filled];
        axl_memset(iface, 0, sizeof(*iface));
        axl_strlcpy(iface->name, nics[i].name, sizeof(iface->name));
        axl_memcpy(iface->mac, nics[i].mac, 6);
        iface->link_up  = nics[i].link_up;
        iface->mtu      = nics[i].mtu;
        iface->has_ipv4 = nics[i].has_ipv4;
        axl_memcpy(iface->ipv4,    nics[i].ipv4,    4);
        axl_memcpy(iface->netmask, nics[i].netmask, 4);
        axl_memcpy(iface->gateway, nics[i].gateway, 4);
        filled++;
    }
    _axl_net_nics_free(nics);
    *count = filled;
    return AXL_OK;
}

int
axl_net_list_interfaces_alloc(AxlNetInterface **out, size_t *count)
{
    if (out == NULL || count == NULL) {
        return AXL_ERR;
    }
    *out = NULL;
    *count = 0;

    size_t n = 0;
    if (axl_net_list_interfaces(NULL, &n) != AXL_OK) {
        return AXL_ERR;
    }
    if (n == 0) {
        return AXL_OK;   /* no NICs present -- not a failure */
    }

    AxlNetInterface *ifs = axl_calloc(n, sizeof *ifs);
    if (ifs == NULL) {
        return AXL_ERR;
    }
    if (axl_net_list_interfaces(ifs, &n) != AXL_OK) {
        axl_free(ifs);
        return AXL_ERR;
    }
    *out = ifs;
    *count = n;
    return AXL_OK;
}
