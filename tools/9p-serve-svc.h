/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-serve-svc.h - the `9p serve` service descriptor, linked into BOTH
 * binaries.
 *
 * 9p-serve-svc.c defines Serve9pOpts, serve9p_descs and serve9p_service
 * unconditionally, but the impl (setup, teardown, the Axl9pServer it
 * builds) is gated on AXL_SERVICE_BUILD_DRIVER, so the launcher build
 * carries only the descriptor. The launcher reads svc->opts_descs and
 * svc->user for axl_service_start_embedded's LoadOptions serialization;
 * it never invokes setup/teardown, which run on the driver side.
 *
 * Cross-binary ABI rule: build both images from the same source tree with
 * identical compile flags except the -DAXL_SERVICE_BUILD_DRIVER toggle,
 * per axl-sdk's AxlService contract.
 */

#ifndef AXL_TOOLS_9P_SERVE_SVC_H
#define AXL_TOOLS_9P_SERVE_SVC_H

#include <axl.h>
#include <axl/axl-net-opts.h>

/// Configuration for the serve service.
///
/// Every field is populated by AxlConfig auto-apply from @ref
/// serve9p_descs - from CLI args in the launcher, from LoadOptions in the
/// driver. @ref Serve9pOpts::net is the canonical AxlNetOpts sub-struct;
/// serve uses its `nic_index` and `port` (the listen port). `local_ip` is
/// unused: the library's axl_9p_server_listen takes no bind address, which
/// is why the tool ships no --listen-ip.
///
/// @ref Serve9pOpts::root is `const char *` per AxlConfig's
/// AXL_CFG_STRING contract - auto-apply assigns a borrowed pointer, not a
/// copy, so an inline char[] would silently fail to populate.
typedef struct {
    AxlNetOpts  net;    ///< nic_index + port (listen); local_ip unused
    const char *root;   ///< AxlFs subtree to export
    bool        ro;     ///< export read-only (mutating ops answer EROFS)
} Serve9pOpts;

/* Defined in 9p-serve-svc.c, linked into both binaries. */
extern Serve9pOpts         g_serve9p_opts;
extern const AxlConfigDesc serve9p_descs[];
extern const AxlService    serve9p_service;

#endif /* AXL_TOOLS_9P_SERVE_SVC_H */
