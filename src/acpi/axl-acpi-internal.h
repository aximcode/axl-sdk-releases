/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-acpi-internal.h
    Shared between the ACPI module's split source files
    (axl-acpi.c discovery + the per-table typed readers).

    Not part of the public SDK surface — consumers go through
    `<axl/axl-acpi.h>`.
**/

#ifndef AXL_ACPI_INTERNAL_H
#define AXL_ACPI_INTERNAL_H

#include "../backend/axl-backend.h"
#include <axl/axl-acpi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All ACPI System Description Tables share a 36-byte header
   (signature, length, revision, checksum, OEM IDs, …) per
   ACPI spec §5.2.6. The typed readers use this to skip past the
   common header to the table-specific body. */
#define ACPI_HEADER_SIZE  36u

/* FADT (signature "FACP") field offsets per ACPI 6.x spec §5.2.9.
   Shared by the FADT typed reader (axl-acpi-fadt.c) and the table
   catalog (axl-acpi.c), which recovers the FADT's FACS + DSDT children
   so they appear in the iteration. */
#define FADT_OFF_FIRMWARE_CTRL    36u
#define FADT_OFF_DSDT             40u
#define FADT_OFF_SMI_CMD          48u
#define FADT_OFF_ACPI_ENABLE      52u
#define FADT_OFF_ACPI_DISABLE     53u
#define FADT_OFF_PM1A_EVT_BLK     56u
#define FADT_OFF_PM1B_EVT_BLK     60u
#define FADT_OFF_PM1A_CNT_BLK     64u
#define FADT_OFF_PM1B_CNT_BLK     68u
#define FADT_OFF_PM1_EVT_LEN      88u
#define FADT_OFF_PM1_CNT_LEN      89u
#define FADT_OFF_IAPC_BOOT_ARCH  109u
#define FADT_OFF_ARM_BOOT_ARCH   129u
#define FADT_OFF_X_FIRMWARE_CTRL 132u
#define FADT_OFF_X_DSDT          140u

#define FADT_MIN_LEN_LEGACY  116u  /* ACPI 1.0 minimum */
#define FADT_MIN_LEN_2_0     244u  /* ACPI 2.0 minimum (covers x_dsdt) */

#ifdef __cplusplus
}
#endif

#endif /* AXL_ACPI_INTERNAL_H */
