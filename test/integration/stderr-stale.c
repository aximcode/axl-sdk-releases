/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stderr-stale.c -- starts a resident driver the way a Shell starts an image
 * it runs by name, with its standard error redirected to a file:
 *
 *   1. LoadImage the driver (stderr_tick-dxe.efi, next to this app);
 *   2. install EFI_SHELL_PARAMETERS_PROTOCOL on its image handle, StdErr a
 *      file the Shell opened (t.txt), as `2> t.txt` does;
 *   3. StartImage; then uninstall the parameters and close the file -- the
 *      Shell's InternalShellExecuteDevicePath and RestoreStdInStdOutStdErr.
 *
 * The EDK2 Shell runs only applications by name ("The image is not an
 * application"), so it never leaves a RESIDENT image holding a handle it
 * then frees; a Shell without that check, or any loader doing the above,
 * does. The driver stays resident and writes on its own clock after step 3:
 * a handle it kept from step 2 is freed by then (test-stderr-stale-qemu.sh).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>   /* the Shell protocols, gBS -- the fixture opts in */

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    EFI_SHELL_PROTOCOL            *shell = NULL;
    EFI_SHELL_PARAMETERS_PROTOCOL *own   = NULL;
    EFI_GUID                       sg    = gEfiShellProtocolGuid;
    EFI_GUID                       pg    = gEfiShellParametersProtocolGuid;
    if (EFI_ERROR(gBS->LocateProtocol(&sg, NULL, (void **)&shell)) || shell == NULL
        || EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)gImageHandle, &pg, (void **)&own))
        || own == NULL) {
        axl_printf("stderr-stale: no Shell\n");
        return 1;
    }

    AxlDriverHandle drv = NULL;
    if (axl_driver_load_sibling("stderr_tick-dxe.efi", &drv) != AXL_OK) {
        axl_printf("stderr-stale: load failed\n");
        return 1;
    }

    SHELL_FILE_HANDLE err = NULL;
    shell->DeleteFileByName(L"t.txt");
    if (EFI_ERROR(shell->OpenFileByName(L"t.txt", &err,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
        || err == NULL) {
        axl_printf("stderr-stale: cannot open t.txt\n");
        axl_driver_unload(drv);
        return 1;
    }

    static CHAR16 *argv0[] = { L"stderr_tick-dxe.efi" };
    static EFI_SHELL_PARAMETERS_PROTOCOL params;
    params.Argv   = argv0;
    params.Argc   = 1;
    params.StdIn  = own->StdIn;
    params.StdOut = own->StdOut;
    params.StdErr = err;
    EFI_HANDLE h = (EFI_HANDLE)drv;
    if (EFI_ERROR(gBS->InstallProtocolInterface(&h, &pg, EFI_NATIVE_INTERFACE,
                                                &params))) {
        axl_printf("stderr-stale: cannot install the parameters\n");
        shell->CloseFile(err);
        axl_driver_unload(drv);
        return 1;
    }
    int rc = axl_driver_start(drv);
    gBS->UninstallProtocolInterface(h, &pg, &params);
    shell->CloseFile(err);
    axl_printf("stderr-stale: started rc=%d, parameters gone, file closed\n", rc);
    return rc == AXL_OK ? 0 : 1;
}
