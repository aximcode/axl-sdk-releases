/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * fbcon.c -- launcher for the `fbcon` graphical-terminal take-over.
 *
 * fbcon is a resident driver (it takes over the running Shell's console and renders
 * it on the GOP framebuffer -- see fbcon-drv.c). Rather than make the user `load` a
 * driver by hand, this thin application EMBEDS the driver and loads it from memory, so
 * `fbcon.efi` runs as an ordinary command. Each run first reaps any resident fbcon
 * instance (a driver cannot unload itself, but this separate launcher image can), then
 * starts a fresh one -- so `fbcon.efi` always leaves you in a clean take-over. Leave
 * fbcon with Ctrl+\ or by exiting the shell.
 *
 * The embed + load-from-buffer + presence-marker reap is the same pattern do.efi /
 * mkrd use (AXL_EMBED_DECLARE + axl_driver_load_buffer_with_image_info).
 */

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-embed.h>
#include <uefi/axl-uefi.h>

#include "fbcon-marker.h"

AXL_LOG_DOMAIN("fbcon");

/* The resident take-over driver, embedded at build time (EMBED_BLOB in the Makefile
   emits axl_embedded_fbcon_drv{,_end}). */
AXL_EMBED_DECLARE(fbcon_drv);

/* UnloadImage every resident fbcon driver (those carrying the presence marker). Safe
   from here because this launcher is a SEPARATE image from the driver being unloaded.
   Returns the count reaped. */
static int
reap_resident_fbcon(void)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    int         reaped  = 0;
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &FBCON_PRESENCE_GUID,
                                           NULL, &n, &handles)) && handles != NULL) {
        for (UINTN i = 0; i < n; i++) {
            if (!EFI_ERROR(gBS->UnloadImage((EFI_HANDLE)handles[i]))) {
                reaped++;
            }
        }
        gBS->FreePool(handles);
    }
    return reaped;
}

int
main(int argc, char **argv)
{
    /* `fbcon --version` / `-V`: print "fbcon <version>" and exit, uniform with
       every AXL_TOOL_MAIN tool (fbcon is a launcher, not a framework tool, so it
       calls the shared hook directly instead of getting it from the macro). */
    if (axl_version_handle("fbcon", argc, argv)) {
        return 0;
    }
    /* -h/--help via the shared hook — fbcon is a launcher, not a framework
       tool, so it answers help the same way it answers --version. */
    if (axl_help_handle("fbcon",
            "Framebuffer console take-over (resident driver)",
            "fbcon [-d <ms>] [-g <ms>]",
            argc, argv)) {
        return 0;
    }

    int reaped = reap_resident_fbcon();
    if (reaped > 0) {
        axl_info("reaped %d prior fbcon instance(s)", reaped);
    }

    /* Load + start the embedded take-over driver. axl_driver_start runs its entry,
       which installs the console device and takes over; it stays resident after we
       return. The identity gives the memory-loaded image a renderable device path
       (the aarch64 shell faults on a NULL one). */
    AxlEmbeddedImageInfo info = { .file_name = "fbcon-drv.efi" };
    AxlDriverHandle      drv  = NULL;
    if (axl_driver_load_buffer_with_image_info(
            AXL_EMBED_DATA(fbcon_drv), AXL_EMBED_SIZE(fbcon_drv), &info, &drv) != 0) {
        axl_error("fbcon: could not load the embedded take-over driver");
        return 1;
    }

    /* Forward our own command line to the resident driver so it can read the optional
       input-gate knobs (`fbcon.efi -d <ms> -g <ms>`). Our LoadOptions is already the
       UCS-2 command-line string the shell handed us; pass it through verbatim (the
       driver reads it back via axl_driver_get_load_options and ignores our arg0). */
    const void *lo      = NULL;
    size_t      lo_size = 0;
    if (axl_driver_get_load_options_raw(&lo, &lo_size) == AXL_OK
        && lo != NULL && lo_size > 0) {
        axl_driver_set_load_options(drv, lo, lo_size);
    }

    if (axl_driver_start(drv) != 0) {
        axl_error("fbcon: could not start the take-over driver");
        axl_driver_unload(drv);
        return 1;
    }
    return 0;   /* the driver is resident; we return to the shell */
}
