/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-driver-select.c
    Per-NIC driver identity + bus location, and driver-selection
    orchestration:

      axl_net_get_driver_info        — resolve a NIC's bound driver name,
                                       binding layer, and bus location
      axl_net_list_available_drivers — enumerate stageable NIC .efi files
      axl_net_try_driver             — load + connect ONE driver and
                                       attribute the SNP handles it added
      axl_net_connect_stack          — ConnectController over all SNP handles

    The driver-name walk (NII -> SNP agent -> loaded-image path) was
    previously private to tools/netinfo.c; it lives here now as the impl
    of axl_net_get_driver_info, and netinfo consumes the public API.
**/

#include "../backend/axl-backend.h"
#include "axl-net-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-sys.h>
#include <axl/axl-fs.h>
#include <axl/axl-path.h>
#include <axl/axl-wait.h>
#include <axl/axl-driver.h>
#include <axl/axl-fv.h>   /* axl_fv_find_file_name — name an FV-dispatched driver */
#include <axl/axl-watchdog.h>
#include <axl/axl-net.h>

AXL_LOG_DOMAIN("net");

// ---------------------------------------------------------------------------
// Device-path constants (named locally so this file doesn't depend on which
// node macros the spec generator happens to emit).
// ---------------------------------------------------------------------------

#define DP_TYPE_MESSAGING   0x03
#define DP_TYPE_MEDIA       0x04
#define DP_MSG_MAC          0x0B   ///< MAC address — start of the network tail
#define DP_MSG_IPV4         0x0C
#define DP_MSG_IPV6         0x0D
#define DP_MSG_VLAN         0x14
#define DP_MEDIA_FILEPATH   0x04   ///< file loaded from a FAT volume (UCS-2 path)
#define DP_MEDIA_FW_FILE    0x06   ///< driver dispatched from a firmware volume
#define DP_END_TYPE         0x7F
#define DP_END_SUBTYPE      0xFF

/* Build-arch label for drivers/<arch>/ — mirrors the static driver_arch
   in src/util/axl-driver.c. axl-sdk builds per-arch, so a compile-time
   choice is exact. */
#if defined(__x86_64__)
static const char net_drv_arch[] = "x64";
#elif defined(__aarch64__)
static const char net_drv_arch[] = "aa64";
#else
#  error "unknown arch"
#endif

// ===========================================================================
//  Bus location — trim the network tail off a NIC's device path
// ===========================================================================

typedef struct {
    const void *dp_base;
    size_t      cut_off;   /* byte offset of the first network-addressing node */
    bool        found;
} BusCutCtx;

static int
bus_cut_cb(
    uint8_t      type,
    uint8_t      subtype,
    const void  *node,
    void        *user
    )
{
    BusCutCtx *c = (BusCutCtx *)user;
    if (type == DP_TYPE_MESSAGING &&
        (subtype == DP_MSG_MAC || subtype == DP_MSG_IPV4 ||
         subtype == DP_MSG_IPV6 || subtype == DP_MSG_VLAN))
    {
        c->cut_off = (size_t)((const uint8_t *)node - (const uint8_t *)c->dp_base);
        c->found = true;
        return 1;  /* stop — the bus location is everything before this node */
    }
    return 0;
}

/* Render a NIC's bus location (PCI/USB topology, network tail trimmed)
   into @p out. Exposed via axl-net-internal.h for unit testing against
   synthetic device paths. Always NUL-terminates @p out; "" when the path
   has no hardware prefix or DevicePathToText is unavailable. */
int
_axl_net_bus_location(
    const void *device_path,
    char       *out,
    size_t      out_size
    )
{
    if (out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';
    if (device_path == NULL) {
        return AXL_ERR;
    }

    BusCutCtx ctx = { .dp_base = device_path, .cut_off = 0, .found = false };
    axl_device_path_for_each(device_path, bus_cut_cb, &ctx);

    char *text = NULL;
    if (!ctx.found) {
        /* No network tail — the whole path (already END-terminated) IS
           the bus location. */
        text = axl_device_path_to_text(device_path);
    } else if (ctx.cut_off == 0) {
        /* Path begins with a network-addressing node — no hardware
           prefix to report. Leave out = "". */
        return AXL_OK;
    } else {
        /* Render the hardware prefix: the bytes before the network tail,
           re-terminated with an END_ENTIRE node. */
        uint8_t *trunc = axl_malloc(ctx.cut_off + 4);
        if (trunc == NULL) {
            return AXL_ERR;
        }
        axl_memcpy(trunc, device_path, ctx.cut_off);
        trunc[ctx.cut_off + 0] = DP_END_TYPE;
        trunc[ctx.cut_off + 1] = DP_END_SUBTYPE;
        trunc[ctx.cut_off + 2] = 0x04;
        trunc[ctx.cut_off + 3] = 0x00;
        text = axl_device_path_to_text(trunc);
        axl_free(trunc);
    }

    if (text == NULL) {
        /* DevicePathToText absent — bus location unavailable per contract. */
        return AXL_OK;
    }
    axl_strlcpy(out, text, out_size);
    axl_free(text);
    return AXL_OK;
}

// ===========================================================================
//  Driver-image-name resolution (lifted from tools/netinfo.c)
// ===========================================================================

typedef struct {
    char    *image_name;    /* set if a MEDIA_FILEPATH node is found */
    bool     saw_fv_node;
    bool     have_fv_guid;  /* fv_guid captured from a MEDIA_FW_FILE node */
    AxlGuid  fv_guid;       /* the FFS file GUID of an FV-dispatched image */
} ImgNameCtx;

static int
resolve_driver_cb(
    uint8_t      type,
    uint8_t      subtype,
    const void  *node,
    void        *user
    )
{
    if (type != DP_TYPE_MEDIA) {
        return 0;
    }
    ImgNameCtx *c = (ImgNameCtx *)user;
    if (subtype == DP_MEDIA_FILEPATH) {
        uint16_t node_len =
            (uint16_t)EFI_DP_LENGTH((EFI_DEVICE_PATH_PROTOCOL *)node);
        if (node_len > 4) {
            c->image_name = axl_ucs2_to_utf8(
                (unsigned short *)((const uint8_t *)node + 4));
            return 1;  /* stop — we have the name */
        }
    }
    if (subtype == DP_MEDIA_FW_FILE) {
        c->saw_fv_node = true;
        /* Body is the 16-byte FFS file GUID (after the 4-byte node header);
           capture it so the caller can resolve it to a module UI name. */
        uint16_t node_len =
            (uint16_t)EFI_DP_LENGTH((EFI_DEVICE_PATH_PROTOCOL *)node);
        if (node_len >= 4 + (uint16_t)sizeof(AxlGuid)) {
            axl_memcpy(&c->fv_guid, (const uint8_t *)node + 4, sizeof(AxlGuid));
            c->have_fv_guid = true;
        }
    }
    return 0;
}

/* Read a driver's EFI_COMPONENT_NAME2 human name (English, else its first
   supported language) from an agent/driver-binding handle into @out. AXL_OK
   if a non-empty name was written. Mirrors read_component_name() in
   src/util/axl-driver-info.c (the dh/drivers path) -- kept local rather than
   sharing across the util/net module boundary for ~15 lines. */
static int
read_driver_component_name(
    EFI_HANDLE  h,
    char       *out,
    size_t      cap
    )
{
    if (out == NULL || cap == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';
    EFI_COMPONENT_NAME2_PROTOCOL *cn = NULL;
    EFI_GUID cn_guid = gEfiComponentName2ProtocolGuid;
    if (EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3, h, &cn_guid, (void **)&cn))
        || cn == NULL || cn->GetDriverName == NULL) {
        return AXL_ERR;
    }
    CHAR16 *name = NULL;
    if (EFI_ERROR(axl_efi_call(cn->GetDriverName, 3, cn, (CHAR8 *)"en", &name))
        || name == NULL) {
        char *langs = cn->SupportedLanguages;
        if (langs == NULL
            || EFI_ERROR(axl_efi_call(cn->GetDriverName, 3, cn, (CHAR8 *)langs, &name))
            || name == NULL) {
            return AXL_ERR;
        }
    }
    axl_ucs2_to_utf8_buf((const unsigned short *)name, out, cap);
    return (out[0] != '\0') ? AXL_OK : AXL_ERR;
}

/* Resolve a driver-image handle to a printable name. Prefers the on-disk .efi
   filename (best for a staged/filesystem driver), then the driver's
   ComponentName2 human name, then — for an FV-dispatched driver with neither —
   the module UI name read from its firmware volume (e.g. "Ip4Dxe"), and only
   then the opaque "<firmware volume>" / "<unknown>". Caller frees. */
static char *
resolve_driver_image_name(
    EFI_HANDLE agent
    )
{
    bool    saw_fv       = false;
    bool    have_fv_guid = false;
    AxlGuid fv_guid;
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    bool have_img = !EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3,
            agent, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&img))
        && img != NULL;
    if (have_img && img->FilePath != NULL)
    {
        ImgNameCtx ctx = { 0 };
        axl_device_path_for_each(img->FilePath, resolve_driver_cb, &ctx);
        if (ctx.image_name != NULL) {
            axl_debug("resolve: agent -> .efi filename '%s'", ctx.image_name);
            return ctx.image_name;   /* .efi filename */
        }
        saw_fv       = ctx.saw_fv_node;
        have_fv_guid = ctx.have_fv_guid;
        if (have_fv_guid) {
            fv_guid = ctx.fv_guid;
        }
    }
    char cn[128];
    if (read_driver_component_name(agent, cn, sizeof cn) == AXL_OK) {
        axl_debug("resolve: agent -> ComponentName2 '%s'", cn);
        return axl_strdup(cn);       /* ComponentName2 human name */
    }
    /* FV-dispatched driver with no filename and no ComponentName2: name it from
       the firmware volume's UI section instead of "<firmware volume>". This is
       the common real-hardware case (an FV-dispatched SNP owner like Ip4Dxe). */
    if (have_fv_guid) {
        char fvname[128];
        if (axl_fv_find_file_name(&fv_guid, fvname, sizeof fvname) == AXL_OK) {
            axl_debug("resolve: agent -> FV UI name '%s'", fvname);
            return axl_strdup(fvname);
        }
    }
    /* Fell through to a placeholder: nothing produced a real name. Trace WHY,
       so a real-hardware resolution mystery (e.g. a USB-UNDI NIC whose SNP is
       owned by an FV-dispatched driver) is diagnosable without a debugger. */
    axl_debug("resolve: agent -> placeholder (loaded_image=%d filepath=%d "
              "saw_fv=%d fv_guid=%d componentname2=absent)",
              (int)have_img, (int)(have_img && img->FilePath != NULL),
              (int)saw_fv, (int)have_fv_guid);
    return axl_strdup(saw_fv ? "<firmware volume>" : "<unknown>");
}

/* Find the BY_DRIVER agent that has @p protocol_guid open on @p handle
   and resolve its image name. NULL if no BY_DRIVER agent exists. Caller
   frees the returned string. */
static char *
find_by_driver_agent(
    EFI_HANDLE  handle,
    EFI_GUID   *protocol_guid
    )
{
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN n_entries = 0;
    EFI_STATUS st = axl_efi_call(axl_bs()->OpenProtocolInformation, 4,
        handle, protocol_guid, &entries, &n_entries);
    if (EFI_ERROR(st) || entries == NULL) {
        return NULL;
    }
    char *result = NULL;
    for (UINTN e = 0; e < n_entries; e++) {
        if (entries[e].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
            result = resolve_driver_image_name(entries[e].AgentHandle);
            break;
        }
    }
    axl_backend_free(entries);
    return result;
}

/* Pure: does @handle expose @guid? (presence check, used to label the stack.) */
static bool
handle_has_protocol(EFI_HANDLE handle, EFI_GUID *guid)
{
    void *iface = NULL;
    return !EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3, handle, guid, &iface));
}

/* Name the real NIC-owning driver by walking @handle's device path DOWN to the
   BUS controller (PCI/USB) with LocateDevicePath, then taking THAT controller's
   BY_DRIVER agent. This is the hardware driver -- crucial because a NIC whose
   SNP sits on a child handle (e.g. VirtioNetDxe) has only the MNP *consumer* as
   the SNP handle's BY_DRIVER agent, never the producer. Caller frees; NULL if
   no bus controller is found along the path. */
static char *
find_bus_driver_name(EFI_HANDLE handle, EFI_GUID *bus_guid)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    if (EFI_ERROR(axl_efi_call(axl_bs()->HandleProtocol, 3,
            handle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&dp)) || dp == NULL) {
        return NULL;
    }
    EFI_DEVICE_PATH_PROTOCOL *walk = dp;   /* LocateDevicePath advances a copy */
    EFI_HANDLE bus = NULL;
    if (EFI_ERROR(axl_efi_call(axl_bs()->LocateDevicePath, 3, bus_guid, &walk, &bus))
        || bus == NULL) {
        return NULL;
    }
    return find_by_driver_agent(bus, bus_guid);
}

/* Resolve driver name + binding layer for one SNP handle. The LAYER labels the
   stack (NII3.1 / NII / SNP by protocol presence); the NAME is the hardware/bus
   driver that actually owns the NIC (via find_bus_driver_name), falling back to
   the NII producer's BY_DRIVER agent for exotic topologies. Leaves driver/layer
   untouched (caller pre-zeroes) when nothing is bound. */
static void
resolve_driver_for_handle(
    EFI_HANDLE        handle,
    AxlNetDriverInfo *out
    )
{
    EFI_GUID nii31g = gEfiNetworkInterfaceIdentifierProtocolGuid_31;
    EFI_GUID niig   = gEfiNetworkInterfaceIdentifierProtocolGuid;
    EFI_GUID snpg   = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
    EFI_GUID pcig   = gEfiPciIoProtocolGuid;
    EFI_GUID usbg   = gEfiUsbIoProtocolGuid;

    const char *layer =
        handle_has_protocol(handle, &nii31g) ? "NII3.1" :
        handle_has_protocol(handle, &niig)   ? "NII"    :
        handle_has_protocol(handle, &snpg)   ? "SNP"    : NULL;

    /* Name: the hardware driver (PCI then USB bus controller), then fall back to
       the NII producer's agent for topologies with no discoverable bus node.
       The source that first yields a (possibly-placeholder) name wins; the
       axl_debug lines trace which source that was so an unexpected
       "<firmware volume>" on real hardware can be pinned to a layer. */
    char *name = find_bus_driver_name(handle, &pcig);
    const char *src = "pci-bus";
    if (name == NULL) { name = find_bus_driver_name(handle, &usbg); src = "usb-bus"; }
    if (name == NULL) { name = find_by_driver_agent(handle, &nii31g); src = "nii3.1-agent"; }
    if (name == NULL) { name = find_by_driver_agent(handle, &niig); src = "nii-agent"; }
    axl_debug("resolve: layer=%s source=%s name=%s",
              layer ? layer : "(none)", name ? src : "(none)",
              name ? name : "(unresolved)");

    if (name != NULL && layer != NULL) {
        axl_strlcpy(out->driver, name, sizeof(out->driver));
        axl_strlcpy(out->layer, layer, sizeof(out->layer));
    }
    axl_free(name);
}

static void
resolve_bus_for_handle(
    EFI_HANDLE        handle,
    AxlNetDriverInfo *out
    )
{
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    EFI_STATUS st = axl_efi_call(axl_bs()->HandleProtocol, 3,
        handle, &EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&dp);
    if (EFI_ERROR(st) || dp == NULL) {
        return;  /* bus_location stays "" */
    }
    _axl_net_bus_location(dp, out->bus_location, sizeof(out->bus_location));
}

// ===========================================================================
//  axl_net_get_driver_info
// ===========================================================================

int
axl_net_get_driver_info(
    const uint8_t     mac[6],
    AxlNetDriverInfo *out
    )
{
    if (mac == NULL || out == NULL) {
        return AXL_ERR;
    }
    axl_memset(out, 0, sizeof(*out));

    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("simple-network", &handles, &count) != AXL_OK
        || count == 0)
    {
        if (handles != NULL) {
            axl_free(handles);
        }
        return AXL_ERR;
    }

    int rc = AXL_ERR;
    for (size_t i = 0; i < count; i++) {
        EFI_HANDLE h = (EFI_HANDLE)handles[i];
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        if (axl_efi_call(axl_bs()->HandleProtocol, 3,
                h, &EFI_SIMPLE_NETWORK_PROTOCOL_GUID, (void **)&snp) != EFI_SUCCESS
            || snp == NULL || snp->Mode == NULL)
        {
            continue;
        }
        size_t mac_len = snp->Mode->HwAddressSize;
        if (mac_len > 6) {
            mac_len = 6;
        }
        if (axl_memcmp(mac, &snp->Mode->CurrentAddress, mac_len) != 0) {
            continue;
        }
        /* First MAC match wins (see header note on shared-MAC handling). */
        resolve_driver_for_handle(h, out);
        resolve_bus_for_handle(h, out);
        rc = AXL_OK;
        break;
    }

    axl_free(handles);
    return rc;
}

// ===========================================================================
//  axl_net_list_available_drivers
// ===========================================================================

#define NET_DRV_MAX_VOLUMES  16

static bool
is_driver_file(
    const char *name
    )
{
    size_t n = axl_strlen(name);
    return (n > 4 && axl_strcmp(name + n - 4, ".efi") == 0)
        || (n > 7 && axl_strcmp(name + n - 7, ".efidrv") == 0);
}

int
axl_net_list_available_drivers(
    AxlNetDriverFile *out,
    size_t           *count
    )
{
    if (count == NULL) {
        return AXL_ERR;
    }

    size_t capacity = (out != NULL) ? *count : 0;
    size_t total  = 0;
    size_t filled = 0;

    AxlVolume volumes[NET_DRV_MAX_VOLUMES];
    size_t    n_vols = 0;
    if (axl_volume_enumerate(volumes, NET_DRV_MAX_VOLUMES, &n_vols) != AXL_OK) {
        n_vols = 0;
    }

    char sub_path[32];
    if (axl_snprintf(sub_path, sizeof(sub_path),
                     "/drivers/%s", net_drv_arch) <= 0) {
        *count = 0;
        return AXL_OK;
    }

    for (size_t v = 0; v < n_vols; v++) {
        char dir_path[80];
        if (axl_path_build_uefi(volumes[v].name, sub_path,
                                dir_path, sizeof(dir_path)) != AXL_OK) {
            continue;
        }
        AxlDir *dir = axl_dir_open(dir_path);
        if (dir == NULL) {
            continue;   /* drivers/<arch>/ absent on this volume */
        }

        AxlFsEntry entry;
        while (axl_dir_read(dir, &entry)) {
            if (axl_fs_entry_is_dir(&entry) || !is_driver_file(entry.name)) {
                continue;
            }
            /* Entries are unique by full path: filenames are unique within
               a directory, and the volume name disambiguates across
               volumes — so no explicit dedup pass is needed. */
            total++;
            if (out != NULL && filled < capacity) {
                AxlNetDriverFile *f = &out[filled];
                axl_memset(f, 0, sizeof(*f));
                axl_strlcpy(f->name, entry.name, sizeof(f->name));
                char full[320];
                axl_snprintf(full, sizeof(full), "%s\\%s", dir_path, entry.name);
                axl_strlcpy(f->path, full, sizeof(f->path));
                f->size = entry.size;
                filled++;
            }
        }
        axl_dir_close(dir);
    }

    *count = (out == NULL) ? total : filled;
    return AXL_OK;
}

// ===========================================================================
//  axl_net_connect_stack
// ===========================================================================

int
axl_net_connect_stack(void)
{
    /* Global ConnectController (shell `connect -r`) wires drivers to
       controllers; the per-SNP-handle reconnect then layers MNP/IP4/TCP4
       on each NIC (matches axl_net_drivers_up's behavior). */
    axl_driver_connect(NULL);
    _axl_net_connect_snp_handles();
    return AXL_OK;
}

// ===========================================================================
//  axl_net_try_driver
// ===========================================================================

/* Snapshot the current SNP handle set. Returns the count; *out is a
   caller-freed (axl_free) handle array, or NULL. */
static size_t
snp_snapshot(
    void ***out
    )
{
    void  **handles = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate("simple-network", &handles, &count) != AXL_OK) {
        *out = NULL;
        return 0;
    }
    *out = handles;
    return count;
}

static bool
handle_in_set(
    void   *h,
    void  **set,
    size_t  n
    )
{
    for (size_t i = 0; i < n; i++) {
        if (set[i] == h) {
            return true;
        }
    }
    return false;
}

static bool
path_has_separator(
    const char *s
    )
{
    for (; *s != '\0'; s++) {
        if (*s == ':' || *s == '\\' || *s == '/') {
            return true;
        }
    }
    return false;
}

/* Filename heuristic only -- see the header doc for the two obligations
   (order last, disarm the watchdog) this recognition exists to support. */
bool
axl_net_driver_is_ipxe(
    const char *path_or_name
    )
{
    return axl_strcasestr(path_or_name, "ipxe") != NULL;
}

/* Diff the post-connect SNP set against @p before: count genuinely-new
   handles, record EVERY new NIC's MAC into a heap array (no cap), and note
   aggregate link. The MAC array is owned by the caller (freed via axl_free);
   NULL when nothing new bound or on allocation failure. */
static void
attribute_new_snp(
    void           **before,
    size_t           n_before,
    AxlNetTryResult *r
    )
{
    void  **after = NULL;
    size_t  n_after = snp_snapshot(&after);

    /* Pass 1: count genuinely-new handles (pure membership; no protocol open). */
    for (size_t i = 0; i < n_after; i++) {
        if (!handle_in_set(after[i], before, n_before)) {
            r->snp_handles_added++;
        }
    }
    if (r->snp_handles_added == 0) {
        if (after != NULL) {
            axl_free(after);
        }
        return;
    }

    /* One MAC slot per new handle; an unreadable Mode (vanishingly rare for a
       just-enumerated SNP handle) keeps a zeroed slot so the array stays 1:1
       with the count. On OOM the honest count survives; the MAC array does
       not (bound_nic_count = 0, bound_nic_macs = NULL). */
    r->bound_nic_macs = axl_malloc((size_t)r->snp_handles_added * 6);
    if (r->bound_nic_macs == NULL) {
        if (after != NULL) {
            axl_free(after);
        }
        return;
    }
    axl_memset(r->bound_nic_macs, 0, (size_t)r->snp_handles_added * 6);

    /* Pass 2: fill MACs + aggregate link for each new handle. */
    size_t slot = 0;
    for (size_t i = 0; i < n_after; i++) {
        if (handle_in_set(after[i], before, n_before)) {
            continue;
        }
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        if (axl_efi_call(axl_bs()->HandleProtocol, 3,
                after[i], &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
                (void **)&snp) == EFI_SUCCESS
            && snp != NULL && snp->Mode != NULL)
        {
            if (snp->Mode->MediaPresent) {
                r->link_up = true;
            }
            size_t mac_len = snp->Mode->HwAddressSize;
            if (mac_len > 6) {
                mac_len = 6;
            }
            axl_memcpy(r->bound_nic_macs[slot], &snp->Mode->CurrentAddress, mac_len);
        }
        slot++;
    }
    r->bound_nic_count = slot;   /* == snp_handles_added */

    if (after != NULL) {
        axl_free(after);
    }
}

/* Settle window after connect — some firmware registers SNP/IP4 handles
   asynchronously; let them appear before the before/after diff. */
#define AXL_NET_TRY_SETTLE_MS  200

int
axl_net_try_driver(
    const char      *path_or_name,
    AxlNetTryResult *out
    )
{
    AxlNetTryResult local;
    AxlNetTryResult *r = (out != NULL) ? out : &local;
    axl_memset(r, 0, sizeof(*r));

    if (path_or_name == NULL || path_or_name[0] == '\0') {
        return AXL_ERR;
    }

    /* Resolve a bare name through the driver search path; use a path
       (anything with a volume/separator) as-is. */
    char resolved[256];
    const char *load_path = path_or_name;
    if (!path_has_separator(path_or_name)) {
        if (axl_driver_locate(path_or_name, resolved, sizeof(resolved)) != AXL_OK) {
            return AXL_ERR;   /* found = false */
        }
        load_path = resolved;
    }
    r->found = true;

    void  **before = NULL;
    size_t  n_before = snp_snapshot(&before);

    AxlDriverHandle drv = NULL;
    if (axl_driver_load(load_path, &drv) != AXL_OK || drv == NULL) {
        if (before != NULL) {
            axl_free(before);
        }
        return AXL_ERR;   /* found = true, loaded = false */
    }

    size_t exit_data_size = 0;
    EFI_STATUS st = axl_efi_call(axl_bs()->StartImage, 3,
        (EFI_HANDLE)drv, &exit_data_size, NULL);
    if (EFI_ERROR(st) && st != EFI_ALREADY_STARTED) {
        axl_warning("try_driver: StartImage failed for '%s': 0x%llx",
                    load_path, (unsigned long long)st);
        axl_driver_unload(drv);
        r->unloaded = true;
        if (before != NULL) {
            axl_free(before);
        }
        return AXL_ERR;
    }
    r->loaded = true;

    /* iPXE arms a 5-min boot-services watchdog; disarm it (recognized
       iPXE names only — see header). */
    if (axl_net_driver_is_ipxe(load_path)) {
        axl_watchdog_disarm();
    }

    axl_net_connect_stack();
    axl_msleep(AXL_NET_TRY_SETTLE_MS);

    attribute_new_snp(before, n_before, r);

    if (before != NULL) {
        axl_free(before);
    }

    if (r->snp_handles_added == 0) {
        /* Bound nothing — roll back so the next candidate starts clean. */
        axl_driver_unload(drv);
        r->unloaded = true;
        return AXL_ERR;
    }
    /* Success: the driver stays resident — hand its handle back so a caller
       running its own sweep can drop it after a failed downstream check. */
    r->driver = (void *)drv;
    return AXL_OK;
}
