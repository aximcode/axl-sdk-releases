/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * shared-driver-demo-driver.c — resident driver image.
 *
 * A passive RPC server image: does all the heavy work and exposes
 * it through a small vtable. Heavy init (here: parsing the
 * `pci-ids.json5` sidecar) happens ONCE in DriverEntry;
 * per-launcher-invocation cost is just the vtable call.
 *
 * Build: `axl-cc --type driver shared-driver-demo-driver.c -o
 *                shared-driver-demo-dxe.efi`
 *
 * For a real consumer build see the CMakeLists.txt sibling.
 */

#include <axl.h>
#include "shared-driver-demo.h"
#include "shared-driver-demo-format.h"   /* defined in -format.c (shared TU) */

AXL_LOG_DOMAIN("shared-driver-demo-dxe");

static SharedDriverDemoVtable  gVtable;
static AxlHandle               gPublishedHandle;

/* The verb body — runs in the driver image's address space. Walks
 * PCI, formats each device with the vendor name from the sidecar
 * singleton (which lives in THIS image's heap, populated by
 * DriverEntry below). Calls into the shared formatting TU
 * (demo_format_vid_did) — that TU is compiled into BOTH the driver
 * and launcher source lists, see CMakeLists.txt. */
static int
demo_run(int argc, char **argv)
{
    (void)argc; (void)argv;

    int devices = 0;
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        uint16_t vid = 0, did = 0;
        if (axl_pci_get_vid_did(*p, &vid, &did) != 0) {
            continue;
        }
        char vid_did[10];
        demo_format_vid_did(vid_did, vid, did);
        const char *vendor = axl_pci_vendor_name(vid);
        axl_printf("  %02x/%02x/%02x.%x  %s  %s\n",
                   p->seg, p->bus, p->dev, p->func, vid_did,
                   vendor != NULL ? vendor : "<unknown>");
        ++devices;
    }
    char tail[64];
    axl_snprintf(tail, sizeof(tail), "%d device(s)", devices);
    demo_print_banner(tail);
    return 0;
}

static int
driver_main(AxlHandle h, AxlSystemTable *st)
{
    (void)h; (void)st;

    /* Sidecar autodiscovery anchors on the LAUNCHER's image path
     * (LoadedImage->ParentHandle walk in axl_app_image_path's
     * fallback). The driver image itself has no FilePath when
     * loaded from the launcher's embedded blob — but the parent
     * does, and that's where pci-ids.json5 lives. */
    axl_pci_ids_load(NULL);

    gVtable.do_run = demo_run;

    return axl_shared_driver_publish(SHARED_DRIVER_DEMO_NAME,
                                     &gVtable, &gPublishedHandle);
}

static int
driver_unload(AxlHandle h)
{
    (void)h;
    /* If DriverEntry failed before the publish call minted a
     * handle, gPublishedHandle is still NULL. Skip the unpublish
     * in that case — there's nothing to undo, and passing NULL
     * would surface as an AXL_ERR from the unload path. */
    if (gPublishedHandle == NULL) {
        return AXL_OK;
    }
    return axl_shared_driver_unpublish(SHARED_DRIVER_DEMO_NAME,
                                       gPublishedHandle, &gVtable);
}

AXL_DRIVER(driver_main, driver_unload)
