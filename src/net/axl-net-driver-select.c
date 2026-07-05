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
    char *image_name;   /* set if a MEDIA_FILEPATH node is found */
    bool  saw_fv_node;
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
    }
    return 0;
}

/* Resolve a driver-image handle to a printable name. "<firmware volume>"
   for FV-dispatched drivers, "<unknown>" when the image has no usable
   file path. Caller frees. */
static char *
resolve_driver_image_name(
    EFI_HANDLE agent
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
    EFI_STATUS st = axl_efi_call(axl_bs()->HandleProtocol, 3,
        agent, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&img);
    if (EFI_ERROR(st) || img == NULL || img->FilePath == NULL) {
        return axl_strdup("<unknown>");
    }
    ImgNameCtx ctx = { .image_name = NULL, .saw_fv_node = false };
    axl_device_path_for_each(img->FilePath, resolve_driver_cb, &ctx);
    if (ctx.image_name != NULL) {
        return ctx.image_name;
    }
    return axl_strdup(ctx.saw_fv_node ? "<firmware volume>" : "<unknown>");
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
    axl_bs()->FreePool(entries);
    return result;
}

/* Resolve driver name + binding layer for one SNP handle. Walks NII3.1 ->
   NII (legacy) -> SNP, first match wins — so the answer is the driver that
   owns the hardware, not the SnpDxe shim on top. Leaves driver/layer
   untouched (caller pre-zeroes) when nothing is bound. */
static void
resolve_driver_for_handle(
    EFI_HANDLE        handle,
    AxlNetDriverInfo *out
    )
{
    char *nii31 = find_by_driver_agent(
        handle, &gEfiNetworkInterfaceIdentifierProtocolGuid_31);
    char *niileg = (nii31 != NULL) ? NULL :
        find_by_driver_agent(
            handle, &gEfiNetworkInterfaceIdentifierProtocolGuid);
    char *snp = (nii31 != NULL || niileg != NULL) ? NULL :
        find_by_driver_agent(handle, &EFI_SIMPLE_NETWORK_PROTOCOL_GUID);

    const char *name  = NULL;
    const char *layer = "";
    if (nii31 != NULL) {
        name = nii31; layer = "NII3.1";
    } else if (niileg != NULL) {
        name = niileg; layer = "NII";
    } else if (snp != NULL) {
        name = snp; layer = "SNP";
    }

    if (name != NULL) {
        axl_strlcpy(out->driver, name, sizeof(out->driver));
        axl_strlcpy(out->layer, layer, sizeof(out->layer));
    }

    axl_free(nii31);
    axl_free(niileg);
    axl_free(snp);
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

/* Recognize iPXE drivers by filename heuristic so the watchdog disarm
   (and the documented load-last guidance) apply — same family
   axl_net_ensure_drivers gates as the iPXE fallback. */
static bool
name_is_ipxe(
    const char *path_or_name
    )
{
    return axl_strcasestr(path_or_name, "ipxe") != NULL;
}

/* Diff the post-connect SNP set against @p before, counting genuinely-new
   handles and recording the first AXL_NET_TRY_MAX_MACS MACs + any link. */
static void
attribute_new_snp(
    void           **before,
    size_t           n_before,
    AxlNetTryResult *r
    )
{
    void  **after = NULL;
    size_t  n_after = snp_snapshot(&after);

    for (size_t i = 0; i < n_after; i++) {
        if (handle_in_set(after[i], before, n_before)) {
            continue;
        }
        r->snp_handles_added++;

        EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
        bool readable = (axl_efi_call(axl_bs()->HandleProtocol, 3,
                             after[i], &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
                             (void **)&snp) == EFI_SUCCESS
                         && snp != NULL && snp->Mode != NULL);
        if (readable && snp->Mode->MediaPresent) {
            r->link_up = true;
        }
        /* Record one slot per new handle so bound_nic_count stays exactly
           min(snp_handles_added, AXL_NET_TRY_MAX_MACS). If the handle's
           Mode isn't readable (vanishingly rare for a just-enumerated SNP
           handle), the slot keeps its zeroed MAC rather than skewing the
           count. */
        if (r->bound_nic_count < AXL_NET_TRY_MAX_MACS) {
            if (readable) {
                size_t mac_len = snp->Mode->HwAddressSize;
                if (mac_len > 6) {
                    mac_len = 6;
                }
                axl_memcpy(r->bound_nic_macs[r->bound_nic_count],
                           &snp->Mode->CurrentAddress, mac_len);
            }
            r->bound_nic_count++;
        }
    }

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
    EFI_STATUS st = axl_bs()->StartImage(
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
    if (name_is_ipxe(load_path)) {
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
    return AXL_OK;
}
