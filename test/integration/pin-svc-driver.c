/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * pin-svc-driver.c — service driver image for the AxlServiceDeploy
 * driver_path pin test (test-service-pin-path-qemu.sh).
 *
 * Built TWICE from this one source with different -DPIN_VARIANT values, so the
 * two binaries are interchangeable as far as the framework is concerned (same
 * service name, same derived GUID) but announce which one actually came up.
 * The harness stages the "shadow" build where the default search would find it
 * first and embeds it in the launcher, and the "good" build only at the
 * pinned path.
 */

#include <axl.h>

#ifndef PIN_VARIANT
#define PIN_VARIANT "unset"
#endif

static int
pin_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    axl_printf("PINSVC: variant=%s SETUP\n", PIN_VARIANT);
    return AXL_OK;
}

static int
pin_teardown(void *user)
{
    (void)user;
    axl_printf("PINSVC: variant=%s TEARDOWN\n", PIN_VARIANT);
    return AXL_OK;
}

static const AxlService pin_svc = {
    .name           = "pin-svc",
    .setup          = pin_setup,
    .teardown       = pin_teardown,
    .driver_tick_ms = 50,
};

AXL_SERVICE_DRIVER(pin_svc)
