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
#include <axl/axl-sys.h>     /* AxlGuid, AXL_GUID */
#include <axl/axl-log.h>
#include "../backend/axl-backend.h"   /* axl_bs() (gBS) + UEFI types/GUIDs */
#include "../fv/axl-fv-internal.h"

AXL_LOG_DOMAIN("shell");

extern EFI_HANDLE gImageHandle;   /* set by the runtime at startup */

/* Well-known UEFI Shell FFS file name GUIDs to look for in firmware
   volumes, in search-preference order:
   1. gUefiShellFileGuid — the GUID EDK2 platform FDFs (OVMF, AAVMF, most
      vendor firmware) name the embedded Shell FV file with. Empirically the
      one OVMF/AAVMF carry.
   2. ShellPkg Shell.inf FILE_GUID — what a firmware that embeds the Shell
      *module* directly (not via gUefiShellFileGuid) names it; also the
      FILE_GUID a standalone Shell.efi binary carries. */
static const AxlGuid SHELL_FV_GUIDS[] = {
    AXL_GUID(0x7C04A583, 0x9E3E, 0x4F1C,
             0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1),
    AXL_GUID(0xEA4BB293, 0x2D7F, 0x4456,
             0xA6, 0x81, 0x1F, 0x22, 0xF4, 0x2C, 0xD0, 0xBC),
};
#define SHELL_FV_GUID_COUNT (sizeof(SHELL_FV_GUIDS) / sizeof(SHELL_FV_GUIDS[0]))

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

int
axl_shell_launch_fv(
    const char *load_options,
    int        *out_exit_code
    )
{
    if (out_exit_code != NULL) {
        *out_exit_code = 0;
    }
    /* Try each known Shell file GUID in turn. axl_image_run_fv_file returns
       AXL_ERR when the GUID isn't carried by any readable FV (move on to the
       next); on the first one that loads it StartImages the Shell, blocks
       until it exits, and returns AXL_OK. */
    for (size_t i = 0; i < SHELL_FV_GUID_COUNT; i++) {
        if (axl_image_run_fv_file(&SHELL_FV_GUIDS[i], load_options,
                                  out_exit_code) == AXL_OK) {
            return AXL_OK;
        }
    }
    axl_warning("shell launch_fv: no firmware-embedded Shell in a readable FV");
    return AXL_ERR;
}

AxlShellSource
axl_shell_locate(void)
{
    /* Prefer a staged file — cheaper to launch and matches a consumer's own
       deployed copy (the axl_shell_launch search path). */
    char path[256];
    if (axl_driver_locate("Shell.efi", path, sizeof(path)) == AXL_OK) {
        return AXL_SHELL_FILE;
    }
    /* Else look for the firmware-embedded Shell in a readable FV. */
    for (size_t i = 0; i < SHELL_FV_GUID_COUNT; i++) {
        AxlHandle fv = NULL;
        if (_axl_fv_find_app_file(&SHELL_FV_GUIDS[i], &fv) == AXL_OK) {
            return AXL_SHELL_FIRMWARE;
        }
    }
    return AXL_SHELL_NONE;
}

AxlShellKind
axl_shell_kind(void)
{
    void *iface = NULL;

    /* Modern EDK2 UEFI Shell first: it never publishes the legacy EFI 1.x
       GUIDs, so a hit here is unambiguous and needs no further probing. */
    EFI_GUID uefi_guid = gEfiShellProtocolGuid;
    if (!EFI_ERROR(axl_bs()->LocateProtocol(&uefi_guid, NULL, &iface))
        && iface != NULL) {
        return AXL_SHELL_KIND_UEFI;
    }

    /* Old EFI 1.x shell: SHELL_ENVIRONMENT is installed globally, so a plain
       LocateProtocol finds it whenever that shell is present. */
    EFI_GUID env_guid = gEfiShellEnvironmentGuid;
    iface = NULL;
    if (!EFI_ERROR(axl_bs()->LocateProtocol(&env_guid, NULL, &iface))
        && iface != NULL) {
        return AXL_SHELL_KIND_EFI_1X;
    }

    /* Fallback: the old shell also installs SHELL_INTERFACE on each
       shell-launched image handle — so probe our own handle in case the
       global environment protocol isn't locatable on a given build. */
    if (gImageHandle != NULL) {
        EFI_GUID if_guid = gEfiShellInterfaceGuid;
        iface = NULL;
        if (!EFI_ERROR(axl_bs()->HandleProtocol(gImageHandle, &if_guid, &iface))
            && iface != NULL) {
            return AXL_SHELL_KIND_EFI_1X;
        }
    }

    return AXL_SHELL_KIND_NONE;
}
