/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-childrun.c — a driver whose entry RUNS A CHILD and connects a
 * controller, THEN exits with an EFI error, for test-heap-release-qemu.sh.
 *
 * While a child image runs, an exit from this image cannot unload it, so
 * libaxl counts every call that can run one (axl_backend_start_image,
 * axl_backend_child_image_enter/leave) and _axl_cleanup does not tear the
 * image down while the count is up. Once each call has RETURNED the count
 * must be back to zero -- a missing leave would keep it up for good, and this
 * exit, which does unload the image, would keep its heap. So the observer
 * reads this as any failing-entry driver: its heap must come back.
 *
 * Its child is named relative to the Shell's current directory, as the
 * observer names this image -- fs0:\ on a booted disk, the test's directory
 * on a shared volume (a leased pool guest). Not beside this image: launched
 * by the observer, it has no file path axl_app_image_dir_path resolves
 * (measured: that call fails in it).
 */

#include <stdlib.h>

#include <axl.h>

static int  childrun_entry(AxlHandle image, AxlSystemTable *st);
static int  childrun_unload(AxlHandle image);

static char *volatile g_held;

static int
childrun_entry(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    int exit_code = -1;

    (void)st;
    g_held = malloc(100);
    /* Said either way: this entry fails by design, so only these lines show
       the observer that the child really ran. */
    if (axl_image_run("heap-rel-c.efi", NULL, &exit_code) != AXL_OK
        || exit_code != 0) {
        axl_printf("CHILDRUN image_run failed\n");
        return 1;
    }
    axl_printf("CHILDRUN child ran\n");
    /* A ConnectController through libaxl. Nothing binds to an image handle,
       so the result is EFI_NOT_FOUND -- the call, not its outcome, is the
       point. */
    if (axl_driver_connect_handle((void *)image) == AXL_UNSUPPORTED) {
        return 1;
    }
    axl_exit_status(AXL_EFI_ABORTED);
}

static int
childrun_unload(
    AxlHandle image
    )
{
    (void)image;
    return 0;
}

AXL_DRIVER(childrun_entry, childrun_unload)
