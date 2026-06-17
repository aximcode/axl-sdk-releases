/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-arp.c
    axl_net_arp_list — read the firmware ARP (IPv4 neighbor) cache via
    EFI_ARP_PROTOCOL.
**/

#include "axl-net-internal.h"
#include "../backend/axl-backend.h"
#include <axl/axl-net.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("net");

int
axl_net_arp_list(size_t nic, AxlArpEntry *out, size_t cap, size_t *count)
{
    if (count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    EFI_HANDLE *handles = NULL;
    size_t      hc      = 0;
    EFI_STATUS  st      = axl_efi_call(axl_bs()->LocateHandleBuffer, 5,
                                       ByProtocol,
                                       &gEfiArpServiceBindingProtocolGuid,
                                       NULL, &hc, &handles);
    if (EFI_ERROR(st) || hc == 0 || handles == NULL) {
        return AXL_ERR;
    }
    if (nic >= hc) {
        axl_backend_free(handles);
        return AXL_ERR;
    }

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    st = axl_efi_call(axl_bs()->HandleProtocol, 3, handles[nic],
                      &gEfiArpServiceBindingProtocolGuid, (void **)&sb);
    axl_backend_free(handles);
    if (EFI_ERROR(st) || sb == NULL) {
        return AXL_ERR;
    }

    EFI_HANDLE child = NULL;
    st = axl_efi_call(sb->CreateChild, 2, sb, &child);
    if (EFI_ERROR(st)) {
        return AXL_ERR;
    }

    EFI_ARP_PROTOCOL *arp = NULL;
    st = axl_efi_call(axl_bs()->HandleProtocol, 3, child,
                      &gEfiArpProtocolGuid, (void **)&arp);
    if (EFI_ERROR(st) || arp == NULL) {
        axl_efi_call(sb->DestroyChild, 2, sb, child);
        return AXL_ERR;
    }

    /* Configure for IPv4 so the instance attaches to the interface's ARP
       service (whose resolved cache it shares). Station address = our IP. */
    AxlIPv4Address station = { { 0, 0, 0, 0 } };
    axl_net_get_ip_address(&station);
    EFI_ARP_CONFIG_DATA cfg;
    axl_memset(&cfg, 0, sizeof(cfg));
    cfg.SwAddressType   = 0x0800;   /* IPv4 */
    cfg.SwAddressLength = 4;
    cfg.StationAddress  = station.addr;
    st = axl_efi_call(arp->Configure, 2, arp, &cfg);
    if (EFI_ERROR(st)) {
        axl_efi_call(sb->DestroyChild, 2, sb, child);
        return AXL_ERR;
    }

    /* Find all resolved entries (BySwAddress=FALSE, AddressBuffer=NULL,
       Refresh=FALSE). EFI_NOT_FOUND = empty cache = success with count 0. */
    UINT32             entry_len   = 0;
    UINT32             entry_count = 0;
    EFI_ARP_FIND_DATA *entries     = NULL;
    int                rc          = AXL_OK;
    st = axl_efi_call(arp->Find, 7, arp, (BOOLEAN)0, (void *)NULL,
                      &entry_len, &entry_count, &entries, (BOOLEAN)0);

    if (!EFI_ERROR(st) && entries != NULL && entry_len > 0) {
        uint8_t *base = (uint8_t *)entries;
        for (UINT32 i = 0; i < entry_count; i++) {
            EFI_ARP_FIND_DATA *fd =
                (EFI_ARP_FIND_DATA *)(base + (size_t)i * entry_len);
            if (fd->HwAddressLength != 6 || fd->SwAddressLength != 4) {
                continue;   /* only Ethernet/IPv4 */
            }
            uint8_t *hw = (uint8_t *)fd + sizeof(EFI_ARP_FIND_DATA);
            uint8_t *sw = hw + fd->HwAddressLength;
            if (out != NULL && *count < cap) {
                axl_memcpy(out[*count].ip, sw, 4);
                axl_memcpy(out[*count].mac, hw, 6);
            }
            (*count)++;
        }
        axl_backend_free(entries);
    } else if (EFI_ERROR(st) && st != EFI_NOT_FOUND) {
        rc = AXL_ERR;
    }

    axl_efi_call(arp->Configure, 2, arp, (EFI_ARP_CONFIG_DATA *)NULL);  /* reset */
    axl_efi_call(sb->DestroyChild, 2, sb, child);
    return rc;
}
