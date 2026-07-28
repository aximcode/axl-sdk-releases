/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * svc-embonly.c — fixture + worked example for AxlServiceDeploy.embedded_only.
 *
 * One source, three artifacts:
 *   - svc_embonly-dxe.efi       (-DAXL_SERVICE_BUILD_DRIVER): the REAL driver;
 *                                setup prints SVC-EMBONLY-EMBEDDED-READY.
 *   - svc_embonly-decoy-dxe.efi (-DAXL_SERVICE_BUILD_DRIVER -DDECOY): a DECOY
 *                                driver for the SAME service name; setup prints
 *                                SVC-EMBONLY-DECOY-READY.
 *   - svc_embonly.efi           (launcher): embeds the REAL driver and deploys
 *                                it with embedded_only = true.
 *
 * test-service-embedded-only-qemu.sh stages the launcher AND the decoy renamed
 * to the disk-search filename (svc-embonly-dxe.efi, search candidate #1 beside
 * the launcher). With embedded_only the search is skipped, so the EMBEDDED
 * marker prints and the DECOY marker never does — the point of the flag: a
 * stale loose driver an older install left beside the launcher cannot shadow
 * the image baked into this binary. Without embedded_only the search finds the
 * decoy first and its marker prints instead.
 */

#include <axl.h>

AXL_LOG_DOMAIN("svc-embonly");

#ifdef DECOY
#define EMB_MARKER "SVC-EMBONLY-DECOY-READY"
#else
#define EMB_MARKER "SVC-EMBONLY-EMBEDDED-READY"
#endif

/* Setup prints which image actually ran and stays resident (returns AXL_OK,
   registers no work) — enough for the launcher's start to verify the protocol
   registered and for the test to see which driver's marker appeared. */
static int
embonly_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    axl_printf("%s\n", EMB_MARKER);
    return AXL_OK;
}

static const AxlService svc_embonly = {
    .name           = "svc-embonly",
    .setup          = embonly_setup,
    .driver_tick_ms = 50,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

AXL_SERVICE_DRIVER(svc_embonly);

#else

/* Launcher: deploy the EMBEDDED driver with embedded_only so the disk search —
   which would find a stale/loose svc-embonly-dxe.efi beside the launcher
   first — is skipped entirely. */
AXL_EMBED_DECLARE(svc_embonly);

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    AxlServiceDeploy deploy = {
        .service         = &svc_embonly,
        .driver_blob     = AXL_EMBED_DATA(svc_embonly),
        .driver_blob_len = AXL_EMBED_SIZE(svc_embonly),
        .driver_name     = "svc-embonly-dxe.efi",   /* what the search WOULD find */
        .embedded_only   = true,
    };
    AxlStatus rc = axl_service_start_embedded(&deploy);
    axl_printf("svc-embonly: start rc=%d\n", (int)rc);
    return rc == AXL_OK ? 0 : 1;
}

#endif
