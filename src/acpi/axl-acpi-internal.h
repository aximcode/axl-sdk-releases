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

#ifdef __cplusplus
}
#endif

#endif /* AXL_ACPI_INTERNAL_H */
