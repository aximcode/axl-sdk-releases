/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* sd-sibling-driver.c — trivial resident driver for the sibling-locate
 * fixture (docs/superpowers/specs/2026-07-04-shared-driver-sibling-locate-
 * design.md). Modeled on sd-ergo-driver.c: nothing but three plain
 * functions + one AXL_SHARED_DRIVER invocation.
 *
 * Built TWICE from this one source (-DDRIVER_TAG=A / -DDRIVER_TAG=B, see
 * the Makefile) so the probe fixture can tell WHICH copy of the driver
 * actually resolved a locate call: run() prints "SDSIB:tag=<TAG>\n".
 */
#include <axl.h>

#include "sd-sibling.h"

#ifndef DRIVER_TAG
#define DRIVER_TAG A   /* default so a stray plain build still compiles */
#endif

/* Two-step stringize so DRIVER_TAG (a bare identifier from -DDRIVER_TAG=A)
 * is expanded before being turned into a string literal. */
#define SDSIB_STR2(x) #x
#define SDSIB_STR(x)  SDSIB_STR2(x)

static int sib_init(void)   { return 0; }   /* no setup needed for the test */
static int sib_unload(void) { return 0; }

static int
sib_run(int argc, char **argv)
{
    (void)argc; (void)argv;
    axl_printf("SDSIB:tag=%s\n", SDSIB_STR(DRIVER_TAG));
    return 0;
}

AXL_SHARED_DRIVER(SDSIB_NAME, sib_init, sib_run, sib_unload)
