/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-svcexit.c — a SERVICE driver whose setup exits with an EFI
 * error, for test-heap-release-qemu.sh.
 *
 * AXL_SERVICE_DRIVER publishes the service's protocol on the image handle
 * BEFORE it runs setup. Setup here calls axl_exit_status with an error, so
 * EDK2 unloads the image through CoreUnloadAndCloseImage -- which removes the
 * LoadedImage protocols but NOT the service's. Left alone, the service GUID
 * stays installed with an interface pointer into an image that is gone, and
 * the next caller that looks the service up dereferences freed memory.
 *
 * The observer checks both halves: the heap came back (as for any failed
 * entry) and no handle still carries the service's GUID.
 */

#include <stdlib.h>

#include <axl.h>

static int  svcexit_setup(AxlLoop *loop, void *user) AXL_CB_NOEXCEPT;

static char *volatile g_held;

static int
svcexit_setup(
    AxlLoop *loop,
    void    *user
    ) AXL_CB_NOEXCEPT
{
    (void)loop;
    (void)user;
    g_held = malloc(100);
    axl_exit_status(AXL_EFI_ABORTED);
}

static const AxlService svcexit = {
    .name           = "heap-rel-svcexit",
    .setup          = svcexit_setup,
    .driver_tick_ms = 50,
};

AXL_SERVICE_DRIVER(svcexit);
