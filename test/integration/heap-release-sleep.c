/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-sleep.c — a child that runs long enough for its parent's
 * timer callback to fire while it is the firmware's current image, for
 * test-heap-release-qemu.sh (heap-release-cbexit.c starts it).
 */

#include <axl.h>

int
main(
    void
    )
{
    axl_msleep(1000);
    axl_printf("SLEEP done\n");
    return 0;
}
