/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-leak.c — a launcher that LEAKS a stdio bridge.
 *
 * Models the consumer (do.efi) warm-path crash condition: a launcher
 * installs the stdio bridge — capturing THIS image's piped StdIn — and
 * then exits via gBS->Exit, which returns straight to firmware WITHOUT
 * running CRT0's _axl_cleanup. The atexit uninstall therefore never
 * fires, so the bridge protocol stays installed in the firmware DB with
 * an `stdin_h` that points at the pipe handle the shell frees the moment
 * this image unloads — a dangling SHELL_FILE_HANDLE.
 *
 * A subsequent resident-driver read that naively dereferences the first
 * bridge instance would pass that freed handle to the shell -> #GP/#PF
 * in Shell.dll (the exact do.efi symptom). The driver must instead skip
 * this stale instance (launcher_image no longer a loaded image).
 *
 * Used only by test-driver-stdio-qemu.sh — never a real-world pattern.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>   /* gBS, gImageHandle — fixture opts in explicitly */
#include "stdio-bridge-fix.h"

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Capture this launcher's (piped) StdIn into the bridge. */
    axl_shared_driver_install_stdio_bridge();

    /* Exit WITHOUT CRT0 cleanup -> the bridge is never uninstalled.
       The shell unloads this image and frees its pipe StdIn handle,
       leaving a stale bridge that references freed memory. */
    gBS->Exit(gImageHandle, EFI_SUCCESS, 0, NULL);
    return 0;   /* unreachable */
}
