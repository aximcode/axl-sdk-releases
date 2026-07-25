/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-shell-launcher.c — a minimal chainloader the QEMU test harness stages as
 * \EFI\BOOT\BOOTX64.EFI (in place of the Shell itself). It loads the sibling
 * Shell.efi and starts it with LoadOptions "-delay 0".
 *
 * Why: when the EDK2 Shell is auto-booted with empty LoadOptions it runs a
 * 5-second "Press ESC in N seconds to skip startup.nsh" countdown — five
 * gBS->Stall(1s) busy-waits — before it runs startup.nsh. The root Shell reads
 * LoadedImage->LoadOptions as its argv (ShellParametersProtocol.c) and
 * ProcessCommandLine parses "-delay 0" into Delay=0, so the countdown loop
 * (Shell.c) never iterates. Skipping it reclaims ~5 s of Stall per guest boot,
 * a large fraction of the suite's wall time across hundreds of boots.
 *
 * This runs auto-booted by BdsDxe (the removable-media \EFI\BOOT fallback) with
 * no parent shell, so it must resolve Shell.efi without a shell environment.
 * axl_driver_load_sibling()'s primary path is shell-independent: it builds the
 * sibling's device path from this image's own LoadedImage->DeviceHandle, so it
 * needs no EFI_SHELL_PROTOCOL and no fsN: mapping.
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AxlDriverHandle sh = NULL;
    if (axl_driver_load_sibling("Shell.efi", &sh) != AXL_OK || sh == NULL) {
        axl_printf("axl-shell-launcher: cannot load Shell.efi beside me\n");
        return 1;
    }

    /* The root Shell reads LoadedImage->LoadOptions as its argv; "-delay 0"
       sets Delay=0 and skips the startup countdown. UCS-2, NUL-terminated —
       the UEFI command-line convention. */
    static const unsigned short delay0[] = {
        '-', 'd', 'e', 'l', 'a', 'y', ' ', '0', 0
    };
    if (axl_driver_set_load_options(sh, delay0, sizeof(delay0)) != AXL_OK) {
        /* Non-fatal: the Shell still boots, just with the 5 s countdown. */
        axl_printf("axl-shell-launcher: warning: could not set -delay 0\n");
    }

    /* Blocks until the Shell exits. In the harness the Shell's startup.nsh ends
       with `reset`, so control never returns here; but on an interactive exit it
       does, so unload to release the image + its LoadOptions copy either way
       (mirrors axl_image_run's load → start → unload). */
    int rc = axl_driver_start(sh);
    axl_driver_unload(sh);
    if (rc != AXL_OK) {
        axl_printf("axl-shell-launcher: Shell.efi failed to start\n");
        return 1;
    }
    return 0;
}
