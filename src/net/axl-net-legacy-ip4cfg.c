/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-legacy-ip4cfg.c
    axl_net_legacy_ip4cfg_install / _uninstall: load and unload the resident
    driver (axl-net-legacy-ip4cfg-drv.c) that gives tools from before UEFI 2.4
    the IPv4 configuration protocols current firmware no longer produces.

    The driver is a separate image embedded in libaxl.a (src/net/CMakeLists.txt
    builds it freestanding and embeds it as a blob), buffer-loaded here. Only
    images that call these functions -- AXL's network bring-up does -- link the
    blob.
**/

#include <axl/axl-driver.h>
#include <axl/axl-embed.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-net.h>
#include <axl/axl-sys.h>

#include "axl-net-legacy-ip4cfg.h"

AXL_LOG_DOMAIN("net");

AXL_EMBED_DECLARE(legacy_ip4cfg);

static const AxlGuid legacy_marker_guid = AXL_NET_LEGACY_IP4CFG_MARKER_GUID;

int
axl_net_legacy_ip4cfg_install(void)
{
    void *found = NULL;
    if (axl_protocol_find_guid(&legacy_marker_guid, &found) == AXL_OK) {
        const AxlNetLegacyIp4CfgMarker *marker = found;
        if (marker->revision >= AXL_NET_LEGACY_IP4CFG_REVISION) {
            return AXL_OK;   /* resident already, this revision or newer */
        }
        /* Left by a tool built against an older SDK: replace it, so the
           behaviour is this SDK's. One that will not unload stays. */
        if (axl_net_legacy_ip4cfg_uninstall() != AXL_OK) {
            axl_debug("legacy ip4cfg: an older resident driver would not unload");
            return AXL_ERR;
        }
    }

    AxlEmbeddedImageInfo info = { .file_name = AXL_LEGACY_IP4CFG_FILE_NAME };
    AxlDriverHandle drv = NULL;
    if (axl_driver_load_buffer_with_image_info(AXL_EMBED_DATA(legacy_ip4cfg),
                                               AXL_EMBED_SIZE(legacy_ip4cfg),
                                               &info, &drv) != AXL_OK) {
        axl_debug("legacy ip4cfg: driver load failed");
        return AXL_ERR;
    }
    if (axl_driver_start(drv) != AXL_OK) {
        axl_debug("legacy ip4cfg: driver start failed");
        axl_driver_unload(drv);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_net_legacy_ip4cfg_uninstall(void)
{
    void  **images = NULL;
    size_t  count = 0;
    if (axl_protocol_enumerate_guid(&legacy_marker_guid, &images, &count) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = AXL_OK;
    for (size_t i = 0; i < count; i++) {
        /* The driver's Unload takes the protocols off every NIC first. */
        if (axl_driver_unload(images[i]) != AXL_OK) {
            rc = AXL_ERR;
        }
    }
    axl_free(images);
    return rc;
}
