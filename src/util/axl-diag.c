/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-diag.c
    Diagnostic helpers — image-launch state dump + protocol probe.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-diag.h>
#include <axl/axl-driver.h>
#include <axl/axl-format.h>
#include <axl/axl-io.h>
#include <axl/axl-mem.h>
#include <axl/axl-path.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>

#define DIAG_VOLUMES_MAX  16

#define DIAG_ARGV_MAX     16

void
axl_diag_startup(
    int    argc,
    char **argv
    )
{
    extern EFI_HANDLE gImageHandle;

    axl_printf("--- AXL diag ---\n");

    /* (1) POSIX argv as it reached main, after axl-app.c parsed
     *     EFI_LOADED_IMAGE_PROTOCOL.LoadOptions. */
    axl_printf("POSIX argc = %d\n", argc);
    int argv_shown = (argc < DIAG_ARGV_MAX) ? argc : DIAG_ARGV_MAX;
    for (int i = 0; i < argv_shown; i++) {
        const char *a = (argv != NULL && argv[i] != NULL) ? argv[i] : "(NULL)";
        axl_printf("POSIX argv[%d] = \"%s\"\n", i, a);
    }
    if (argc > DIAG_ARGV_MAX) {
        axl_printf("POSIX (... %d more not shown)\n", argc - DIAG_ARGV_MAX);
    }

    /* (2) Raw LoadOptions — the spec-mandated source of truth. */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_GUID li_guid = gEfiLoadedImageProtocolGuid;
    EFI_STATUS li_st = axl_bs()->HandleProtocol(
        gImageHandle, &li_guid, (void **)&li);
    if (!EFI_ERROR(li_st) && li != NULL) {
        axl_printf("LOADOPT: size = %lu bytes\n",
                   (unsigned long)li->LoadOptionsSize);
        if (li->LoadOptions != NULL
            && li->LoadOptionsSize >= sizeof(unsigned short)) {
            AXL_AUTO_FREE char *utf =
                axl_ucs2_to_utf8((const unsigned short *)li->LoadOptions);
            axl_printf("LOADOPT: utf8 = \"%s\"\n",
                       utf != NULL ? utf : "(conv-failed)");
        }
    } else {
        axl_printf("LOADOPT: HandleProtocol failed (status=0x%lx)\n",
                   (unsigned long)li_st);
    }

    /* (3) EFI_SHELL_PARAMETERS_PROTOCOL probe + its argv. Optional;
     *     Dell firmware sometimes doesn't publish it for cross-volume
     *     invocations. */
    EFI_SHELL_PARAMETERS_PROTOCOL *sp = NULL;
    EFI_GUID sp_guid = gEfiShellParametersProtocolGuid;
    EFI_STATUS sp_st = axl_bs()->HandleProtocol(
        gImageHandle, &sp_guid, (void **)&sp);
    if (EFI_ERROR(sp_st) || sp == NULL) {
        axl_printf("SHELL: protocol NOT AVAILABLE (status=0x%lx)\n",
                   (unsigned long)sp_st);
    } else {
        axl_printf("SHELL: protocol OK, Argc = %lu\n",
                   (unsigned long)sp->Argc);
        unsigned shown = (sp->Argc < DIAG_ARGV_MAX)
            ? (unsigned)sp->Argc : DIAG_ARGV_MAX;
        for (unsigned i = 0; i < shown; i++) {
            AXL_AUTO_FREE char *utf8 = axl_ucs2_to_utf8(sp->Argv[i]);
            axl_printf("SHELL: Argv[%u] = \"%s\"\n",
                       i, utf8 != NULL ? utf8 : "(conv-failed)");
        }
        if (sp->Argc > DIAG_ARGV_MAX) {
            axl_printf("SHELL: (... %lu more not shown)\n",
                       (unsigned long)(sp->Argc - DIAG_ARGV_MAX));
        }
    }

    /* (4) Image path — anchors axl_driver_ensure's first-priority
     *     search paths. */
    AXL_AUTO_FREE char *img_path = axl_driver_get_image_path();
    axl_printf("IMG: path = %s\n",
               img_path != NULL ? img_path : "(unavailable)");

    /* (5) Mounted FAT volumes. n_vols is what enumerate filled, capped
     * at DIAG_VOLUMES_MAX. If a system has more, we say so explicitly
     * rather than silently truncating. */
    AxlVolume vols[DIAG_VOLUMES_MAX];
    size_t n_vols = 0;
    axl_volume_enumerate(vols, DIAG_VOLUMES_MAX, &n_vols);
    axl_printf("VOLUMES: %zu mounted%s\n",
               n_vols,
               n_vols == DIAG_VOLUMES_MAX
                   ? " (cap reached; more may exist)" : "");
    for (size_t i = 0; i < n_vols; i++) {
        axl_printf("  %s\n", vols[i].name);
    }

    axl_printf("--- /AXL diag ---\n");
}

int
axl_diag_probe_protocol(
    const AxlGuid *protocol_guid,
    const char    *display_name
    )
{
    if (protocol_guid == NULL) {
        return -1;
    }
    void *iface = NULL;
    /* See axl-driver.c for why this cast is non-const. */
    EFI_STATUS st = axl_bs()->LocateProtocol(
        (EFI_GUID *)protocol_guid, NULL, &iface);
    const char *tag = (display_name != NULL) ? display_name : "(unnamed)";
    axl_printf("PROBE: %s %s (LocateProtocol=0x%lx)\n",
               tag,
               EFI_ERROR(st) ? "NOT registered" : "ALREADY REGISTERED",
               (unsigned long)st);
    return EFI_ERROR(st) ? -1 : 0;
}
