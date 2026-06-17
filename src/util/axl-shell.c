/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-shell.c
    Launch a real UEFI Shell as a foreground child image.

    Shell *policy* over the generic foreground launcher: locate a
    `Shell.efi` (the EDK2 ShellLauncher search) and run it via
    @ref axl_image_run with the `-nostartup` convention — so a child Shell
    started *from* `startup.nsh` does not re-run `startup.nsh` and recurse
    back into the launcher. The generic "run a blocking UEFI app and get
    its exit code" mechanism is `axl_image_run` (`<axl/axl-image.h>`); this
    is just the Shell-specific wrapper around it.
**/

#include <axl/axl-shell.h>
#include <axl/axl-image.h>
#include <axl/axl-driver.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("shell");

int
axl_shell_launch(
    int *out_exit_code
    )
{
    char path[256];
    if (axl_driver_locate("Shell.efi", path, sizeof(path)) != AXL_OK) {
        axl_warning("shell launch: Shell.efi not found");
        if (out_exit_code != NULL) {
            *out_exit_code = 0;
        }
        return AXL_ERR;
    }
    /* -nostartup: the launcher is typically itself started from startup.nsh;
       without this the child Shell would re-run startup.nsh and recurse. */
    return axl_image_run(path, "-nostartup", out_exit_code);
}
