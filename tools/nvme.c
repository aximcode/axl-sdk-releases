/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file nvme.c
    nvme — list NVMe controllers with identity, SMART/health, and
    namespaces. The dogfood tool for <axl/axl-nvme.h>: a thin renderer
    over axl_nvme_next / _identify_controller / _smart / _namespace_next /
    _identify_namespace. A smartctl-style health view for UEFI.
**/

#include <axl.h>

static void
print_smart(AxlHandle ctrl)
{
    AxlNvmeSmart s;
    if (axl_nvme_smart(ctrl, &s) != AXL_OK) {
        axl_printf("  health: (SMART log unavailable)\n");
        return;
    }
    axl_printf("  health: %s", s.healthy ? "OK" : "FAILING");
    if (s.temperature_c == INT32_MIN) {
        axl_printf("  temp: n/a");
    } else {
        axl_printf("  temp: %d C", s.temperature_c);
    }
    axl_printf("  used: %u%%  spare: %u%%/%u%%\n",
               s.percent_used, s.available_spare, s.available_spare_threshold);
    axl_printf("  power-on: %llu h  cycles: %llu  unsafe-shutdowns: %llu\n",
               (unsigned long long)s.power_on_hours,
               (unsigned long long)s.power_cycles,
               (unsigned long long)s.unsafe_shutdowns);
    axl_printf("  data read: %llu  written: %llu (units of 512000 B)\n",
               (unsigned long long)s.data_units_read,
               (unsigned long long)s.data_units_written);
    axl_printf("  media errors: %llu  error-log entries: %llu\n",
               (unsigned long long)s.media_errors,
               (unsigned long long)s.error_log_entries);
    if (!s.healthy) {
        axl_printf("  warnings:%s%s%s%s%s\n",
                   s.warn_spare_low ? " spare-low" : "",
                   s.warn_temperature ? " temperature" : "",
                   s.warn_reliability ? " reliability" : "",
                   s.warn_read_only ? " read-only" : "",
                   s.warn_volatile_backup ? " backup-failed" : "");
    }
}

int
main(void)
{
    AxlHandle ctrl = NULL;
    int       n = 0;

    while ((ctrl = axl_nvme_next(ctrl)) != NULL) {
        AxlNvmeController c;
        if (axl_nvme_identify_controller(ctrl, &c) != AXL_OK) {
            axl_printf("nvme%d: Identify Controller failed\n", n);
            n++;
            continue;
        }
        axl_printf("nvme%d: %s\n", n, c.model);
        axl_printf("  serial: %s  firmware: %s  NVMe %u.%u.%u  namespaces: %u\n",
                   c.serial, c.firmware,
                   (c.nvme_version >> 16) & 0xFFFF,
                   (c.nvme_version >> 8) & 0xFF,
                   c.nvme_version & 0xFF,
                   c.namespace_count);

        print_smart(ctrl);

        /* The firmware iterator can report every namespace id the
           controller allows (QEMU exposes 256), so skip the inactive ones
           — an unattached namespace reports size_blocks == 0. */
        uint32_t nsid = 0;
        while ((nsid = axl_nvme_namespace_next(ctrl, nsid)) != 0) {
            AxlNvmeNamespace ns;
            if (axl_nvme_identify_namespace(ctrl, nsid, &ns) == AXL_OK
                && ns.size_blocks > 0) {
                axl_printf("  ns %u: %llu blocks x %u B = %llu bytes\n",
                           ns.nsid,
                           (unsigned long long)ns.size_blocks,
                           ns.block_size,
                           (unsigned long long)ns.capacity_bytes);
            }
        }
        n++;
    }

    if (n == 0) {
        axl_printf("nvme: no NVMe controllers found\n");
    }
    return 0;
}
