/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file NetInfo.c
    Network interface diagnostics and ping (UEFI ifconfig/ping equivalent).

    Build with axl-cc:
      axl-cc NetInfo.c -o NetInfo.efi

    Usage:
      NetInfo.efi [-v] [-h] [list | ping <ip> [-c <count>]]
**/

#include <axl.h>
#include <uefi/axl-uefi.h>

static bool verbose = false;

static const AxlArgDesc global_flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output (debug-level driver-locate logs)" },
    {0}
};

static const AxlArgDesc ping_flags[] = {
    { .name = "count", .short_name = 'c', .type = AXL_ARG_U32, .base = 10,
      .help = "Number of pings (default: 4)" },
    {0}
};

static const AxlArgDesc ping_pos[] = {
    { .name = "target", .type = AXL_ARG_STRING, .required = true,
      .help = "Target IP address" },
    {0}
};

/* NII protocol GUIDs — not in axl-sdk's generated UEFI headers.
 * iPXE and other UEFI Driver Model NIC drivers install one of these
 * on the NIC's PCI/USB controller handle; firmware-bundled SnpDxe
 * (or equivalent) then binds to NII and produces SNP. To find the
 * NIC driver image we walk past the SNP wrapper to the NII installer.
 * The 3.1 GUID is the modern variant; we check both. */
static const AxlGuid nii_31_guid = {
    0x1ACED566, 0x76ED, 0x4218,
    { 0xBC, 0x81, 0x76, 0x7F, 0x1F, 0x97, 0x7A, 0x89 }
};
static const AxlGuid nii_legacy_guid = {
    0xE18541CD, 0xF755, 0x4F73,
    { 0x92, 0x8D, 0x64, 0x3C, 0x8A, 0x79, 0xB2, 0x29 }
};

/* Resolve a driver-image handle to a printable path. Walks the
 * EFI_LOADED_IMAGE_PROTOCOL.FilePath device path until it hits a
 * MEDIA_FILEPATH_DP node, returning a UTF-8 copy of the UCS-2 path
 * string. Returns "<firmware volume>" for FV-dispatched drivers
 * (their file path is a MEDIA_FW_VOL_FILEPATH_DP rather than a
 * filesystem path), "<unknown>" if neither applies. Caller frees. */
typedef struct {
    char *image_name;       /* set if a MEDIA_FILEPATH_DP node is found */
    bool  saw_fv_node;
} ImgNameCtx;

static int
resolve_driver_cb(uint8_t type, uint8_t subtype, const void *node, void *user)
{
    /* MEDIA_DEVICE_PATH = 0x04 */
    if (type != 0x04) {
        return 0;
    }
    ImgNameCtx *c = (ImgNameCtx *)user;
    /* MEDIA_FILEPATH_DP = 0x04 — file loaded from a FAT volume; data
       is a UCS-2 path string after the 4-byte node header. */
    if (subtype == 0x04) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH((EFI_DEVICE_PATH_PROTOCOL *)node);
        if (node_len > 4) {
            c->image_name = axl_ucs2_to_utf8(
                (unsigned short *)((const uint8_t *)node + 4));
            return 1;  /* stop — we have the name */
        }
    }
    /* MEDIA_PIWG_FW_FILE_DP = 0x06 — driver dispatched from an FV
       (firmware volume), e.g. OVMF's bundled VirtioNetDxe. The node
       payload is a GUID, not a printable name, so we just flag it. */
    if (subtype == 0x06) {
        c->saw_fv_node = true;
    }
    return 0;
}

static char *
resolve_driver_image_name(EFI_HANDLE agent)
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS st = gBS->HandleProtocol(
        agent, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&img);
    if (EFI_ERROR(st) || img == NULL || img->FilePath == NULL) {
        return axl_strdup("<unknown>");
    }

    ImgNameCtx ctx = { .image_name = NULL, .saw_fv_node = false };
    (void)axl_device_path_for_each(img->FilePath, resolve_driver_cb, &ctx);
    if (ctx.image_name != NULL) {
        return ctx.image_name;
    }
    return axl_strdup(ctx.saw_fv_node ? "<firmware volume>" : "<unknown>");
}

/* OpenProtocol attribute bits per UEFI 2.10 §7.3.10. We only test
 * BY_DRIVER here, but the others are listed for reference. */
#define EFI_OPEN_PROTOCOL_BY_DRIVER          0x00000010

/* Find the BY_DRIVER agent handle that has @p protocol_guid open on
 * @p handle, and resolve its image name. Returns NULL if no BY_DRIVER
 * agent exists for that protocol. Caller frees the returned string. */
static char *
find_by_driver_agent(EFI_HANDLE handle, const EFI_GUID *protocol_guid)
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN n_entries = 0;
    EFI_STATUS st = gBS->OpenProtocolInformation(
        handle, (EFI_GUID *)protocol_guid, &entries, &n_entries);
    if (EFI_ERROR(st) || entries == NULL) return NULL;

    char *result = NULL;
    for (UINTN e = 0; e < n_entries; e++) {
        if (entries[e].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
            result = resolve_driver_image_name(entries[e].AgentHandle);
            break;
        }
    }
    gBS->FreePool(entries);
    return result;
}

/* Walk the SNP handles and report which driver image is bound to each.
 *
 * On UEFI driver-model NIC drivers (iPXE, vendor UNDI binaries) the
 * actual NIC binding installs EFI_NETWORK_INTERFACE_IDENTIFIER (NII)
 * — and a higher SnpDxe wrapper then attaches SNP on top. So the
 * "driver bound to SNP" question is one layer too high; we walk to
 * NII first and only fall back to the SNP-installer agent when no
 * NII is present (e.g. drivers that publish SNP directly, like
 * OVMF's VirtioNetDxe).
 *
 * This makes the diagnostic show `\drivers\x64\ipxe-intel.efi` for
 * an iPXE-driven NIC instead of `<firmware volume>` (which is what
 * SnpDxe's image path resolves to on a typical FV-dispatched
 * SnpDxe).  Confirms in one glance that the v0.6.0 staged-iPXE
 * bundle is in fact what's driving the hardware. */
static void
show_nic_drivers(void)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_service_enumerate("simple-network", &handles, &count) != 0
        || count == 0)
    {
        axl_printf("=== NIC Drivers ===\n  (no simple-network handles)\n\n");
        if (handles) axl_free(handles);
        return;
    }

    axl_printf("=== NIC Drivers ===\n\n");
    for (size_t i = 0; i < count; i++) {
        EFI_HANDLE handle = (EFI_HANDLE)handles[i];

        /* Try NII (3.1) → NII (legacy) → SNP. First match wins. */
        AXL_AUTO_FREE char *nii31  = find_by_driver_agent(
            handle, (const EFI_GUID *)&nii_31_guid);
        AXL_AUTO_FREE char *niileg = (nii31 != NULL) ? NULL :
            find_by_driver_agent(
                handle, (const EFI_GUID *)&nii_legacy_guid);
        AXL_AUTO_FREE char *snp = (nii31 != NULL || niileg != NULL) ? NULL :
            find_by_driver_agent(
                handle, &EFI_SIMPLE_NETWORK_PROTOCOL_GUID);

        const char *layer;
        const char *image_label;
        if (nii31 != NULL)       { layer = "NII3.1"; image_label = nii31; }
        else if (niileg != NULL) { layer = "NII";    image_label = niileg; }
        else if (snp != NULL)    { layer = "SNP";    image_label = snp; }
        else                     { layer = "—";      image_label = "<no driver attached>"; }

        axl_printf("  NIC[%zu] handle=%p [%s] driver=%s\n",
                   i, handle, layer, image_label);
    }
    axl_printf("\n");

    axl_free(handles);
}

/* Auto-load NIC drivers so NetInfo works from a bare UEFI shell without
 * a startup.nsh that pre-loads them. Same shape as MkRd for RamDiskDxe. */
static int
ensure_net_drivers(void)
{
    switch (axl_net_ensure_drivers()) {
    case AXL_NET_DRIVERS_OK:
        return 0;
    case AXL_NET_DRIVERS_NOT_FOUND:
        axl_printf("NetInfo: no NIC drivers found in drivers/<arch>/ "
                   "on any mounted volume.\n");
        return 1;
    case AXL_NET_DRIVERS_NO_LINK:
        axl_printf("NetInfo: drivers loaded but no NIC came up — "
                   "is a NIC plugged in?\n");
        return 1;
    default:
        axl_printf("NetInfo: failed to bring up networking.\n");
        return 1;
    }
}

// ---------------------------------------------------------------------------
// List network interfaces
// ---------------------------------------------------------------------------

static void
show_interfaces(void)
{
    size_t count = 0;
    if (axl_net_list_interfaces(NULL, &count) != 0 || count == 0) {
        axl_printf("No network interfaces found.\n");
        return;
    }

    AxlNetInterface *ifaces = axl_calloc(count, sizeof(AxlNetInterface));
    if (ifaces == NULL) {
        axl_printf("NetInfo: out of memory\n");
        return;
    }
    axl_net_list_interfaces(ifaces, &count);

    axl_printf("=== Network Interfaces ===\n\n");
    axl_printf("  %-4s %-18s %-6s %-16s %s\n",
               "IF#", "MAC", "LINK", "IPv4", "MTU");
    axl_printf("  %-4s %-18s %-6s %-16s %s\n",
               "---", "------------------", "------",
               "----------------", "----");

    for (size_t i = 0; i < count; i++) {
        AxlNetInterface *iface = &ifaces[i];

        /* Format MAC */
        char mac_str[24];
        axl_snprintf(mac_str, sizeof(mac_str),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     iface->mac[0], iface->mac[1], iface->mac[2],
                     iface->mac[3], iface->mac[4], iface->mac[5]);

        /* Format IP */
        char ip_str[20] = "-";
        if (iface->has_ipv4) {
            axl_snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                         iface->ipv4[0], iface->ipv4[1],
                         iface->ipv4[2], iface->ipv4[3]);
        }

        axl_printf("  %-4zu %-18s %-6s %-16s %u\n",
                   i, mac_str,
                   iface->link_up ? "UP" : "DOWN",
                   ip_str,
                   (unsigned)iface->mtu);

        if (verbose && iface->has_ipv4) {
            axl_printf("       Netmask: %u.%u.%u.%u  Gateway: %u.%u.%u.%u\n",
                       iface->netmask[0], iface->netmask[1],
                       iface->netmask[2], iface->netmask[3],
                       iface->gateway[0], iface->gateway[1],
                       iface->gateway[2], iface->gateway[3]);
        }
    }

    axl_printf("\n");
    axl_free(ifaces);
}

// ---------------------------------------------------------------------------
// Ping
// ---------------------------------------------------------------------------

static bool
parse_ipv4(
    const char     *str,
    AxlIPv4Address *out
    )
{
    unsigned int a, b, c, d;
    const char *p = str;

    /* Simple manual parse: a.b.c.d */
    a = b = c = d = 0;
    while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || a > 255) { return false; }
    while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || b > 255) { return false; }
    while (*p >= '0' && *p <= '9') { c = c * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || c > 255) { return false; }
    while (*p >= '0' && *p <= '9') { d = d * 10 + (*p - '0'); p++; }
    if (*p != '\0' || d > 255) { return false; }

    out->addr[0] = (uint8_t)a;
    out->addr[1] = (uint8_t)b;
    out->addr[2] = (uint8_t)c;
    out->addr[3] = (uint8_t)d;
    return true;
}

static int
do_ping(
    const char *target,
    size_t      count
    )
{
    AxlIPv4Address addr;
    if (!parse_ipv4(target, &addr)) {
        axl_printf("NetInfo: invalid IPv4 address '%s'\n", target);
        return 1;
    }

    axl_printf("PING %s — %zu packets\n", target, count);

    size_t sent = 0;
    size_t received = 0;
    size_t rtt_min = (size_t)-1;
    size_t rtt_max = 0;
    size_t rtt_total = 0;

    for (size_t i = 0; i < count; i++) {
        size_t rtt_ms = 0;
        int rc = axl_net_ping(&addr, 5000, &rtt_ms);
        sent++;

        if (rc == 0) {
            received++;
            rtt_total += rtt_ms;
            if (rtt_ms < rtt_min) {
                rtt_min = rtt_ms;
            }
            if (rtt_ms > rtt_max) {
                rtt_max = rtt_ms;
            }
            axl_printf("  Reply from %s: time=%zums\n",
                       target, rtt_ms);
        } else {
            axl_printf("  Request timed out.\n");
        }
    }

    /* Statistics */
    axl_printf("\n--- %s ping statistics ---\n", target);
    axl_printf("%zu packets sent, %zu received", sent, received);
    if (sent > 0) {
        size_t loss = ((sent - received) * 100) / sent;
        axl_printf(", %zu%% packet loss", loss);
    }
    axl_printf("\n");

    if (received > 0) {
        size_t avg = rtt_total / received;
        axl_printf("rtt min/avg/max = %zu/%zu/%zums\n",
                   rtt_min, avg, rtt_max);
    }

    return (received > 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/* Pre-run hook: surface verbose mode + ensure NIC drivers are
   loaded before any verb runs. */
static int g_netinfo_setup_failed = 0;

static void
netinfo_pre_run(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");
    if (verbose) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }
    if (ensure_net_drivers() != 0) {
        g_netinfo_setup_failed = 1;
        return;
    }
    if (verbose) {
        show_nic_drivers();
    }
}

static int
do_list_verb(AxlArgs *a)
{
    (void)a;
    if (g_netinfo_setup_failed) { return 1; }
    show_interfaces();
    return 0;
}

static int
do_ping_verb(AxlArgs *a)
{
    if (g_netinfo_setup_failed) { return 1; }

    const char *target     = axl_args_get_string(a, "target");
    uint32_t    ping_count = (uint32_t)axl_args_get_uint(a, "count");
    if (ping_count == 0) {
        ping_count = 4;
    }
    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("NetInfo: no IP address — DHCP did not complete.\n");
        return 1;
    }
    return do_ping(target, ping_count);
}

static const AxlArgsNode verbs[] = {
    { .name = "list", .handler = do_list_verb,
      .help = "List network interfaces and their state" },
    { .name = "ping", .handler = do_ping_verb,
      .flags = ping_flags, .positionals = ping_pos,
      .help = "Send ICMP echo to a target IP" },
    {0}
};

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "NetInfo",
        .help         = "Network interface inventory and ICMP ping",
        .flags        = global_flags,
        .verbs        = verbs,
        .pre_run      = netinfo_pre_run,
    });
}
