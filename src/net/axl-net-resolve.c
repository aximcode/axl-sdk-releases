/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-resolve.c
    axl_net_resolve — DNS4 hostname lookup with IPv4 literal fallback.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>

#include "axl-net-internal.h"

AXL_LOG_DOMAIN("net");

/* ascii_str_to_ucs2 — ASCII to UCS-2 conversion */
static inline EFI_STATUS
ascii_str_to_ucs2(const char *src, unsigned short *dst, size_t dst_max)
{
    size_t i;
    for (i = 0; src[i] != '\0' && i + 1 < dst_max; i++) {
        dst[i] = (unsigned short)(uint8_t)src[i];
    }
    dst[i] = 0;
    return EFI_SUCCESS;
}

int
axl_net_resolve(const char *hostname, AxlIPv4Address *addr)
{
    EFI_STATUS                    status;
    EFI_HANDLE                   *handles;
    size_t                        handle_count;
    EFI_SERVICE_BINDING_PROTOCOL *dns_sb;
    EFI_HANDLE                    dns_child;
    EFI_DNS4_PROTOCOL            *dns4;
    EFI_DNS4_CONFIG_DATA          dns_config;
    EFI_DNS4_COMPLETION_TOKEN     dns_token;
    EFI_EVENT                     dns_event;
    EFI_IPv4_ADDRESS              dns_server;

    if (hostname == NULL || addr == NULL) {
        return AXL_ERR;
    }

    //
    // Try to parse as IP address first
    //
    status = net_parse_ip_address(hostname, (EFI_IPv4_ADDRESS *)addr);
    if (!EFI_ERROR(status)) {
        return AXL_OK;
    }

    //
    // Locate DNS4 service binding
    //
    status = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                    ByProtocol,
                    &gEfiDns4ServiceBindingProtocolGuid,
                    NULL,
                    &handle_count,
                    &handles);
    if (EFI_ERROR(status) || handle_count == 0) {
        axl_warning("no DNS4 service binding — cannot resolve '%s'", hostname);
        return AXL_ERR;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    handles[0],
                    &gEfiDns4ServiceBindingProtocolGuid,
                    (void **)&dns_sb);
    axl_backend_free(handles);
    if (EFI_ERROR(status)) {
        return AXL_ERR;
    }

    //
    // Create DNS4 child
    //
    dns_child = NULL;
    status = axl_efi_call(dns_sb->CreateChild, 2, dns_sb, &dns_child);
    if (EFI_ERROR(status)) {
        axl_error("DNS4 CreateChild: %llx", (unsigned long long)status);
        return AXL_ERR;
    }

    status = axl_efi_call(axl_bs()->HandleProtocol, 3,
                    dns_child,
                    &gEfiDns4ProtocolGuid,
                    (void **)&dns4);
    if (EFI_ERROR(status)) {
        axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
        return AXL_ERR;
    }

    //
    // Configure DNS4
    //
    axl_memset(&dns_config, 0, sizeof(dns_config));
    dns_config.DnsServerListCount = 0;
    dns_config.DnsServerList      = NULL;
    dns_config.UseDefaultSetting  = true;
    dns_config.RetryInterval      = 3;
    dns_config.RetryCount         = 2;

    //
    // Try with default settings first; if that fails, use 8.8.8.8
    //
    status = axl_efi_call(dns4->Configure, 2, dns4, &dns_config);
    if (EFI_ERROR(status)) {
        axl_memset(&dns_server, 0, sizeof(dns_server));
        dns_server.Addr[0] = 8;
        dns_server.Addr[1] = 8;
        dns_server.Addr[2] = 8;
        dns_server.Addr[3] = 8;

        dns_config.UseDefaultSetting  = false;
        dns_config.DnsServerListCount = 1;
        dns_config.DnsServerList      = &dns_server;
        dns_config.StationIp.Addr[0]  = 0;
        dns_config.Protocol           = 17;  // UDP

        status = axl_efi_call(dns4->Configure, 2, dns4, &dns_config);
        if (EFI_ERROR(status)) {
            axl_error("DNS4 Configure: %llx", (unsigned long long)status);
            axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
            return AXL_ERR;
        }
    }

    //
    // Create completion event
    //
    dns_event = NULL;
    if (axl_backend_event_create((AxlEventHandle *)&dns_event) != AXL_OK) {
        axl_efi_call(dns4->Configure, 2, dns4, NULL);
        axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
        return AXL_ERR;
    }

    //
    // Resolve hostname
    //
    axl_memset(&dns_token, 0, sizeof(dns_token));
    dns_token.Event  = dns_event;
    dns_token.Status = EFI_ABORTED;

    //
    // Convert char hostname to unsigned short for DNS API
    //
    size_t host_len = axl_strlen(hostname);
    unsigned short *host_w = axl_backend_alloc((host_len + 1) * sizeof(unsigned short));
    if (host_w == NULL) {
        axl_backend_event_close((AxlEventHandle)dns_event);
        axl_efi_call(dns4->Configure, 2, dns4, NULL);
        axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
        return AXL_ERR;
    }

    ascii_str_to_ucs2(hostname, host_w, host_len + 1);

    status = axl_efi_call(dns4->HostNameToIp, 3, dns4, host_w, &dns_token);
    axl_backend_free(host_w);

    if (EFI_ERROR(status)) {
        axl_error("DNS4 HostNameToIp: %llx", (unsigned long long)status);
        axl_backend_event_close((AxlEventHandle)dns_event);
        axl_efi_call(dns4->Configure, 2, dns4, NULL);
        axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
        return AXL_ERR;
    }

    //
    // Wait for resolution (5 second timeout)
    //
    bool dns_completed = (_axl_dns_wait(dns4, dns_event, 5000000) == 0);

    //
    // Cancel pending operation on timeout before touching token data,
    // then give it a brief window to settle.
    //
    if (!dns_completed) {
        axl_efi_call(dns4->Cancel, 2, dns4, &dns_token);
        (void)_axl_dns_wait(dns4, dns_event, 100 * 1000);
    }

    status = dns_token.Status;
    if (!EFI_ERROR(status) &&
        dns_token.RspData.H2AData != NULL &&
        dns_token.RspData.H2AData->IpCount > 0 &&
        dns_token.RspData.H2AData->IpList != NULL)
    {
        axl_memcpy(addr, &dns_token.RspData.H2AData->IpList[0], sizeof(EFI_IPv4_ADDRESS));
        status = EFI_SUCCESS;
    } else if (!EFI_ERROR(status)) {
        status = EFI_NOT_FOUND;
    }

    //
    // Cleanup
    //
    if (dns_token.RspData.H2AData != NULL) {
        if (dns_token.RspData.H2AData->IpList != NULL) {
            axl_backend_free(dns_token.RspData.H2AData->IpList);
        }

        axl_backend_free(dns_token.RspData.H2AData);
    }

    axl_backend_event_close((AxlEventHandle)dns_event);
    axl_efi_call(dns4->Configure, 2, dns4, NULL);
    axl_efi_call(dns_sb->DestroyChild, 2, dns_sb, dns_child);
    return EFI_ERROR(status) ? AXL_ERR : AXL_OK;
}
