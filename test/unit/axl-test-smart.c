/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-smart.c
    Unit tests for the AxlSmart pure normalizers — map an NVMe / ATA / SCSI
    decoded identity+health pair onto the uniform AxlSmartHealth record.
    Pure functions over constructed structs — no device, no QEMU. The union
    walk (axl_storage_next) and axl_smart_health device path are exercised by
    test/integration/test-smart-qemu.sh against an emulated NVMe controller.
**/

#include <axl.h>
#include "axl-test.h"

// ---------------------------------------------------------------------------
// NVMe
// ---------------------------------------------------------------------------

static void
test_from_nvme(void)
{
    AxlNvmeController id = { 0 };
    axl_strlcpy(id.model,    "NVME MODEL X", sizeof(id.model));
    axl_strlcpy(id.serial,   "NVSER123",     sizeof(id.serial));
    axl_strlcpy(id.firmware, "FW1.0",        sizeof(id.firmware));

    AxlNvmeSmart s = { 0 };
    s.healthy            = true;
    s.warn_temperature   = true;          /* -> WARN_TEMPERATURE */
    s.temperature_c      = 42;
    s.percent_used       = 7;
    s.media_errors       = 3;
    s.data_units_read    = 1000;
    s.data_units_written = 2000;
    s.power_on_hours     = 500;
    s.power_cycles       = 12;

    AxlSmartHealth h = { 0 };
    test_check(axl_smart_from_nvme(&id, &s, &h) == AXL_OK,
               "smart nvme: normalize -> AXL_OK");
    test_check(h.transport == AXL_STORAGE_NVME, "smart nvme: transport");
    test_check(axl_strcmp(h.model, "NVME MODEL X") == 0, "smart nvme: model");
    test_check(axl_strcmp(h.serial, "NVSER123") == 0, "smart nvme: serial");
    test_check(axl_strcmp(h.firmware, "FW1.0") == 0, "smart nvme: firmware");
    test_check(h.healthy, "smart nvme: healthy");
    test_check(h.warning_flags == AXL_SMART_WARN_TEMPERATURE,
               "smart nvme: warning_flags maps temperature bit");
    test_check(h.temperature_c == 42, "smart nvme: temperature");
    test_check(h.percent_used == 7, "smart nvme: percent_used");
    test_check(h.media_errors == 3, "smart nvme: media_errors");
    test_check(h.data_units_read == 1000 && h.data_units_written == 2000,
               "smart nvme: data units");
    test_check(h.power_on_hours == 500 && h.power_cycles == 12,
               "smart nvme: power on/cycles");

    /* Multiple critical-warning bits compose. */
    s.warn_temperature   = false;
    s.warn_spare_low     = true;
    s.warn_reliability   = true;
    test_check(axl_smart_from_nvme(&id, &s, &h) == AXL_OK
                   && h.warning_flags
                          == (AXL_SMART_WARN_SPARE | AXL_SMART_WARN_RELIABILITY),
               "smart nvme: warning_flags compose spare|reliability");

    test_check(axl_smart_from_nvme(NULL, &s, &h) == AXL_ERR,
               "smart nvme: NULL id -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// ATA
// ---------------------------------------------------------------------------

static void
test_from_ata(void)
{
    AxlAtaIdentify id = { 0 };
    axl_strlcpy(id.model,    "ATA MODEL", sizeof(id.model));
    axl_strlcpy(id.serial,   "ATASER",    sizeof(id.serial));
    axl_strlcpy(id.firmware, "AF2",       sizeof(id.firmware));

    AxlAtaSmart s = { 0 };
    s.healthy             = false;        /* -> WARN_RELIABILITY */
    s.temperature_c       = 30;
    s.power_on_hours      = 100;
    s.power_cycles        = 5;
    s.reallocated_sectors = 9;
    s.percent_used        = 0xFF;

    AxlSmartHealth h = { 0 };
    test_check(axl_smart_from_ata(&id, &s, &h) == AXL_OK,
               "smart ata: normalize -> AXL_OK");
    test_check(h.transport == AXL_STORAGE_ATA, "smart ata: transport");
    test_check(axl_strcmp(h.model, "ATA MODEL") == 0, "smart ata: model");
    test_check(axl_strcmp(h.serial, "ATASER") == 0, "smart ata: serial");
    test_check(axl_strcmp(h.firmware, "AF2") == 0, "smart ata: firmware");
    test_check(!h.healthy, "smart ata: unhealthy");
    test_check(h.warning_flags == AXL_SMART_WARN_RELIABILITY,
               "smart ata: unhealthy -> RELIABILITY bit");
    test_check(h.temperature_c == 30, "smart ata: temperature");
    test_check(h.media_errors == 9, "smart ata: media_errors = reallocated");
    test_check(h.percent_used == 0xFF, "smart ata: percent_used 0xFF");
    test_check(h.data_units_read == UINT64_MAX
                   && h.data_units_written == UINT64_MAX,
               "smart ata: data units unknown");
    test_check(h.power_on_hours == 100 && h.power_cycles == 5,
               "smart ata: power on/cycles");

    /* Healthy ATA -> no warning bits. */
    s.healthy = true;
    test_check(axl_smart_from_ata(&id, &s, &h) == AXL_OK
                   && h.healthy && h.warning_flags == 0,
               "smart ata: healthy -> no warning bits");
}

// ---------------------------------------------------------------------------
// SCSI
// ---------------------------------------------------------------------------

static void
test_from_scsi(void)
{
    AxlScsiInquiry inq = { 0 };
    axl_strlcpy(inq.vendor,   "AXLVND",    sizeof(inq.vendor));
    axl_strlcpy(inq.product,  "SCSI DISK", sizeof(inq.product));
    axl_strlcpy(inq.revision, "R1",        sizeof(inq.revision));
    axl_strlcpy(inq.serial,   "SCSISER",   sizeof(inq.serial));

    AxlScsiHealth sh = { 0 };
    sh.healthy       = true;
    sh.temperature_c = 25;

    AxlSmartHealth h = { 0 };
    test_check(axl_smart_from_scsi(&inq, &sh, &h) == AXL_OK,
               "smart scsi: normalize -> AXL_OK");
    test_check(h.transport == AXL_STORAGE_SCSI, "smart scsi: transport");
    test_check(axl_strcmp(h.model, "AXLVND SCSI DISK") == 0,
               "smart scsi: model = vendor product");
    test_check(axl_strcmp(h.serial, "SCSISER") == 0, "smart scsi: serial");
    test_check(axl_strcmp(h.firmware, "R1") == 0, "smart scsi: firmware = revision");
    test_check(h.healthy && h.warning_flags == 0, "smart scsi: healthy");
    test_check(h.temperature_c == 25, "smart scsi: temperature");
    test_check(h.power_on_hours == UINT64_MAX && h.power_cycles == UINT64_MAX
                   && h.media_errors == UINT64_MAX && h.percent_used == 0xFF,
               "smart scsi: unsupported fields use sentinels");

    /* Empty vendor -> model is just the product (no leading space). */
    inq.vendor[0] = '\0';
    test_check(axl_smart_from_scsi(&inq, &sh, &h) == AXL_OK
                   && axl_strcmp(h.model, "SCSI DISK") == 0,
               "smart scsi: empty vendor -> product only");

    test_check(axl_smart_from_scsi(&inq, NULL, &h) == AXL_ERR,
               "smart scsi: NULL health -> AXL_ERR");
}

static int
test_smart_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("AxlSmart");

    test_from_nvme();
    test_from_ata();
    test_from_scsi();

    return test_print_results();
}

AXL_APP(test_smart_main)
