/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * reload-svc-stuck-dxe.c -- a "reload-svc" that will not stop, for
 * test-service-reload-stuck-qemu.sh.
 *
 * It publishes the service GUID that axl_service_guid derives from the name
 * "reload-svc", as a running AXL_SERVICE_DRIVER would, but its unload refuses
 * -- as a firmware-shipped or foreign image with no Unload handler does. A
 * reload must then find the service still published and still serving, and
 * say so with a non-fatal code, not the AXL_ERR that means "down".
 */

#include <axl.h>

static const AxlService stuck_svc = {
    .name = "reload-svc",
};

int
stuck_main(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)st;
    AxlGuid   g;
    AxlHandle h = image;
    if (axl_service_guid(&stuck_svc, &g) != AXL_OK
        || axl_protocol_install(&g, (void *)&stuck_svc, &h) != AXL_OK) {
        axl_printf("RSVCS: could not publish\n");
        return 1;
    }
    axl_printf("RSVCS: published, will not unload\n");
    return 0;
}

int
stuck_unload(
    AxlHandle image
    )
{
    (void)image;
    axl_printf("RSVCS: refusing unload\n");
    return 1;
}

AXL_DRIVER(stuck_main, stuck_unload)
