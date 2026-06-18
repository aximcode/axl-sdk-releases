/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file smart.c
    smart — list every storage device (NVMe / ATA / SCSI) with normalized
    identity + health. The dogfood tool for <axl/axl-smart.h>: a thin renderer
    over axl_storage_next / axl_smart_health. `smartctl --scan -H` for UEFI,
    across all transports.
**/

#include <axl.h>

static const char *
transport_name(AxlStorageTransport t)
{
    switch (t) {
    case AXL_STORAGE_NVME: return "nvme";
    case AXL_STORAGE_ATA:  return "ata";
    case AXL_STORAGE_SCSI: return "scsi";
    default:               return "?";
    }
}

AXL_TOOL_MAIN(smart)
{
    (void)argc;
    (void)argv;
    AxlStorageDev *dev = NULL;
    int            n = 0;

    while ((dev = axl_storage_next(dev)) != NULL) {
        AxlStorageTransport t = AXL_STORAGE_NVME;
        axl_storage_get_transport(dev, &t);

        char loc[160] = "";
        axl_storage_get_location(dev, loc, sizeof(loc));

        AxlSmartHealth h;
        if (axl_smart_health(dev, &h) != AXL_OK) {
            axl_printf("[%-4s] %s: health unavailable\n",
                       transport_name(t), loc);
            n++;
            continue;
        }

        axl_printf("[%-4s] %s\n", transport_name(t), loc);
        axl_printf("  %s  serial %s  fw %s\n",
                   h.model[0] ? h.model : "(unknown)",
                   h.serial[0] ? h.serial : "(none)",
                   h.firmware[0] ? h.firmware : "(none)");
        axl_printf("  health: %s", h.healthy ? "OK" : "FAILING");
        if (h.warning_flags != 0) {
            axl_printf("  warnings: 0x%02x", h.warning_flags);
        }
        if (h.temperature_c != INT32_MIN) {
            axl_printf("  temp: %d C", h.temperature_c);
        }
        if (h.percent_used != 0xFF) {
            axl_printf("  used: %u%%", h.percent_used);
        }
        if (h.power_on_hours != UINT64_MAX) {
            axl_printf("  power-on: %llu h",
                       (unsigned long long)h.power_on_hours);
        }
        if (h.media_errors != UINT64_MAX) {
            axl_printf("  media-errors: %llu",
                       (unsigned long long)h.media_errors);
        }
        axl_printf("\n");
        n++;
    }

    if (n == 0) {
        axl_printf("smart: no storage devices found\n");
    }
    return 0;
}
