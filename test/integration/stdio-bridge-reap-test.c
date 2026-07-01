/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-reap-test.c — probe for the stdio-bridge dead-instance leak.
 *
 * The stdio bridge (protocol GUID c8f517d7-…) carries a launcher's shell
 * handles across the image boundary to a resident driver. A launcher that
 * exits without running CRT0's atexit uninstall — a --minimal-runtime image
 * (the do.efi shape) or one that calls gBS->Exit — leaves its bridge installed
 * in the firmware protocol DB. Each launcher is a fresh PE image whose static
 * install handle can't see prior images' bridges, so before the fix these dead
 * instances ACCUMULATED one per invocation (visible in `dh`), reaped only on
 * the driver's stdin-read path (which `do bios irq` / `do -u` never hit).
 *
 * The fix reaps every dead-launcher bridge instance (a) at the start of each
 * install (so a fresh launcher sweeps its predecessors) and (b) in
 * axl_shared_driver_unload (so `do -u` clears the residual).
 *
 * This test is shell-driven (startup.nsh): stdio-bridge-leak.efi runs TWICE
 * from the shell — each installs a bridge (it has shell params) then gBS->Exit's,
 * leaking it. Then this probe runs and:
 *   1. counts bridge instances — before the fix that's 2 (both leaked); after
 *      the fix it's 1 (the 2nd leaker's install reaped the 1st).
 *   2. calls axl_shared_driver_unload — which now reaps the residual dead
 *      bridge — and re-counts: must be 0.
 *
 * RED  (before fix): initial=2, after_unload=2.
 * GREEN (after fix): initial<=1, after_unload=0.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

/* Fixed identity of the stdio-bridge protocol (mirrors AXL_STDIO_BRIDGE_GUID
 * in the backend; tests use public headers only, so it's restated here). */
static const EFI_GUID STDIO_BRIDGE_GUID = {
    0xc8f517d7, 0x36cc, 0x458d,
    {0x98, 0xd6, 0xb1, 0x16, 0x82, 0x5e, 0x30, 0xbf}
};

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

/* Count handles carrying the stdio-bridge protocol. SIZE_MAX on failure. */
static size_t
count_bridges(void)
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_STATUS  st = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&STDIO_BRIDGE_GUID, NULL, &count, &handles);
    if (st == EFI_NOT_FOUND) {
        return 0;
    }
    if (EFI_ERROR(st) || handles == NULL) {
        return (size_t)-1;
    }
    gBS->FreePool(handles);
    return (size_t)count;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("stdio-bridge-reap-test: start\n");

    /* Two leakers ran from the shell before us; each leaked a bridge whose
     * launcher is now gone. With the reap-at-install fix the 2nd leaker swept
     * the 1st, so at most one dead instance remains. */
    size_t initial = count_bridges();
    axl_printf("INFO: BRIDGES_INITIAL=%zu\n", initial);
    check(initial != (size_t)-1 && initial <= 1,
          "dead bridges did not accumulate across launchers (reap at install)");

    /* axl_shared_driver_unload reaps dead bridges even when the named driver
     * is not resident (the reap runs before the not-found early return). */
    (void)axl_shared_driver_unload("stdio-bridge-fix");

    size_t after = count_bridges();
    axl_printf("INFO: BRIDGES_AFTER_UNLOAD=%zu\n", after);
    check(after == 0, "shared_driver_unload reaped the residual dead bridge");

    axl_printf("stdio-bridge-reap-test: %d passed, %d failed\n",
               g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
