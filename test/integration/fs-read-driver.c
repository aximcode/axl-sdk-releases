/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* fs-read-driver.c — resident driver that reads a file the exact way a
 * `-f<file>`-style consumer does (fopen -> text-wrap -> readline, in
 * fs-read-common.c). Reproduces the reported old-shell gap where a file
 * STREAM READ from inside a resident driver (whose LoadedImage carries no
 * SHELL_INTERFACE / SHELL_ENVIRONMENT of its own) returned empty, while
 * file_info on the same path from the same driver worked.
 */
#include <axl.h>

#include "fs-read.h"

static int fsr_init(void)   { return 0; }
static int fsr_unload(void) { return 0; }

static int
fsr_run(int argc, char **argv)
{
    const char *path = (argc > 0 && argv != NULL && argv[0] != NULL)
                       ? argv[0] : FSREAD_DEFAULT_PATH;
    fsread_report("drv", path);
    return 0;
}

AXL_SHARED_DRIVER(FSREAD_NAME, fsr_init, fsr_run, fsr_unload)
