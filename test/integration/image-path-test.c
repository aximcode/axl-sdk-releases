/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * image-path-test.c — launcher for the axl_app_image_path() synthetic-load
 * contract (test-image-path-qemu.sh).
 *
 * Starts image-path-driver.efi twice from its own directory:
 *   1. buffer-loaded  — a synthetic load context: the firmware leaves
 *      LoadedImage->FilePath NULL and AXL synthesizes a
 *      MemoryMapped(...)/FilePath device path so the aa64 shell can render
 *      the handle. The driver must report self=(null) per the documented
 *      contract, and must still find its sidecar (via this launcher, its
 *      nearest ancestor that WAS loaded from a file).
 *   2. path-loaded    — an ordinary file load. The driver must report its
 *      real path. This is the regression guard on the change.
 */

#include <axl.h>

#define IMGPATH_DRIVER "image-path-driver.efi"

static int
run_from_buffer(const char *path)
{
    void   *buf = NULL;
    size_t  len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK
        || buf == NULL || len == 0) {
        axl_printf("IMGPATH: FAIL cannot read %s\n", path);
        return AXL_ERR;
    }

    AxlDriverHandle drv = NULL;
    int rc = axl_driver_load_buffer((const unsigned char *)buf, len, &drv);
    axl_free(buf);   /* LoadImage copies the source buffer */
    if (rc != AXL_OK || drv == NULL) {
        axl_printf("IMGPATH: FAIL buffer load\n");
        return AXL_ERR;
    }
    axl_printf("IMGPATH_BUFFER_LOAD\n");
    if (axl_driver_start(drv) != AXL_OK) {
        axl_printf("IMGPATH: FAIL buffer start\n");
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
run_from_path(const char *path)
{
    AxlDriverHandle drv = NULL;
    if (axl_driver_load(path, &drv) != AXL_OK || drv == NULL) {
        axl_printf("IMGPATH: FAIL path load\n");
        return AXL_ERR;
    }
    axl_printf("IMGPATH_PATH_LOAD\n");
    if (axl_driver_start(drv) != AXL_OK) {
        axl_printf("IMGPATH: FAIL path start\n");
        return AXL_ERR;
    }
    return AXL_OK;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Resolve the driver beside us so the test works wherever the harness
       stages the pair. axl_app_boot_path is not usable here (it anchors on
       the volume root, not our directory), so build it from our own image
       path — which, being a shell-loaded app, is exactly what this change
       leaves untouched. */
    const char *self = axl_app_image_path();
    if (self == NULL) {
        axl_printf("IMGPATH: FAIL launcher has no image path\n");
        return 1;
    }
    axl_printf("IMGPATH: launcher self=%s\n", self);

    AXL_AUTO_FREE char *dir  = axl_path_get_dirname(self);
    AXL_AUTO_FREE char *drv  = axl_path_join(dir != NULL ? dir : "",
                                             IMGPATH_DRIVER);
    if (drv == NULL) {
        axl_printf("IMGPATH: FAIL cannot build driver path\n");
        return 1;
    }

    int rc = run_from_buffer(drv);
    if (rc == AXL_OK) {
        rc = run_from_path(drv);
    }
    axl_printf("IMGPATH_DONE\n");
    return (rc == AXL_OK) ? 0 : 1;
}
