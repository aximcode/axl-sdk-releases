/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-exit-drv.c — a driver that ends its entry with axl_exit(0),
 * for test-heap-release-qemu.sh.
 *
 * axl_exit runs _axl_cleanup and then gBS->Exit with EFI_SUCCESS, which for a
 * driver image means "stay resident" -- so the image goes on living, with its
 * globals and its C heap. _axl_cleanup knows this and skips the atexit drain
 * for a driver image; the heap release must be skipped with it, or the
 * resident driver keeps allocating from pages the firmware has taken back.
 * The observer's `hold` mode reads that directly: while this driver is
 * resident its arena must still be held, and unload must give it back.
 */

#include <stdlib.h>

#include <axl.h>

static int  exit_drv_entry(AxlHandle image, AxlSystemTable *st);
static int  exit_drv_unload(AxlHandle image);

static char *volatile g_held;

static int
exit_drv_entry(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;
    g_held = malloc(100);
    if (g_held == NULL) {
        return 1;
    }
    axl_exit(0);
}

static int
exit_drv_unload(
    AxlHandle image
    )
{
    (void)image;
    free(g_held);
    g_held = NULL;
    return 0;
}

AXL_DRIVER(exit_drv_entry, exit_drv_unload)
