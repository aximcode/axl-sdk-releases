/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * svc-tdfail-foreign.c -- a "svc-tdfail" that will not stop, for
 * test-service-teardown-fail-qemu.sh.
 *
 * It publishes the service GUID that axl_service_guid derives from the name
 * "svc-tdfail", as a running AXL_SERVICE_DRIVER would, but its unload refuses
 * -- as a firmware-shipped or foreign image with no Unload handler does. It
 * never tags itself stopping, since it is not an AXL_SERVICE_DRIVER, so
 * `svc_tdfail.efi stop --reset` against it must report the failed stop and
 * NOT reset the machine: a reset would not even help, as such an image
 * comes back at boot.
 */

#include <axl.h>

static const AxlService stuck_svc = {
    .name = "svc-tdfail",
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
        axl_printf("TDFOREIGN: could not publish\n");
        return 1;
    }
    axl_printf("TDFOREIGN: published, will not unload\n");
    return 0;
}

int
stuck_unload(
    AxlHandle image
    )
{
    (void)image;
    axl_printf("TDFOREIGN: refusing unload\n");
    return 1;
}

AXL_DRIVER(stuck_main, stuck_unload)
