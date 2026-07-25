/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-mount-svc.h - the `9p mount` service descriptor, linked into BOTH
 * binaries.
 *
 * 9p-mount-svc.c defines Mount9pOpts, mount9p_descs and mount9p_service
 * unconditionally, but the impl (setup, teardown, the Axl9pClient and the
 * published volume) is gated on AXL_SERVICE_BUILD_DRIVER, so the launcher
 * build carries only the descriptor. The launcher reads svc->opts_descs
 * and svc->user for axl_service_start_embedded's LoadOptions
 * serialization; it never invokes setup/teardown, which run on the driver
 * side.
 *
 * Cross-binary ABI rule: build both images from the same source tree with
 * identical compile flags except the -DAXL_SERVICE_BUILD_DRIVER toggle,
 * per axl-sdk's AxlService contract.
 */

#ifndef AXL_TOOLS_9P_MOUNT_SVC_H
#define AXL_TOOLS_9P_MOUNT_SVC_H

#include <axl.h>
#include <axl/axl-net-opts.h>

/// Configuration for the mount service.
///
/// Every field is populated by AxlConfig auto-apply from @ref
/// mount9p_descs - from CLI args in the launcher, from LoadOptions in the
/// driver. @ref Mount9pOpts::net is the canonical AxlNetOpts sub-struct;
/// mount uses its `nic_index` and `port` (the REMOTE server's port, where
/// serve's is the listen port). `local_ip` is unused, as in serve, but for
/// a different underlying reason: axl_9p_connect takes no source-address
/// parameter even though axl_tcp_connect_via does, which is why the tool
/// ships no --source-ip.
///
/// @ref Mount9pOpts::host and @ref Mount9pOpts::aname are `const char *`
/// per AxlConfig's AXL_CFG_STRING contract - auto-apply assigns a borrowed
/// pointer, not a copy, so an inline char[] would silently fail to
/// populate.
typedef struct {
    AxlNetOpts  net;     ///< nic_index + port (remote); local_ip unused
    const char *host;    ///< remote 9P server address
    const char *aname;   ///< exported tree to attach
    bool        ro;      ///< publish the volume read-only
} Mount9pOpts;

/* Defined in 9p-mount-svc.c, linked into both binaries. */
extern Mount9pOpts         g_mount9p_opts;
extern const AxlConfigDesc mount9p_descs[];
extern const AxlService    mount9p_service;

#endif /* AXL_TOOLS_9P_MOUNT_SVC_H */
