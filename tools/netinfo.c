/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file NetInfo.c
    Network interface diagnostics and ping (UEFI ifconfig/ping equivalent).

    Build with axl-cc:
      axl-cc NetInfo.c -o NetInfo.efi

    Usage:
      NetInfo.efi [-v] [-n] [-h] [list | ping <ip> [-c <count>] | list-bundle]
**/

#include <axl.h>
#include <uefi/axl-uefi.h>

static bool verbose = false;
static bool no_load = false;

static const AxlArgDesc global_flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output: device-path text, NII layer info, debug logs" },
    { .name = "no-load", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Skip auto-loading NIC drivers from drivers/<arch>/" },
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

/* Build-arch label for drivers/<arch>/ — must match the static
 * driver_arch in src/util/axl-driver.c. axl-sdk tools are built per-arch
 * so a compile-time choice is exact. */
#if defined(__x86_64__)
static const char netinfo_arch[] = "x64";
#elif defined(__aarch64__)
static const char netinfo_arch[] = "aa64";
#else
#  error "unknown arch"
#endif

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

/* NII protocol layout — first field is a UINT64 Revision per the
 * UEFI/PI spec. We only read the revision; the rest of the struct is
 * driver-internal. */
typedef struct {
    uint64_t  revision;
    /* trailing fields elided */
} NiiProtocolHead;

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

/* Read the NII Revision word (8 bytes at offset 0) for a handle that
 * has a BY_DRIVER agent on it for the given NII GUID. Returns 0 if NII
 * isn't installed on @p handle or the protocol pointer can't be read. */
static uint64_t
read_nii_revision(EFI_HANDLE handle, const EFI_GUID *nii_guid)
{
    NiiProtocolHead *p = NULL;
    EFI_STATUS st = gBS->HandleProtocol(
        handle, (EFI_GUID *)nii_guid, (void **)&p);
    if (EFI_ERROR(st) || p == NULL) return 0;
    return p->revision;
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
 * Under -v, also emit:
 *  - the SNP handle's device path (gives PCI BDF / USB topology / MAC
 *    in the canonical UEFI format, e.g.
 *    `PciRoot(0x0)/Pci(0x3,0x0)/MAC(525400123456,0x1)`)
 *  - the NII protocol revision when we found a BY_DRIVER agent for it. */
static void
show_nic_drivers(const char *label)
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_service_enumerate("simple-network", &handles, &count) != AXL_OK
        || count == 0)
    {
        axl_printf("=== %s ===\n  (no simple-network handles)\n\n", label);
        if (handles) axl_free(handles);
        return;
    }

    axl_printf("=== %s ===\n\n", label);
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
        const EFI_GUID *nii_guid_for_rev = NULL;
        if (nii31 != NULL) {
            layer = "NII3.1"; image_label = nii31;
            nii_guid_for_rev = (const EFI_GUID *)&nii_31_guid;
        } else if (niileg != NULL) {
            layer = "NII"; image_label = niileg;
            nii_guid_for_rev = (const EFI_GUID *)&nii_legacy_guid;
        } else if (snp != NULL) {
            layer = "SNP"; image_label = snp;
        } else {
            layer = "—"; image_label = "<no driver attached>";
        }

        axl_printf("  NIC[%zu] handle=%p [%s] driver=%s\n",
                   i, handle, layer, image_label);

        if (verbose) {
            EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
            if (gBS->HandleProtocol(handle, &EFI_DEVICE_PATH_PROTOCOL_GUID,
                                    (void **)&dp) == EFI_SUCCESS && dp != NULL)
            {
                AXL_AUTO_FREE char *dp_text = axl_device_path_to_text(dp);
                axl_printf("         path=%s\n",
                           dp_text != NULL ? dp_text : "<unavailable>");
            }
            if (nii_guid_for_rev != NULL) {
                uint64_t rev = read_nii_revision(handle, nii_guid_for_rev);
                if (rev != 0) {
                    axl_printf("         nii revision=0x%llx\n",
                               (unsigned long long)rev);
                }
            }
        }
    }
    axl_printf("\n");

    axl_free(handles);
}

/* Auto-load NIC drivers so NetInfo works from a bare UEFI shell without
 * a startup.nsh that pre-loads them. Same shape as MkRd for RamDiskDxe.
 * Returns 0 on success-or-no-load, non-zero if hard fail (no NICs and
 * driver-load couldn't change that). */
static int
ensure_net_drivers_warn(void)
{
    if (no_load) {
        /* User opted out of disk-driver loading, but still want
           firmware-provided drivers to bind. UEFI doesn't run the
           equivalent of `connect -r` automatically — without it,
           NICs that have a firmware-provided driver bound at boot
           may not have SNP up yet. Trigger the connect explicitly
           so `--no-load` produces a useful "firmware baseline"
           instead of silently zero NICs. */
        axl_driver_connect(NULL);
        return 0;
    }
    switch (axl_net_ensure_drivers()) {
    case AXL_NET_DRIVERS_OK:
        return 0;
    case AXL_NET_DRIVERS_NOT_FOUND:
        axl_printf("NetInfo: warning: no NIC drivers found in "
                   "drivers/<arch>/ on any mounted volume.\n");
        return 1;
    case AXL_NET_DRIVERS_NO_LINK:
        axl_printf("NetInfo: warning: drivers loaded but no NIC "
                   "came up — is a NIC plugged in?\n");
        return 1;
    default:
        axl_printf("NetInfo: warning: failed to bring up networking.\n");
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
    if (axl_net_list_interfaces(NULL, &count) != AXL_OK || count == 0) {
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
// list-bundle: enumerate drivers/<arch>/ on every mounted volume
// ---------------------------------------------------------------------------

/* Scan one volume's drivers/<arch>/ directory and print any .efi /
 * .efidrv entries. Returns the number of entries listed (0 means
 * "directory absent or empty"). */
static size_t
list_bundle_volume(const char *fs_name)
{
    char dir_path[64];
    char sub_path[32];
    if (axl_snprintf(sub_path, sizeof(sub_path),
                     "/drivers/%s", netinfo_arch) <= 0) {
        return 0;
    }
    if (axl_path_build_uefi(fs_name, sub_path,
                            dir_path, sizeof(dir_path)) != AXL_OK) {
        return 0;
    }

    AxlDir *dir = axl_dir_open(dir_path);
    if (dir == NULL) {
        return 0;
    }

    size_t shown = 0;
    AxlDirEntry entry;
    while (axl_dir_read(dir, &entry)) {
        if (entry.is_dir) continue;
        size_t nlen = axl_strlen(entry.name);
        bool is_driver =
            (nlen > 4 && axl_strcmp(entry.name + nlen - 4, ".efi") == 0)
         || (nlen > 7 && axl_strcmp(entry.name + nlen - 7, ".efidrv") == 0);
        if (!is_driver) continue;
        if (shown == 0) {
            axl_printf("  %s/\n", dir_path);
        }
        axl_printf("    %-32s %llu bytes\n",
                   entry.name, (unsigned long long)entry.size);
        shown++;
    }
    axl_dir_close(dir);
    return shown;
}

static int
do_list_bundle_verb(AxlArgs *a)
{
    (void)a;
    AxlVolume volumes[16];
    size_t n_vols = 0;
    if (axl_volume_enumerate(volumes, 16, &n_vols) != AXL_OK || n_vols == 0) {
        axl_printf("=== Driver Bundle ===\n  (no FAT volumes mounted)\n\n");
        return 0;
    }

    axl_printf("=== Driver Bundle (drivers/%s/ on each volume) ===\n\n",
               netinfo_arch);
    size_t total = 0;
    for (size_t i = 0; i < n_vols; i++) {
        total += list_bundle_volume(volumes[i].name);
    }
    if (total == 0) {
        axl_printf("  (no drivers staged)\n");
    }
    axl_printf("\n");
    return 0;
}

// ---------------------------------------------------------------------------
// diag: composite report intended to be copy-pasted to a maintainer
// ---------------------------------------------------------------------------

static void
diag_show_firmware(void)
{
    AxlFirmwareInfo info = {0};
    if (axl_sys_get_firmware_info(&info) != AXL_OK) {
        axl_printf("=== Firmware ===\n  (info unavailable)\n\n");
        return;
    }
    axl_printf("=== Firmware ===\n");
    axl_printf("  vendor    : %s\n", info.vendor);
    axl_printf("  rev       : 0x%08x\n", info.firmware_revision);
    axl_printf("  uefi spec : %u.%u\n", info.spec_major, info.spec_minor);
    axl_printf("  arch      : %s\n", netinfo_arch);
    axl_printf("\n");
}

static void
diag_show_volumes(void)
{
    AxlVolume volumes[16];
    size_t n = 0;
    if (axl_volume_enumerate(volumes, 16, &n) != AXL_OK || n == 0) {
        axl_printf("=== Mounted Volumes ===\n  (none)\n\n");
        return;
    }
    axl_printf("=== Mounted Volumes ===\n");
    for (size_t i = 0; i < n; i++) {
        AXL_AUTO_FREE char *dp_text =
            axl_device_path_to_text(volumes[i].device_path);
        axl_printf("  %-6s %s\n",
                   volumes[i].name,
                   dp_text != NULL ? dp_text : "<no device path>");
    }
    axl_printf("\n");
}

/* PCI class 0x02 = Network controller. Walking by class shows every
 * NIC the firmware sees in config space — including ones with no UEFI
 * driver attached. The gap between PCI Network Controllers and
 * Simple Network Protocol handles is the "missing driver" smoking gun. */
static void
diag_show_pci_nics(void)
{
    axl_printf("=== PCI Network Controllers (class 0x02xxxx) ===\n");
    /* Walk every PCI function, filtering by base class 0x02.
     * axl_pci_find_by_class needs an exact 24-bit class match, which
     * misses subclasses; the cursor walk lets us match base alone. */
    AxlPciAddr *p = NULL;
    bool any = false;
    while ((p = axl_pci_next(p)) != NULL) {
        uint32_t class_code = 0;
        if (axl_pci_get_class_code(*p, &class_code) != AXL_OK) continue;
        if (((class_code >> 16) & 0xFF) != 0x02) continue;

        uint16_t vid = 0, did = 0;
        (void)axl_pci_get_vid_did(*p, &vid, &did);

        char addr_buf[AXL_PCI_ADDR_STR_MAX];
        axl_pci_addr_format(*p, addr_buf, sizeof(addr_buf));

        char id_buf[160];
        if (axl_pci_format_name(vid, did, id_buf, sizeof(id_buf)) <= 0) {
            axl_snprintf(id_buf, sizeof(id_buf), "%04x:%04x", vid, did);
        }

        char class_buf[80];
        (void)axl_pci_class_string(class_code, class_buf, sizeof(class_buf));

        axl_printf("  %-13s  %s  (%s)\n", addr_buf, id_buf, class_buf);
        any = true;
    }
    if (!any) {
        axl_printf("  (no network-class PCI functions enumerated)\n");
    }
    axl_printf("\n");
}

static int
do_diag_verb(AxlArgs *a)
{
    (void)a;

    /* diag implies verbose — every section gets its richer payload.
     * Library debug logs come from AXL_LOG_LEVEL=debug (or per-domain
     * filters); we don't override here. */
    verbose = true;

    axl_printf("\n=== NetInfo diag (paste this back to the maintainer) ===\n\n");

    diag_show_firmware();
    diag_show_volumes();
    diag_show_pci_nics();

    /* Driver bundle inventory — same shape as `list-bundle`. */
    (void)do_list_bundle_verb(a);

    /* NIC handle table BEFORE driver-load attempt — shows what
     * firmware natively provides. */
    show_nic_drivers("NIC Drivers (before driver-load)");

    /* Try to bring drivers up unless explicitly suppressed. */
    int rc = ensure_net_drivers_warn();
    axl_printf("=== ensure_drivers status ===\n");
    if (no_load) {
        axl_printf("  --no-load set: skipped\n\n");
    } else {
        axl_printf("  result: %s\n\n", rc == 0 ? "OK" : "no link / not found");
    }

    if (!no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    show_interfaces();
    return 0;
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

        if (rc == AXL_OK) {
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

/* Pre-run hook: pick up global flags. Per-verb logic owns the
 * driver-load decision so list-bundle never tries to bring up NICs and
 * `list` doesn't fail-closed when drivers can't be loaded. */
static void
netinfo_pre_run(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");
    no_load = axl_args_get_bool(a, "no-load");
    /* For library debug logs use AXL_LOG_LEVEL=debug rather than
     * a tool-level switch — frees -v to carry tool-specific
     * verbose-output semantics. */
}

static int
do_list_verb(AxlArgs *a)
{
    (void)a;

    if (verbose) {
        show_nic_drivers(no_load
            ? "NIC Drivers (firmware-provided, --no-load)"
            : "NIC Drivers (before driver-load)");
    }

    /* Try to bring up drivers, but never gate `list` on the result —
     * showing zero interfaces when nothing's there is itself a useful
     * diagnostic. */
    (void)ensure_net_drivers_warn();

    if (verbose && !no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    show_interfaces();
    return 0;
}

static int
do_ping_verb(AxlArgs *a)
{
    if (verbose) {
        show_nic_drivers(no_load
            ? "NIC Drivers (firmware-provided, --no-load)"
            : "NIC Drivers (before driver-load)");
    }

    /* ping requires a NIC; fail-closed if drivers can't be loaded. */
    if (ensure_net_drivers_warn() != 0) {
        return 1;
    }

    if (verbose && !no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    const char *target     = axl_args_get_string(a, "target");
    uint32_t    ping_count = (uint32_t)axl_args_get_uint(a, "count");
    if (ping_count == 0) {
        ping_count = 4;
    }
    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
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
    { .name = "list-bundle", .handler = do_list_bundle_verb,
      .help = "List staged drivers in drivers/<arch>/ on each mounted volume" },
    { .name = "diag", .handler = do_diag_verb,
      .help = "Composite report (firmware, PCI NICs, drivers, interfaces)" },
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
