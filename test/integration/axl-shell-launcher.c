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
 *
 * An optional axl-shell.opt beside it (one line, ASCII) is appended to
 * "-delay 0": the harness writes AXL_SHELL_OPTIONS there (stage_boot_shell,
 * scripts/axl-common.sh), which is how a guest boots its Shell `-nonest` --
 * the mode in which EFI_SHELL_PROTOCOL.Execute reports whether a command ran
 * (docs/AXL-Test-Guest-Pool-Design.md §2.4).
 */

#include <axl.h>

#define LAUNCHER_OPT_FILE "axl-shell.opt"
#define LAUNCHER_OPT_MAX  200

/* The Shell's command line: "-delay 0", then axl-shell.opt's line if present.
   An unreadable or oversized file is reported and left out; the Shell still
   boots, and the caller that asked for the option sees it missing. */
static void
shell_options(
    char   *out,
    size_t  out_size
    )
{
    axl_snprintf(out, out_size, "-delay 0");
    char       path[256];
    AxlFsEntry info;
    if (axl_app_image_dir_path(LAUNCHER_OPT_FILE, path, sizeof(path)) != AXL_OK) {
        /* Not "no options": we could not even look. Say so, or a caller
           relying on an option sees only its absence. */
        axl_printf("axl-shell-launcher: warning: cannot resolve %s beside me\n",
                   LAUNCHER_OPT_FILE);
        return;
    }
    if (axl_file_info(path, &info) != AXL_OK) {
        return;   /* no options file: the usual case */
    }
    void   *buf = NULL;
    size_t  len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK) {
        axl_printf("axl-shell-launcher: warning: cannot read %s\n", path);
        return;
    }
    if (len > LAUNCHER_OPT_MAX) {
        axl_printf("axl-shell-launcher: warning: %s is over %u bytes; ignored\n",
                   path, (unsigned)LAUNCHER_OPT_MAX);
        axl_free(buf);
        return;
    }
    char extra[LAUNCHER_OPT_MAX + 1];
    axl_memcpy(extra, buf, len);
    extra[len] = '\0';
    axl_free(buf);
    axl_strstrip(extra);
    if (extra[0] != '\0') {
        axl_snprintf(out, out_size, "-delay 0 %s", extra);
    }
}

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
    char opts[LAUNCHER_OPT_MAX + 16];
    shell_options(opts, sizeof(opts));
    unsigned short *wopts = axl_utf8_to_ucs2(opts);
    /* Sized from the UCS-2 string itself: a UTF-8 byte count over-reads it
       for anything non-ASCII (the conversion emits fewer code units). */
    size_t units = 0;
    while (wopts != NULL && wopts[units] != 0) {
        units++;
    }
    if (wopts == NULL ||
        axl_driver_set_load_options(sh, wopts, (units + 1) * sizeof(*wopts)) != AXL_OK) {
        /* Non-fatal: the Shell still boots, just with the 5 s countdown. */
        axl_printf("axl-shell-launcher: warning: could not set \"%s\"\n", opts);
    }
    axl_free(wopts);

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
