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

/* What one bring-up may spend: an overall deadline every wait inside it is cut
   to, and a token that ends any of those waits. Both optional: NULL (or a zero
   deadline and no token) is the unbounded bring-up the older entry points
   make, whose waits keep their own fixed timeouts. Firmware calls that block
   (driver connects, the IP4Config2-free ladder's DHCP) are not waits, so
   neither can cut them short. axl_net_auto_init_opts is the one bring-up that
   sets them (AxlNetAutoOpts.timeout_sec / cancel). */
typedef struct {
    AxlCancellable *cancel;       ///< ends a wait with AXL_CANCELLED, or NULL
    uint64_t        deadline_ms;  ///< axl_time_get_ms() limit, or 0 for none
} NetBudget;

/* axl_net_auto_init's real body, defined far below beside the rest of the DHCP
   machinery; forward-declared because axl_net_bring_up (near the top of this
   file, beside the other one-call entry points) is its other caller. @p used is
   NOT optional -- the body always writes it, which is what lets every exit be a
   plain return. */
static int
auto_init_body(
    size_t           nic_index,
    size_t           dhcp_timeout_sec,
    ConfiguredNic   *used
);

/* The DHCP-configure tail of auto_init_body: set the IP4Config2 DHCP policy and
   wait for a lease (or fall down the IP4Config2-free ladder), and record which
   NIC that landed on. Extracted so a caller that ran its OWN driver acquisition
   (the axl_net_auto_init_opts SWEEP_DIR / FIRMWARE_ONLY paths) can configure DHCP
   WITHOUT the curated-list load axl_net_drivers_up performs. Callers pass an
   already-defaulted, non-zero @p timeout (a 0 would mean a 0-second DHCP poll). */
static int
net_dhcp_configure(
    size_t           nic_index,
    size_t           timeout,
    ConfiguredNic   *used,
    const NetBudget *budget
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

/* Every bring-up that leaves a NIC configured ends here, so a tool from before
   UEFI 2.4 (the old EFI 1.x shell's ifconfig) sees the address too. Best
   effort: the NIC is up whether or not the compatibility driver loads (Secure
   Boot refuses its unsigned image, for one), and the bring-up's result says so
   already -- so nothing here can turn success into failure. */
static void
net_publish_legacy_ip4cfg(void)
{
    if (axl_net_legacy_ip4cfg_install() != AXL_OK) {
        axl_debug("pre-UEFI-2.4 IPv4 config protocols not published");
    }
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
            axl_debug("bring_up: no IP4Config2 for nic %zu", nic_index);
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

    net_publish_legacy_ip4cfg();
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
        axl_debug("failed to set static policy: %llx",
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
        axl_debug("failed to set manual address: %llx",
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

    net_publish_legacy_ip4cfg();
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
        axl_debug("set_static_ip: no IP4Config2 for nic %zu", nic_index);
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
        axl_debug("set_static_ip_by_mac: no IP4Config2 NIC carrying %s", macbuf);
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
        axl_debug("set_dns: SetData(DnsServer) failed: 0x%llx "
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

/* The NIC drivers axl_net_ensure_drivers looks for: USB-CDC ECM/NCM/RNDIS
 * for BMC virtual NICs and USB dongles, plus Realtek/Asix vendor UNDI
 * drivers. ~20-200 KB each, and safe to load unconditionally -- they only
 * bind controllers they recognize.
 *
 * NO iPXE driver is on it, and none is ever loaded as a fallback. Loading one
 * takes the USB host controller from the firmware, so on a BMC console the
 * virtual keyboard and the virtual-media stick -- both USB -- vanish under a
 * tool that only wanted a NIC; it also hooks LoadImage (later .efi loads in
 * the session fail) and arms a 5-minute watchdog. An iPXE driver loads only
 * when a caller names it (netload -p, axl_net_try_driver).
 *
 * Each name is silently skipped when the file isn't on any mounted volume --
 * the cost of a miss is one file existence check per candidate. */
static const char *const net_drivers_core[] = {
    "NetworkCommon.efi",
    "UsbCdcEcm.efi",
    "UsbCdcNcm.efi",
    "UsbRndis.efi",
    "RtkUndiDxe.efi",
    "RtkUsbUndiDxe.efi",
    "AsixUsbUndiDxe.efi",
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
        EFI_STATUS st = (EFI_STATUS)axl_backend_start_image((void *)drv);

        if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
            axl_warning("ensure_drivers: StartImage failed for '%s': 0x%llx",
                        path, (unsigned long long)st);
            axl_driver_unload(drv);
            axl_backend_exit_if_deferred();
            continue;
        }
        axl_backend_exit_if_deferred();

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

    /* Load the drivers + ConnectController.
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

    /* Disarm any boot-services watchdog a loaded driver may have armed.
     * None on the list is known to, but an iPXE resident from an earlier
     * explicit load does: it installs a 10-second holdoff timer that
     * re-arms a 5-minute SetWatchdogTimer continuously while it runs, and
     * only disarms when chaining to an OS -- the host resets ~5 minutes
     * later. Verified on an AMD-EPYC server 2026-05-04 (iPXE source:
     * src/interface/efi/efi_watchdog.c). Disarming here is unconditionally
     * safe -- no axl-sdk caller of this function relies on the watchdog
     * being armed. */
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

/* net_dhcp_configure's DHCP-completion cond: has THE IP4Config2 instance we
   set to DHCP (@p ctx) got a station address -- re-read on every wakeup.

   Per-instance, never "does ANY NIC have an address": that was the old
   condition, and with a second NIC already up it held before the wait began,
   so the wait returned at once and the read-back of the NIC we configured
   found no address yet. netload then reported "no DHCP lease in 15s" after
   ~0 s, and a NIC whose lease was milliseconds away failed whenever the guest
   was slow (an iPXE NIC under host load). The predicate is the one the
   read-back applies (StationAddress != 0 on this IP4Config2), so the wait and
   the read-back cannot disagree about the same NIC. */
static bool
ip4cfg_addr_acquired(void *ctx)
{
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    uint8_t station[4] = { 0 };
    return read_station_address((EFI_IP4_CONFIG2_PROTOCOL *)ctx, station) == AXL_OK
        && axl_memcmp(station, zero4, 4) != 0;
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

/* @p cap_us, cut to what is left of @p budget; 0 when the deadline has
   passed. */
static uint64_t
budget_us(const NetBudget *budget, uint64_t cap_us)
{
    if (budget == NULL || budget->deadline_ms == 0) {
        return cap_us;
    }
    uint64_t now = axl_time_get_ms();
    if (now >= budget->deadline_ms) {
        return 0;
    }
    uint64_t left = (budget->deadline_ms - now) * 1000ULL;
    return left < cap_us ? left : cap_us;
}

/* axl_net_drivers_up's body: the wait's own status, so auto_init stops on a
   Ctrl-C (AXL_CANCELLED) instead of going on to DHCP. */
static AxlStatus
net_drivers_up(void)
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
        5ULL * 1000ULL * 1000ULL);
    if (rc != AXL_OK) {
        axl_debug("no link detected on any NIC");
    }
    return rc;
}

int
axl_net_drivers_up(void)
{
    return net_drivers_up() == AXL_OK ? AXL_OK : AXL_ERR;
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

    /* No link is not the end -- auto_init still continues (older callers
       rely on the DHCP poll being attempted even on no-link paths to surface
       a clearer error). A cancelled wait is: the caller asked to stop. */
    if (net_drivers_up() == AXL_CANCELLED) {
        return AXL_CANCELLED;
    }

    return net_dhcp_configure(nic_index, timeout, used, NULL);
}

static int
net_dhcp_configure(size_t nic_index, size_t timeout, ConfiguredNic *used,
                   const NetBudget *budget)
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
        /* The ladder blocks in firmware: never started past the deadline. */
        if (budget_us(budget, 1) == 0) {
            return AXL_TIMEOUT;
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
        axl_debug("no IP4Config2 and no DHCP4-SB/PXE fallback succeeded");
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

    /* A bounded bring-up never starts DHCP once its time is up or it has
       been cancelled: the firmware would go on asking after the caller has
       reported failure. */
    if (budget != NULL && axl_cancellable_is_cancelled(budget->cancel)) {
        return AXL_CANCELLED;
    }
    if (budget_us(budget, 1) == 0) {
        return AXL_TIMEOUT;
    }

    /* The policy as found, so a bounded bring-up that fails can put it back
       (0 = not read: nothing to restore). */
    uint32_t old_policy  = 0;
    size_t   policy_size = sizeof(old_policy);
    bool     have_old    = budget != NULL
        && axl_efi_call(ip4cfg->GetData, 4, ip4cfg, Ip4Config2DataTypePolicy,
                        &policy_size, &old_policy) == EFI_SUCCESS
        && policy_size == sizeof(old_policy);

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

    /* A deadline already passed still checks once: the condition's fast
       path answers an address that is already there. */
    uint64_t  wait_us = budget_us(budget, timeout * 1000ULL * 1000ULL);
    AxlStatus wait_rc = _axl_event_wait_timeout_with_tick(
        (AxlEventHandle)cfg_event,
        ip4cfg_addr_acquired, ip4cfg,
        NULL, NULL,
        1000ULL * 1000ULL,             /* tick_us = 1 s — same fallback cadence */
        budget != NULL ? budget->cancel : NULL,
        wait_us > 0 ? wait_us : 1);    /* timeout_us */

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

    /* A bounded bring-up that gives up stops the DHCP it started: the
       policy goes back to what it was (Static stops the asking). */
    if (have_old && old_policy != policy) {
        EFI_STATUS st = axl_efi_call(ip4cfg->SetData, 4, ip4cfg, Ip4Config2DataTypePolicy,
                                     (size_t)sizeof(old_policy), &old_policy);
        axl_debug("DHCP gave up: policy %u restored (0x%llx)", (unsigned)old_policy,
                  (unsigned long long)st);
    }
    if (wait_rc == AXL_CANCELLED) {
        axl_debug("DHCP wait cancelled");
        return AXL_CANCELLED;
    }
    axl_debug("DHCP timeout after %llu seconds",
              (unsigned long long)((wait_us + 999999ULL) / 1000000ULL));
    return wait_rc == AXL_TIMEOUT ? AXL_TIMEOUT : AXL_ERR;
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
    if (auto_init_body(nic_index, dhcp_timeout_sec, &used) != AXL_OK) {
        return AXL_ERR;
    }
    net_publish_legacy_ip4cfg();
    return AXL_OK;
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

/* What is left of @p budget: AXL_OK to go on, AXL_CANCELLED when its
   cancellable fired, AXL_TIMEOUT when its deadline passed. A Ctrl-C the
   cancellable claims (axl_cancellable_set_cancel_on_break) is seen only by a
   wait, so a cancellable is polled with a 1 ms one: that observes a break
   latched while a driver was loading -- the firmware call no wait covers. */
static int
net_budget_left(const NetBudget *budget)
{
    if (budget == NULL) {
        return AXL_OK;
    }
    if (budget->cancel != NULL
        && (axl_cancellable_is_cancelled(budget->cancel)
            || axl_wait_ms(budget->cancel, 1) == AXL_CANCELLED)) {
        return AXL_CANCELLED;
    }
    if (budget->deadline_ms != 0 && axl_time_get_ms() >= budget->deadline_ms) {
        return AXL_TIMEOUT;
    }
    return AXL_OK;
}

/* Assign an address to NIC @p eff (a list_interfaces ordinal, or
   AXL_NET_NIC_AUTO) WITHOUT acquiring drivers (the caller already did
   firmware-first / a sweep): DHCP via net_dhcp_configure under @p budget, or
   static via the shared static_ip_apply, then read the address back through the
   same registry build that configured it. AXL_OK; AXL_CANCELLED; AXL_TIMEOUT
   when the deadline cut a DHCP wait short and it ran out; or AXL_ERR
   (unresolvable NIC, a failed set, no lease in the DHCP's own time). */
static int
net_assign_on(const AxlNetAutoOpts *opts, bool is_static, size_t timeout, size_t eff,
              const NetBudget *budget, AxlIPv4Address *addr_out)
{
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
           lease on the first poll (0/1 == a single attempt) -- never past a
           cancel or the deadline. */
        size_t   tries   = (opts->dhcp_retries > 0) ? opts->dhcp_retries : 1;
        uint64_t full_us = (uint64_t)timeout * 1000ULL * 1000ULL;
        int      rc      = AXL_ERR;
        for (size_t t = 0; t < tries && rc != AXL_OK; t++) {
            if (t > 0 && net_budget_left(budget) != AXL_OK) {
                break;
            }
            /* A wait the deadline cut short that then timed out IS the
               deadline, said here rather than read off the clock after: the
               wait's own timer can fire a millisecond before the deadline,
               which would let a sweep start that the deadline forbids. */
            bool cut = budget_us(budget, full_us) < full_us;
            rc = net_dhcp_configure(eff, timeout, &used, budget);
            if (rc == AXL_CANCELLED || (rc == AXL_TIMEOUT && cut)) {
                return rc;
            }
        }
        if (rc != AXL_OK) {
            return AXL_ERR;
        }
    }
    return configured_nic_address(&used, addr_out) == AXL_OK ? AXL_OK : AXL_ERR;
}

/* Assign an address to the NIC opts->nic_select resolves (the ungated path). */
static int
net_engine_assign(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
                  const NetBudget *budget, AxlIPv4Address *addr_out)
{
    size_t eff;
    if (!net_engine_nic(opts, &eff)) {
        return AXL_ERR;   /* SEL_MAC target not present */
    }
    return net_assign_on(opts, is_static, timeout, eff, budget, addr_out);
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
   from the registry. @p embedded marks a driver from AxlNetAutoOpts.embedded. */
static void
net_sweep_emit_ex(const AxlNetAutoOpts *opts, const char *driver,
                  AxlNetDriverOutcome outcome, const uint8_t *mac, bool embedded)
{
    if (opts->on_driver == NULL) {
        return;
    }
    AxlNetDriverEvent ev = { .driver = driver, .outcome = outcome, .is_embedded = embedded };
    if (mac != NULL) {
        ev.have_nic = true;
        axl_memcpy(ev.mac, mac, 6);
        net_event_nic_state(&ev, mac);
    }
    opts->on_driver(&ev, opts->on_driver_ctx);
}

/* net_sweep_emit_ex for a driver that is not embedded (a file, "firmware"). */
static void
net_sweep_emit(const AxlNetAutoOpts *opts, const char *driver,
               AxlNetDriverOutcome outcome, const uint8_t *mac)
{
    net_sweep_emit_ex(opts, driver, outcome, mac, false);
}

/* The installed verifier's answer for the configured NIC @mac; true with none. */
static bool
net_verified(const AxlNetAutoOpts *opts, const uint8_t *mac)
{
    return opts->verifier == NULL || opts->verifier(opts, mac);
}

bool
axl_net_verify_reachable(const AxlNetAutoOpts *opts, const uint8_t *mac)
{
    if (opts == NULL) {
        return false;
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
        /* A check with no usable target (no known gateway) is skipped, not
           failed -- matching netload's verify. */
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

// ---------------------------------------------------------------------------
// The link gate (AxlNetAutoOpts.link_wait_sec)
// ---------------------------------------------------------------------------

/* The NICs one gated pass may use: those the selection allows and, in a swept
   pass, that the candidate bound (@c macs NULL in the firmware pass). */
typedef struct {
    const AxlNetAutoOpts *opts;
    const uint8_t       (*macs)[6];   ///< the candidate's NICs, or NULL for any
    size_t                nmacs;
    bool                  embedded;   ///< the candidate is an embedded driver
} NetGate;

/* How one gated pass ended. */
typedef enum {
    NET_GATE_UP = 0,      ///< a NIC got an address (and verified)
    NET_GATE_NO_LINK,     ///< no NIC it may use had a link
    NET_GATE_NO_ADDR,     ///< a link-up NIC got no address
    NET_GATE_NO_REACH,    ///< a link-up NIC got one but failed the verifier
    NET_GATE_STOPPED,     ///< cancelled, or the deadline passed
} NetGateEnd;

static bool
net_gate_allows(const NetGate *g, size_t idx, const uint8_t mac[6])
{
    if (g->macs != NULL) {
        bool bound = false;
        for (size_t k = 0; k < g->nmacs && !bound; k++) {
            bound = axl_memcmp(g->macs[k], mac, 6) == 0;
        }
        if (!bound) {
            return false;
        }
    }
    switch (g->opts->nic_select) {
        case AXL_NET_NIC_SEL_INDEX:
            return idx == g->opts->nic_index;
        case AXL_NET_NIC_SEL_MAC:
            return axl_memcmp(g->opts->nic_mac, mac, 6) == 0;
        case AXL_NET_NIC_SEL_AUTO:
        default:
            return true;
    }
}

/* True when the firmware has an IP4Config2 at all (else DHCP goes down the
   NIC-agnostic Dhcp4-SB / PXE ladder, which cannot race). */
static bool
net_have_ip4cfg2(void)
{
    void  **h = NULL;
    size_t  n = 0;
    bool    have = axl_protocol_enumerate("ip4-config2", &h, &n) == AXL_OK && n > 0;
    axl_free(h);
    return have;
}

/* The link wait's condition: a NIC the pass may use has a link. */
static bool
net_gate_link_up(void *ctx)
{
    const NetGate   *g   = (const NetGate *)ctx;
    AxlNetInterface *ifs = NULL;
    size_t           n   = 0;
    bool             up  = false;

    if (axl_net_list_interfaces_alloc(&ifs, &n) != AXL_OK) {
        return false;
    }
    for (size_t i = 0; i < n && !up; i++) {
        up = ifs[i].link_up && net_gate_allows(g, i, ifs[i].mac);
    }
    axl_free(ifs);
    return up;
}


// ---------------------------------------------------------------------------
// The DHCP race: every link-up NIC of a gated pass at once, first lease wins
// ---------------------------------------------------------------------------

/* One NIC in a gated pass's DHCP race. */
typedef struct {
    size_t                    idx;       ///< its list_interfaces ordinal
    uint8_t                   mac[6];
    EFI_IP4_CONFIG2_PROTOCOL *cfg;       ///< NULL: no IP4Config2, not racing
    ConfiguredNic             used;      ///< for the winner's read-back
    uint32_t                  old;       ///< its policy as found
    bool                      have_old;  ///< @c old was read
    bool                      out;       ///< rejected by the verifier: out of the race
    void                     *manual;    ///< static as found: its ManualAddress, or NULL
    size_t                    manual_sz;
    void                     *gw;        ///< ... its Gateway list, or NULL
    size_t                    gw_sz;
    void                     *dns;       ///< ... its DnsServer list, or NULL
    size_t                    dns_sz;
} NetRacer;

typedef struct {
    NetRacer *r;
    size_t    n;
} NetRace;

/* The race's wait condition: some NIC still in the race has an address. */
static bool
net_race_any_addr(void *ctx)
{
    const NetRace *race = (const NetRace *)ctx;
    for (size_t k = 0; k < race->n; k++) {
        if (race->r[k].cfg != NULL && !race->r[k].out
            && ip4cfg_addr_acquired(race->r[k].cfg)) {
            return true;
        }
    }
    return false;
}

/* Put @p r back as it was found, which stops its DHCP: the policy, and for a
   NIC that was static its ManualAddress, Gateway and DnsServer, which EDK2
   frees whenever the policy changes. Settings it had none of stay none. A
   NIC that was already DHCP is left so. Clears @p r's snapshot, so a second
   call does nothing. */
static void
net_racer_restore(NetRacer *r)
{
    static const uint32_t dhcp = 1;
    if (r->cfg == NULL || !r->have_old || r->old == dhcp) {
        return;
    }
    uint32_t   old = r->old;
    EFI_STATUS st  = axl_efi_call(r->cfg->SetData, 4, r->cfg, Ip4Config2DataTypePolicy,
                                  (size_t)sizeof(old), &old);
    axl_debug("DHCP race: NIC %zu policy %u restored (0x%llx)", r->idx, (unsigned)old,
              (unsigned long long)st);
    if (r->manual != NULL) {
        axl_efi_call(r->cfg->SetData, 4, r->cfg, Ip4Config2DataTypeManualAddress,
                     r->manual_sz, r->manual);
    }
    if (r->gw != NULL) {
        axl_efi_call(r->cfg->SetData, 4, r->cfg, Ip4Config2DataTypeGateway, r->gw_sz, r->gw);
    }
    if (r->dns != NULL) {
        axl_efi_call(r->cfg->SetData, 4, r->cfg, Ip4Config2DataTypeDnsServer,
                     r->dns_sz, r->dns);
    }
    r->have_old = false;
}

/* Free @p r's snapshot of its static settings. */
static void
net_racer_free(NetRacer *r)
{
    axl_backend_free(r->manual);
    axl_backend_free(r->gw);
    axl_backend_free(r->dns);
    r->manual = r->gw = r->dns = NULL;
}

/* Set the DHCP policy on every racer still in the race. A racer that is
   already DHCP gets the same policy again: EDK2 restarts a DHCP that has not
   succeeded on a same-policy SetData, which is what a retry needs. */
static void
net_race_start(NetRace *race)
{
    static const uint32_t dhcp = 1;
    for (size_t k = 0; k < race->n; k++) {
        NetRacer *r = &race->r[k];
        if (r->cfg == NULL || r->out) {
            continue;
        }
        uint32_t   policy = dhcp;
        EFI_STATUS st     = axl_efi_call(r->cfg->SetData, 4, r->cfg, Ip4Config2DataTypePolicy,
                                         (size_t)sizeof(policy), &policy);
        if (EFI_ERROR(st)) {
            axl_warning("failed to set DHCP policy on NIC %zu: %llx", r->idx,
                        (unsigned long long)st);
        }
    }
}

/* Take @p r into the race: read its policy, and when it is static, snapshot
   the settings the switch to DHCP would discard. */
static void
net_racer_enter(NetRacer *r)
{
    size_t sz   = sizeof(r->old);
    r->have_old = axl_efi_call(r->cfg->GetData, 4, r->cfg, Ip4Config2DataTypePolicy,
                               &sz, &r->old) == EFI_SUCCESS && sz == sizeof(r->old);
    if (r->have_old && r->old == 0) {
        r->manual = ip4cfg_getdata(r->cfg, Ip4Config2DataTypeManualAddress, &r->manual_sz);
        r->gw     = ip4cfg_getdata(r->cfg, Ip4Config2DataTypeGateway, &r->gw_sz);
        r->dns    = ip4cfg_getdata(r->cfg, Ip4Config2DataTypeDnsServer, &r->dns_sz);
    }
}

/* True while some racer is still in the race (has an IP4Config2 and was not
   rejected): once none is, no wait or retry can produce a winner. */
static bool
net_race_running(const NetRace *race)
{
    for (size_t k = 0; k < race->n; k++) {
        if (race->r[k].cfg != NULL && !race->r[k].out) {
            return true;
        }
    }
    return false;
}

/* The first racer, in ordinal order, with an address -- or NULL. */
static NetRacer *
net_race_leased(NetRace *race)
{
    for (size_t k = 0; k < race->n; k++) {
        NetRacer *r = &race->r[k];
        if (r->cfg != NULL && !r->out && ip4cfg_addr_acquired(r->cfg)) {
            return r;
        }
    }
    return NULL;
}

/* DHCP on every NIC in @p up (ordinals into @p ifs) AT ONCE, under one DHCP
   budget (@p timeout s, cut to @p budget; retried per opts->dhcp_retries), as
   Linux's in-kernel ip=dhcp, systemd-networkd and dhcpcd do. The first NIC to
   lease that the verifier accepts wins; a NIC it rejects is put back and
   dropped from the race, which goes on waiting for the others in the same
   budget. Every NIC but the winner is put back as it was found (see
   net_racer_restore). CONFIGURING fires for each NIC before the wait.
   Returns as net_gate_pass. */
static NetGateEnd
net_gate_race(const AxlNetAutoOpts *opts, size_t timeout, const NetBudget *budget,
              const char *driver, const NetGate *g, const AxlNetInterface *ifs,
              const size_t *up, size_t nup, AxlIPv4Address *addr_out, uint8_t mac_out[6],
              int *rc_out)
{
    NetRacer *racers = axl_calloc(nup, sizeof *racers);
    if (racers == NULL) {
        return NET_GATE_NO_ADDR;
    }
    NetRace    race   = { .r = racers, .n = 0 };
    NetGateEnd end    = NET_GATE_NO_ADDR;
    bool       racing = false;

    for (size_t k = 0; k < nup; k++) {
        int left = net_budget_left(budget);
        if (left != AXL_OK) {
            *rc_out = left;
            end     = NET_GATE_STOPPED;
            break;
        }
        NetRacer *r = &racers[race.n++];
        r->idx = up[k];
        axl_memcpy(r->mac, ifs[up[k]].mac, 6);
        net_sweep_emit_ex(opts, driver, AXL_NET_DRV_EV_CONFIGURING, r->mac, g->embedded);
        axl_memcpy(mac_out, r->mac, 6);
        r->cfg = ip4cfg_for_ex(r->idx, &r->used);
        if (r->cfg != NULL) {
            net_racer_enter(r);
            racing = true;
        }
    }

    NetRacer *win = NULL;
    if (end != NET_GATE_STOPPED && racing) {
        /* One event for every racer's address notification: a NIC whose
           lease is committed wakes the wait at once; the 1 s tick re-checks
           all of them on firmware that never signals. */
        AxlEventHandle ev = NULL;
        if (axl_backend_event_create(&ev) != AXL_OK) {
            ev = NULL;
        }
        bool *notified = axl_calloc(race.n, sizeof *notified);
        for (size_t k = 0; k < race.n && ev != NULL && notified != NULL; k++) {
            NetRacer *r = &racers[k];
            notified[k] = r->cfg != NULL
                && axl_efi_call(r->cfg->RegisterDataNotify, 3, r->cfg,
                                Ip4Config2DataTypeInterfaceInfo, (EFI_EVENT)ev) == EFI_SUCCESS;
        }

        size_t   tries   = (opts->dhcp_retries > 0) ? opts->dhcp_retries : 1;
        uint64_t full_us = (uint64_t)timeout * 1000ULL * 1000ULL;
        for (size_t t = 0; t < tries && win == NULL && end != NET_GATE_STOPPED; t++) {
            if (t > 0) {
                int left = net_budget_left(budget);
                if (left != AXL_OK) {
                    *rc_out = left;
                    end     = NET_GATE_STOPPED;
                    break;
                }
            }
            net_race_start(&race);
            /* ONE budget per try, however many leases the verifier rejects:
               each wait after a reject gets only what is left of it. A wait
               the deadline cut short that then timed out IS the deadline
               (see net_assign_on). */
            uint64_t try_end = axl_time_get_ms() + full_us / 1000ULL;
            for (;;) {
                uint64_t now  = axl_time_get_ms();
                uint64_t left = now < try_end ? (try_end - now) * 1000ULL : 0;
                if (left == 0) {
                    break;
                }
                uint64_t  wait_us = budget_us(budget, left);
                bool      cut     = wait_us < left;
                AxlStatus w       = _axl_event_wait_timeout_with_tick(
                    ev, net_race_any_addr, &race, NULL, NULL,
                    1000ULL * 1000ULL,             /* tick_us = 1 s */
                    budget != NULL ? budget->cancel : NULL,
                    wait_us > 0 ? wait_us : 1);
                if (w == AXL_CANCELLED || (w == AXL_TIMEOUT && cut)) {
                    *rc_out = w == AXL_CANCELLED ? AXL_CANCELLED : AXL_TIMEOUT;
                    end     = NET_GATE_STOPPED;
                    break;
                }
                if (w != AXL_OK) {
                    break;   /* this try's budget is spent */
                }
                /* Leases in ordinal order: the first the verifier accepts
                   wins; each it rejects is put back and leaves the race. */
                for (NetRacer *r = net_race_leased(&race); r != NULL && win == NULL;
                     r = net_race_leased(&race)) {
                    axl_memcpy(mac_out, r->mac, 6);
                    if (net_verified(opts, r->mac)) {
                        win = r;
                    } else {
                        end    = NET_GATE_NO_REACH;
                        r->out = true;
                        net_racer_restore(r);
                    }
                }
                if (win != NULL || !net_race_running(&race)) {
                    break;   /* a winner, or every racer rejected */
                }
            }
            if (!net_race_running(&race)) {
                break;       /* nobody left to retry: end NO_REACH now */
            }
        }

        for (size_t k = 0; k < race.n && notified != NULL; k++) {
            if (notified[k]) {
                axl_efi_call(racers[k].cfg->UnregisterDataNotify, 3, racers[k].cfg,
                             Ip4Config2DataTypeInterfaceInfo, (EFI_EVENT)ev);
            }
        }
        axl_free(notified);
        if (ev != NULL) {
            axl_backend_event_close(ev);
        }

        if (win != NULL) {
            axl_memcpy(mac_out, win->mac, 6);
            if (configured_nic_address(&win->used, addr_out) == AXL_OK) {
                g_config_method = AXL_NET_CONFIG_IP4CONFIG2;
                end             = NET_GATE_UP;
            } else {
                end = NET_GATE_NO_ADDR;
                win = NULL;   /* not a winner after all: put back below */
            }
        }
    }
    for (size_t k = 0; k < race.n; k++) {
        if (&racers[k] != win) {
            net_racer_restore(&racers[k]);
        }
        net_racer_free(&racers[k]);
    }
    axl_free(racers);
    return end;
}

/* One gated pass for @p driver: wait for a link (cut to the budget), then
   LINK_DOWN for each NIC it may use that has none, and the address step on the
   link-up ones: DHCP on all of them at once (net_gate_race), or a static
   address on each in ordinal order until one is up, CONFIGURING before each. @p *rc_out is
   AXL_CANCELLED / AXL_TIMEOUT for NET_GATE_STOPPED. On NET_GATE_UP @p addr_out
   and @p mac_out name the NIC; on NO_ADDR / NO_REACH @p mac_out names the last
   NIC tried. */
static NetGateEnd
net_gate_pass(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
              const NetBudget *budget, const char *driver, const NetGate *g,
              AxlIPv4Address *addr_out, uint8_t mac_out[6], int *rc_out)
{
    AxlNetInterface *ifs = NULL;
    size_t           n   = 0;

    /* The wait runs even when no NIC this pass may use exists yet: its
       condition lists the NICs afresh on every tick, so one whose interface
       appears during it (a USB NIC still enumerating, the ordinal -n names)
       is found. A pass that ends the wait with none ends without an event. */
    uint64_t wait_us = budget_us(budget, (uint64_t)opts->link_wait_sec * 1000000ULL);
    if (wait_us > 0) {
        AxlStatus w = _axl_event_wait_timeout_with_tick(
            NULL, net_gate_link_up, (void *)g, NULL, NULL,
            100ULL * 1000ULL,             /* tick_us = 100 ms */
            budget != NULL ? budget->cancel : NULL, wait_us);
        if (w == AXL_CANCELLED) {
            *rc_out = AXL_CANCELLED;
            return NET_GATE_STOPPED;
        }
    }

    /* Again after the wait: link states have moved. */
    if (axl_net_list_interfaces_alloc(&ifs, &n) != AXL_OK) {
        n = 0;
    }
    size_t *up  = n > 0 ? axl_calloc(n, sizeof *up) : NULL;
    size_t  nup = 0;
    for (size_t i = 0; i < n && up != NULL; i++) {
        if (!net_gate_allows(g, i, ifs[i].mac)) {
            continue;
        }
        if (!ifs[i].link_up) {
            net_sweep_emit_ex(opts, driver, AXL_NET_DRV_EV_LINK_DOWN, ifs[i].mac, g->embedded);
            continue;
        }
        up[nup++] = i;
    }

    NetGateEnd end = NET_GATE_NO_LINK;
    if (nup > 0 && !is_static && net_have_ip4cfg2()) {
        end = net_gate_race(opts, timeout, budget, driver, g, ifs, up, nup, addr_out,
                            mac_out, rc_out);
    } else {
        /* Static (or DHCP with no IP4Config2, whose fallback ladder is not
           per NIC): one NIC at a time, in ordinal order. */
        for (size_t k = 0; k < nup; k++) {
            size_t i    = up[k];
            int    left = net_budget_left(budget);
            if (left != AXL_OK) {
                *rc_out = left;
                end = NET_GATE_STOPPED;
                break;
            }
            net_sweep_emit_ex(opts, driver, AXL_NET_DRV_EV_CONFIGURING, ifs[i].mac,
                              g->embedded);
            axl_memcpy(mac_out, ifs[i].mac, 6);
            int a = net_assign_on(opts, is_static, timeout, i, budget, addr_out);
            if (a == AXL_CANCELLED || a == AXL_TIMEOUT) {
                *rc_out = a;
                end = NET_GATE_STOPPED;
                break;
            }
            if (a != AXL_OK) {
                end = NET_GATE_NO_ADDR;
                continue;
            }
            if (!net_verified(opts, mac_out)) {
                end = NET_GATE_NO_REACH;
                continue;
            }
            end = NET_GATE_UP;
            break;
        }
    }
    axl_free(up);
    axl_free(ifs);
    return end;
}

// ---------------------------------------------------------------------------
// The SWEEP_DIR sweep
// ---------------------------------------------------------------------------

#define NET_SWEEP_MAX 64

typedef struct {
    char        name[AXL_NET_DRVQ_NAME_MAX]; ///< basename (breadcrumb/quarantine/callback key)
    char        path[256];               ///< full path handed to axl_net_try_driver ("" for
                                         ///<   an embedded one)
    const void *image;                   ///< an embedded driver's image
    size_t      size;                    ///< bytes at @c image
    bool        embedded;                ///< from AxlNetAutoOpts.embedded, not a file
} NetSweepEntry;

/* The volume handle of @p dir's "fsN:" prefix, or NULL (no dir, no prefix, no
   such volume). */
static void *
net_dir_volume(const char *dir)
{
    const char *colon = dir != NULL ? axl_strchr(dir, ':') : NULL;
    AxlVolume   vols[32];
    size_t      n = 0;

    if (colon == NULL || colon == dir || (size_t)(colon - dir) >= sizeof(vols[0].name)
        || axl_volume_enumerate(vols, AXL_ARRAY_SIZE(vols), &n) != AXL_OK) {
        return NULL;
    }
    for (size_t i = 0; i < n && i < AXL_ARRAY_SIZE(vols); i++) {
        if (axl_strlen(vols[i].name) == (size_t)(colon - dir)
            && axl_strncasecmp(vols[i].name, dir, (size_t)(colon - dir)) == 0) {
            return vols[i].handle;
        }
    }
    return NULL;
}

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
   multi-volume search path (axl_net_list_available_drivers, every volume's
   drivers dir); a directory is scanned for *.efi. Returns the count (capped at
   @max). */
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

// ---------------------------------------------------------------------------
// Companion-driver co-load (AxlNetAutoOpts.deps) -- linked only by an image
// that installs axl_net_sweep_deps_sidecar()
// ---------------------------------------------------------------------------

/* Shared driver-dependency sidecar filename (historically netload's; kept stable
   so deployed boxes' sidecars keep working). */
#define NET_SWEEP_DEPS_FILE   "netload-drivers.json5"
#define NET_DEP_TRIED_MAX     16

/* The co-loader's operations, behind the opaque public handle: a function table,
   so the sweep reaches the sidecar reader only through what a caller installed. */
struct AxlNetSweepDeps {
    void *(*open)(const AxlNetAutoOpts *opts, const char *dir, AxlAttempt *guard);
    bool  (*is_dependency)(void *state, const char *name);
    void  (*co_load)(void *state, const char *candidate);
    void  (*release)(void *state, const char *winner);
    void  (*close)(void *state);
};

/* One sweep's co-load state: the parsed sidecar, the dir to load from, the crash
   guard, a small already-attempted set so a companion shared by several
   candidates loads once, and the companions this sweep loaded -- resident for
   the rest of the sweep, then unloaded (release) unless the winning driver
   needs them, so a failed bring-up leaves nothing behind. Every companion here
   is an image this sweep loaded itself: one already resident before it is
   another image, and is never unloaded. */
typedef struct {
    const AxlNetAutoOpts *opts;    /* for the on_driver hook */
    const char           *dir;
    AxlAttempt           *guard;   /* NULL if unguarded */
    AxlDriverDeps         deps;
    char                  tried[NET_DEP_TRIED_MAX][AXL_NET_DRVQ_NAME_MAX];
    size_t                ntried;
    AxlDriverHandle       held[NET_DEP_TRIED_MAX];
    char                  held_name[NET_DEP_TRIED_MAX][AXL_NET_DRVQ_NAME_MAX];
    size_t                nheld;
} NetDepState;

static bool
net_dep_seen(NetDepState *c, const char *dep)
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
    NetDepState *c = (NetDepState *)ctx;
    if (net_dep_seen(c, dep)) {
        return false;
    }
    if (c->guard != NULL && axl_attempt_is_quarantined(c->guard, dep)) {
        return false;
    }
    return true;
}

/* axl_driver_deps_walk load hook: breadcrumb + load+start a dependency so the
   candidate that needs it can bind. Leaves it resident on success, held for
   net_deps_release; one past the hold's capacity stays resident. */
static void
net_dep_load(const char *dep, const char *parent, void *ctx)
{
    (void)parent;
    NetDepState *c = (NetDepState *)ctx;
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
        if (c->nheld < NET_DEP_TRIED_MAX) {
            c->held[c->nheld] = h;
            axl_strlcpy(c->held_name[c->nheld], dep, AXL_NET_DRVQ_NAME_MAX);
            c->nheld++;
        }
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

static void *
net_deps_open(const AxlNetAutoOpts *opts, const char *dir, AxlAttempt *guard)
{
    NetDepState *c = axl_calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    if (axl_driver_deps_load(dir, NET_SWEEP_DEPS_FILE, "netload", &c->deps)
            != AXL_SIDECAR_OK) {
        axl_free(c);
        return NULL;   /* no sidecar, or one that does not parse: sweep standalone */
    }
    c->opts  = opts;
    c->dir   = dir;
    c->guard = guard;
    return c;
}

static bool
net_deps_is_dependency(void *state, const char *name)
{
    return axl_driver_deps_is_required(&((NetDepState *)state)->deps, name);
}

static void
net_deps_co_load(void *state, const char *candidate)
{
    NetDepState        *c = (NetDepState *)state;
    AxlDriverDepVisitor v = { net_dep_enter, net_dep_load, c };
    axl_driver_deps_walk(&c->deps, candidate, &v);
}

/* What the winner needs: every name the walk would load for it. */
typedef struct {
    bool  keep[NET_DEP_TRIED_MAX];   ///< parallel to NetDepState.held
    const NetDepState *c;
} NetDepKeep;

static void
net_dep_keep(const char *dep, const char *parent, void *ctx)
{
    (void)parent;
    NetDepKeep *k = (NetDepKeep *)ctx;
    for (size_t i = 0; i < k->c->nheld; i++) {
        if (axl_strcmp(k->c->held_name[i], dep) == 0) {
            k->keep[i] = true;
        }
    }
}

/* At the sweep's end: unload the companions it loaded, newest first (a
   companion's own companion was loaded before it), except those @p winner
   (the driver that brought a NIC up, or NULL for none) depends on. */
static void
net_deps_release(void *state, const char *winner)
{
    NetDepState *c = (NetDepState *)state;
    NetDepKeep   k = { .keep = { false }, .c = c };

    if (winner != NULL) {
        AxlDriverDepVisitor v = { NULL, net_dep_keep, &k };
        axl_driver_deps_walk(&c->deps, winner, &v);
    }
    for (size_t i = c->nheld; i > 0; i--) {
        if (!k.keep[i - 1]) {
            axl_driver_unload(c->held[i - 1]);
        }
    }
    c->nheld = 0;
}

static void
net_deps_close(void *state)
{
    axl_free(state);   /* AxlDriverDeps is fixed-size: nothing else to free */
}

static const AxlNetSweepDeps net_deps_sidecar = {
    net_deps_open, net_deps_is_dependency, net_deps_co_load, net_deps_release,
    net_deps_close,
};

const AxlNetSweepDeps *
axl_net_sweep_deps_sidecar(void)
{
    return &net_deps_sidecar;
}

/* The sweep. AXL_OK when a driver brought a NIC up (res->via names it),
   AXL_CANCELLED / AXL_TIMEOUT when the budget ended it, AXL_ERR when every
   candidate was tried. */
static int
net_sweep_dir(const AxlNetAutoOpts *opts, const char *dir, bool is_static, size_t timeout,
              const NetBudget *budget, AxlIPv4Address *addr_out, AxlNetBringUpResult *res)
{
    /* The order: the embedded drivers (in the caller's order), the
       directory's (sorted), then -- iPXE's LoadImage hook breaks later loads,
       so iPXE goes after every other candidate -- the embedded iPXE ones, then
       the directory's. */
    size_t         nemb    = opts->embedded != NULL ? opts->embedded_count : 0;
    NetSweepEntry *all     = axl_calloc(nemb + NET_SWEEP_MAX, sizeof *all);
    NetSweepEntry *entries = axl_calloc(nemb + NET_SWEEP_MAX, sizeof *entries);
    if (all == NULL || entries == NULL) {
        axl_free(all);
        axl_free(entries);
        return AXL_ERR;
    }
    for (size_t i = 0; i < nemb; i++) {
        const AxlNetEmbeddedDriver *e = &opts->embedded[i];
        if (e->name != NULL) {
            axl_strlcpy(all[i].name, e->name, sizeof all[i].name);
        }
        all[i].image    = e->image;
        all[i].size     = e->size;
        all[i].embedded = true;
    }
    size_t n = nemb + net_sweep_enumerate(dir, all + nemb, NET_SWEEP_MAX);
    size_t m = 0;
    for (int ipxe = 0; ipxe <= 1; ipxe++) {
        for (size_t i = 0; i < n; i++) {
            if (axl_net_driver_is_ipxe(all[i].name) == (ipxe == 1)) {
                entries[m++] = all[i];
            }
        }
    }
    axl_free(all);
    /* An embedded driver is given the sweep directory's volume, as if loaded
       from a file there, else the caller's opts->device_handle. */
    void *vol = nemb > 0 ? net_dir_volume(dir) : NULL;
    if (vol == NULL) {
        vol = opts->device_handle;
    }
    axl_debug("sweep: %zu embedded and %zu driver(s) in %s", nemb, n - nemb,
              dir != NULL ? dir : "every volume's drivers dir");

    AxlAttempt guard;
    bool guarded = (net_drvq_init(&guard) == AXL_OK);
    if (guarded) {
        /* Heal any prior boot's crash first: a breadcrumb that outlived its load
           names the driver that RSOD'd the box, which moves to quarantine here so
           the walk below skips it. */
        char culprit[AXL_NET_DRVQ_NAME_MAX];
        axl_attempt_recover(&guard, culprit, sizeof culprit);
    }

    /* Companion co-load, when installed: a single directory's sidecar declares
       which candidates need a companion loaded first (USB-RNDIS/CDC). The
       multi-volume search (dir == NULL) has no single sidecar. */
    void *dep_state = NULL;
    if (opts->deps != NULL && dir != NULL) {
        dep_state = opts->deps->open(opts, dir, guarded ? &guard : NULL);
    }

    int rc = AXL_ERR;
    for (size_t i = 0; i < n && rc == AXL_ERR; i++) {
        const char *name = entries[i].name;
        bool        emb  = entries[i].embedded;

        /* An embedded entry with no name, image or size cannot be loaded or
           keyed: say so and go on (nothing touched, no breadcrumb). */
        if (emb && (name[0] == '\0' || entries[i].image == NULL || entries[i].size == 0)) {
            net_sweep_emit_ex(opts, name[0] != '\0' ? name : "(embedded)",
                              AXL_NET_DRV_EV_LOAD_FAIL, NULL, true);
            continue;
        }

        /* A dependency-only driver is co-loaded on demand, never swept as a
           candidate -- mirror netload's candidate filter. An embedded driver
           is a candidate whatever the sidecar names: the caller handed it in
           to be tried. */
        if (!entries[i].embedded && dep_state != NULL
            && opts->deps->is_dependency(dep_state, name)) {
            continue;
        }
        if (guarded && axl_attempt_is_quarantined(&guard, name)) {
            net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_SKIPPED_QUAR, NULL, emb);
            res->drivers_quarantined++;
            continue;
        }
        int left = net_budget_left(budget);
        if (left != AXL_OK) {
            rc = left;
            break;
        }

        /* Co-load declared dependencies BEFORE this candidate. */
        if (dep_state != NULL) {
            opts->deps->co_load(dep_state, name);
        }

        /* Announce the attempt BEFORE the load/connect blocks, so a UI can show
           progress and warn of a slow (USB-RNDIS) connect before it stalls. */
        net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_TRYING, NULL, emb);

        if (guarded) {
            axl_attempt_begin(&guard, name);   /* durable breadcrumb BEFORE the risky load */
        }
        AxlNetTryResult tr;
        int try_rc = emb ? _axl_net_try_driver_image(entries[i].image, entries[i].size, name,
                                                     vol, &tr)
                         : axl_net_try_driver(entries[i].path, &tr);
        if (guarded) {
            axl_attempt_end(&guard);           /* survived */
        }
        res->drivers_tried++;

        if (try_rc != AXL_OK || tr.bound_nic_count == 0) {
            /* not found / load failed / bound no NIC -- try_driver already
               unloaded a non-binder; nothing resident to drop. */
            net_sweep_emit_ex(opts, name,
                              tr.loaded ? AXL_NET_DRV_EV_NO_NIC : AXL_NET_DRV_EV_LOAD_FAIL,
                              NULL, emb);
            axl_free(tr.bound_nic_macs);
            continue;
        }

        /* A NIC bound. Bring it online, then (if asked) verify it. */
        if (opts->link_wait_sec > 0) {
            NetGate g = { .opts = opts, .macs = tr.bound_nic_macs,
                          .nmacs = tr.bound_nic_count, .embedded = emb };
            uint8_t mac[6] = { 0 };
            int     stop   = AXL_ERR;
            switch (net_gate_pass(opts, is_static, timeout, budget, name, &g, addr_out,
                                  mac, &stop)) {
                case NET_GATE_UP:
                    net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_UP, mac, emb);
                    rc = AXL_OK;
                    break;
                case NET_GATE_NO_ADDR:
                    net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_LINK_NO_LEASE, mac, emb);
                    break;
                case NET_GATE_NO_REACH:
                    net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_NO_REACH, mac, emb);
                    break;
                case NET_GATE_NO_LINK:
                    net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_NO_NIC, NULL, emb);
                    break;
                case NET_GATE_STOPPED:
                    rc = stop;
                    break;
            }
        } else {
            const uint8_t *first_mac = tr.bound_nic_macs[0];
            int a = net_engine_assign(opts, is_static, timeout, budget, addr_out);
            if (a == AXL_CANCELLED || a == AXL_TIMEOUT) {
                rc = a;
            } else if (a != AXL_OK) {
                net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_LINK_NO_LEASE, first_mac, emb);
            } else if (!net_verified(opts, first_mac)) {
                net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_NO_REACH, first_mac, emb);
            } else {
                net_sweep_emit_ex(opts, name, AXL_NET_DRV_EV_UP, first_mac, emb);
                rc = AXL_OK;
            }
        }
        if (rc == AXL_OK) {
            axl_strlcpy(res->via, name, sizeof res->via);
        } else if (tr.driver != NULL) {
            /* configured-but-not-a-win, or stopped -- drop this driver so the
               next starts clean and a stopped bring-up leaves nothing behind. */
            axl_driver_unload((AxlDriverHandle)tr.driver);
        }
        axl_free(tr.bound_nic_macs);
    }

    if (dep_state != NULL) {
        /* Companions stay resident through the sweep (a later candidate
           reuses them); at its end, only the winner's are kept. */
        opts->deps->release(dep_state, rc == AXL_OK ? res->via : NULL);
        opts->deps->close(dep_state);
    }
    axl_free(entries);
    return rc;
}

/* The firmware-first pass: connect the drivers the firmware staged, then try to
   configure, reported as the "firmware" row. AXL_OK (res->via set), AXL_CANCELLED,
   AXL_TIMEOUT, or AXL_ERR (nothing came up). */
static int
net_firmware_pass(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
                  const NetBudget *budget, AxlIPv4Address *addr, AxlNetBringUpResult *res)
{
    net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_TRYING, NULL);
    axl_driver_connect(NULL);
    _axl_net_connect_snp_handles();

    if (opts->link_wait_sec > 0) {
        NetGate g    = { .opts = opts, .macs = NULL, .nmacs = 0 };
        uint8_t mac[6] = { 0 };
        int     stop = AXL_ERR;
        switch (net_gate_pass(opts, is_static, timeout, budget, "firmware", &g, addr,
                              mac, &stop)) {
            case NET_GATE_UP:
                net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_UP, mac);
                axl_strlcpy(res->via, "firmware", sizeof res->via);
                return AXL_OK;
            case NET_GATE_NO_ADDR:
                net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_LINK_NO_LEASE, mac);
                return AXL_ERR;
            case NET_GATE_NO_REACH:
                net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_REACH, mac);
                return AXL_ERR;
            case NET_GATE_NO_LINK:
                net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_NIC, NULL);
                return AXL_ERR;
            case NET_GATE_STOPPED:
            default:
                return stop;
        }
    }

    int a = net_engine_assign(opts, is_static, timeout, budget, addr);
    if (a == AXL_CANCELLED || a == AXL_TIMEOUT) {
        return a;
    }
    if (a != AXL_OK) {
        net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_NIC, NULL);
        return AXL_ERR;
    }
    AxlNetBringUpResult fw = { 0 };
    net_result_identify(&fw, addr->addr);
    const uint8_t *fwmac = fw.have_nic ? fw.mac : NULL;
    if (!net_verified(opts, fwmac)) {
        /* Configured but unreachable -- report it and keep looking. */
        net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_NO_REACH, fwmac);
        return AXL_ERR;
    }
    net_sweep_emit(opts, "firmware", AXL_NET_DRV_EV_UP, fwmac);
    axl_strlcpy(res->via, "firmware", sizeof res->via);
    return AXL_OK;
}

/* Room for the default sweep directory axl_net_driver_dir() names. */
#define NET_DRIVER_DIR_MAX  512

/* The validated request's budget: NULL keeps the unbounded bring-up exactly as
   it was (no policy restore, no break polling). */
static const NetBudget *
net_budget_init(const AxlNetAutoOpts *opts, NetBudget *budget)
{
    budget->cancel      = opts->cancel;
    budget->deadline_ms = opts->timeout_sec > 0
                              ? axl_time_get_ms() + (uint64_t)opts->timeout_sec * 1000ULL
                              : 0;
    return (budget->cancel != NULL || budget->deadline_ms != 0) ? budget : NULL;
}

/* Acquire drivers per opts->driver_strategy and configure, after firmware-first
   did not satisfy the request. AXL_OK (@p res->via set), AXL_ERR, AXL_CANCELLED or
   AXL_TIMEOUT. */
static int
net_acquire(const AxlNetAutoOpts *opts, bool is_static, size_t timeout,
            const NetBudget *budget, AxlIPv4Address *addr, AxlNetBringUpResult *res)
{
    int rc = net_budget_left(budget);
    if (rc != AXL_OK) {
        return rc;
    }
    if (opts->driver_strategy == AXL_NET_DRV_SWEEP_DIR) {
        /* NULL: the stick's own drivers directory when it has one, else every
           volume's drivers\<arch>. */
        char        defdir[NET_DRIVER_DIR_MAX];
        const char *dir = opts->sweep_dir;
        if (dir == NULL && axl_net_driver_dir(defdir, sizeof defdir) == AXL_OK) {
            dir = defdir;
        }
        return net_sweep_dir(opts, dir, is_static, timeout, budget, addr, res);
    }
    if (opts->driver_strategy == AXL_NET_DRV_CURATED) {
        axl_net_ensure_drivers();
        _axl_net_connect_snp_handles();
        rc = net_engine_assign(opts, is_static, timeout, budget, addr);
        if (rc == AXL_OK) {
            axl_strlcpy(res->via, "curated", sizeof res->via);
        }
        return rc;
    }
    return AXL_ERR;   /* AXL_NET_DRV_FIRMWARE_ONLY: firmware-first was the whole job */
}

int
axl_net_auto_init_opts(const AxlNetAutoOpts *opts, AxlNetBringUpResult *out)
{
    AxlNetBringUpResult res = { 0 };
    AxlIPv4Address      addr;
    int                 rc  = AXL_ERR;

    if (out != NULL) {
        *out = res;
    }
    /* Validate BEFORE any firmware bring-up, so a malformed request is rejected
       even when some NIC is already up (never answer "online" via a stray DHCP
       lease to a caller that asked for static). */
    if (opts == NULL || opts->timeout_sec > AXL_NET_AUTO_TIMEOUT_MAX_SEC) {
        return AXL_ERR;
    }
    bool is_static = (opts->ip_mode == AXL_NET_IP_STATIC);
    if (is_static && (opts->static_ipv4 == NULL || opts->static_mask == NULL)) {
        return AXL_ERR;
    }
    if (opts->cancel != NULL && axl_cancellable_is_cancelled(opts->cancel)) {
        return AXL_CANCELLED;
    }
    size_t           timeout = (opts->dhcp_timeout_sec > 0) ? opts->dhcp_timeout_sec : 10;
    NetBudget        budget;
    const NetBudget *bp = net_budget_init(opts, &budget);

    /* Firmware-first (default): on a server whose vendor ships onboard NIC
       drivers this alone brings a NIC up with nothing loaded. A caller that ran
       its own firmware-first pass sets skip_firmware_first. */
    if (!opts->skip_firmware_first) {
        rc = net_firmware_pass(opts, is_static, timeout, bp, &addr, &res);
    }
    if (rc == AXL_ERR) {
        rc = net_acquire(opts, is_static, timeout, bp, &addr, &res);
    }

    if (rc == AXL_OK) {
        res.online = true;
        axl_memcpy(res.ipv4, addr.addr, 4);
        net_result_identify(&res, addr.addr);
        net_publish_legacy_ip4cfg();
    } else if (rc == AXL_ERR && bp != NULL && bp->deadline_ms != 0
               && axl_time_get_ms() >= bp->deadline_ms) {
        rc = AXL_TIMEOUT;   /* nothing came up, and the deadline is why the search ended */
    }
    if (out != NULL) {
        *out = res;
    }
    return rc;
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
