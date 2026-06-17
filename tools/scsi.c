/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file scsi.c
    scsi — list SCSI/SAS logical units with identity, capacity, and health.
    The dogfood tool for <axl/axl-scsi.h>: a thin renderer over
    axl_scsi_next / _inquiry / _read_capacity / _health. A smartctl-style
    view for UEFI SCSI devices.
**/

#include <axl.h>

static const char *
devtype_name(uint8_t t)
{
    switch (t) {
    case AXL_SCSI_DEVTYPE_DISK:  return "disk";
    case AXL_SCSI_DEVTYPE_TAPE:  return "tape";
    case AXL_SCSI_DEVTYPE_CDROM: return "cdrom";
    case AXL_SCSI_DEVTYPE_SES:   return "enclosure";
    case AXL_SCSI_DEVTYPE_RBC:   return "rbc";
    default:                     return "other";
    }
}

int
main(void)
{
    AxlScsiDev *dev = NULL;
    int         n = 0;

    while ((dev = axl_scsi_next(dev)) != NULL) {
        uint64_t lun = 0;
        axl_scsi_get_address(dev, NULL, &lun);

        AxlScsiInquiry inq;
        if (axl_scsi_inquiry(dev, &inq) != AXL_OK) {
            axl_printf("scsi%d (lun %llu): INQUIRY failed\n",
                       n, (unsigned long long)lun);
            n++;
            continue;
        }
        axl_printf("scsi%d (lun %llu): %s %s [%s]\n",
                   n, (unsigned long long)lun,
                   inq.vendor, inq.product, devtype_name(inq.device_type));
        axl_printf("  rev: %s  serial: %s\n",
                   inq.revision,
                   inq.serial[0] != '\0' ? inq.serial : "(none)");

        AxlScsiCapacity cap;
        if (axl_scsi_read_capacity(dev, &cap) == AXL_OK) {
            axl_printf("  %llu blocks x %u B = %llu bytes\n",
                       (unsigned long long)cap.size_blocks, cap.block_size,
                       (unsigned long long)cap.capacity_bytes);
        }

        AxlScsiHealth h;
        if (axl_scsi_health(dev, &h) == AXL_OK) {
            axl_printf("  health: %s", h.healthy ? "OK" : "FAILING");
            if (h.temperature_c != INT32_MIN) {
                axl_printf("  temp: %d C", h.temperature_c);
            }
            if (!h.healthy) {
                axl_printf("  (asc 0x%02x/0x%02x)", h.asc, h.ascq);
            }
            axl_printf("\n");
        } else {
            axl_printf("  health: (IE log not available)\n");
        }
        n++;
    }

    if (n == 0) {
        axl_printf("scsi: no SCSI devices found\n");
    }
    return 0;
}
