/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-legacy-ip4cfg.h
    What the library loader (axl-net-legacy-ip4cfg.c) and the resident driver
    it loads (axl-net-legacy-ip4cfg-drv.c) share beyond the public marker
    contract in <axl/axl-net.h> (AXL_NET_LEGACY_IP4CFG_MARKER_GUID,
    AXL_NET_LEGACY_IP4CFG_REVISION, AxlNetLegacyIp4CfgMarker). The driver links
    no libaxl; it uses only those declarations, never a function.
**/

#ifndef AXL_NET_LEGACY_IP4CFG_H
#define AXL_NET_LEGACY_IP4CFG_H

#include <axl/axl-net.h>

/* The file name the loader gives the buffer-loaded image, which `drivers` and
   `dh` show for it. */
#define AXL_LEGACY_IP4CFG_FILE_NAME "AxlLegacyIp4Config.efi"

#endif /* AXL_NET_LEGACY_IP4CFG_H */
