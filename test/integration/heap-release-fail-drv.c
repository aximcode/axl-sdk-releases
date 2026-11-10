/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-fail-drv.c — a driver whose entry EXITS with an EFI error, for
 * test-heap-release-qemu.sh.
 *
 * axl_exit_status with an error status runs _axl_cleanup and then gBS->Exit,
 * and EDK2's StartImage unloads a driver whose entry exits with an EFI error
 * through CoreUnloadAndCloseImage -- which never calls the image's Unload. So
 * axl_driver_cleanup never runs for it, and _axl_cleanup, which (rightly)
 * keeps a RESIDENT driver's heap, used to keep this one's too: its arena was
 * gone with nothing left to free it, 1 MiB per failed load.
 *
 * A non-error status (axl_exit(1), what std::terminate reaches) leaves the
 * driver resident instead; heap-release-exit-drv.c covers that side.
 *
 * ADVERSARIAL on purpose: an atexit handler arms EFI_SUCCESS during the
 * teardown that exit runs. The decision to tear the image down was taken on
 * the ERROR status; if the success then reached gBS->Exit, the driver would
 * stay resident with its destructors run and its heap released. The status
 * the decision was taken on must be the one Exit gets.
 *
 * And a second handler then EXITS AGAIN from inside the drain, with a
 * non-error code -- what a destructor reaching std::terminate does. That
 * nested axl_exit must not re-decide: the image is already half torn down.
 * atexit is LIFO, so it is registered first to run last.
 */

#include <stdlib.h>

#include <axl.h>

static int  fail_drv_entry(AxlHandle image, AxlSystemTable *st);
static int  fail_drv_unload(AxlHandle image);

static char *volatile g_held;

static void
exit_again(
    void *unused
    )
{
    (void)unused;
    axl_exit(1);
}

static void
arm_success(
    void *unused
    )
{
    (void)unused;
    axl_set_exit_status(AXL_EFI_SUCCESS);
}

static int
fail_drv_entry(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;
    g_held = malloc(100);
    if (axl_atexit(exit_again, NULL) == 0
        || axl_atexit(arm_success, NULL) == 0) {
        return 1;
    }
    axl_exit_status(AXL_EFI_ABORTED);
}

static int
fail_drv_unload(
    AxlHandle image
    )
{
    (void)image;
    return 0;
}

AXL_DRIVER(fail_drv_entry, fail_drv_unload)
