/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-smart.c
    The AxlSmart union device walk + normalized health rollup. Enumerates
    NVMe controllers, ATA/SATA devices, and SCSI logical units back-to-back
    (axl_nvme_next / axl_ata_next / axl_scsi_next), caches them as opaque
    AxlStorageDev handles, and dispatches axl_smart_health by transport to the
    per-transport identity+health readers + the pure normalizers in
    axl-smart-normalize.c.
**/

#include <axl/axl-smart.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>       /* axl_handle_get_protocol / axl_device_path_to_text */
#include <axl/axl-atexit.h>

struct AxlStorageDev {
    AxlStorageTransport transport;
    AxlHandle           nvme;   /* AXL_STORAGE_NVME: controller handle */
    AxlAtaDev          *ata;    /* AXL_STORAGE_ATA */
    AxlScsiDev         *scsi;   /* AXL_STORAGE_SCSI */
};

static struct AxlStorageDev *g_devs   = NULL;
static size_t                g_count  = 0;
static bool                  g_inited = false;

// ---------------------------------------------------------------------------
// Enumeration cache (image-lifetime; the per-transport walks own their own
// caches, so this just records which transport each device came from)
// ---------------------------------------------------------------------------

static void
storage_cleanup(void *ctx)
{
    (void)ctx;
    axl_free(g_devs);
    g_devs = NULL;
    g_count = 0;
}

static struct AxlStorageDev *
storage_add(void)
{
    struct AxlStorageDev *grown =
        axl_realloc(g_devs, (g_count + 1) * sizeof(*g_devs));
    if (grown == NULL) {
        return NULL;
    }
    g_devs = grown;
    struct AxlStorageDev *d = &g_devs[g_count++];
    axl_memset(d, 0, sizeof(*d));
    return d;
}

static void
storage_ensure(void)
{
    if (g_inited) {
        return;
    }
    g_inited = true;

    AxlHandle ctrl = NULL;
    while ((ctrl = axl_nvme_next(ctrl)) != NULL) {
        struct AxlStorageDev *d = storage_add();
        if (d != NULL) {
            d->transport = AXL_STORAGE_NVME;
            d->nvme      = ctrl;
        }
    }

    AxlAtaDev *a = NULL;
    while ((a = axl_ata_next(a)) != NULL) {
        struct AxlStorageDev *d = storage_add();
        if (d != NULL) {
            d->transport = AXL_STORAGE_ATA;
            d->ata       = a;
        }
    }

    AxlScsiDev *s = NULL;
    while ((s = axl_scsi_next(s)) != NULL) {
        struct AxlStorageDev *d = storage_add();
        if (d != NULL) {
            d->transport = AXL_STORAGE_SCSI;
            d->scsi      = s;
        }
    }

    if (g_count > 0) {
        axl_atexit(storage_cleanup, NULL);
    }
}

AxlStorageDev *
axl_storage_next(AxlStorageDev *prev)
{
    storage_ensure();
    if (g_count == 0) {
        return NULL;
    }
    if (prev == NULL) {
        return &g_devs[0];
    }
    for (size_t i = 0; i < g_count; i++) {
        if (&g_devs[i] == prev) {
            return (i + 1 < g_count) ? &g_devs[i + 1] : NULL;
        }
    }
    return &g_devs[0];   /* foreign pointer: rewind */
}

int
axl_storage_get_transport(const AxlStorageDev *dev, AxlStorageTransport *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }
    *out = dev->transport;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_storage_get_location
// ---------------------------------------------------------------------------

int
axl_storage_get_location(const AxlStorageDev *dev, char *out, size_t out_size)
{
    if (dev == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';

    switch (dev->transport) {
    case AXL_STORAGE_NVME: {
        void *dp = NULL;
        if (axl_handle_get_protocol(dev->nvme, "device-path", &dp) != AXL_OK
            || dp == NULL) {
            return AXL_ERR;
        }
        AXL_AUTO_FREE char *text = axl_device_path_to_text(dp);
        if (text == NULL) {
            return AXL_ERR;
        }
        axl_strlcpy(out, text, out_size);
        return AXL_OK;
    }
    case AXL_STORAGE_ATA: {
        uint16_t port = 0, pmport = 0;
        axl_ata_get_address(dev->ata, &port, &pmport);
        axl_snprintf(out, out_size, "ata %u.%u", port, pmport);
        return AXL_OK;
    }
    case AXL_STORAGE_SCSI: {
        uint8_t  tgt[AXL_SCSI_TARGET_LEN] = { 0 };
        uint64_t lun = 0;
        axl_scsi_get_address(dev->scsi, tgt, &lun);
        axl_snprintf(out, out_size, "scsi %02x%02x:%llu",
                     tgt[0], tgt[1], (unsigned long long)lun);
        return AXL_OK;
    }
    default:
        return AXL_ERR;
    }
}

// ---------------------------------------------------------------------------
// axl_smart_health — dispatch + read + normalize
// ---------------------------------------------------------------------------

int
axl_smart_health(AxlStorageDev *dev, AxlSmartHealth *out)
{
    if (dev == NULL || out == NULL) {
        return AXL_ERR;
    }

    switch (dev->transport) {
    case AXL_STORAGE_NVME: {
        AxlNvmeController id;
        AxlNvmeSmart      s;
        if (axl_nvme_identify_controller(dev->nvme, &id) != AXL_OK
            || axl_nvme_smart(dev->nvme, &s) != AXL_OK) {
            return AXL_ERR;
        }
        return axl_smart_from_nvme(&id, &s, out);
    }
    case AXL_STORAGE_ATA: {
        AxlAtaIdentify id;
        AxlAtaSmart    s;
        if (axl_ata_identify(dev->ata, &id) != AXL_OK
            || axl_ata_smart(dev->ata, &s) != AXL_OK) {
            return AXL_ERR;
        }
        return axl_smart_from_ata(&id, &s, out);
    }
    case AXL_STORAGE_SCSI: {
        AxlScsiInquiry inq;
        AxlScsiHealth  h;
        if (axl_scsi_inquiry(dev->scsi, &inq) != AXL_OK
            || axl_scsi_health(dev->scsi, &h) != AXL_OK) {
            return AXL_ERR;
        }
        return axl_smart_from_scsi(&inq, &h, out);
    }
    default:
        return AXL_ERR;
    }
}
