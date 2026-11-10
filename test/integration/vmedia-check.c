/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * vmedia-check.c -- AXL's own reads against media that corrupts large reads.
 *
 * Driven by test-vmedia-reads-qemu.sh, against the AXLVM: volume that
 * vmedia-fs-driver.efi publishes (any single Read over 64 KB comes back with
 * one byte flipped). One mode per run:
 *
 *   sha FILE      axl_file_get_contents(FILE), then prints
 *                 `VMCHECK: bytes=<n> sha256=<hex>`; the script compares the
 *                 digest with the host's
 *   load FILE     axl_driver_load(FILE) + axl_driver_start
 *   readerr FILE  axl_file_get_contents(FILE) must FAIL (the media errors
 *                 part-way through) and return no buffer: truncated
 *                 contents reported as the file is the defect
 *   fread FILE    one axl_fread of the whole file, then prints
 *                 `VMCHECK: fread=<bytes> error=<0|1>`: the bytes read
 *                 before an error are delivered, and the error is flagged
 *   refuse FILE   axl_driver_load(FILE) must fail and return no handle
 *
 * Public headers only.
 */

#include <axl.h>

static int g_pass;
static int g_fail;

static void
check(
    bool        ok,
    const char *msg
    )
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void
mode_sha(
    const char *file
    )
{
    void  *buf = NULL;
    size_t len = 0;
    check(axl_file_get_contents(file, &buf, &len) == AXL_OK && buf != NULL,
          "sha: read the file");
    if (buf == NULL) {
        return;
    }
    AXL_AUTO_FREE char *hex = axl_compute_checksum(AXL_CHECKSUM_SHA256, buf,
                                                   len);
    axl_free(buf);
    check(hex != NULL, "sha: digest computed");
    axl_printf("VMCHECK: bytes=%zu sha256=%s\n", len,
               (hex != NULL) ? hex : "(none)");
}

static void
mode_load(
    const char *file
    )
{
    AxlDriverHandle drv = NULL;
    check(axl_driver_load(file, &drv) == AXL_OK && drv != NULL, "load: load");
    if (drv != NULL) {
        check(axl_driver_start(drv) == AXL_OK, "load: start");
    }
}

static void
mode_readerr(
    const char *file
    )
{
    void  *buf = NULL;
    size_t len = 0;
    check(axl_file_get_contents(file, &buf, &len) != AXL_OK,
          "readerr: the read reports an error");
    check(buf == NULL, "readerr: no contents returned");
    if (buf != NULL) {
        axl_printf("VMCHECK: returned %zu bytes\n", len);
        axl_free(buf);
    }
}

static void
mode_fread(
    const char *file
    )
{
    AxlFsEntry info;
    check(axl_file_info(file, &info) == AXL_OK, "fread: size known");
    AXL_AUTO_FREE void *buf = axl_malloc((size_t)info.size);
    AxlStream          *s   = axl_fopen(file, "r");
    check(buf != NULL && s != NULL, "fread: opened");
    if (buf == NULL || s == NULL) {
        axl_fclose(s);
        return;
    }
    size_t n = axl_fread(buf, 1, (size_t)info.size, s);
    axl_printf("VMCHECK: fread=%zu error=%d\n", n, axl_ferror(s) ? 1 : 0);
    axl_fclose(s);
}

static void
mode_refuse(
    const char *file
    )
{
    AxlDriverHandle drv = (AxlDriverHandle)(uintptr_t)1;
    check(axl_driver_load(file, &drv) != AXL_OK, "refuse: load fails");
    check(drv == NULL, "refuse: no handle returned");
}

int
main(
    int    argc,
    char **argv
    )
{
    axl_printf("=== vmedia-check ===\n");
    const char *mode = (argc > 1) ? argv[1] : "";
    if (axl_strcmp(mode, "sha") == 0 && argc == 3) {
        mode_sha(argv[2]);
    } else if (axl_strcmp(mode, "load") == 0 && argc == 3) {
        mode_load(argv[2]);
    } else if (axl_strcmp(mode, "readerr") == 0 && argc == 3) {
        mode_readerr(argv[2]);
    } else if (axl_strcmp(mode, "fread") == 0 && argc == 3) {
        mode_fread(argv[2]);
    } else if (axl_strcmp(mode, "refuse") == 0 && argc == 3) {
        mode_refuse(argv[2]);
    } else {
        check(false, "usage: sha FILE | load FILE | readerr FILE | "
                     "fread FILE | refuse FILE");
    }
    axl_printf("=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
