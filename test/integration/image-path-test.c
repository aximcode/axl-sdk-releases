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

/* What axl_app_image_dir_path makes of @rel, from \app\ -- the nested case the
   unit binary (staged at the root) cannot reach. */
static void
report_image_dir_path(const char *rel)
{
    char out[256];

    axl_printf("IMGPATH: dir[%s]=%s\n", rel,
               axl_app_image_dir_path(rel, out, sizeof(out)) == AXL_OK ? out : "(err)");
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const char *self = axl_app_image_path();
    if (self == NULL) {
        axl_printf("IMGPATH: FAIL launcher has no image path\n");
        return 1;
    }
    axl_printf("IMGPATH: launcher self=%s\n", self);
    report_image_dir_path("sshd.conf");
    report_image_dir_path("keys\\ak");
    report_image_dir_path("");
    report_image_dir_path("\\keys\\ak");
    report_image_dir_path("fs1:\\x.conf");

    /* The driver beside us, wherever the harness stages the pair. */
    char drv[256];
    if (axl_app_image_dir_path(IMGPATH_DRIVER, drv, sizeof(drv)) != AXL_OK) {
        axl_printf("IMGPATH: FAIL cannot build driver path\n");
        return 1;
    }

    /* The driver search's first candidate is this image's own directory:
       in a SUBDIRECTORY it must name \app\, once. */
    char loc[256];
    axl_printf("IMGPATH: locate=%s\n",
               axl_driver_locate(IMGPATH_DRIVER, loc, sizeof(loc)) == AXL_OK ? loc : "(err)");

    int rc = run_from_buffer(drv);
    if (rc == AXL_OK) {
        rc = run_from_path(drv);
    }
    axl_printf("IMGPATH_DONE\n");
    return (rc == AXL_OK) ? 0 : 1;
}
