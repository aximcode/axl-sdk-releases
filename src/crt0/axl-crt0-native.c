/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-crt0-native.c — Native UEFI entry point for AXL applications.
 *
 * Initializes firmware table globals and bridges to int main().
 * Used only for applications (not drivers — drivers provide their
 * own entry point and call _axl_init() directly).
 *
 * Part of the AximCode AXL SDK.
 */

#include <uefi/axl-uefi.h>

/* gST, gBS, gRT declared in <uefi/axl-uefi-extra.h> */

/* AXL runtime init/cleanup (provided by libaxl.a) */
void _axl_init(void *image_handle, void *system_table);
void _axl_get_args(int *argc, char ***argv);
void _axl_cleanup(void);

/* User's application entry point */
int main(int argc, char **argv);

EFI_STATUS
EFIAPI
_AxlEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    /* Set globals before _axl_init — backend functions need them */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;

    _axl_init(ImageHandle, SystemTable);

    int argc;
    char **argv;
    _axl_get_args(&argc, &argv);

    int rc = main(argc, argv);

    _axl_cleanup();
    return (rc == 0) ? EFI_SUCCESS : EFI_ABORTED;
}
