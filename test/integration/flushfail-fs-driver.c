/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* flushfail-fs-driver.c -- publish a volume whose FLUSH always fails and stay
 * resident, so a TOOL can be run against it from the shell.
 *
 * The unit fixture (test/unit/axl-test-flushfail-fs.h) can only test the
 * library: an AxlFsProvider's thunks live in the address space of whoever
 * published it, so the publication dies with the publisher. A tool is a
 * separate EFI image, which puts "run tar against a flush-failing volume"
 * out of a unit test's reach.
 *
 * A DRIVER closes that gap. `load`ed from the shell it stays resident, so
 * `AXLFF:` is a live volume for every later command in the SAME shell --
 * which matters twice over: EFI_SHELL_PROTOCOL.Execute spawns a NESTED
 * shell that re-enumerates the map (the publication's alias is gone in the
 * child, and %lasterror% is a different instance's), so driving the tool
 * from a resident app through Execute could not work. Everything therefore
 * happens in the one top-level shell, where `AXLFF:` resolves and
 * %lasterror% means what it says.
 *
 * Also seeds `fs0:\ffsrc.txt`, so the harness's .nsh has something to feed
 * the tool without a second image.
 *
 * Driven by test/integration/test-flushfail-tools-qemu.sh.
 *
 * Output contract (exact lines the harness greps for):
 *   FFDRV: READY volume=AXLFF     -- published, mapped, seed written
 *   FFDRV: SKIP no-shell-map      -- no shell to map through; nothing to run
 *   FFDRV: FAIL <stage>
 */
#include <axl.h>

/* tar pads its archive to a 10240-byte record boundary, so the unit
   fixture's 1 KiB slots would fail the WRITE and never reach the flush --
   which would test the wrong thing entirely. */
#define FF_MAX_FILES  4u
#define FF_MAX_BYTES  16384u
#include "axl-test-flushfail-fs.h"

#define SRC       "fs0:\\ffsrc.txt"
#define SRC_BODY  "payload\n"

static int
flushfail_fs_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    /* Seeded on fs0: (a real volume), NOT on the fixture -- the tool needs
       something it can genuinely read, and the fixture's job is to be the
       destination. */
    if (axl_file_set_contents(SRC, SRC_BODY, sizeof(SRC_BODY) - 1) != AXL_OK) {
        axl_printf("FFDRV: FAIL seed\n");
        return AXL_ERR;
    }
    if (!ff_fs_up()) {
        axl_printf("FFDRV: SKIP no-shell-map\n");
        return AXL_ERR;
    }
    axl_printf("FFDRV: READY volume=%s\n", FF_MAP);
    return AXL_OK;
}

static int
flushfail_fs_unload(AxlHandle image)
{
    (void)image;

    ff_fs_down();
    if (axl_file_delete(SRC) != AXL_OK) {
        axl_printf("FFDRV: NOTE seed file not removed\n");
    }
    return AXL_OK;
}

AXL_DRIVER(flushfail_fs_main, flushfail_fs_unload)
