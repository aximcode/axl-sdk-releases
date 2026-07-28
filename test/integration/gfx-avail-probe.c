/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * gfx-avail-probe.c — report whether the guest firmware exposes a GOP.
 *
 * Prints "GFX-AVAILABLE: 1" when axl_gfx_available() (a GOP is present) and
 * "GFX-AVAILABLE: 0" when it is not. Exits 0 either way — it is a probe, not a
 * pass/fail test — and never touches the render/present pipeline, so it is also
 * the "logic-only app runs and exits fast" case.
 *
 * Drives test-no-gpu-qemu.sh: a `--no-gpu` (GOP-less) run must report 0 on
 * every arch; a default x64 run reports 1 (q35's built-in std-VGA gives OVMF a
 * GOP), proving the flag actually removes the adapter.
 */

#include <axl.h>

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    axl_printf("GFX-AVAILABLE: %d\n", axl_gfx_available() ? 1 : 0);
    return 0;
}
