/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * svc-startfail.c — regression fixture: an embedded service driver whose
 * setup returns AXL_ERR.
 *
 * The AXL_SERVICE macro builds this source twice (like service-demo.c): a
 * launcher app (svc-startfail.efi) with the driver image baked in, and the
 * driver image (svc-startfail-dxe.efi) whose setup deliberately fails.
 *
 * Deployed via `svc-startfail.efi start`, axl_service_start_embedded buffer-
 * loads the embedded driver (a buffer load gets a SYNTHESIZED device path),
 * StartImage runs its DriverEntry, setup returns AXL_ERR, DriverEntry returns
 * an EFI error, and the firmware auto-unloads the errored image — freeing the
 * synthesized device path blocks. Before the liveness-aware image_dp_release /
 * axl_driver_unload fix, AXL then freed those same blocks a second time,
 * corrupting the DXE pool: a silent 100%-CPU spin on RELEASE firmware, an
 * `ASSERT [DxeCore] ... Pool.c` on DEBUG. The launcher must instead report the
 * start failure, exit non-zero, and hand control back to the shell.
 *
 * Driven by test-service-startfail-qemu.sh (AARCH64 DEBUG, so a pool
 * double-free ASSERTs loudly rather than spinning silently).
 */

#include <axl.h>

AXL_LOG_DOMAIN("svc-startfail");

/* Setup fails on purpose. The print is the marker proving setup actually ran
   (the driver loaded and started) before the failure — so the test knows it
   exercised the buffer-loaded start path, not an earlier bail-out. */
static int
startfail_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    axl_printf("SVCSTARTFAIL: setup failing on purpose\n");
    return AXL_ERR;
}

static const AxlService svc_startfail = {
    .name           = "svc-startfail",
    .setup          = startfail_setup,
    .driver_tick_ms = 50,
};

AXL_SERVICE(svc_startfail);
