/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-tplexit.c — a driver whose entry RAISES the TPL, never
 * restores it, and exits with an EFI error, for test-heap-release-qemu.sh.
 *
 * The image is still the current one (it started no child), so gBS->Exit
 * succeeds and EDK2 unloads it -- the exit must take its teardown, heap
 * included, exactly as at TPL_APPLICATION. _axl_cleanup repairs the raise
 * first. The case to NOT tear down is an exit while a child this image
 * started is running (Exit then fails and spins); a raised TPL is not that,
 * and was once used as a stand-in for it, which kept this image's 1 MiB.
 */

#include <stdlib.h>

#include <axl.h>

static int  tplexit_entry(AxlHandle image, AxlSystemTable *st);
static int  tplexit_unload(AxlHandle image);

static char *volatile g_held;

static int
tplexit_entry(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;
    g_held = malloc(100);
    /* Raised and never restored: that is the case under test. A previous
       level already above CALLBACK would not be this case at all. */
    if (axl_tpl_raise(AXL_TPL_CALLBACK) > AXL_TPL_CALLBACK) {
        return 1;
    }
    axl_exit_status(AXL_EFI_ABORTED);
}

static int
tplexit_unload(
    AxlHandle image
    )
{
    (void)image;
    return 0;
}

AXL_DRIVER(tplexit_entry, tplexit_unload)
