/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-dhcp.c
    Static IP configuration and DHCP auto-init:
    axl_net_set_static_ip, axl_net_auto_init.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-sys.h>
#include <axl/axl-driver.h>
#include <axl/axl-net.h>
#include <axl/axl-wait.h>

AXL_LOG_DOMAIN("net");

/* IP4Config2 types not in generated UEFI headers */
typedef struct {
    EFI_IPv4_ADDRESS  Address;
    EFI_IPv4_ADDRESS  SubnetMask;
} Ip4Config2ManualAddress;

// ---------------------------------------------------------------------------
// axl_net_set_static_ip
// ---------------------------------------------------------------------------

int
axl_net_set_static_ip(
    size_t         nic_index,
    const uint8_t  ip[4],
    const uint8_t  netmask[4],
    const uint8_t *gateway)
{
    void  **handles = NULL;
    size_t  count = 0;
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    EFI_STATUS status;

    if (ip == NULL || netmask == NULL) {
        return -1;
    }

    if (axl_service_enumerate("ip4-config2", &handles, &count) != 0
        || count == 0)
    {
        axl_warning("no IP4Config2 protocol found");
        return -1;
    }

    size_t idx = (nic_index < count) ? nic_index : 0;

    axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handles[idx],
        &gEfiIp4Config2ProtocolGuid,
        (void **)&cfg);

    axl_free(handles);

    if (cfg == NULL) {
        return -1;
    }

    /* Set policy to static (value 0) */
    uint32_t static_policy = 0;
    status = axl_efi_call(cfg->SetData, 4, cfg,
        Ip4Config2DataTypePolicy,
        (size_t)sizeof(static_policy), &static_policy);

    if (EFI_ERROR(status)) {
        axl_warning("failed to set static policy: %llx",
                   (unsigned long long)status);
        return -1;
    }

    /* Set manual address + subnet mask */
    Ip4Config2ManualAddress manual;
    axl_memset(&manual, 0, sizeof(manual));
    axl_memcpy(&manual.Address, ip, 4);
    axl_memcpy(&manual.SubnetMask, netmask, 4);

    status = axl_efi_call(cfg->SetData, 4, cfg,
        Ip4Config2DataTypeManualAddress,
        (size_t)sizeof(manual), &manual);

    if (EFI_ERROR(status)) {
        axl_warning("failed to set manual address: %llx",
                   (unsigned long long)status);
        return -1;
    }

    /* Set gateway if provided */
    if (gateway != NULL) {
        EFI_IPv4_ADDRESS gw;
        axl_memcpy(&gw, gateway, 4);

        status = axl_efi_call(cfg->SetData, 4, cfg,
            Ip4Config2DataTypeGateway,
            (size_t)sizeof(gw), &gw);

        if (EFI_ERROR(status)) {
            axl_warning("failed to set gateway: %llx",
                       (unsigned long long)status);
            /* Non-fatal — IP is configured, gateway is optional */
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// axl_net_ensure_drivers
// ---------------------------------------------------------------------------

/* Drivers we try to load, in order. NetworkCommon ships the MNP/IP4/TCP4
 * stack on top of any SNP-providing NIC driver. The remaining names cover
 * the NIC drivers staged by uefi-devkit. Each is silently skipped if the
 * .efi isn't found on any mounted volume — the cost of a miss is one file
 * existence check per candidate. */
static const char *const net_drivers[] = {
    "NetworkCommon.efi",
    "RtkUndiDxe.efi",
    "RtkUsbUndiDxe.efi",
    "AsixUsbUndiDxe.efi",
    "UsbCdcEcm.efi",
    "UsbCdcNcm.efi",
    "UsbRndis.efi",
    "ipxe-intel.efi",
    "ipxe-broadcom.efi",
};

static size_t
net_count_snp(void)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_service_enumerate("simple-network", &handles, &count) == 0) {
        axl_free(handles);
    }
    return count;
}

int
axl_net_ensure_drivers(void)
{
    /* Short-circuit if SNP is already registered. This makes the call
     * idempotent — safe for tools to invoke unconditionally before any
     * networking work. */
    if (net_count_snp() > 0) {
        return AXL_NET_DRIVERS_OK;
    }

    char path[256];
    size_t loaded_count = 0;

    for (size_t i = 0; i < sizeof(net_drivers) / sizeof(net_drivers[0]); i++) {
        const char *name = net_drivers[i];

        /* Locate first — distinguishes "not on volume" (skip silently)
         * from "found but failed to start" (warn). axl_driver_ensure is
         * the wrong primitive here: it unloads on protocol-not-registered,
         * but the chain SNP→MNP→IP→TCP requires multiple drivers loaded
         * before any single protocol becomes available. */
        if (axl_driver_locate(name, path, sizeof(path)) != 0) {
            axl_debug("ensure_drivers: %s not on any volume", name);
            continue;
        }

        AxlDriverHandle drv = NULL;
        if (axl_driver_load(path, &drv) != 0 || drv == NULL) {
            axl_warning("ensure_drivers: load failed for '%s'", path);
            continue;
        }

        /* Inline StartImage so EFI_ALREADY_STARTED counts as success — a
         * subsequent invocation finds the driver already loaded and just
         * re-runs ConnectController below. */
        size_t exit_data_size = 0;
        EFI_STATUS st = axl_bs()->StartImage(
            (EFI_HANDLE)drv, &exit_data_size, NULL);

        if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
            axl_warning("ensure_drivers: StartImage failed for '%s': 0x%llx",
                        path, (unsigned long long)st);
            axl_driver_unload(drv);
            continue;
        }

        axl_info("ensure_drivers: loaded '%s'", path);
        loaded_count++;
    }

    /* Wire up driver bindings globally. NIC drivers register
     * DRIVER_BINDING_PROTOCOL in their entry point but only bind to PCI/
     * USB controllers when ConnectController runs. NULL handle reconnects
     * everything. */
    axl_bs()->ConnectController(NULL, NULL, NULL, TRUE);

    if (net_count_snp() > 0) {
        return AXL_NET_DRIVERS_OK;
    }

    if (loaded_count == 0) {
        return AXL_NET_DRIVERS_NOT_FOUND;
    }

    return AXL_NET_DRIVERS_NO_LINK;
}

// ---------------------------------------------------------------------------
// axl_net_auto_init
// ---------------------------------------------------------------------------

static void
net_connect_snp_handles(void)
{
    /* Reconnect every SNP handle to make sure the MNP/IP/TCP/UDP stack
     * is bound on top of each NIC. axl_net_ensure_drivers() already ran
     * a global ConnectController, but explicit per-handle reconnect is
     * harmless and matches the prior behavior tools relied on. */
    void  **snp_handles = NULL;
    size_t  snp_count = 0;
    if (axl_service_enumerate("simple-network", &snp_handles, &snp_count) == 0) {
        for (size_t i = 0; i < snp_count; i++) {
            axl_driver_connect_handle(snp_handles[i]);
        }
        axl_free(snp_handles);
    }
}

int
axl_net_auto_init(size_t nic_index, size_t dhcp_timeout_sec)
{
    size_t timeout;
    size_t elapsed;

    AxlIPv4Address addr;

    timeout = (dhcp_timeout_sec > 0) ? dhcp_timeout_sec : 10;

    /*
     * Check if networking is already configured. We need a real IP, not
     * just a TCP4 service binding — axl_net_is_available() returns true
     * when TCP4 is present even before DHCP runs (PixieFail-era firmware
     * installs TCP4ServiceBinding at boot), so it would short-circuit
     * the DHCP path below and leave the caller with no IP. Use the
     * stricter get_ip_address() check that matches our contract.
     */
    if (axl_net_get_ip_address(&addr) == 0) {
        return 0;
    }

    /* Locate and load NIC drivers (and NetworkCommon) if SNP isn't up
     * yet. We don't care about the specific NOT_FOUND vs NO_LINK error
     * here — auto_init's contract is "DHCP succeeded or didn't"; the
     * diagnostic distinction matters to tools that call ensure_drivers
     * directly. */
    axl_net_ensure_drivers();
    net_connect_snp_handles();

    /* Wait for link-up before attempting DHCP (max 5 seconds) */
    {
        AxlNetInterface ifaces[4];
        size_t iface_count = 4;
        bool link_found = false;

        for (int attempt = 0; attempt < 50; attempt++) {
            iface_count = 4;
            if (axl_net_list_interfaces(ifaces, &iface_count) == 0) {
                for (size_t i = 0; i < iface_count; i++) {
                    if (ifaces[i].link_up) {
                        link_found = true;
                        break;
                    }
                }
            }
            if (link_found) {
                break;
            }
            axl_msleep(100);
        }
        if (!link_found) {
            axl_warning("no link detected on any NIC");
        }
    }

    /*
     * Set DHCP policy via IP4Config2. We find the right handle,
     * set policy to DHCP, then poll for an IP address.
     */
    void  **cfg_handles = NULL;
    size_t  cfg_count = 0;
    if (axl_service_enumerate("ip4-config2", &cfg_handles, &cfg_count) != 0
        || cfg_count == 0)
    {
        axl_warning("no IP4Config2 protocol found");
        return -1;
    }

    /* Select NIC */
    size_t idx = (nic_index < cfg_count) ? nic_index : 0;
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg = NULL;

    axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)cfg_handles[idx],
        &gEfiIp4Config2ProtocolGuid,
        (void **)&ip4cfg);

    axl_free(cfg_handles);

    if (ip4cfg == NULL) {
        return -1;
    }

    /* Set DHCP policy (value 1 = DHCP per UEFI spec) */
    uint32_t policy = 1;
    EFI_STATUS dhcp_status = axl_efi_call(ip4cfg->SetData, 4, ip4cfg,
        Ip4Config2DataTypePolicy,
        (size_t)sizeof(policy), &policy);

    if (EFI_ERROR(dhcp_status)) {
        axl_warning("failed to set DHCP policy: %llx",
                   (unsigned long long)dhcp_status);
    }

    /* Poll for IP assignment — strict get_ip_address() check, see above */
    for (elapsed = 0; elapsed < timeout; elapsed++) {
        if (axl_net_get_ip_address(&addr) == 0) {
            axl_info("network ready after %zu seconds", elapsed + 1);
            return 0;
        }
        if (axl_wait_ms(NULL, 1000) == AXL_CANCELLED) {
            break;  /* Ctrl-C */
        }
    }

    axl_warning("DHCP timeout after %zu seconds", timeout);
    return -1;
}
