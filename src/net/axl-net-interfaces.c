/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-interfaces.c
    Network interface enumeration and IPv4 query:
    axl_net_get_ip_address, axl_net_is_available, axl_net_list_interfaces.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>

AXL_LOG_DOMAIN("net");

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
        return -1;
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
        return -1;
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
        return 0;
    }

    axl_backend_free(handles);
    /* Quiet by design — this function is polled by axl_net_auto_init
       while waiting for DHCP, so a warning here would spam every second.
       Callers that care log their own message on final timeout. */
    axl_debug("no configured NIC found");
    return -1;
}

// ---------------------------------------------------------------------------
// axl_net_is_available
// ---------------------------------------------------------------------------

bool
axl_net_is_available(void)
{
    AxlIPv4Address addr;

    if (axl_net_get_ip_address(&addr) == 0) {
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
    EFI_STATUS   status;
    EFI_HANDLE  *handles = NULL;
    size_t       num_handles = 0;
    size_t       capacity;
    size_t       filled = 0;

    if (count == NULL) {
        return -1;
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
        return 0;
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

        /* Try to get IPv4 config from IP4Config2 on the same handle */
        EFI_IP4_CONFIG2_PROTOCOL *ip4cfg = NULL;
        status = axl_efi_call(axl_bs()->HandleProtocol, 3,
            handles[i],
            &EFI_IP4_CONFIG2_PROTOCOL_GUID,
            (void **)&ip4cfg);

        if (!EFI_ERROR(status) && ip4cfg != NULL) {
            EFI_IP4_CONFIG2_INTERFACE_INFO info;
            size_t info_size = sizeof(info);

            status = axl_efi_call(ip4cfg->GetData, 4,
                ip4cfg,
                Ip4Config2DataTypeInterfaceInfo,
                &info_size,
                &info);

            if (!EFI_ERROR(status)) {
                /* Check if IP is nonzero */
                if (info.StationAddress.Addr[0] != 0 ||
                    info.StationAddress.Addr[1] != 0 ||
                    info.StationAddress.Addr[2] != 0 ||
                    info.StationAddress.Addr[3] != 0)
                {
                    iface->has_ipv4 = true;
                    axl_memcpy(iface->ipv4, &info.StationAddress, 4);
                    axl_memcpy(iface->netmask, &info.SubnetMask, 4);
                }
            }

            /* Try to get gateway via policy data */
            EFI_IPv4_ADDRESS gw;
            size_t gw_size = sizeof(gw);
            status = axl_efi_call(ip4cfg->GetData, 4,
                ip4cfg,
                Ip4Config2DataTypeGateway,
                &gw_size,
                &gw);

            if (!EFI_ERROR(status) && iface->has_ipv4) {
                axl_memcpy(iface->gateway, &gw, 4);
            }
        }

        filled++;
    }

    axl_backend_free(handles);

    if (out == NULL) {
        *count = (size_t)num_handles;
    } else {
        *count = filled;
    }

    return 0;
}
