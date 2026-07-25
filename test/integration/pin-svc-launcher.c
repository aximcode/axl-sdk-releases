/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * pin-svc-launcher.c — launcher for the AxlServiceDeploy driver_path pin
 * test (test-service-pin-path-qemu.sh).
 *
 * Usage:
 *   pin-svc-launcher.efi start          default resolution (4-path search,
 *                                       then the embedded blob)
 *   pin-svc-launcher.efi start <path>   pinned: load exactly <path>
 *   pin-svc-launcher.efi stop
 *
 * The embedded blob is the SHADOW build, and the harness also stages the
 * shadow build in the slot the default search tries first — so any run that
 * reports `variant=good` can only have come from the pinned path.
 */

#include <axl.h>

AXL_EMBED_DECLARE(pin_svc_shadow);

/* Launcher-side descriptor. setup/teardown never run here (this binary is
 * the supervisor, not the service) but the name is what derives the GUID
 * both binaries publish and look up. */
static int
pin_launcher_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    return AXL_OK;
}

static const AxlService pin_svc = {
    .name           = "pin-svc",
    .setup          = pin_launcher_setup,
    .driver_tick_ms = 50,
};

int
main(int argc, char **argv)
{
    AxlServiceDeploy deploy = {
        .service         = &pin_svc,
        .driver_blob     = AXL_EMBED_DATA(pin_svc_shadow),
        .driver_blob_len = AXL_EMBED_SIZE(pin_svc_shadow),
        .driver_name     = "pin-svc-dxe.efi",
    };

    if (argc >= 2 && axl_strcmp(argv[1], "stop") == 0) {
        int rc = axl_service_stop(&deploy);
        axl_printf("PINSVC: launcher stop rc=%d\n", rc);
        return (rc == AXL_OK) ? 0 : 1;
    }

    if (argc >= 3) {
        deploy.driver_path = argv[2];
        axl_printf("PINSVC: launcher pinning %s\n", deploy.driver_path);
    } else {
        axl_printf("PINSVC: launcher using the default search\n");
    }

    int rc = axl_service_start_embedded(&deploy);
    axl_printf("PINSVC: launcher start rc=%d\n", rc);
    return (rc == AXL_OK) ? 0 : 1;
}
