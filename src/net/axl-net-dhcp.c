/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-dhcp.c
    Static IP configuration, DHCP auto-init, and the DHCP lease view:
    axl_net_set_static_ip, axl_net_auto_init, axl_net_get_dhcp_lease.
**/

#include "../backend/axl-backend.h"
#include "../event/axl-wait-internal.h"
#include "axl-net-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-sys.h>
#include <axl/axl-driver.h>
#include <axl/axl-driver-info.h>
#include <axl/axl-pci.h>
#include <axl/axl-fs.h>
#include <axl/axl-path.h>
#include <axl/axl-sort.h>
#include <axl/axl-driver-deps.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>
#include <axl/axl-wait.h>
#include <axl/axl-watchdog.h>
#include <axl/axl-nvstore.h>

AXL_LOG_DOMAIN("net");

/// Vendor namespace for AXL net non-volatile values (hostname).
static const AxlGuid AXL_NET_NAMESPACE = AXL_GUID(
    0x7a3e9c14, 0x2b5d, 0x4f86,
    0x9c, 0x71, 0x3d, 0x8a, 0x6f, 0x21, 0xb4, 0x09);
#define AXL_NET_NS  "axl-net"

/* IP4Config2 types not in generated UEFI headers */
typedef struct {
    EFI_IPv4_ADDRESS  Address;
    EFI_IPv4_ADDRESS  SubnetMask;
} Ip4Config2ManualAddress;

/**
 * The NIC a bring-up's configure step settled on, carried to its read-back.
 *
 * Keyed by MAC, not by ordinal. An ordinal is only stable while the NIC SET is
 * -- axl-net.h says it outright: a NIC appearing shifts later ordinals, a MAC
 * never moves -- and the read-back rebuilds the registry, so an ordinal could
 * name a different NIC than the one that was configured. The MAC is the
 * registry's own documented stable key (see AxlNic.mac), so carrying it makes
 * "resolved once, reused" airtight rather than merely near-airtight.
 *
 * @c have is an explicit flag rather than an all-zero-MAC sentinel: the
 * registry rejects a hardware address only for HwAddressSize < 6, never for its
 * VALUE, so an all-zero MAC is a possible (if pathological) real key and cannot
 * unambiguously mean "none". No ordinal field: the only consumer is the
 * read-back (configured_nic_address), which looks the NIC up by MAC. The
 * configure step never needs one handed back out -- it holds the IP4Config2
 * pointer ip4cfg_for_ex resolved in the same registry build that filled this
 * struct, so it configures through that directly rather than re-resolving a
 * NIC by any key.
 */
typedef struct {
    bool    have;     ///< false = the configure step picked no NIC of ours
    uint8_t mac[6];   ///< stable key (valid iff have) -- for the read-back
} ConfiguredNic;

/* axl_net_auto_init's real body, defined far below beside the rest of the DHCP
   machinery; forward-declared because axl_net_bring_up (near the top of this
   file, beside the other one-call entry points) is its other caller. @p used is
   NOT optional -- the body always writes it, which is what lets every exit be a
   plain return. */
static int
auto_init_body(
    size_t         nic_index,
    size_t         dhcp_timeout_sec,
    ConfiguredNic *used
);

/* The DHCP-configure tail of auto_init_body: set the IP4Config2 DHCP policy and
   wait for a lease (or fall down the IP4Config2-free ladder), and record which
   NIC that landed on. Extracted so a caller that ran its OWN driver acquisition
   (the axl_net_auto_init_opts SWEEP_DIR / FIRMWARE_ONLY paths) can configure DHCP
   WITHOUT the curated-list load axl_net_drivers_up performs. Callers pass an
   already-defaulted, non-zero @p timeout (a 0 would mean a 0-second DHCP poll). */
static int
net_dhcp_configure(
    size_t         nic_index,
    size_t         timeout,
    ConfiguredNic *used
);

/* The shared static-IP SetData sequence, defined below beside the public
   setters it backs; forward-declared because axl_net_bring_up's static path
   (near the top of this file, beside the other one-call entry points) applies
   it directly to the protocol pointer ip4cfg_for_ex already resolved, rather
   than re-entering a public setter that would resolve the NIC a second time. */
static int
static_ip_apply(
    EFI_IP4_CONFIG2_PROTOCOL *cfg,
    const uint8_t             ip[4],
    const uint8_t             netmask[4],
    const uint8_t            *gateway
);

/* Record @p nics[idx] as the NIC we settled on. */
static void
configured_nic_set(ConfiguredNic *out, const AxlNic *nics, size_t idx)
{
    out->have = true;
    axl_memcpy(out->mac, nics[idx].mac, sizeof(out->mac));
}

/* Resolve a NIC ordinal (or AXL_NET_NIC_AUTO) to its IP4Config2 protocol via a
   freshly-built registry, additionally reporting WHICH NIC it resolved through
   @p out_nic (optional). The registry is built per call and released before
   returning: the protocol pointer stays valid (it belongs to the firmware, not
   to us), and NICs appear as drivers load, so a cached list would go stale
   exactly when it matters. Returns NULL if unresolvable -- callers must treat
   that as an error, never as "use NIC 0".

   @p out_nic exists so a caller that must later read back the state of the NIC
   it just configured can reuse the identity resolved HERE rather than
   re-resolving AXL_NET_NIC_AUTO against a second registry. AUTO is
   deterministic for a GIVEN registry but not across two of them: a NIC earlier
   in ordinal order going link-down -> link-up between the two builds flips
   AUTO's choice. It is set only on success; @c have stays false otherwise. */
static EFI_IP4_CONFIG2_PROTOCOL *
ip4cfg_for_ex(size_t nic_index, ConfiguredNic *out_nic)
{
    if (out_nic != NULL) {
        out_nic->have = false;
    }
    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK) {
        return NULL;
    }
    /* Resolve the ordinal first, then hand the CONCRETE ordinal to
       _axl_net_nic_resolve_ip4cfg. Resolution is idempotent on a concrete
       index, so this is the same protocol the AUTO spelling would have
       reached -- but now we also know which NIC it belongs to. */
    size_t idx = _axl_net_nic_resolve_index(nics, nnic, nic_index);
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    if (idx < nnic) {
        cfg = _axl_net_nic_resolve_ip4cfg(nics, nnic, idx);
        if (cfg != NULL && out_nic != NULL) {
            configured_nic_set(out_nic, nics, idx);
        }
    }
    _axl_net_nics_free(nics);
    return cfg;
}

/* ip4cfg_for_ex for callers that only need the protocol. */
static EFI_IP4_CONFIG2_PROTOCOL *
ip4cfg_for(size_t nic_index)
{
    return ip4cfg_for_ex(nic_index, NULL);
}

/* Resolve the IP4Config2 protocol for the NIC carrying @p mac. Walks every
   IP4Config2 handle and correlates by the SimpleNetwork MAC on the same handle
   — the same MAC correlation axl_net_list_interfaces uses — so it's correct
   regardless of IP4Config2-vs-SNP handle ordering (IP4Config2 lives on a child
   handle on some OEM firmware). First exact 6-byte match wins. The MAC
   counterpart to ip4cfg_for, backing the public _by_mac readers/setters
   (axl_net_get_dhcp_lease_by_mac, axl_net_set_static_ip_by_mac).

   A MAC match is REQUIRED -- there is deliberately no positional fallback
   here, unlike _axl_net_nic_resolve_ip4cfg (which guesses the sole
   IP4Config2 handle when count == 1 && ncfg == 1). That asymmetry is the
   point: the _by_mac contract is "this NIC or an error", and a positional
   guess would quietly configure whatever NIC happened to be there. Callers
   that WANT the guess -- axl_net_bring_up's paths -- go through
   ip4cfg_for_ex / ip4cfg_for instead. */
static EFI_IP4_CONFIG2_PROTOCOL *
ip4cfg_for_mac(const uint8_t mac[6])
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("ip4-config2", &handles, &count) != AXL_OK
        || count == 0) {
        if (handles != NULL) {
            axl_free(handles);
        }
        return NULL;
    }

    EFI_IP4_CONFIG2_PROTOCOL *match = NULL;
    for (size_t i = 0; i < count; i++) {
        uint8_t handle_mac[6];
        if (!_axl_net_snp_mac(_axl_net_snp_on((EFI_HANDLE)handles[i]), handle_mac)) {
            continue;
        }
        if (axl_memcmp(mac, handle_mac, 6) != 0) {
            continue;
        }
        EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
        axl_efi_call(axl_bs()->HandleProtocol, 3,
            (EFI_HANDLE)handles[i], &gEfiIp4Config2ProtocolGuid, (void **)&cfg);
        if (cfg != NULL) {
            match = cfg;
            break;
        }
    }
    axl_free(handles);
    return match;
}

// ---------------------------------------------------------------------------
// IP4Config2-free bring-up state (which mechanism configured the NIC + a
// cached lease from a non-IP4Config2 path, so the IP4Config2-keyed readers
// still report a result on firmware that lacks IP4Config2). Process-global,
// matching the single-NIC scope of the bring-up helpers.
// ---------------------------------------------------------------------------

/* Set once per process by a successful fallback bring-up and never reset: the
   readers consult it only when IP4Config2 is absent (the live IP4Config2 path
   is always preferred), so a cached lease can never shadow a live result. */
static AxlNetConfigMethod g_config_method = AXL_NET_CONFIG_NONE;
static AxlDhcpLease       g_fallback_lease;
static bool               g_have_fallback_lease = false;

AxlNetConfigMethod
axl_net_last_config_method(void)
{
    return g_config_method;
}

bool
_axl_net_fallback_lease(AxlDhcpLease *out)
{
    if (!g_have_fallback_lease || out == NULL) {
        return false;
    }
    *out = g_fallback_lease;
    return true;
}

// ---------------------------------------------------------------------------
// Path C — DHCP via EFI_DHCP4_SERVICE_BINDING (no IP4Config2)
//
// For OEM firmware that ships a full network stack (SNP/MNP/ARP/IP4/TCP4 +
// Dhcp4ServiceBinding) but NOT the IP4Config2 policy layer EDK2 ifconfig and
// axl-sdk normally drive. Mirrors SoftBmcDiag's DiagDhcp Path C, the path that
// brought up the HP ZBook: CreateChild -> Configure -> Start (blocking) ->
// read the lease off the completed child. The child is deliberately NOT
// destroyed — that keeps the lease active for the life of the process (the
// same one-shot persistence SoftBmcDiag relies on; durable cross-process
// config without IP4Config2 needs a resident driver — out of scope here).
// ---------------------------------------------------------------------------

static int
dhcp4_sb_bringup(void)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("dhcp4-service-binding", &handles, &count) != AXL_OK
        || count == 0) {
        if (handles != NULL) {
            axl_free(handles);
        }
        axl_debug("dhcp4_sb: no Dhcp4ServiceBinding handles");
        return AXL_ERR;
    }

    /* SoftBmcDiag's proven retry schedule (worked on the ZBook). The arrays
       are read by Configure and must outlive the call — keep them static. */
    static UINT32 discover_timeout[] = { 4, 8, 16 };
    static UINT32 request_timeout[]  = { 4, 8 };

    int rc = AXL_ERR;
    for (size_t i = 0; i < count && rc != AXL_OK; i++) {
        EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
        axl_efi_call(axl_bs()->HandleProtocol, 3,
            (EFI_HANDLE)handles[i], &gEfiDhcp4ServiceBindingProtocolGuid,
            (void **)&sb);
        if (sb == NULL) {
            continue;
        }

        EFI_HANDLE child = NULL;
        EFI_STATUS st = axl_efi_call(sb->CreateChild, 2, sb, &child);
        if (EFI_ERROR(st) || child == NULL) {
            axl_debug("dhcp4_sb: CreateChild failed: 0x%llx",
                      (unsigned long long)st);
            continue;
        }

        EFI_DHCP4_PROTOCOL *dhcp4 = NULL;
        axl_efi_call(axl_bs()->HandleProtocol, 3,
            child, &gEfiDhcp4ProtocolGuid, (void **)&dhcp4);
        if (dhcp4 == NULL) {
            axl_efi_call(sb->DestroyChild, 2, sb, child);
            continue;
        }

        EFI_DHCP4_CONFIG_DATA cfg;
        axl_memset(&cfg, 0, sizeof(cfg));
        cfg.DiscoverTryCount = 3;
        cfg.DiscoverTimeout  = discover_timeout;
        cfg.RequestTryCount  = 2;
        cfg.RequestTimeout   = request_timeout;

        st = axl_efi_call(dhcp4->Configure, 2, dhcp4, &cfg);
        if (EFI_ERROR(st)) {
            axl_debug("dhcp4_sb: Configure failed: 0x%llx",
                      (unsigned long long)st);
            axl_efi_call(sb->DestroyChild, 2, sb, child);
            continue;
        }

        /* Blocking DHCP (no completion event) — uses the retry schedule above,
           ~30 s worst case. */
        st = axl_efi_call(dhcp4->Start, 2, dhcp4, (EFI_EVENT)NULL);
        if (EFI_ERROR(st)) {
            axl_debug("dhcp4_sb: Start failed: 0x%llx",
                      (unsigned long long)st);
            axl_efi_call(sb->DestroyChild, 2, sb, child);
            continue;
        }

        EFI_DHCP4_MODE_DATA mode;
        axl_memset(&mode, 0, sizeof(mode));
        st = axl_efi_call(dhcp4->GetModeData, 2, dhcp4, &mode);
        static const uint8_t zero4[4] = { 0, 0, 0, 0 };
        if (EFI_ERROR(st)
            || axl_memcmp(&mode.ClientAddress, zero4, 4) == 0) {
            axl_efi_call(sb->DestroyChild, 2, sb, child);
            continue;
        }

        /* Cache the lease. DNS is not in EFI_DHCP4_MODE_DATA (it lives in the
           reply packet's options); SoftBmcDiag's Path C did not surface it
           either, so dns_count stays 0 here. */
        axl_memset(&g_fallback_lease, 0, sizeof(g_fallback_lease));
        axl_memcpy(g_fallback_lease.address, &mode.ClientAddress, 4);
        axl_memcpy(g_fallback_lease.subnet,  &mode.SubnetMask, 4);
        axl_memcpy(g_fallback_lease.router,  &mode.RouterAddress, 4);
        g_have_fallback_lease = true;
        axl_debug("dhcp4_sb: leased %u.%u.%u.%u (no IP4Config2)",
                  g_fallback_lease.address[0], g_fallback_lease.address[1],
                  g_fallback_lease.address[2], g_fallback_lease.address[3]);
        rc = AXL_OK;
        /* Intentionally NOT DestroyChild — keep the lease active. */
    }

    axl_free(handles);
    return rc;
}

/* Path D — DHCP via EFI_PXE_BASE_CODE_PROTOCOL.Dhcp(), the documented last
   resort when neither IP4Config2 nor Dhcp4ServiceBinding is usable. Mirrors
   SoftBmcDiag's Path D: for each IPv4 PXE instance, reuse an existing lease or
   Start(IPv4) + Dhcp(); read the station IP/mask off Mode. IPv6 instances are
   skipped. PXE is left started — like the Dhcp4 child, that keeps the address
   active for the process. */
static int
pxe_bc_dhcp(void)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("pxe-base-code", &handles, &count) != AXL_OK
        || count == 0) {
        if (handles != NULL) {
            axl_free(handles);
        }
        axl_debug("pxe_bc: no PXE Base Code handles");
        return AXL_ERR;
    }

    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    int rc = AXL_ERR;
    for (size_t i = 0; i < count && rc != AXL_OK; i++) {
        EFI_PXE_BASE_CODE_PROTOCOL *pxe = NULL;
        axl_efi_call(axl_bs()->HandleProtocol, 3,
            (EFI_HANDLE)handles[i], &gEfiPxeBaseCodeProtocolGuid, (void **)&pxe);
        if (pxe == NULL || pxe->Mode == NULL) {
            continue;
        }

        /* Already-running IPv4 instance with an address: just read it. */
        if (pxe->Mode->Started) {
            if (pxe->Mode->UsingIpv6) {
                continue;   /* IPv4 only */
            }
        } else {
            EFI_STATUS st = axl_efi_call(pxe->Start, 2, pxe, (BOOLEAN)0);
            if (EFI_ERROR(st)) {
                axl_debug("pxe_bc: Start failed: 0x%llx",
                          (unsigned long long)st);
                continue;
            }
        }

        if (axl_memcmp(&pxe->Mode->StationIp, zero4, 4) != 0) {
            /* Reuse the lease the firmware already has. */
        } else {
            EFI_STATUS st = axl_efi_call(pxe->Dhcp, 2, pxe, (BOOLEAN)0);
            if (EFI_ERROR(st)
                || axl_memcmp(&pxe->Mode->StationIp, zero4, 4) == 0) {
                axl_debug("pxe_bc: Dhcp failed: 0x%llx",
                          (unsigned long long)st);
                continue;
            }
        }

        axl_memset(&g_fallback_lease, 0, sizeof(g_fallback_lease));
        axl_memcpy(g_fallback_lease.address, &pxe->Mode->StationIp, 4);
        axl_memcpy(g_fallback_lease.subnet,  &pxe->Mode->SubnetMask, 4);
        g_have_fallback_lease = true;
        axl_debug("pxe_bc: leased %u.%u.%u.%u (no IP4Config2/Dhcp4-SB)",
                  g_fallback_lease.address[0], g_fallback_lease.address[1],
                  g_fallback_lease.address[2], g_fallback_lease.address[3]);
        rc = AXL_OK;
        /* Leave PXE started — keep the address active. */
    }

    axl_free(handles);
    return rc;
}

// ---------------------------------------------------------------------------
// axl_net_bring_up — one-call DHCP-or-static bring-up + address read-back
// ---------------------------------------------------------------------------

#define AXL_NET_STATIC_IP_SETTLE_MS  500

/* Read back the IPv4 address of the NIC axl_net_bring_up actually configured.
   @p used identifies that NIC by MAC, resolved ONCE by the configure step.
   This is the fix for reporting a DIFFERENT NIC's address than the one just
   configured: axl_net_get_ip_address is NIC-agnostic BY DESIGN (it answers
   "does ANY NIC have an address", first configured IP4Config2 wins), which is
   the wrong question once the caller named a specific NIC -- or once AUTO has
   already picked one.

   The rule is three-way, because "has no address" and "cannot be attributed an
   address" are DIFFERENT predicates and only the second one may fall back:

     - has_ipv4                      -> report it. AXL_OK.
     - !has_ipv4, ip4cfg_handle != NULL -> AXL_ERR. The NIC is attributable, it
       simply has no address YET. Falling back here would report a DIFFERENT
       NIC's address, which is precisely the bug this function exists to close.
       Reachable, not theoretical: the static path's static_ip_apply sets the
       policy to Static first, which makes the firmware drop its prior address,
       so StationAddress transiently reads 0.0.0.0 -- that is why
       AXL_NET_STATIC_IP_SETTLE_MS exists, and 500 ms is an empirical budget,
       not a guarantee. On firmware slower than that budget a fallback would
       hand back some other NIC's address and call it success.
     - !has_ipv4, ip4cfg_handle == NULL -> fall back to the NIC-agnostic
       reader. The NIC cannot be correlated to an IP4Config2 at all, so it was
       configured through _axl_net_nic_resolve_ip4cfg's positional guess, which
       only ever runs when there is exactly ONE NIC and ONE IP4Config2 handle.
       With no second NIC in play there is nothing to confuse it with, and the
       NIC-agnostic reader is the only reader that can see the address. This
       branch is live on BOTH paths, which is exactly why they both reach
       IP4Config2 through ip4cfg_for_ex: a route that resolved by MAC instead
       would never reach the positional guess (no MAC correlates, by
       definition of this branch), so it would fail to configure the NIC at all
       rather than arriving here -- and the branch would be dead on static
       while live on DHCP.

   A false @p used->have ("no NIC of ours was picked" -- see auto_init_body's
   exit table) also falls back: NIC-agnostic is the CORRECT answer there.
   get_ip_address additionally surfaces the IP4Config2-free Dhcp4-SB / PXE lease
   via _axl_net_fallback_lease, which is why that ladder can report have=false
   and still read back correctly. */
static int
configured_nic_address(const ConfiguredNic *used, AxlIPv4Address *out)
{
    if (!used->have) {
        return axl_net_get_ip_address(out);
    }

    AxlNic *nics = NULL;
    size_t  nnic = 0;
    if (_axl_net_nics_build(&nics, &nnic) != AXL_OK) {
        return AXL_ERR;
    }

    /* Find the NIC we configured by its MAC -- not by ordinal, which the
       rebuild above could have shifted under us. */
    const AxlNic *row = NULL;
    for (size_t i = 0; i < nnic; i++) {
        if (axl_memcmp(nics[i].mac, used->mac, sizeof(used->mac)) == 0) {
            row = &nics[i];
            break;
        }
    }

    int  rc        = AXL_ERR;
    bool fall_back = false;
    char macbuf[18];
    axl_mac_format(used->mac, macbuf, sizeof macbuf);
    if (row == NULL) {
        /* The NIC we configured is no longer in the registry. We cannot
           attribute an address to it, and falling back would report a
           DIFFERENT NIC's -- so this is an honest error, not a fallback. */
        axl_warning("bring_up: configured NIC %s is gone before read-back",
                    macbuf);
    } else if (row->has_ipv4) {
        axl_memcpy(out->addr, row->ipv4, sizeof(out->addr));
        rc = AXL_OK;
    } else if (row->ip4cfg_handle != NULL) {
        axl_warning("bring_up: NIC %s has no address "
                    "yet - reporting another NIC's would be wrong",
                    macbuf);
    } else {
        fall_back = true;
    }
    _axl_net_nics_free(nics);   /* invalidates row */

    return fall_back ? axl_net_get_ip_address(out) : rc;
}

int
axl_net_bring_up(
    size_t          nic_index,
    const uint8_t  *static_ipv4,
    const uint8_t  *netmask,
    const uint8_t  *gateway,
    size_t          timeout_sec,
    AxlIPv4Address *addr_out)
{
    /* The NIC the configure step below actually settled on, for the read-back.
       have == false means "no NIC of ours was picked" -- read back
       NIC-agnostically. Resolved ONCE per path and carried by MAC; never
       re-derived from @p nic_index afterward. */
    ConfiguredNic used = { .have = false, .mac = { 0 } };

    if (static_ipv4 == NULL) {
        /* auto_init reports which NIC it configured: its AUTO short-circuit is
           deliberately "any NIC has an IP" (NOT "this NIC"), so we cannot
           pre-resolve AUTO and call it with a concrete index without silently
           changing that approved rule. Letting it report back preserves the
           rule AND closes the re-resolution window. */
        if (auto_init_body(nic_index, timeout_sec, &used) != AXL_OK) {
            return AXL_ERR;
        }
    } else {
        /* Static path: drivers_up gets us link without burning the
           DHCP timeout (which would fire if we routed through
           auto_init). Then set the static address. */
        if (axl_net_drivers_up() != AXL_OK) {
            return AXL_ERR;
        }
        /* Resolve the NIC and its IP4Config2 from ONE registry build, then
           apply. ip4cfg_for_ex hands back both halves -- the protocol pointer
           to configure through AND (via configured_nic_set) the MAC to read
           back by -- so there is no second registry build and hence no
           re-resolution window at all, not even a MAC-keyed one.

           Deliberately NOT routed through the public per-NIC setters. Neither
           spelling is equivalent to this:
             - set_static_ip(ordinal) would rebuild the registry and re-resolve
               the ordinal, which is the drift window we are closing.
             - set_static_ip_by_mac(mac) would rebuild it AND drop the
               positional guess: _axl_net_nic_resolve_ip4cfg falls back to the
               single IP4Config2 handle when count == 1 && ncfg == 1 (firmware
               where SNP isn't reachable from the IP4Config2 handle, so no MAC
               correlates), while ip4cfg_for_mac REQUIRES a MAC match by
               contract. Resolving the ordinal alone would still yield a NIC
               there -- _axl_net_nic_resolve_index never consults
               ip4cfg_handle -- so we WOULD have a MAC to pass, and the by-MAC
               route would then fail on firmware this one configures fine.
               Going through ip4cfg_for_ex keeps that fallback, and keeps the
               static path symmetric with the DHCP path, which reaches it the
               same way. */
        EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for_ex(nic_index, &used);
        if (cfg == NULL) {
            axl_warning("bring_up: no IP4Config2 for nic %zu", nic_index);
            return AXL_ERR;
        }
        static const uint8_t mask24[4] = { 255, 255, 255, 0 };
        const uint8_t *m = (netmask != NULL) ? netmask : mask24;
        if (static_ip_apply(cfg, static_ipv4, m, gateway) != AXL_OK) {
            return AXL_ERR;
        }
        /* IP4Config2 applies the policy + address asynchronously; without
           a short settle, the next GetData can still report the prior
           state. 500 ms is the empirically-stable budget. */
        axl_msleep(AXL_NET_STATIC_IP_SETTLE_MS);
    }

    if (addr_out != NULL) {
        if (configured_nic_address(&used, addr_out) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_net_set_static_ip / axl_net_set_static_ip_by_mac
// ---------------------------------------------------------------------------

/* Shared SetData sequence: policy=Static, ManualAddress, optional Gateway.
   Backs both axl_net_set_static_ip (registry ordinal) and
   axl_net_set_static_ip_by_mac (MAC) -- same shape as lease_from_cfg backing
   the two DHCP-lease readers below. @p cfg is the already-resolved
   IP4Config2 protocol; this never resolves a NIC itself. */
static int
static_ip_apply(
    EFI_IP4_CONFIG2_PROTOCOL *cfg,
    const uint8_t             ip[4],
    const uint8_t             netmask[4],
    const uint8_t            *gateway)
{
    EFI_STATUS status;

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

int
axl_net_set_static_ip(
    size_t         nic_index,
    const uint8_t  ip[4],
    const uint8_t  netmask[4],
    const uint8_t *gateway)
{
    if (ip == NULL || netmask == NULL) {
        return AXL_ERR;
    }

    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        axl_warning("set_static_ip: no IP4Config2 for nic %zu", nic_index);
        return AXL_ERR;
    }

    return static_ip_apply(cfg, ip, netmask, gateway);
}

int
axl_net_set_static_ip_by_mac(
    const uint8_t  mac[6],
    const uint8_t  ip[4],
    const uint8_t  netmask[4],
    const uint8_t *gateway)
{
    if (mac == NULL || ip == NULL || netmask == NULL) {
        return AXL_ERR;
    }

    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for_mac(mac);
    if (cfg == NULL) {
        char macbuf[18];
        axl_mac_format(mac, macbuf, sizeof macbuf);
        axl_warning("set_static_ip_by_mac: no IP4Config2 NIC carrying %s", macbuf);
        return AXL_ERR;   /* no fallback -- a MAC that names no NIC is an error */
    }

    return static_ip_apply(cfg, ip, netmask, gateway);
}

// ---------------------------------------------------------------------------
// DNS / hostname / settle
// ---------------------------------------------------------------------------

int
axl_net_set_dns(size_t nic_index, const uint8_t dns[4], const uint8_t *dns2)
{
    if (dns == NULL) {
        return AXL_ERR;
    }
    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        return AXL_ERR;
    }

    EFI_IPv4_ADDRESS servers[2];
    axl_memcpy(&servers[0], dns, 4);
    size_t nserv = 1;
    if (dns2 != NULL) {
        axl_memcpy(&servers[1], dns2, 4);
        nserv = 2;
    }
    EFI_STATUS st = axl_efi_call(cfg->SetData, 4, cfg,
        Ip4Config2DataTypeDnsServer,
        (size_t)(nserv * sizeof(EFI_IPv4_ADDRESS)), servers);
    if (EFI_ERROR(st)) {
        axl_warning("set_dns: SetData(DnsServer) failed: 0x%llx "
                    "(DNS is read-only under DHCP policy)",
                    (unsigned long long)st);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_net_set_hostname(const char *name)
{
    if (name == NULL || name[0] == '\0' || axl_strlen(name) > 63) {
        return AXL_ERR;
    }
    /* Lazy bind. A rejected registration leaves AXL_NET_NS unregistered,
       which makes the set below a guaranteed failure — report it here
       rather than letting the store silently swallow the hostname. */
    if (axl_nvstore_register_namespace(AXL_NET_NS, &AXL_NET_NAMESPACE) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_nvstore_set_str(AXL_NET_NS, "Hostname", name,
                               AXL_NV_PERSISTENT | AXL_NV_BOOT);
}

int
axl_net_get_hostname(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return AXL_ERR;
    }
    buf[0] = '\0';
    /* A rejected registration is not "unset" — it means the namespace
       is unreachable and the read below could never find anything. */
    if (axl_nvstore_register_namespace(AXL_NET_NS, &AXL_NET_NAMESPACE) != AXL_OK) {
        return AXL_ERR;
    }
    char *stored = NULL;
    if (axl_nvstore_get_str(AXL_NET_NS, "Hostname", &stored) != AXL_OK
        || stored == NULL) {
        return AXL_OK;   /* unset is a normal state: "" + AXL_OK */
    }
    axl_strlcpy(buf, stored, size);
    axl_free(stored);
    return AXL_OK;
}

/* Read the NIC's current StationAddress from IP4Config2 InterfaceInfo into
   @p out (4 octets). Returns AXL_OK on success. */
static int
read_station_address(EFI_IP4_CONFIG2_PROTOCOL *cfg, uint8_t out[4])
{
    size_t info_size = 0;
    EFI_STATUS st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        return AXL_ERR;
    }
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = axl_backend_alloc(info_size);
    if (info == NULL) {
        return AXL_ERR;
    }
    st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypeInterfaceInfo, &info_size, info);
    if (EFI_ERROR(st)) {
        axl_backend_free(info);
        return AXL_ERR;
    }
    axl_memcpy(out, &info->StationAddress, 4);
    axl_backend_free(info);
    return AXL_OK;
}

#define AXL_NET_SETTLE_POLL_MS  50u

int
axl_net_wait_ip_settled(size_t nic_index, const uint8_t *expect_ipv4,
                        size_t timeout_ms)
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        return AXL_ERR;
    }
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    size_t budget = (timeout_ms > 0) ? timeout_ms : 1000;
    size_t waited = 0;
    for (;;) {
        uint8_t station[4] = { 0 };
        if (read_station_address(cfg, station) == AXL_OK) {
            bool settled = (expect_ipv4 != NULL)
                ? (axl_memcmp(station, expect_ipv4, 4) == 0)
                : (axl_memcmp(station, zero4, 4) != 0);
            if (settled) {
                return AXL_OK;
            }
        }
        if (waited >= budget) {
            return AXL_ERR;
        }
        axl_msleep(AXL_NET_SETTLE_POLL_MS);
        waited += AXL_NET_SETTLE_POLL_MS;
    }
}

// ---------------------------------------------------------------------------
// axl_net_get_dhcp_lease — the active DHCP-leased config, via IP4Config2
// ---------------------------------------------------------------------------

/* GetData a variable-size IP4Config2 data type into a freshly allocated
   buffer (the two-call BUFFER_TOO_SMALL idiom). Caller frees via
   axl_backend_free. Returns NULL (and *out_size unset) on any failure. */
static void *
ip4cfg_getdata(
    EFI_IP4_CONFIG2_PROTOCOL    *cfg,
    EFI_IP4_CONFIG2_DATA_TYPE    type,
    size_t                      *out_size)
{
    size_t sz = 0;
    EFI_STATUS st = axl_efi_call(cfg->GetData, 4, cfg, type, &sz, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || sz == 0) {
        return NULL;
    }
    void *buf = axl_backend_alloc(sz);
    if (buf == NULL) {
        return NULL;
    }
    st = axl_efi_call(cfg->GetData, 4, cfg, type, &sz, buf);
    if (EFI_ERROR(st)) {
        axl_backend_free(buf);
        return NULL;
    }
    *out_size = sz;
    return buf;
}

/* Shared lease extraction: read the live IP4Config2 DHCP lease from @p cfg into
   @p out. Backs both axl_net_get_dhcp_lease (registry ordinal) and
   axl_net_get_dhcp_lease_by_mac (MAC). Returns AXL_ERR for a non-DHCP policy or
   an unleased NIC. @p out must already be zeroed by the caller. */
static int
lease_from_cfg(EFI_IP4_CONFIG2_PROTOCOL *cfg, AxlDhcpLease *out)
{
    /* A "lease" requires a DHCP policy — a static NIC has no lease to view. */
    uint32_t   policy = 0;
    size_t     psz = sizeof(policy);
    EFI_STATUS st = axl_efi_call(cfg->GetData, 4, cfg,
        Ip4Config2DataTypePolicy, &psz, &policy);
    if (EFI_ERROR(st) || policy != 1) {   /* 1 = DHCP (matches auto_init) */
        return AXL_ERR;
    }

    /* Address / mask / default gateway from the live interface info. */
    size_t info_size = 0;
    EFI_IP4_CONFIG2_INTERFACE_INFO *info =
        ip4cfg_getdata(cfg, Ip4Config2DataTypeInterfaceInfo, &info_size);
    if (info == NULL || info_size < sizeof(*info)) {
        if (info != NULL) {
            axl_backend_free(info);
        }
        return AXL_ERR;
    }

    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    if (axl_memcmp(&info->StationAddress, zero4, 4) == 0) {
        axl_backend_free(info);   /* DHCP policy but no address leased yet */
        return AXL_ERR;
    }

    axl_memcpy(out->address, &info->StationAddress, 4);
    axl_memcpy(out->subnet,  &info->SubnetMask, 4);

    /* Default gateway = the route-table entry to 0.0.0.0/0. */
    if (info->RouteTable != NULL) {
        size_t routes = info->RouteTableSize;
        for (size_t i = 0; i < routes; i++) {
            if (axl_memcmp(&info->RouteTable[i].SubnetAddress, zero4, 4) == 0
                && axl_memcmp(&info->RouteTable[i].SubnetMask, zero4, 4) == 0) {
                axl_memcpy(out->router, &info->RouteTable[i].GatewayAddress, 4);
                break;
            }
        }
    }
    axl_backend_free(info);

    /* DHCP-provided resolver(s). */
    size_t dns_size = 0;
    EFI_IPv4_ADDRESS *dns =
        ip4cfg_getdata(cfg, Ip4Config2DataTypeDnsServer, &dns_size);
    if (dns != NULL) {
        size_t n = dns_size / sizeof(EFI_IPv4_ADDRESS);
        for (size_t i = 0; i < n && out->dns_count < 2; i++) {
            axl_memcpy(out->dns[out->dns_count], &dns[i], 4);
            out->dns_count++;
        }
        axl_backend_free(dns);
    }

    return AXL_OK;
}

int
axl_net_get_dhcp_lease(size_t nic_index, AxlDhcpLease *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for(nic_index);
    if (cfg == NULL) {
        /* No IP4Config2 — return the lease cached by an IP4Config2-free
           bring-up (Dhcp4-SB / PXE BC), if any. */
        return _axl_net_fallback_lease(out) ? AXL_OK : AXL_ERR;
    }
    return lease_from_cfg(cfg, out);
}

int
axl_net_get_dhcp_lease_by_mac(const uint8_t mac[6], AxlDhcpLease *out)
{
    if (mac == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for_mac(mac);
    if (cfg == NULL) {
        return AXL_ERR;   /* no NIC carries that MAC */
    }
    return lease_from_cfg(cfg, out);
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

        axl_debug("ensure_drivers: loaded '%s'", path);
        loaded++;
    }
    return loaded;
}

AxlNetDriversStatus
axl_net_ensure_drivers(void)
{
    size_t snp_before = net_count_snp();
    axl_debug("ensure_drivers: starting (%zu SNP handles already present)",
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
        axl_debug(
            "ensure_drivers: no SNP after core drivers - falling back to iPXE");
        loaded_count += load_driver_list(
            net_drivers_ipxe,
            sizeof(net_drivers_ipxe) / sizeof(net_drivers_ipxe[0]));
        axl_driver_connect(NULL);
    } else {
        axl_debug(
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
    axl_debug("ensure_drivers: %zu drivers loaded, SNP handles %zu->%zu",
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

void
_axl_net_connect_snp_handles(void)
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
    _axl_net_connect_snp_handles();

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

/* axl_net_auto_init's real body, plus @p used: WHICH NIC this call configured,
   so axl_net_bring_up can read that NIC's address back without re-resolving
   AXL_NET_NIC_AUTO against a second, independently-built registry. AUTO is
   deterministic for a GIVEN registry but not across two of them -- a NIC
   earlier in ordinal order transitioning link-down -> link-up between the two
   builds flips AUTO's choice, and that window is exactly when link states are
   settling.

   @p used is NOT optional and is ALWAYS written (have=false at entry), so
   every exit below is a plain return with nothing to remember.

   What each exit reports, and why:

     - DHCP bring-up succeeded: the NIC we resolved and drove. This is the case
       the out-param exists for.

     - The AXL_NET_NIC_AUTO short-circuit fired (some NIC was already up):
       have=false. We configured NOTHING, so we did not pick a NIC -- the
       caller asked for "any networking" and got it. A NIC-agnostic read-back
       is the honest answer, and have=false is what tells axl_net_bring_up to
       do that.

     - The EXPLICIT-index short-circuit fired: that NIC -- NOT have=false. We
       configured nothing here either, but unlike the AUTO case we know exactly
       which NIC the caller meant AND we read has_ipv4 off THAT NIC's row, so
       the address is attributable by construction. Reporting have=false here
       would route a verified-attributable case into the NIC-agnostic reader
       and reintroduce the very wrong-NIC bug this out-param exists to close.

     - The IP4Config2-free Dhcp4-SB / PXE ladder succeeded: have=false. Those
       paths never touch IP4Config2, so they have no registry NIC at all; their
       lease lives in the _axl_net_fallback_lease cache, which
       axl_net_get_ip_address already surfaces.

     - Any failure exit: whatever was resolved so far (usually have=false). The
       value is meaningless when we return AXL_ERR -- the caller must not read
       it -- but it is always initialized, so a caller cannot consume
       uninitialized memory. */
static int
auto_init_body(size_t nic_index, size_t dhcp_timeout_sec, ConfiguredNic *used)
{
    size_t timeout;

    used->have = false;

    timeout = (dhcp_timeout_sec > 0) ? dhcp_timeout_sec : 10;

    /*
     * Check if networking is already configured -- deliberately split by
     * how the caller named the NIC, because "already configured" is a
     * different question for each:
     *
     * AXL_NET_NIC_AUTO means "get me networking, I don't care which NIC",
     * so "does ANY NIC already have an IP" is the right question, and
     * axl_net_get_ip_address answers exactly that (first configured
     * IP4Config2 wins). We need a real IP, not just a TCP4 service
     * binding — axl_net_is_available() returns true when TCP4 is present
     * even before DHCP runs (PixieFail-era firmware installs
     * TCP4ServiceBinding at boot), so it would short-circuit the DHCP
     * path below and leave the caller with no IP. get_ip_address()'s
     * stricter check matches our contract, and re-running DHCP when some
     * NIC is already up would only burn dhcp_timeout_sec for nothing.
     *
     * An explicit nic_index asks "is THIS NIC already up" -- a question
     * axl_net_get_ip_address cannot answer, since it doesn't know or
     * care which NIC it found. "Configure NIC 1" must not report success
     * merely because NIC 0 already leased, so the explicit-index path
     * checks the per-physical-NIC registry's has_ipv4 for that ordinal
     * instead. An out-of-range ordinal has no registry row, so it never
     * short-circuits — it falls through to the real bring-up below,
     * which resolves the same ordinal and correctly fails.
     */
    if (nic_index == (size_t)AXL_NET_NIC_AUTO) {
        AxlIPv4Address addr;
        if (axl_net_get_ip_address(&addr) == AXL_OK) {
            if (g_config_method == AXL_NET_CONFIG_NONE) {
                g_config_method = AXL_NET_CONFIG_IP4CONFIG2;
            }
            /* used->have stays false: we picked no NIC. */
            return AXL_OK;
        }
    } else {
        AxlNic *nics = NULL;
        size_t  nnic = 0;
        bool    nic_already_up = false;
        if (_axl_net_nics_build(&nics, &nnic) == AXL_OK
            && nic_index < nnic && nics[nic_index].has_ipv4) {
            nic_already_up = true;
            /* Configured nothing, but we know exactly which NIC is up: the one
               asked for, whose has_ipv4 we just read. Attributable by
               construction -- record it rather than reporting have=false. */
            configured_nic_set(used, nics, nic_index);
        }
        _axl_net_nics_free(nics);
        if (nic_already_up) {
            if (g_config_method == AXL_NET_CONFIG_NONE) {
                g_config_method = AXL_NET_CONFIG_IP4CONFIG2;
            }
            return AXL_OK;
        }
    }

    /* drivers_up returns AXL_ERR if no link came up — auto_init still
       continues (older callers rely on the DHCP poll being attempted
       even on no-link paths to surface a clearer error). */
    axl_net_drivers_up();

    return net_dhcp_configure(nic_index, timeout, used);
}

static int
net_dhcp_configure(size_t nic_index, size_t timeout, ConfiguredNic *used)
{
    /*
     * Set DHCP policy via IP4Config2. We find the right handle,
     * set policy to DHCP, then poll for an IP address.
     */
    void  **cfg_handles = NULL;
    size_t  cfg_count = 0;
    if (axl_protocol_enumerate("ip4-config2", &cfg_handles, &cfg_count) != AXL_OK
        || cfg_count == 0)
    {
        /* No IP4Config2 policy layer (some OEM firmware, e.g. HP). Fall back
           down the IP4Config2-free ladder: DHCP4-ServiceBinding, then PXE BC.
           Sets g_config_method so axl_net_last_config_method() reports it. */
        if (cfg_handles != NULL) {
            axl_free(cfg_handles);
        }
        axl_debug("no IP4Config2 - trying IP4Config2-free DHCP fallback");
        /* Both ladder rungs keep have=false: they never touch IP4Config2, so
           there is no registry NIC to report. Their lease is cached in
           _axl_net_fallback_lease, which axl_net_get_ip_address surfaces. */
        if (dhcp4_sb_bringup() == AXL_OK) {
            g_config_method = AXL_NET_CONFIG_DHCP4_SB;
            return AXL_OK;
        }
        if (pxe_bc_dhcp() == AXL_OK) {
            g_config_method = AXL_NET_CONFIG_PXE_BC;
            return AXL_OK;
        }
        axl_warning("no IP4Config2 and no DHCP4-SB/PXE fallback succeeded");
        return AXL_ERR;
    }

    /* Resolve the NIC's IP4Config2 through the registry: by MAC for an explicit
       ordinal, or the AUTO rule (first link-up NIC with an IP4Config2, else the
       first with one). No positional clamp -- that clamp sent DHCP to a
       link-down sibling on real hardware. This ALSO records WHICH NIC that
       landed on (into used), so the caller reads back the NIC we actually
       drove instead of re-resolving AUTO against a fresh registry. The
       protocol pointer and the recorded MAC come from the SAME registry
       build, so the NIC we drive and the NIC we report cannot diverge. */
    EFI_IP4_CONFIG2_PROTOCOL *ip4cfg = ip4cfg_for_ex(nic_index, used);

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
        g_config_method = AXL_NET_CONFIG_IP4CONFIG2;
        axl_debug("network ready");
        /* The exit this out-param exists for: ip4cfg_for_ex already recorded
           the NIC we drove DHCP on, so there is nothing to set here. */
        return AXL_OK;
    }

    axl_warning("DHCP timeout after %zu seconds", timeout);
    return AXL_ERR;
}

int
axl_net_auto_init(size_t nic_index, size_t dhcp_timeout_sec)
{
    /* Thin wrapper: the public contract is unchanged -- callers that don't need
       to know which NIC was configured just discard it. Initialized even though
       auto_init_body's first act is to clear it: the always-written invariant is
       what the removed return-hiding macro used to enforce structurally, so keep
       it true on both sides of the call rather than only on the callee's. */
    ConfiguredNic used = { .have = false, .mac = { 0 } };
    return auto_init_body(nic_index, dhcp_timeout_sec, &used);
}

// ---------------------------------------------------------------------------
// Shared driver-quarantine namespace + axl_net_auto_init_opts engine
// ---------------------------------------------------------------------------

/* ONE quarantine namespace every consumer of the crash-safe sweep shares: a
   driver that RSODs the box is bad no matter who loads it, so netload -a and any
   resident consumer honor the same list, and axl_net_clear_driver_quarantine()
   clears the one they all read. netload migrated onto this from its own vendor
   GUID. The GUID + keys + bounds name persistent NVRAM variables -- keep them
   stable. Bounds match netload's historical sizing so nothing on disk is
   orphaned by format drift. */
#define AXL_NET_DRVQ_NS         "axl-net-drv"
#define AXL_NET_DRVQ_NAME_MAX   64
#define AXL_NET_DRVQ_Q_MAX      1024
#define AXL_NET_DRVQ_LOG_MAX    2048
#define AXL_NET_DRVQ_NV_FLAGS   (AXL_NV_PERSISTENT | AXL_NV_BOOT)

static const AxlGuid AXL_NET_DRVQ_GUID =
    AXL_GUID(0x6178c04e, 0x7464, 0x71ff, 0xb3, 0x0c, 0x00, 0x11, 0x22, 0x33, 0x44, 0x56);

/* Init @at onto the shared namespace with the pinned format. The ONE place the
   on-disk layout is defined; the engine and netload both come through here so
   their breadcrumb/quarantine/log formats cannot diverge. */
static int
net_drvq_init(AxlAttempt *at)
{
    if (at == NULL) {
        return AXL_ERR;
    }
    if (axl_attempt_init(at, AXL_NET_DRVQ_NS, &AXL_NET_DRVQ_GUID) != AXL_OK) {
        return AXL_ERR;
    }
    at->trying_key     = "Trying";
    at->quarantine_key = "Quarantine";
    at->log_key        = "Log";
    at->name_max       = AXL_NET_DRVQ_NAME_MAX;
    at->quarantine_max = AXL_NET_DRVQ_Q_MAX;
    at->log_max        = AXL_NET_DRVQ_LOG_MAX;
    at->flags          = AXL_NET_DRVQ_NV_FLAGS;
    return AXL_OK;
}

int
axl_net_driver_quarantine_init(AxlAttempt *at)
{
    return net_drvq_init(at);
}

int
axl_net_clear_driver_quarantine(void)
{
    AxlAttempt at;
    if (net_drvq_init(&at) == AXL_OK) {
        axl_attempt_clear(&at);   /* Trying + Quarantine + Log */
    }
    return AXL_OK;   /* best-effort: a never-written namespace is a clean no-op */
}

/* Resolve opts->nic_mac to its list_interfaces ordinal. Returns false if no
   present interface carries that MAC (the caller fails cleanly rather than
   guessing a NIC). */
static bool
net_mac_to_index(const uint8_t mac[6], size_t *out_index)
{
    AxlNetInterface ifs[8];
    size_t          n = sizeof(ifs) / sizeof(ifs[0]);
    if (axl_net_list_interfaces(ifs, &n) != AXL_OK) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (axl_memcmp(ifs[i].mac, mac, 6) == 0) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

/* Fill @res's NIC identity (have_nic/mac/nic_index) from whichever registry row
   now holds @addr. The bring-up primitives return only the address; this maps it
   back to a specific NIC so the result names one. */
static void
net_result_identify(AxlNetBringUpResult *res, const uint8_t addr[4])
{
    AxlNetInterface ifs[8];
    size_t          n = sizeof(ifs) / sizeof(ifs[0]);
    if (axl_net_list_interfaces(ifs, &n) != AXL_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (ifs[i].has_ipv4 && axl_memcmp(ifs[i].ipv4, addr, 4) == 0) {
            res->have_nic  = true;
            res->nic_index = i;
            axl_memcpy(res->mac, ifs[i].mac, 6);
            return;
        }
    }
}

/* Resolve opts->nic_select to a concrete index against the CURRENT registry.
   Returns false only for SEL_MAC when the target MAC isn't present yet (a caller
   mid-sweep keeps loading drivers until it appears). AUTO -> AXL_NET_NIC_AUTO;
   INDEX -> the ordinal (bring-up validates its range). */
static bool
net_engine_nic(const AxlNetAutoOpts *opts, size_t *eff)
{
    switch (opts->nic_select) {
        case AXL_NET_NIC_SEL_INDEX:
            *eff = opts->nic_index;
            return true;
        case AXL_NET_NIC_SEL_MAC:
            return net_mac_to_index(opts->nic_mac, eff);
        case AXL_NET_NIC_SEL_AUTO:
        default:
            *eff = (size_t)AXL_NET_NIC_AUTO;
            return true;
    }
}

/* Assign an address to the selected NIC WITHOUT acquiring drivers (the caller
   already did firmware-first / a sweep): DHCP via net_dhcp_configure, or static
   via the shared static_ip_apply, then read the address back through the same
   registry build that configured it. AXL_ERR if the NIC can't be resolved or the
   assignment fails. */
static int
net_engine_assign(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
                  AxlIPv4Address *addr_out)
{
    size_t eff;
    if (!net_engine_nic(opts, &eff)) {
        return AXL_ERR;   /* SEL_MAC target not present */
    }
    ConfiguredNic used = { .have = false, .mac = { 0 } };

    if (is_static) {
        EFI_IP4_CONFIG2_PROTOCOL *cfg = ip4cfg_for_ex(eff, &used);
        if (cfg == NULL) {
            return AXL_ERR;
        }
        static const uint8_t mask24[4] = { 255, 255, 255, 0 };
        const uint8_t *m = (opts->static_mask != NULL) ? opts->static_mask : mask24;
        if (static_ip_apply(cfg, opts->static_ipv4, m, opts->static_gw) != AXL_OK) {
            return AXL_ERR;
        }
        axl_msleep(AXL_NET_STATIC_IP_SETTLE_MS);
        if (opts->dns1 != NULL) {
            axl_net_set_dns(eff, opts->dns1, opts->dns2);
        }
    } else {
        /* DHCP, retried up to dhcp_retries times for a link-up NIC that doesn't
           lease on the first poll (0/1 == a single attempt). */
        size_t tries = (opts->dhcp_retries > 0) ? opts->dhcp_retries : 1;
        int rc = AXL_ERR;
        for (size_t t = 0; t < tries && rc != AXL_OK; t++) {
            rc = net_dhcp_configure(eff, timeout, &used);
        }
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
    }
    return configured_nic_address(&used, addr_out);
}

/* Fill @ev's live NIC state (link_up / have_ip / ipv4) from the registry row for
   @mac. @ev->have_nic + @ev->mac are set by the caller. */
static void
net_event_nic_state(AxlNetDriverEvent *ev, const uint8_t *mac)
{
    AxlNetInterface ifs[8];
    size_t          n = sizeof(ifs) / sizeof(ifs[0]);
    if (axl_net_list_interfaces(ifs, &n) != AXL_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (axl_memcmp(ifs[i].mac, mac, 6) == 0) {
            ev->link_up = ifs[i].link_up;
            ev->have_ip = ifs[i].has_ipv4;
            if (ifs[i].has_ipv4) {
                axl_memcpy(ev->ipv4, ifs[i].ipv4, 4);
            }
            return;
        }
    }
}

/* Emit a per-driver progress event to opts->on_driver, if a hook is set. @mac is
   NULL when no NIC was attributed; otherwise the event's link/ip state is filled
   from the registry. */
static void
net_sweep_emit(const AxlNetAutoOpts *opts, const char *driver,
               AxlNetDriverOutcome outcome, const uint8_t *mac)
{
    if (opts->on_driver == NULL) {
        return;
    }
    AxlNetDriverEvent ev = { .driver = driver, .outcome = outcome };
    if (mac != NULL) {
        ev.have_nic = true;
        axl_memcpy(ev.mac, mac, 6);
        net_event_nic_state(&ev, mac);
    }
    opts->on_driver(&ev, opts->on_driver_ctx);
}

/* Run the requested reachability checks against the configured NIC @mac. True if
   REACHABLE wasn't asked, or every configured check passes. A check with no
   usable target (e.g. --ping-gw with no known gateway) is skipped, not failed --
   matching netload's verify. */
static bool
net_verify_reachable(const AxlNetAutoOpts *opts, const uint8_t *mac)
{
    if (opts->verify != AXL_NET_VERIFY_REACHABLE) {
        return true;
    }
    bool ok = true;
    size_t rtt = 0;

    if (opts->ping_ipv4 != NULL) {
        AxlIPv4Address tgt = { 0 };
        axl_memcpy(tgt.addr, opts->ping_ipv4, 4);
        if (axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
            ok = false;
        }
    }
    if (opts->ping_gateway) {
        uint8_t gw[4] = { 0 };
        bool    have_gw = false;
        AxlNetInterface ifs[8];
        size_t n = sizeof(ifs) / sizeof(ifs[0]);
        if (mac != NULL && axl_net_list_interfaces(ifs, &n) == AXL_OK) {
            for (size_t i = 0; i < n; i++) {
                if (axl_memcmp(ifs[i].mac, mac, 6) == 0 && ifs[i].has_ipv4) {
                    axl_memcpy(gw, ifs[i].gateway, 4);
                    have_gw = true;
                    break;
                }
            }
        }
        if (have_gw) {
            AxlIPv4Address tgt = { 0 };
            axl_memcpy(tgt.addr, gw, 4);
            if (axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
                ok = false;
            }
        }
    }
    if (opts->resolve_host != NULL && opts->resolve_host[0] != '\0') {
        AxlIPv4Address ra = { 0 };
        if (axl_net_resolve(opts->resolve_host, &ra) != AXL_OK) {
            ok = false;
        }
    }
    return ok;
}

#define NET_SWEEP_MAX 64

typedef struct {
    char name[AXL_NET_DRVQ_NAME_MAX];   ///< basename (breadcrumb/quarantine/callback key)
    char path[256];                     ///< full path handed to axl_net_try_driver
} NetSweepEntry;

/* iPXE candidates sort AFTER every non-iPXE one (iPXE's LoadImage hook breaks
   later loads in the same session), else plain byte order for determinism. */
static int
net_sweep_cmp(const void *a, const void *b)
{
    const NetSweepEntry *ea = (const NetSweepEntry *)a;
    const NetSweepEntry *eb = (const NetSweepEntry *)b;
    bool ia = axl_net_driver_is_ipxe(ea->name);
    bool ib = axl_net_driver_is_ipxe(eb->name);
    if (ia != ib) {
        return ia ? 1 : -1;
    }
    return axl_strcmp(ea->name, eb->name);
}

/* Fill @out with the drivers to sweep, sorted iPXE-last. A NULL @dir uses the
   default search path (axl_net_list_available_drivers, all volumes' drivers dir);
   an explicit @dir is scanned for *.efi. Returns the count (capped at @max). */
static size_t
net_sweep_enumerate(const char *dir, NetSweepEntry *out, size_t max)
{
    size_t n = 0;
    if (dir == NULL) {
        size_t nf = 0;
        if (axl_net_list_available_drivers(NULL, &nf) == AXL_OK && nf > 0) {
            AxlNetDriverFile *files = axl_calloc(nf, sizeof *files);
            if (files != NULL) {
                size_t got = nf;
                if (axl_net_list_available_drivers(files, &got) == AXL_OK) {
                    for (size_t i = 0; i < got && n < max; i++) {
                        axl_strlcpy(out[n].name, files[i].name, sizeof out[n].name);
                        axl_strlcpy(out[n].path, files[i].path, sizeof out[n].path);
                        n++;
                    }
                }
                axl_free(files);
            }
        }
    } else {
        AxlDir *d = axl_dir_open(dir);
        if (d != NULL) {
            AxlFsEntry e;
            while (n < max && axl_dir_read(d, &e)) {
                const char *ext = axl_path_extension(e.name);
                if (e.name[0] == '.' || ext == NULL || axl_strcasecmp(ext, "efi") != 0) {
                    continue;
                }
                if (axl_strlen(e.name) >= sizeof out[n].name) {
                    continue;   /* a truncated name wouldn't resolve back to the file */
                }
                axl_strlcpy(out[n].name, e.name, sizeof out[n].name);
                axl_snprintf(out[n].path, sizeof out[n].path, "%s\\%s", dir, e.name);
                n++;
            }
            axl_dir_close(d);
        }
    }
    axl_qsort(out, n, sizeof *out, net_sweep_cmp);
    return n;
}

/* Shared driver-dependency sidecar filename (historically netload's; kept stable
   so deployed boxes' sidecars keep working when netload rides this engine). */
#define NET_SWEEP_DEPS_FILE   "netload-drivers.json5"
#define NET_DEP_TRIED_MAX     16

/* Co-load context for a candidate's declared dependencies (axl_driver_deps_walk
   visitor state): the dir to load from, the crash guard, and a small
   already-attempted set so a dependency shared by several candidates loads once. */
typedef struct {
    const AxlNetAutoOpts *opts;   /* for the on_driver hook */
    const char *dir;
    AxlAttempt *guard;   /* NULL if unguarded */
    char        tried[NET_DEP_TRIED_MAX][AXL_NET_DRVQ_NAME_MAX];
    size_t      ntried;
} NetDepCtx;

static bool
net_dep_seen(NetDepCtx *c, const char *dep)
{
    for (size_t i = 0; i < c->ntried; i++) {
        if (axl_strcmp(c->tried[i], dep) == 0) {
            return true;
        }
    }
    if (c->ntried < NET_DEP_TRIED_MAX) {
        axl_strlcpy(c->tried[c->ntried++], dep, AXL_NET_DRVQ_NAME_MAX);
    }
    return false;
}

/* axl_driver_deps_walk enter hook: skip a dep already attempted this sweep or on
   the shared quarantine list. */
static bool
net_dep_enter(const char *dep, const char *parent, void *ctx)
{
    (void)parent;
    NetDepCtx *c = (NetDepCtx *)ctx;
    if (net_dep_seen(c, dep)) {
        return false;
    }
    if (c->guard != NULL && axl_attempt_is_quarantined(c->guard, dep)) {
        return false;
    }
    return true;
}

/* axl_driver_deps_walk load hook: breadcrumb + load+start a dependency so the
   candidate that needs it can bind. Leaves it resident on success. */
static void
net_dep_load(const char *dep, const char *parent, void *ctx)
{
    (void)parent;
    NetDepCtx *c = (NetDepCtx *)ctx;
    char path[256];
    axl_snprintf(path, sizeof path, "%s\\%s", c->dir, dep);
    if (c->guard != NULL) {
        axl_attempt_begin(c->guard, dep);
    }
    AxlDriverHandle h = NULL;
    int rc = axl_driver_load(path, &h);
    bool ok = false;
    if (rc == 0 && axl_driver_start(h) == 0) {
        axl_driver_connect(h);
        ok = true;
    } else if (h != NULL) {
        axl_driver_unload(h);
    }
    if (c->guard != NULL) {
        axl_attempt_end(c->guard);
    }
    if (c->opts->on_driver != NULL) {
        AxlNetDriverEvent ev = {
            .driver = dep,
            .outcome = ok ? AXL_NET_DRV_EV_UP : AXL_NET_DRV_EV_LOAD_FAIL,
            .is_dependency = true,
        };
        c->opts->on_driver(&ev, c->opts->on_driver_ctx);
    }
}

static bool
net_sweep_dir(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
              AxlIPv4Address *addr_out, AxlNetBringUpResult *res)
{
    NetSweepEntry *entries = axl_calloc(NET_SWEEP_MAX, sizeof *entries);
    if (entries == NULL) {
        return false;
    }
    size_t n = net_sweep_enumerate(opts->sweep_dir, entries, NET_SWEEP_MAX);

    AxlAttempt guard;
    bool guarded = (net_drvq_init(&guard) == AXL_OK);
    if (guarded) {
        /* Heal any prior boot's crash first: a breadcrumb that outlived its load
           names the driver that RSOD'd the box, which moves to quarantine here so
           the walk below skips it. */
        char culprit[AXL_NET_DRVQ_NAME_MAX];
        axl_attempt_recover(&guard, culprit, sizeof culprit);
    }

    /* Optional dependency co-load: an explicit sweep_dir's sidecar declares which
       candidates need a companion loaded first (USB-RNDIS/CDC). The multi-volume
       default path (sweep_dir == NULL) has no single sidecar, so deps apply only
       to an explicit dir. */
    NetDepCtx dep_ctx = { .opts = opts, .dir = opts->sweep_dir,
                          .guard = guarded ? &guard : NULL, .ntried = 0 };
    AxlDriverDeps deps = { 0 };   /* fixed-size, no cleanup */
    bool have_deps = false;
    if (opts->load_deps && opts->sweep_dir != NULL
        && axl_driver_deps_load(opts->sweep_dir, NET_SWEEP_DEPS_FILE, "netload", &deps)
               == AXL_SIDECAR_OK) {
        have_deps = true;
    }

    bool won = false;
    for (size_t i = 0; i < n && !won; i++) {
        const char *name = entries[i].name;

        /* A dependency-only driver is co-loaded on demand, never swept as a
           candidate -- mirror netload's candidate filter. */
        if (have_deps && axl_driver_deps_is_required(&deps, name)) {
            continue;
        }

        if (guarded && axl_attempt_is_quarantined(&guard, name)) {
            net_sweep_emit(opts, name, AXL_NET_DRV_EV_SKIPPED_QUAR, NULL);
            res->drivers_quarantined++;
            continue;
        }

        /* Co-load declared dependencies BEFORE this candidate. */
        if (have_deps) {
            AxlDriverDepVisitor v = { net_dep_enter, net_dep_load, &dep_ctx };
            axl_driver_deps_walk(&deps, name, &v);
        }

        /* Announce the attempt BEFORE the load/connect blocks, so a UI can show
           progress and warn of a slow (USB-RNDIS) connect before it stalls. */
        net_sweep_emit(opts, name, AXL_NET_DRV_EV_TRYING, NULL);

        if (guarded) {
            axl_attempt_begin(&guard, name);   /* durable breadcrumb BEFORE the risky load */
        }
        AxlNetTryResult tr;
        int try_rc = axl_net_try_driver(entries[i].path, &tr);
        if (guarded) {
            axl_attempt_end(&guard);           /* survived */
        }
        res->drivers_tried++;

        if (try_rc != AXL_OK || tr.bound_nic_count == 0) {
            /* not found / load failed / bound no NIC -- try_driver already
               unloaded a non-binder; nothing resident to drop. */
            net_sweep_emit(opts, name,
                           tr.loaded ? AXL_NET_DRV_EV_NO_NIC : AXL_NET_DRV_EV_LOAD_FAIL,
                           NULL);
            axl_free(tr.bound_nic_macs);
            continue;
        }

        /* A NIC bound. Bring it online, then (if asked) require reachability. */
        const uint8_t *first_mac = tr.bound_nic_macs[0];
        if (net_engine_assign(opts, is_static, timeout, addr_out) != AXL_OK) {
            net_sweep_emit(opts, name, AXL_NET_DRV_EV_LINK_NO_LEASE, first_mac);
        } else if (!net_verify_reachable(opts, first_mac)) {
            net_sweep_emit(opts, name, AXL_NET_DRV_EV_NO_REACH, first_mac);
        } else {
            net_sweep_emit(opts, name, AXL_NET_DRV_EV_UP, first_mac);
            axl_strlcpy(res->via, name, sizeof res->via);
            won = true;
        }
        if (!won && tr.driver != NULL) {
            /* configured-but-not-a-win -- drop this driver so the next starts clean. */
            axl_driver_unload((AxlDriverHandle)tr.driver);
        }
        axl_free(tr.bound_nic_macs);
    }

    (void)have_deps;   /* AxlDriverDeps is fixed-size -- nothing to free */
    axl_free(entries);
    return won;
}

int
axl_net_auto_init_opts(const AxlNetAutoOpts *opts, AxlNetBringUpResult *out)
{
    AxlNetBringUpResult res = { 0 };

    if (opts == NULL) {
        goto fail;
    }

    /* Validate the static request BEFORE any firmware bring-up, so a malformed
       static config is rejected even when some NIC is already up (never answer
       "online" via a stray DHCP lease to a caller that asked for static). */
    bool is_static = (opts->ip_mode == AXL_NET_IP_STATIC);
    if (is_static && (opts->static_ipv4 == NULL || opts->static_mask == NULL)) {
        goto fail;
    }
    size_t timeout = (opts->dhcp_timeout_sec > 0) ? opts->dhcp_timeout_sec : 10;
    AxlIPv4Address addr;

    /* Firmware-first (default): bind NIC drivers the firmware already staged, then
       try to configure. On a server whose vendor ships onboard NIC drivers this
       alone brings a NIC up with nothing loaded. Reported as the "firmware" row.
       A caller that ran its own firmware-first pass sets skip_firmware_first. */
    if (!opts->skip_firmware_first) {
        axl_driver_connect(NULL);
        _axl_net_connect_snp_handles();
        if (net_engine_assign(opts, is_static, timeout, &addr) == AXL_OK) {
            AxlNetBringUpResult fw = { 0 };
            net_result_identify(&fw, addr.addr);
            const uint8_t *fwmac = fw.have_nic ? fw.mac : NULL;
            if (net_verify_reachable(opts, fwmac)) {
                net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_UP, fwmac);
                axl_strlcpy(res.via, "firmware", sizeof res.via);
                goto online;
            }
            /* Configured but unreachable -- report it and keep looking. */
            net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_REACH, fwmac);
        } else {
            net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_NIC, NULL);
        }
    }

    /* Firmware-first didn't satisfy the request -- acquire more drivers. */
    if (opts->driver_strategy == AXL_NET_DRV_SWEEP_DIR) {
        if (net_sweep_dir(opts, is_static, timeout, &addr, &res)) {
            goto online;   /* res.via set to the winning driver */
        }
    } else if (opts->driver_strategy == AXL_NET_DRV_CURATED) {
        axl_net_ensure_drivers();
        _axl_net_connect_snp_handles();
        if (net_engine_assign(opts, is_static, timeout, &addr) == AXL_OK) {
            axl_strlcpy(res.via, "curated", sizeof res.via);
            goto online;
        }
    }
    /* AXL_NET_DRV_FIRMWARE_ONLY: the firmware-first attempt was the whole job. */

fail:
    if (out != NULL) {
        *out = res;   /* zeroed: online=false */
    }
    return AXL_ERR;

online:
    res.online = true;
    axl_memcpy(res.ipv4, addr.addr, 4);
    net_result_identify(&res, addr.addr);
    if (out != NULL) {
        *out = res;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_net_takeover_if_no_snp — orchestrated NIC takeover, gated on zero SNP
// ---------------------------------------------------------------------------

/* Disconnect the firmware drivers from every network-class PCI controller and
   let AXL's staged drivers bind instead. Returns the number of controllers
   re-driven. Network class = PCI base class 0x02; find_by_class matches the
   full 24-bit class, so we wildcard-enumerate and filter on the base byte to
   catch every network subclass (Ethernet, etc.). */
static size_t
takeover_pci_nics(void)
{
    size_t taken = 0;
    AxlPciAddr addr;
    for (uint16_t nth = 0;
         axl_pci_find_by_class(0xFFFFFFu, nth, &addr) == AXL_OK;
         nth++)
    {
        uint32_t class_code = 0;
        if (axl_pci_get_class_code(addr, &class_code) != AXL_OK) {
            continue;
        }
        if (((class_code >> 16) & 0xFFu) != AXL_PCI_CLASS_NETWORK) {
            continue;
        }
        AxlHandle controller = NULL;
        if (axl_pci_to_handle(addr, &controller) != AXL_OK || controller == NULL) {
            continue;
        }
        /* Detach whatever (proprietary) driver the firmware bound, then let any
           applicable staged driver take it. axl_driver_connect(NULL) below does
           the recursive stack buildup (MNP/IP4/TCP4/Dhcp4 on the new SNP). */
        axl_driver_disconnect_handle(controller);
        axl_driver_connect_handle(controller);
        taken++;
    }
    return taken;
}

int
axl_net_takeover_if_no_snp(void)
{
    /* Guard: a takeover destroys a working firmware stack, so only act when the
       firmware provides NO SimpleNetwork of its own. If SNP is already present,
       this is a safe no-op. */
    if (net_count_snp() > 0) {
        axl_debug("takeover: SNP already present (%zu) - no-op", net_count_snp());
        return AXL_OK;
    }

    /* First try the non-destructive path: load staged drivers + connect. On
       many boxes this alone brings SNP up without disconnecting anything. */
    axl_net_ensure_drivers();
    _axl_net_connect_snp_handles();
    if (net_count_snp() > 0) {
        axl_debug("takeover: SNP came up via staged drivers (no disconnect)");
        return AXL_OK;
    }

    /* Still zero SNP: take over the network-class PCI controllers from the
       firmware drivers, then rebuild the stack. */
    size_t taken = takeover_pci_nics();
    axl_driver_connect(NULL);             /* recursive connect-all stack buildup */
    _axl_net_connect_snp_handles();

    size_t snp_after = net_count_snp();
    axl_debug("takeover: re-drove %zu NIC(s), SNP handles now %zu",
              taken, snp_after);
    return (snp_after > 0) ? AXL_OK : AXL_ERR;
}
