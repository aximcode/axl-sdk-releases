/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * pack-load-test.c -- AXL's own driver loaders handed a packed (wrapped) image.
 *
 * Driven by test-efi-pack-drivers-qemu.sh. Each invocation runs ONE mode, so the
 * script can put a Shell `dh` between them; the script, not this app, decides
 * whether a stub image stayed resident (dh's PdbFileName) -- the loader's
 * contract is that the handle it returns is the ORIGINAL image's, which only
 * the firmware's handle database can show.
 *
 *   dir    DIR N        axl_driver_load_dir(DIR) loads and starts exactly N
 *   buffer FILE NAME    the file's bytes through axl_driver_load_buffer_with_image_info
 *   path   FILE         axl_driver_load + axl_driver_start (a relative FILE
 *                       takes the read-it-ourselves path)
 *   refuse FILE         axl_driver_load must fail and return no handle
 *   ensure              the sd-sibling shared driver located beside this app
 *                       (as sdsib-drv.efi) twice: one load, the same
 *                       interface both times
 *   unload              that shared driver unloaded by its protocol GUID
 *
 * Public headers only, plus sd-sibling.h for the shared driver's name.
 */

#include <axl.h>
#include "sd-sibling.h"

#define PACK_LOAD_SDSIB_FILE "sdsib-drv.efi"

static int g_pass;
static int g_fail;

static void
check(
    bool        ok,
    const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void
mode_dir(
    const char *dir,
    const char *want)
{
    uint64_t expect = 0;
    check(axl_str_to_u64(want, 10, &expect, NULL) == AXL_OK, "dir: N parses");
    size_t n = 0;
    int rc = axl_driver_load_dir(dir, NULL, &n);
    check(rc == AXL_OK, "dir: axl_driver_load_dir returns AXL_OK");
    axl_printf("LOADDIR: loaded=%zu\n", n);
    check(n == expect, "dir: loaded count");
}

static void
mode_buffer(
    const char *file,
    const char *name)
{
    void  *buf = NULL;
    size_t len = 0;
    check(axl_file_get_contents(file, &buf, &len) == AXL_OK && buf != NULL,
          "buffer: read the file");
    if (buf == NULL) {
        return;
    }
    AxlEmbeddedImageInfo info = { .file_name = name };
    AxlDriverHandle      drv  = NULL;
    int rc = axl_driver_load_buffer_with_image_info(buf, len, &info, &drv);
    axl_free(buf);
    check(rc == AXL_OK && drv != NULL, "buffer: load");
    if (drv != NULL) {
        check(axl_driver_start(drv) == AXL_OK, "buffer: start");
    }
}

static void
mode_path(
    const char *file)
{
    AxlDriverHandle drv = NULL;
    check(axl_driver_load(file, &drv) == AXL_OK && drv != NULL, "path: load");
    if (drv != NULL) {
        check(axl_driver_start(drv) == AXL_OK, "path: start");
    }
}

static void
mode_refuse(
    const char *file)
{
    AxlDriverHandle drv = (AxlDriverHandle)(uintptr_t)1;
    check(axl_driver_load(file, &drv) != AXL_OK, "refuse: load fails");
    check(drv == NULL, "refuse: no handle returned");
}

static void
mode_ensure(void)
{
    void *a = NULL;
    void *b = NULL;
    check(axl_shared_driver_locate_sibling(SDSIB_NAME, PACK_LOAD_SDSIB_FILE,
                                           &a) == AXL_OK && a != NULL,
          "ensure: first locate loads the sibling");
    check(axl_shared_driver_locate_sibling(SDSIB_NAME, PACK_LOAD_SDSIB_FILE,
                                           &b) == AXL_OK && b == a,
          "ensure: second locate returns the resident interface");
}

static void
mode_unload(void)
{
    check(axl_shared_driver_unload(SDSIB_NAME) == AXL_OK,
          "unload: by protocol GUID");
}

int
main(
    int    argc,
    char **argv)
{
    axl_printf("=== pack-load Tests ===\n");
    const char *mode = (argc > 1) ? argv[1] : "";
    if (axl_strcmp(mode, "dir") == 0 && argc == 4) {
        mode_dir(argv[2], argv[3]);
    } else if (axl_strcmp(mode, "buffer") == 0 && argc == 4) {
        mode_buffer(argv[2], argv[3]);
    } else if (axl_strcmp(mode, "path") == 0 && argc == 3) {
        mode_path(argv[2]);
    } else if (axl_strcmp(mode, "refuse") == 0 && argc == 3) {
        mode_refuse(argv[2]);
    } else if (axl_strcmp(mode, "ensure") == 0 && argc == 2) {
        mode_ensure();
    } else if (axl_strcmp(mode, "unload") == 0 && argc == 2) {
        mode_unload();
    } else {
        check(false, "usage: dir DIR N | buffer FILE NAME | path FILE | "
                     "refuse FILE | ensure | unload");
    }
    axl_printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
