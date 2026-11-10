/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * refuse-unload-driver.c -- a driver whose Unload refuses, so UnloadImage
 * fails and the image stays resident: the candidate a bring-up sweep tries,
 * cannot use, and cannot drop (test-net-gate-qemu.sh's `refuse` scenario).
 *
 * Output contract (exact lines):
 *   REFUSE: loaded
 *   REFUSE: unload refused
 */

#include <axl.h>

static int refuse_main(AxlHandle image, AxlSystemTable *st);
static int refuse_unload(AxlHandle image);

AXL_DRIVER(refuse_main, refuse_unload)

static int
refuse_main(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;
    axl_printf("REFUSE: loaded\n");
    return 0;
}

static int
refuse_unload(
    AxlHandle image
    )
{
    (void)image;
    axl_printf("REFUSE: unload refused\n");
    return 1;
}
