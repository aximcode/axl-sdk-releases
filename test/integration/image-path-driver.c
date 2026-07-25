/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * image-path-driver.c — reports what a loaded driver image sees for its own
 * `axl_app_image_path()` and for sidecar discovery.
 *
 * Driven by test-image-path-qemu.sh, which starts this driver twice: once
 * buffer-loaded (a synthetic load context — the firmware sets no FilePath and
 * AXL synthesizes a MemoryMapped(...)/FilePath device path for it) and once
 * loaded from its real path on disk.
 *
 * `<axl/axl-app.h>` documents NULL for "synthetic load contexts that bypass
 * the usual file-load path"; the buffer-loaded run is the one that has to
 * honour it. Sidecar discovery must keep working on that run regardless — it
 * anchors on the nearest ancestor image that WAS loaded from a file (the
 * launcher), which is exactly what the doc-honouring NULL frees it to do.
 */

#include <axl.h>

#define IMGPATH_SIDECAR "imgpath-sidecar.txt"

static int
imgpath_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    const char *self = axl_app_image_path();
    axl_printf("IMGPATH: self=%s\n", (self != NULL) ? self : "(null)");

    AXL_AUTO_FREE char *sidecar = axl_resolve_data_file(NULL, IMGPATH_SIDECAR);
    axl_printf("IMGPATH: sidecar=%s\n",
               (sidecar != NULL) ? sidecar : "(null)");
    return AXL_OK;
}

static int
imgpath_unload(AxlHandle image)
{
    (void)image;
    return AXL_OK;
}

AXL_DRIVER(imgpath_main, imgpath_unload)
