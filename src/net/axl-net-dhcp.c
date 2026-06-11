/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-dhcp.c
    Static IP configuration and DHCP auto-init:
    axl_net_set_static_ip, axl_net_auto_init.
**/

#include "../backend/axl-backend.h"
#include "../event/axl-wait-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-sys.h>
#include <axl/axl-driver.h>
#include <axl/axl-net.h>
#include <axl/axl-wait.h>
#include <axl/axl-watchdog.h>

AXL_LOG_DOMAIN("net");

/* IP4Config2 types not in generated UEFI headers */
typedef struct {
    EFI_IPv4_ADDRESS  Address;
    EFI_IPv4_ADDRESS  SubnetMask;
} Ip4Config2ManualAddress;

// ---------------------------------------------------------------------------
// axl_net_bring_up — one-call DHCP-or-static bring-up + address read-back
// ---------------------------------------------------------------------------

#define AXL_NET_STATIC_IP_SETTLE_MS  500

int
axl_net_bring_up(
    size_t          nic_index,
    const uint8_t  *static_ipv4,
    const uint8_t  *netmask,
    const uint8_t  *gateway,
    size_t          timeout_sec,
    AxlIPv4Address *addr_out)
{
    if (static_ipv4 == NULL) {
        if (axl_net_auto_init(nic_index, timeout_sec) != AXL_OK) {
            return AXL_ERR;
        }
    } else {
        /* Static path: drivers_up gets us link without burning the
           DHCP timeout (which would fire if we routed through
           auto_init). Then set the static address. */
        if (axl_net_drivers_up() != AXL_OK) {
            return AXL_ERR;
        }
        static const uint8_t mask24[4] = { 255, 255, 255, 0 };
        const uint8_t *m = (netmask != NULL) ? netmask : mask24;
        if (axl_net_set_static_ip(nic_index, static_ipv4, m, gateway) != AXL_OK) {
            return AXL_ERR;
        }
        /* IP4Config2 applies the policy + address asynchronously; without
           a short settle, the next GetData can still report the prior
           state. 500 ms is the empirically-stable budget. */
        axl_msleep(AXL_NET_STATIC_IP_SETTLE_MS);
    }

    if (addr_out != NULL) {
        if (axl_net_get_ip_address(addr_out) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

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
        return AXL_ERR;
    }

    if (axl_protocol_enumerate("ip4-config2", &handles, &count) != AXL_OK
        || count == 0)
    {
        axl_warning("no IP4Config2 protocol found");
        return AXL_ERR;
    }

    size_t idx = (nic_index < count) ? nic_index : 0;

    axl_efi_call(axl_bs()->HandleProtocol, 3,
        (EFI_HANDLE)handles[idx],
        &gEfiIp4Config2ProtocolGuid,
        (void **)&cfg);

    axl_free(handles);

    if (cfg == NULL) {
        return AXL_ERR;
    }

    /* Set policy to static (value 0) */
    uint32_t static_policy = 0;
    status = axl_efi_call(cfg->SetData, 4, cfg,
        Ip4Config2DataTypePolicy,
        (size_t)sizeof(static_policy), &static_policy);

    if (EFI_ERROR(status)) {
        axl_warning("failed to set static policy: %llx",
                   (unsigned long long)status);
        return AXL_ERR;
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
        return AXL_ERR;
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

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_net_ensure_drivers
// ---------------------------------------------------------------------------

/* Drivers tried in two phases:
 *
 *   Phase 1 (core): non-iPXE class drivers covering the common cases —
 *   USB-CDC ECM/NCM/RNDIS for BMC virtual NICs and USB dongles, plus
 *   Realtek/Asix vendor UNDI drivers. ~20-200 KB each. Safe to load
 *   unconditionally — they only bind controllers they recognize.
 *
 *   Phase 2 (iPXE fallback): iPXE-derived drivers (~280-1100 KB each).
 *   Only loaded if Phase 1 didn't produce any SNP handles. Two reasons
 *   to gate iPXE this way (discovered 2026-05-04 on an AMD-EPYC server):
 *
 *   (a) iPXE's UEFI option-ROM appears to hook LoadImage in ways that
 *       break subsequent EFI image loads in the same shell session
 *       — observed as "Command Error Status: Load Error" on every
 *       fs0:\foo.efi attempt after iPXE has been resident, even on
 *       healthy hardware. Same shape as the original mkrd Load Error
 *       mystery, which also followed netinfo running iPXE.
 *
 *   (b) iPXE's efi_watchdog.c arms a 5-min boot-services watchdog
 *       continuously while it's loaded and only disarms when chaining
 *       to an OS. axl_net_ensure_drivers calls axl_watchdog_disarm()
 *       at the end as a safety net, but skipping iPXE entirely when
 *       it's not needed is cleaner.
 *
 *   Each name is silently skipped when the file isn't on any mounted
 *   volume — the cost of a miss is one file existence check per
 *   candidate. */
static const char *const net_drivers_core[] = {
    "NetworkCommon.efi",
    "UsbCdcEcm.efi",
    "UsbCdcNcm.efi",
    "UsbRndis.efi",
    "RtkUndiDxe.efi",
    "RtkUsbUndiDxe.efi",
    "AsixUsbUndiDxe.efi",
};

static const char *const net_drivers_ipxe[] = {
    "ipxe-all.efidrv",
    "ipxe-intel.efi",
    "ipxe-broadcom.efi",
};

static size_t
net_count_snp(void)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("simple-network", &handles, &count) == AXL_OK) {
        axl_free(handles);
    }
    return count;
}

/* Locate-and-load helper for a single driver-name list. Returns the
 * count of drivers actually loaded (excluding files not present on
 * any volume). Each name not on any volume is skipped silently; load
 * or StartImage failures emit a warning but don't abort the rest. */
static size_t
load_driver_list(
    const char *const *names,
    size_t             n
    )
{
    char path[256];
    size_t loaded = 0;
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];

        /* Locate first — distinguishes "not on volume" (skip silently)
         * from "found but failed to start" (warn). axl_driver_ensure is
         * the wrong primitive here: it unloads on protocol-not-registered,
         * but the chain SNP→MNP→IP→TCP requires multiple drivers loaded
         * before any single protocol becomes available. */
        if (axl_driver_locate(name, path, sizeof(path)) != AXL_OK) {
            axl_debug("ensure_drivers: %s not on any volume", name);
            continue;
        }

        AxlDriverHandle drv = NULL;
        if (axl_driver_load(path, &drv) != AXL_OK || drv == NULL) {
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
        loaded++;
    }
    return loaded;
}

int
axl_net_ensure_drivers(void)
{
    size_t snp_before = net_count_snp();
    axl_info("ensure_drivers: starting (%zu SNP handles already present)",
             snp_before);

    /* Short-circuit if SNP is already registered. This makes the call
     * idempotent — safe for tools to invoke unconditionally before any
     * networking work. */
    if (snp_before > 0) {
        return AXL_NET_DRIVERS_OK;
    }

    /* Phase 1: load core (non-iPXE) drivers + ConnectController.
     *
     * Wire up driver bindings globally. NIC drivers register
     * DRIVER_BINDING_PROTOCOL in their entry point but only bind to
     * PCI/USB controllers when ConnectController is called on those
     * controller handles. UEFI's ConnectController(ControllerHandle=
     * NULL,...) returns EFI_INVALID_PARAMETER per spec, so we go
     * through axl_driver_connect(NULL), which enumerates every
     * handle and per-handle reconnects (mirroring shell `connect -r`). */
    size_t loaded_count = load_driver_list(
        net_drivers_core,
        sizeof(net_drivers_core) / sizeof(net_drivers_core[0]));
    axl_driver_connect(NULL);

    size_t snp_after_core = net_count_snp();

    /* Phase 2 (iPXE fallback): only if core drivers didn't produce
     * any SNP handle. iPXE's UEFI driver (a) hooks LoadImage in ways
     * that break subsequent EFI image loads in the same shell session
     * and (b) arms a 5-min boot-services watchdog. Skipping it when
     * unnecessary avoids both side-effects. See net_drivers_ipxe[]
     * comment for the empirical evidence captured during development. */
    if (snp_after_core == 0) {
        axl_info(
            "ensure_drivers: no SNP after core drivers - falling back to iPXE");
        loaded_count += load_driver_list(
            net_drivers_ipxe,
            sizeof(net_drivers_ipxe) / sizeof(net_drivers_ipxe[0]));
        axl_driver_connect(NULL);
    } else {
        axl_info(
            "ensure_drivers: SNP came up via core drivers (%zu handles) - skipping iPXE",
            snp_after_core);
    }

    /* Disarm any boot-services watchdog the loaded drivers may have
     * armed. iPXE in particular installs a 10-second holdoff timer
     * that re-arms a 5-minute SetWatchdogTimer continuously while it
     * runs, and only disarms in its shutdown path when chaining to
     * an OS (booting==true). When axl-sdk loads iPXE just for SNP
     * binding, iPXE exits with the watchdog still armed → host
     * resets ~5 minutes later. Verified on an AMD-EPYC server 2026-05-04
     * (iPXE source: src/interface/efi/efi_watchdog.c). Disarming here
     * is unconditionally safe — no axl-sdk caller of this function is
     * relying on the watchdog being armed. */
    axl_watchdog_disarm();

    size_t snp_after = net_count_snp();
    axl_info("ensure_drivers: %zu drivers loaded, SNP handles %zu->%zu",
             loaded_count, snp_before, snp_after);

    if (snp_after > 0) {
        return AXL_NET_DRIVERS_OK;
    }

    if (loaded_count == 0) {
        return AXL_NET_DRIVERS_NOT_FOUND;
    }

    return AXL_NET_DRIVERS_NO_LINK;
}

// ---------------------------------------------------------------------------
// axl_net_drivers_up — load NIC drivers, connect SNP, wait for link
// (no DHCP, no IP assignment — that's auto_init / set_static_ip / bring_up's
// job). Used both internally by axl_net_auto_init and by axl_net_bring_up's
// static-IP path, where waiting 10s for a DHCP response is dead time.
// ---------------------------------------------------------------------------

#define AXL_NET_DRIVERS_UP_IFACE_MAX  4

/* axl_net_auto_init's DHCP-completion cond — re-checks GetData on every
   wakeup. Lives at file scope so axl_net_drivers_up's static link cond
   isn't the only example here. */
static bool
ip4cfg_addr_acquired(void *ctx)
{
    (void)ctx;
    AxlIPv4Address addr;
    return axl_net_get_ip_address(&addr) == AXL_OK;
}

static void
net_connect_snp_handles(void)
{
    /* Reconnect every SNP handle to make sure the MNP/IP/TCP/UDP stack
     * is bound on top of each NIC. axl_net_ensure_drivers() already ran
     * a global ConnectController, but explicit per-handle reconnect is
     * harmless and matches the prior behavior tools relied on. */
    void  **snp_handles = NULL;
    size_t  snp_count = 0;
    if (axl_protocol_enumerate("simple-network", &snp_handles, &snp_count) == AXL_OK) {
        for (size_t i = 0; i < snp_count; i++) {
            axl_driver_connect_handle(snp_handles[i]);
        }
        axl_free(snp_handles);
    }
}

/* axl_net_drivers_up's link-up cond — runs as the AxlWait condition
   function; returns true when at least one NIC reports link_up.
   No SNP-side notify event covers link-state portably across drivers,
   so the wait stays a 100ms-tick poll routed through the SDK's central
   AxlWait infrastructure (event-loop integrated, Ctrl-C cancellable). */
static bool
link_is_up(void *ctx)
{
    AxlNetInterface *ifaces = (AxlNetInterface *)ctx;
    size_t iface_count = AXL_NET_DRIVERS_UP_IFACE_MAX;
    if (axl_net_list_interfaces(ifaces, &iface_count) != AXL_OK) {
        return false;
    }
    for (size_t i = 0; i < iface_count; i++) {
        if (ifaces[i].link_up) {
            return true;
        }
    }
    return false;
}

int
axl_net_drivers_up(void)
{
    /* Locate and load NIC drivers (and NetworkCommon) if SNP isn't up
     * yet. We don't care about the specific NOT_FOUND vs NO_LINK error
     * here — drivers_up's contract is "did we get a link or not"; the
     * diagnostic distinction matters to tools that call ensure_drivers
     * directly. */
    axl_net_ensure_drivers();
    net_connect_snp_handles();

    /* Wait for link-up (max 5s, condition polled by AxlWait at 100ms).
       Fast-path: if a link is already up, AxlWait returns AXL_OK without
       creating a loop. */
    AxlNetInterface ifaces[AXL_NET_DRIVERS_UP_IFACE_MAX];
    AxlStatus rc = _axl_event_wait_timeout_with_tick(
        NULL,
        link_is_up, ifaces,
        NULL, NULL,
        100ULL * 1000ULL,        /* tick_us = 100 ms */
        NULL,
        5ULL * 1000ULL * 1000ULL); /* timeout_us = 5 s */
    if (rc != AXL_OK) {
        axl_warning("no link detected on any NIC");
        return AXL_ERR;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_net_auto_init
// ---------------------------------------------------------------------------

int
axl_net_auto_init(size_t nic_index, size_t dhcp_timeout_sec)
{
    size_t timeout;

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
    if (axl_net_get_ip_address(&addr) == AXL_OK) {
        return AXL_OK;
    }

    /* drivers_up returns AXL_ERR if no link came up — auto_init still
       continues (older callers rely on the DHCP poll being attempted
       even on no-link paths to surface a clearer error). */
    (void)axl_net_drivers_up();

    /*
     * Set DHCP policy via IP4Config2. We find the right handle,
     * set policy to DHCP, then poll for an IP address.
     */
    void  **cfg_handles = NULL;
    size_t  cfg_count = 0;
    if (axl_protocol_enumerate("ip4-config2", &cfg_handles, &cfg_count) != AXL_OK
        || cfg_count == 0)
    {
        axl_warning("no IP4Config2 protocol found");
        return AXL_ERR;
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
        return AXL_ERR;
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

    /* Wait for DHCP completion via IP4Config2 RegisterDataNotify on
       Ip4Config2DataTypeInterfaceInfo. The IP4Config2 driver fires the
       event from Ip4Config2OnDhcp4Complete (see EDK2
       NetworkPkg/Ip4Dxe/Ip4Config2Impl.c) at the moment the address is
       committed — sub-millisecond latency vs. the prior 1 Hz polling
       loop's worst-case 1 s of dead time per startup. The condition
       function still re-checks GetData on every wakeup so a firmware
       that signals the event without the address actually being live
       (or a firmware that doesn't fire DataNotify at all) still
       converges via the AxlWait tick — the 1 s tick keeps total CPU
       use comparable to the prior poll. */
    EFI_EVENT cfg_event = NULL;
    bool      cfg_event_registered = false;
    if (axl_bs()->CreateEvent(0, 0, NULL, NULL, &cfg_event) == EFI_SUCCESS
        && cfg_event != NULL)
    {
        EFI_STATUS reg_st = axl_efi_call(ip4cfg->RegisterDataNotify, 3,
            ip4cfg, Ip4Config2DataTypeInterfaceInfo, cfg_event);
        if (reg_st == EFI_SUCCESS) {
            cfg_event_registered = true;
        } else {
            axl_debug("RegisterDataNotify(InterfaceInfo) returned 0x%llx - "
                      "falling back to tick-only poll",
                      (unsigned long long)reg_st);
            axl_bs()->CloseEvent(cfg_event);
            cfg_event = NULL;
        }
    }

    AxlStatus wait_rc = _axl_event_wait_timeout_with_tick(
        (AxlEventHandle)cfg_event,
        ip4cfg_addr_acquired, NULL,
        NULL, NULL,
        1000ULL * 1000ULL,             /* tick_us = 1 s — same fallback cadence */
        NULL,
        timeout * 1000ULL * 1000ULL);  /* timeout_us */

    if (cfg_event_registered) {
        axl_efi_call(ip4cfg->UnregisterDataNotify, 3,
            ip4cfg, Ip4Config2DataTypeInterfaceInfo, cfg_event);
    }
    if (cfg_event != NULL) {
        axl_bs()->CloseEvent(cfg_event);
    }

    if (wait_rc == AXL_OK) {
        axl_info("network ready");
        return AXL_OK;
    }

    axl_warning("DHCP timeout after %zu seconds", timeout);
    return AXL_ERR;
}
