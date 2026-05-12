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
                    sb_guid,
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
                    sb_guid,
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

/*
 * Populate iface->ipv4/netmask/gateway/has_ipv4 from any IP4Config2
 * instance whose SNP (or IP4 service-binding parent) MAC matches the
 * caller's mac[6]. some OEM UEFI implementations bind IP4Config2 to a child handle
 * separate from the bare-NIC SNP handle, so we have to enumerate
 * IP4Config2 handles independently and correlate by MAC rather than
 * looking it up on the SNP handle directly.
 *
 * EDK2 shell `ifconfig` does the same enumeration. Pre-fix behavior
 * was to call HandleProtocol(IP4Config2) on each SNP handle, which
 * returned NOT_FOUND for that style of binding → IPv4 column showed "-"
 * for DHCP-bound NICs.
 */
static void
populate_ipv4_for_mac(
    AxlNetInterface *iface
    )
{
    EFI_STATUS   status;
    EFI_HANDLE  *handles = NULL;
    size_t       num_handles = 0;

    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol,
        &EFI_IP4_CONFIG2_PROTOCOL_GUID,
        NULL,
        &num_handles,
        &handles);
    if (EFI_ERROR(status) || num_handles == 0) {
        return;
    }

    for (size_t i = 0; i < num_handles; i++) {
        /* Match by MAC: SNP on the same handle (or inherited via the
         * binding chain) carries the controller's MAC. If no SNP is
         * reachable from this handle, we can't safely correlate and
         * skip the entry. */
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        status = axl_efi_call(axl_bs()->HandleProtocol, 3,
            handles[i],
            &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
            (void **)&snp);
        if (EFI_ERROR(status) || snp == NULL || snp->Mode == NULL) {
            continue;
        }

        size_t mac_len = snp->Mode->HwAddressSize;
        if (mac_len > 6) mac_len = 6;
        if (axl_memcmp(iface->mac, &snp->Mode->CurrentAddress, mac_len) != 0) {
            continue;
        }

        EFI_IP4_CONFIG2_PROTOCOL *ip4cfg = NULL;
        status = axl_efi_call(axl_bs()->HandleProtocol, 3,
            handles[i],
            &EFI_IP4_CONFIG2_PROTOCOL_GUID,
            (void **)&ip4cfg);
        if (EFI_ERROR(status) || ip4cfg == NULL) {
            continue;
        }

        /* Two-call pattern: first GetData with NULL buf returns
         * EFI_BUFFER_TOO_SMALL with the required size. */
        size_t info_size = 0;
        status = axl_efi_call(ip4cfg->GetData, 4,
            ip4cfg, Ip4Config2DataTypeInterfaceInfo, &info_size, NULL);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            continue;
        }
        EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_backend_alloc(info_size);
        if (info == NULL) {
            continue;
        }
        status = axl_efi_call(ip4cfg->GetData, 4,
            ip4cfg, Ip4Config2DataTypeInterfaceInfo, &info_size, info);
        if (EFI_ERROR(status)) {
            axl_backend_free(info);
            continue;
        }
        EFI_IPv4_ADDRESS *sa = &info->StationAddress;
        if (sa->Addr[0] != 0 || sa->Addr[1] != 0 ||
            sa->Addr[2] != 0 || sa->Addr[3] != 0)
        {
            iface->has_ipv4 = true;
            axl_memcpy(iface->ipv4,    &info->StationAddress, 4);
            axl_memcpy(iface->netmask, &info->SubnetMask,     4);
        }
        axl_backend_free(info);

        if (iface->has_ipv4) {
            EFI_IPv4_ADDRESS gw;
            size_t gw_size = sizeof(gw);
            status = axl_efi_call(ip4cfg->GetData, 4,
                ip4cfg, Ip4Config2DataTypeGateway, &gw_size, &gw);
            if (!EFI_ERROR(status)) {
                axl_memcpy(iface->gateway, &gw, 4);
            }
            break;     /* one IP per NIC is enough for the listing */
        }
    }
    axl_backend_free(handles);
}

int
axl_net_list_interfaces(AxlNetInterface *out, size_t *count)
{
    EFI_STATUS   status;
    EFI_HANDLE  *handles = NULL;
    size_t       num_handles = 0;
    size_t       capacity;
    size_t       filled = 0;

    if (count == NULL) {
        return AXL_ERR;
    }

    capacity = (out != NULL) ? *count : 0;

    /* Find all handles with SimpleNetwork protocol */
    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
        ByProtocol,
        &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
        NULL,
        &num_handles,
        &handles);

    if (EFI_ERROR(status) || num_handles == 0) {
        *count = 0;
        return AXL_OK;
    }

    for (size_t i = 0; i < num_handles && filled < capacity; i++) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;

        status = axl_efi_call(axl_bs()->HandleProtocol, 3,
            handles[i],
            &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
            (void **)&snp);

        if (EFI_ERROR(status) || snp == NULL || snp->Mode == NULL) {
            continue;
        }

        AxlNetInterface *iface = &out[filled];
        axl_memset(iface, 0, sizeof(*iface));

        /* Name: eth0, eth1, ... */
        axl_snprintf(iface->name, sizeof(iface->name), "eth%zu", filled);

        /* MAC address (first 6 bytes) */
        size_t mac_len = snp->Mode->HwAddressSize;
        if (mac_len > 6) {
            mac_len = 6;
        }
        axl_memcpy(iface->mac, &snp->Mode->CurrentAddress, mac_len);

        /* Link status and MTU from SNP mode */
        iface->link_up = snp->Mode->MediaPresent ? true : false;
        iface->mtu = snp->Mode->MaxPacketSize;

        /* IP4Config2 lives on a child handle on some OEM firmware; can't
         * just HandleProtocol on this SNP handle. Walk all
         * IP4Config2 instances and match by MAC. */
        populate_ipv4_for_mac(iface);

        filled++;
    }

    axl_backend_free(handles);

    if (out == NULL) {
        *count = (size_t)num_handles;
    } else {
        *count = filled;
    }

    return AXL_OK;
}
