/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sys.c
    System operations — reset, device mapping, firmware/memory info.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-sys.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("sys");

void
axl_reset(int type)
{
    EFI_RESET_TYPE reset_type;

    switch (type) {
    case AXL_RESET_WARM:
        reset_type = EfiResetWarm;
        break;
    case AXL_RESET_SHUTDOWN:
        reset_type = EfiResetShutdown;
        break;
    case AXL_RESET_COLD:
    default:
        reset_type = EfiResetCold;
        break;
    }

    axl_rt()->ResetSystem(reset_type, 0, 0, NULL);
}

int
axl_map_refresh(void)
{
    return axl_backend_shell_execute((const unsigned short *)L"map -r");
}

// ---------------------------------------------------------------------------
// Handle protocol + stall
// ---------------------------------------------------------------------------

int
axl_handle_get_service(
    void        *handle,
    const char  *name,
    void       **interface
    )
{
    EFI_GUID        fallback;
    const EFI_GUID *guid;
    EFI_STATUS      status;

    if (handle == NULL || name == NULL || interface == NULL) {
        return AXL_ERR;
    }
    /* Defensive: ensure the caller sees NULL (not stale data) on
       any failure path. UEFI HandleProtocol does not guarantee
       *Interface is preserved on error — EDK2 zeroes it but other
       firmware may not. Callers like AxlVolume.device_path gate on
       NULL, so a leftover stale pointer would be a real footgun. */
    *interface = NULL;

    /* Reuse the service name→GUID lookup from axl-service.c */
    extern const EFI_GUID *axl_service_lookup_guid(const char *name, EFI_GUID *fallback);
    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return AXL_ERR;
    }

    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        (EFI_GUID *)guid,
        interface);

    if (EFI_ERROR(status)) {
        *interface = NULL;
        return AXL_ERR;
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Firmware info
// ---------------------------------------------------------------------------

int
axl_sys_get_firmware_info(
    AxlFirmwareInfo *info
    )
{
    EFI_SYSTEM_TABLE *st;
    size_t i;

    if (info == NULL) {
        return AXL_ERR;
    }

    st = axl_st();
    if (st == NULL) {
        return AXL_ERR;
    }

    axl_memset(info, 0, sizeof(*info));

    /* Vendor name: UCS-2 → UTF-8 */
    if (st->FirmwareVendor != NULL) {
        unsigned short *w = st->FirmwareVendor;
        for (i = 0; i < sizeof(info->vendor) - 1 && w[i] != 0; i++) {
            info->vendor[i] = (char)(w[i] < 128 ? w[i] : '?');
        }
        info->vendor[i] = '\0';
    }

    info->firmware_revision = st->FirmwareRevision;

    /* EFI spec revision: high 16 bits = major, low 16 bits = minor*10 */
    info->spec_major = (uint16_t)(st->Hdr.Revision >> 16);
    info->spec_minor = (uint16_t)(st->Hdr.Revision & 0xFFFF);

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Memory size
// ---------------------------------------------------------------------------

int
axl_sys_get_memory_size(
    uint64_t *total_bytes
    )
{
    EFI_STATUS             status;
    size_t                  map_size = 0;
    size_t                  map_key;
    size_t                  desc_size;
    UINT32                 desc_ver;
    uint8_t               *map = NULL;
    uint64_t               total = 0;

    if (total_bytes == NULL) {
        return AXL_ERR;
    }

    /* First call: get required size */
    status = axl_bs()->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)NULL,
        &map_key, &desc_size, &desc_ver);

    if (status != EFI_BUFFER_TOO_SMALL) {
        return AXL_ERR;
    }

    /* Add slack for map growth between calls */
    map_size += desc_size * 4;
    map = (uint8_t *)axl_malloc(map_size);
    if (map == NULL) {
        axl_error(
            "axl_sys_get_memory_size: OOM allocating %zu-byte memory map buffer",
            map_size
            );
        return AXL_ERR;
    }

    status = axl_bs()->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)map,
        &map_key, &desc_size, &desc_ver);

    if (EFI_ERROR(status)) {
        axl_free(map);
        return AXL_ERR;
    }

    /* Sum usable memory regions */
    for (size_t off = 0; off < map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(map + off);

        switch (desc->Type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiConventionalMemory:
            total += desc->NumberOfPages * 4096ULL;
            break;
        default:
            break;
        }
    }

    axl_free(map);
    *total_bytes = total;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Device-path iteration
// ---------------------------------------------------------------------------

/* Cap on node count — guards against malformed firmware data that
   doesn't terminate. 64 nodes is well above any real device path
   the SDK has seen (typical UEFI paths max at ~6-12 nodes). */
#define AXL_DP_MAX_NODES  64u

int
axl_device_path_for_each(
    const void       *device_path,
    AxlDevicePathFn   fn,
    void             *user
    )
{
    if (device_path == NULL || fn == NULL) {
        return -1;
    }

    const EFI_DEVICE_PATH_PROTOCOL *dp =
        (const EFI_DEVICE_PATH_PROTOCOL *)device_path;

    for (unsigned steps = 0; steps < AXL_DP_MAX_NODES; steps++) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH(dp);
        if (node_len < 4) {
            return -1;  /* malformed: each node header is 4 bytes */
        }
        if (EFI_DP_IS_END(dp)) {
            return 0;
        }
        int rc = fn((uint8_t)EFI_DP_TYPE(dp), (uint8_t)EFI_DP_SUBTYPE(dp),
                    dp, user);
        if (rc != 0) {
            return rc;
        }
        dp = EFI_DP_NEXT(dp);
    }
    return -1;  /* step cap exhausted — treat as malformed */
}

typedef struct {
    uint8_t      type;
    uint8_t      subtype;
    const void  *match;
} DpFindCtx;

static int
dp_find_cb(uint8_t type, uint8_t subtype, const void *node, void *user)
{
    DpFindCtx *c = (DpFindCtx *)user;
    if (type == c->type && subtype == c->subtype) {
        c->match = node;
        return 1;  /* stop */
    }
    return 0;
}

const void *
axl_device_path_find(
    const void *device_path,
    uint8_t     type,
    uint8_t     subtype
    )
{
    DpFindCtx ctx = { .type = type, .subtype = subtype, .match = NULL };
    (void)axl_device_path_for_each(device_path, dp_find_cb, &ctx);
    return ctx.match;
}

size_t
axl_device_path_size(const void *device_path)
{
    if (device_path == NULL) {
        return 0;
    }
    const EFI_DEVICE_PATH_PROTOCOL *dp =
        (const EFI_DEVICE_PATH_PROTOCOL *)device_path;
    const uint8_t *base = (const uint8_t *)dp;
    for (unsigned steps = 0; steps < AXL_DP_MAX_NODES; steps++) {
        uint16_t node_len = (uint16_t)EFI_DP_LENGTH(dp);
        if (node_len < 4) return 0;
        if (EFI_DP_IS_END(dp)) {
            return (size_t)((const uint8_t *)dp - base) + node_len;
        }
        dp = EFI_DP_NEXT(dp);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// axl_device_path_has_vendor
// ---------------------------------------------------------------------------

typedef struct {
    const AxlGuid *guid;
    bool           found;
} VendorMatchCtx;

static int
vendor_match_cb(uint8_t type, uint8_t subtype, const void *node, void *user)
{
    if (type == HARDWARE_DEVICE_PATH && subtype == HW_VENDOR_DP) {
        VendorMatchCtx *c = (VendorMatchCtx *)user;
        const VENDOR_DEVICE_PATH *v = (const VENDOR_DEVICE_PATH *)node;
        if (axl_guid_cmp((const AxlGuid *)&v->Guid, c->guid)) {
            c->found = true;
            return 1;  /* stop */
        }
    }
    return 0;
}

bool
axl_device_path_has_vendor(void *device_path, const AxlGuid *guid)
{
    if (device_path == NULL || guid == NULL) {
        return false;
    }
    VendorMatchCtx ctx = { .guid = guid, .found = false };
    (void)axl_device_path_for_each(device_path, vendor_match_cb, &ctx);
    return ctx.found;
}

// ---------------------------------------------------------------------------
// axl_device_path_to_text
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct {
    void *ConvertDeviceNodeToText;
    unsigned short *(EFIAPI *ConvertDevicePathToText)(
        const void *DevicePath,
        BOOLEAN     DisplayOnly,
        BOOLEAN     AllowShortcuts);
} DevicePathToTextProtocol;
#pragma pack(pop)

static const AxlGuid DEVICE_PATH_TO_TEXT_GUID = AXL_GUID(
    0x8b843e20, 0x8132, 0x4852,
    0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c);

char *
axl_device_path_to_text(const void *device_path)
{
    if (device_path == NULL) {
        return NULL;
    }
    static DevicePathToTextProtocol *cached;
    if (cached == NULL) {
        void *p = NULL;
        if (axl_bs()->LocateProtocol(
                (EFI_GUID *)&DEVICE_PATH_TO_TEXT_GUID, NULL, &p) != EFI_SUCCESS
            || p == NULL)
        {
            return NULL;
        }
        cached = (DevicePathToTextProtocol *)p;
    }
    if (cached->ConvertDevicePathToText == NULL) {
        return NULL;
    }
    unsigned short *text = cached->ConvertDevicePathToText(
        device_path, FALSE, FALSE);
    if (text == NULL) {
        return NULL;
    }
    char *utf8 = axl_ucs2_to_utf8(text);
    axl_bs()->FreePool(text);
    return utf8;
}
