/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-format.c — implementation of the driver's small
 * output helper. See shared-driver-demo-format.h for why this lives
 * in its own translation unit rather than inline in the driver.
 */

#include <axl.h>
#include "shared-driver-demo-format.h"

void
demo_print_banner(const char *message)
{
    axl_print("demo: %s\n", message != NULL ? message : "(no message)");
}
