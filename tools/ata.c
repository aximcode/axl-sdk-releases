/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file ata.c
    ata — list ATA/SATA devices with identity and SMART health. The
    dogfood tool for <axl/axl-ata.h>: a thin renderer over axl_ata_next /
    _identify / _smart / _self_test_result. A smartctl-style view for UEFI.
**/

#include <axl.h>

AXL_TOOL_MAIN(ata)
{
    (void)argc;
    (void)argv;
    AxlAtaDev *dev = NULL;
    int        n = 0;

    while ((dev = axl_ata_next(dev)) != NULL) {
        uint16_t port = 0, pmport = 0;
        axl_ata_get_address(dev, &port, &pmport);

        AxlAtaIdentify id;
        if (axl_ata_identify(dev, &id) != AXL_OK) {
            axl_printf("ata%d (port %u.%u): IDENTIFY failed\n", n, port, pmport);
            n++;
            continue;
        }
        axl_printf("ata%d (port %u.%u): %s\n", n, port, pmport, id.model);
        axl_printf("  serial: %s  firmware: %s  SMART: %s\n",
                   id.serial, id.firmware,
                   id.smart_supported ? "supported" : "no");
        axl_printf("  %llu blocks x %u B = %llu bytes\n",
                   (unsigned long long)id.size_blocks, id.block_size,
                   (unsigned long long)id.capacity_bytes);

        if (id.smart_supported) {
            AxlAtaSmart s;
            if (axl_ata_smart(dev, &s) == AXL_OK) {
                axl_printf("  health: %s", s.healthy ? "OK" : "FAILING");
                if (s.temperature_c == INT32_MIN) {
                    axl_printf("  temp: n/a");
                } else {
                    axl_printf("  temp: %d C", s.temperature_c);
                }
                if (s.power_on_hours != UINT64_MAX) {
                    axl_printf("  power-on: %llu h",
                               (unsigned long long)s.power_on_hours);
                }
                if (s.reallocated_sectors != UINT64_MAX) {
                    axl_printf("  reallocated: %llu",
                               (unsigned long long)s.reallocated_sectors);
                }
                axl_printf("\n");
            } else {
                axl_printf("  health: (SMART read failed)\n");
            }

            AxlAtaSelfTestResult st;
            if (axl_ata_self_test_result(dev, &st) == AXL_OK) {
                if (st.in_progress) {
                    axl_printf("  self-test: in progress (%u%%)\n",
                               st.percent_complete);
                } else {
                    axl_printf("  self-test: last result %s (code %u)\n",
                               st.passed ? "passed" : "failed", st.result_code);
                }
            }
        }
        n++;
    }

    if (n == 0) {
        axl_printf("ata: no ATA/SATA devices found\n");
    }
    return 0;
}
