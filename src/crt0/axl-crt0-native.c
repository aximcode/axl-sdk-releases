/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-crt0-native.c — Native UEFI entry point for AXL applications.
 *
 * Initializes firmware table globals and bridges to int main().
 * Used only for applications. A driver provides its own entry point
 * (DriverEntry) and calls axl_driver_init() / axl_driver_cleanup()
 * instead — see <axl/axl-driver.h>. Both pairs run .init_array and
 * drain axl_atexit; they differ in what else they set up.
 *
 * Part of the AximCode AXL SDK.
 */

#include <uefi/axl-uefi.h>

/* gST, gBS, gRT declared in <uefi/axl-uefi-extra.h> */

/* AXL runtime init/cleanup (provided by libaxl.a) */
void _axl_init(void *image_handle, void *system_table);
void _axl_get_args(int *argc, char ***argv);
void _axl_cleanup(void);

/* Resolve the exit status for a main() return code: a pending
 * axl_set_exit_status wins verbatim, else rc maps 0 -> EFI_SUCCESS /
 * nonzero -> EFI_ABORTED. Defined in the backend (libaxl.a); UINTN-width. */
unsigned long long axl_backend_resolve_exit_status(int rc);

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
    /* A pending axl_set_exit_status (if any) overrides the rc->status map,
     * so a `return N` from main can yield an exact, verbatim EFI_STATUS. */
    return (EFI_STATUS)axl_backend_resolve_exit_status(rc);
}
