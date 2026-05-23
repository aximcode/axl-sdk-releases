/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-format.c — implementation of the shared
 * formatting helpers. Compiled into BOTH the launcher and driver
 * builds (see CMakeLists.txt). Each image links its own copy of
 * these symbols.
 */

#include <axl.h>
#include "shared-driver-demo-format.h"

void
demo_print_banner(const char *message)
{
    axl_printf("demo: %s\n", message != NULL ? message : "(no message)");
}

int
demo_format_vid_did(char *buf, uint16_t vid, uint16_t did)
{
    if (buf == NULL) {
        return 0;
    }
    /* axl_snprintf writes "XXXX:XXXX" + NUL — 10 bytes total.
     * Returns int directly (no cast needed); SDK convention. */
    return axl_snprintf(buf, 10, "%04x:%04x", vid, did);
}
