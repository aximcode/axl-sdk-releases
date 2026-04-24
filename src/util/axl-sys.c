/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-sys.c
    System operations — reset, device mapping, firmware/memory info.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-sys.h>
#include <axl/axl-str.h>
#include <axl/axl-mem.h>
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
        return -1;
    }

    /* Reuse the service name→GUID lookup from axl-service.c */
    extern const EFI_GUID *axl_service_lookup_guid(const char *name, EFI_GUID *fallback);
    guid = axl_service_lookup_guid(name, &fallback);
    if (guid == NULL) {
        return -1;
    }

    status = axl_bs()->HandleProtocol(
        (EFI_HANDLE)handle,
        (EFI_GUID *)guid,
        interface);

    return EFI_ERROR(status) ? -1 : 0;
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
        return -1;
    }

    st = axl_st();
    if (st == NULL) {
        return -1;
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

    return 0;
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
        return -1;
    }

    /* First call: get required size */
    status = axl_bs()->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)NULL,
        &map_key, &desc_size, &desc_ver);

    if (status != EFI_BUFFER_TOO_SMALL) {
        return -1;
    }

    /* Add slack for map growth between calls */
    map_size += desc_size * 4;
    map = (uint8_t *)axl_malloc(map_size);
    if (map == NULL) {
        axl_error(
            "axl_sys_get_memory_size: OOM allocating %zu-byte memory map buffer",
            map_size
            );
        return -1;
    }

    status = axl_bs()->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)map,
        &map_key, &desc_size, &desc_ver);

    if (EFI_ERROR(status)) {
        axl_free(map);
        return -1;
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
    return 0;
}

// ---------------------------------------------------------------------------
// axl_device_path_has_vendor
// ---------------------------------------------------------------------------

bool
axl_device_path_has_vendor(void *device_path, const AxlGuid *guid)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)device_path;

    if (dp == NULL || guid == NULL) {
        return false;
    }

    while (!EFI_DP_IS_END(dp)) {
        if (EFI_DP_TYPE(dp) == HARDWARE_DEVICE_PATH &&
            EFI_DP_SUBTYPE(dp) == HW_VENDOR_DP)
        {
            VENDOR_DEVICE_PATH *v = (VENDOR_DEVICE_PATH *)dp;
            if (axl_guid_cmp((const AxlGuid *)&v->Guid, guid)) {
                return true;
            }
        }
        dp = EFI_DP_NEXT(dp);
    }

    return false;
}
