/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * driver-parent-leak-test.c — regression test for the shared-driver
 * cross-image "orphaned synthesized device path" image-handle leak.
 *
 * The shape (matching the delldiags do.efi/doDriver.efi report):
 *   1. A short-lived launcher image buffer-loads (embedded blob) + starts a
 *      resident driver. A buffer load has no firmware device path, so
 *      axl_driver_load synthesizes a MemoryMapped(...)/FilePath
 *      LoadedImageDevicePath, installs it on the driver's image handle, and
 *      records it in a PER-IMAGE side table so unload can release it.
 *   2. The launcher exits (it is an EFI_APPLICATION — the firmware unloads it
 *      when StartImage returns). Its side table dies with it.
 *   3. A DIFFERENT image unloads the resident driver by name
 *      (axl_shared_driver_unload -> axl_driver_unload -> gBS->UnloadImage).
 *      image_dp_release runs in THIS image, whose side table is empty, so the
 *      synthesized device path is never uninstalled. gBS->UnloadImage then
 *      removes LoadedImage (the image handle's LoadedImage goes away) but
 *      cannot remove our synth device path (its internal pointer is stale),
 *      leaving an orphaned handle that carries only LoadedImageDevicePath —
 *      one accumulates in `dh` on every load/unload cycle.
 *
 * This controller drives the whole scenario inside ONE observing image so the
 * leak is asserted programmatically (CI-gateable):
 *   - It loads + starts the stdio-bridge-fix launcher as a child. The launcher
 *     makes the stdio-bridge-driver resident FROM ITS EMBEDDED BLOB (the driver
 *     is intentionally NOT staged on disk, so the launcher takes the buffer
 *     path that synthesizes the device path) and returns; the firmware unloads
 *     the launcher.
 *   - The controller survives, locates the resident driver by its published
 *     GUID, snapshots the count of LoadedImageDevicePath handles, unloads the
 *     driver, and re-counts.
 *
 * RED  (before fix): the count does not return to baseline — the synthesized
 *   device path is orphaned on a handle the firmware could not reclaim.
 * GREEN (after fix): the count returns to baseline; nothing orphaned.
 *
 * The stdio-bridge fixture is reused deliberately: it is precisely the
 * "transient launcher makes a resident driver" pattern this bug lives in.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>
#include "stdio-bridge-fix.h"

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

/* Count handles carrying `guid`. Returns SIZE_MAX on failure. */
static size_t
count_handles_with(const EFI_GUID *guid)
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_STATUS  st = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)guid, NULL, &count, &handles);
    if (st == EFI_NOT_FOUND) {
        return 0;
    }
    if (EFI_ERROR(st) || handles == NULL) {
        return (size_t)-1;
    }
    gBS->FreePool(handles);
    return (size_t)count;
}

/* Resolve the single image handle carrying the shared-driver GUID.
 * Returns NULL if the driver is not resident. */
static EFI_HANDLE
locate_driver_handle(const AxlGuid *guid)
{
    EFI_HANDLE *handles = NULL;
    UINTN       count   = 0;
    EFI_STATUS  st = gBS->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)guid, NULL, &count, &handles);
    if (EFI_ERROR(st) || handles == NULL || count == 0) {
        return NULL;
    }
    EFI_HANDLE h = handles[0];
    gBS->FreePool(handles);
    return h;
}

/* True if `handle` still carries EFI_LOADED_IMAGE_PROTOCOL. Safe on a freed
 * handle: CoreValidateHandle rejects an unknown handle with
 * EFI_INVALID_PARAMETER rather than dereferencing it. */
static bool
image_handle_alive(EFI_HANDLE handle)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS st = gBS->HandleProtocol(
        handle, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    return !EFI_ERROR(st) && li != NULL;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    axl_printf("driver-parent-leak-test: start\n");

    AxlGuid guid;
    if (axl_shared_driver_guid(STDIO_BRIDGE_FIX_NAME, &guid) != AXL_OK) {
        check(false, "derive shared-driver GUID");
        goto done;
    }

    /* Sanity: the driver must NOT already be resident (clean boot). */
    if (locate_driver_handle(&guid) != NULL) {
        check(false, "precondition: driver not already resident");
        goto done;
    }

    /* Baseline: number of handles carrying a loaded-image device path,
     * measured while quiescent (before we load anything). */
    size_t dp_before = count_handles_with(
        &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID);
    check(dp_before != (size_t)-1, "baseline LoadedImageDevicePath count");

    /* 1. Load + start the launcher as a child. It buffer-loads the driver
     *    from its embedded blob (driver not on disk), synthesizing a device
     *    path, then returns; the firmware unloads it, leaving the driver's
     *    parent handle dead and the launcher's side table gone. */
    AxlDriverHandle launcher = NULL;
    if (axl_driver_load("fs0:\\stdio-bridge-fix.efi", &launcher) != AXL_OK
        || launcher == NULL) {
        check(false, "load launcher stdio-bridge-fix.efi");
        goto done;
    }
    /* Non-zero exit from the launcher (its verb dispatch gets no argv) is
     * expected and fine — the driver is published during locate, before the
     * verb runs. What matters is that StartImage returns so the launcher is
     * auto-unloaded. */
    axl_driver_start(launcher);

    /* 2. The driver must now be resident and discoverable by its GUID. */
    EFI_HANDLE drv = locate_driver_handle(&guid);
    check(drv != NULL, "driver resident after launcher exit");
    if (drv == NULL) {
        goto done;
    }

    /* 3. Confirm the driver was buffer-loaded (synth device path present on
     *    its image handle) — otherwise this test would not exercise the bug. */
    EFI_DEVICE_PATH_PROTOCOL *drv_dp = NULL;
    check(gBS->HandleProtocol(drv, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
                              (void **)&drv_dp) == EFI_SUCCESS && drv_dp != NULL,
          "driver image handle has a (synthesized) device path");

    /* 4. Unload the driver from THIS image (not the loader). */
    int rc = axl_shared_driver_unload(STDIO_BRIDGE_FIX_NAME);
    check(rc == AXL_OK, "axl_shared_driver_unload returns AXL_OK");

    /* Protocol must be unpublished (correctness — this already worked). */
    check(locate_driver_handle(&guid) == NULL,
          "driver protocol unpublished after unload");

    /* The image handle's LoadedImage must be gone (this already worked too —
     * the image itself is reclaimed; the orphan is a separate handle). */
    check(!image_handle_alive(drv), "driver image handle reclaimed");

    /* THE LEAK ASSERTION: no orphaned loaded-image device path survives. The
     * count must return to the quiescent baseline. */
    size_t dp_after = count_handles_with(
        &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID);
    axl_printf("INFO: LoadedImageDevicePath handles before=%zu after=%zu\n",
               dp_before, dp_after);
    check(dp_after != (size_t)-1 && dp_after == dp_before,
          "no orphaned LoadedImageDevicePath handle after unload");

done:
    axl_printf("driver-parent-leak-test: %d passed, %d failed\n",
               g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
