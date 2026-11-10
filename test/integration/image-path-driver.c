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
 *
 * It also carries a PAD table of IMGPATH_PAD_FILL bytes and checks every
 * byte at entry, printing `IMGPATH: pad=ok` or `IMGPATH: pad=corrupt` (and
 * refusing to start). The default pad is small; image-path-driver-big.efi is
 * built with -DIMGPATH_PAD_BYTES large enough that the table is most of the
 * file, so test-vmedia-reads-qemu.sh can tell an image whose bytes were read
 * intact from one that came back with a byte flipped.
 */

#include <axl.h>

#define IMGPATH_SIDECAR "imgpath-sidecar.txt"
#ifndef IMGPATH_PAD_BYTES
#define IMGPATH_PAD_BYTES 64u
#endif
#define IMGPATH_PAD_FILL  0x5Au

/* volatile: the check must read the bytes the loader put in memory, not
   values the compiler already knows. */
static const volatile uint8_t imgpath_pad[IMGPATH_PAD_BYTES] = {
    [0 ... IMGPATH_PAD_BYTES - 1] = IMGPATH_PAD_FILL
};

static bool
imgpath_pad_intact(void)
{
    for (size_t i = 0; i < IMGPATH_PAD_BYTES; i++) {
        if (imgpath_pad[i] != IMGPATH_PAD_FILL) {
            return false;
        }
    }
    return true;
}

static int
imgpath_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    if (!imgpath_pad_intact()) {
        axl_printf("IMGPATH: pad=corrupt\n");
        return AXL_ERR;
    }
    axl_printf("IMGPATH: pad=ok\n");

    const char *self = axl_app_image_path();
    axl_printf("IMGPATH: self=%s\n", (self != NULL) ? self : "(null)");

    /* A buffer-loaded driver has no file, so nothing to be beside. */
    char beside[256];
    axl_printf("IMGPATH: drvdir=%s\n",
               axl_app_image_dir_path("x.conf", beside, sizeof(beside)) == AXL_OK
                   ? beside : "(err)");

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
