/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-serve-svc.c - the `9p serve` service, dual-compiled.
 *
 * With -DAXL_SERVICE_BUILD_DRIVER this file IS 9p-serve-dxe.efi: a
 * resident DXE driver whose setup() brings a NIC online, builds an
 * Axl9pServer over the requested root on the driver's loop, and listens.
 * Without it, the same file contributes only the descriptor + descs +
 * opts that 9p-cmd-serve.c hands to axl_service_start_embedded - so the
 * launcher does not link Axl9pServer at all.
 *
 * The descriptor is the cross-binary ABI: one definition, two images.
 * See 9p-serve-svc.h.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-serve-svc.h"

AXL_LOG_DOMAIN("9p-serve");

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

#ifdef AXL_SERVICE_BUILD_DRIVER

static int
serve_setup(
    AxlLoop *loop,   ///< the driver's event loop
    void    *user    ///< &g_serve9p_opts, decoded from LoadOptions
);

static int
serve_teardown(
    void *user   ///< &g_serve9p_opts (unused)
);

#endif /* AXL_SERVICE_BUILD_DRIVER */

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

Serve9pOpts g_serve9p_opts;

const AxlConfigDesc serve9p_descs[] = {
    AXL_9P_NET_CFG_DESCS(Serve9pOpts),
    { "root", AXL_CFG_STRING, "fs0:\\",
      "AxlFs subtree to export",
      offsetof(Serve9pOpts, root), sizeof(const char *) },
    { "ro",   AXL_CFG_BOOL,   "false",
      "Export read-only (every mutating request answers EROFS)",
      offsetof(Serve9pOpts, ro),   sizeof(bool) },
    { 0 }
};

/* Defined in both images. The setup/teardown MEMBERS are gated, not just
   their functions - a launcher-side reference to serve_setup would drag
   Axl9pServer into 9p.efi, which is the whole point of the split. Leaving
   them NULL launcher-side is safe: axl_service_start_embedded and
   axl_service_is_running read only .name, .opts_descs and .user; the
   svc->setup != NULL requirement is enforced by _axl_service_driver_init
   and axl_service_attach_driver, both driver-side only. Same shape as
   axl-webfs's webfs-serve.c. */
const AxlService serve9p_service = {
    .name           = "9p-serve",
    .opts_descs     = serve9p_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = serve_setup,
    .teardown       = serve_teardown,
#endif
    .user           = &g_serve9p_opts,
    .driver_tick_ms = 20,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static Axl9pServer *m_server;

// ---------------------------------------------------------------------------
// Service lifecycle (driver image only)
// ---------------------------------------------------------------------------

static int
serve_setup(
    AxlLoop *loop,
    void    *user
)
{
    Serve9pOpts *o = (Serve9pOpts *)user;

    if (o->root == NULL || o->root[0] == '\0') {
        axl_error("no export root");
        return AXL_ERR;
    }
    if (axl_net_init_from_opts(&o->net, 10) != AXL_OK) {
        axl_error("could not bring a NIC online");
        return AXL_ERR;
    }
    if (axl_9p_server_new(loop, o->root, o->ro, &m_server) != AXL_OK) {
        axl_error("cannot export %s", o->root);
        return AXL_ERR;
    }
    if (axl_9p_server_listen(m_server, o->net.port) != AXL_OK) {
        axl_error("cannot listen on port %u", (unsigned)o->net.port);
        axl_9p_server_free(m_server);
        m_server = NULL;
        return AXL_ERR;
    }
    axl_info("exporting %s on port %u%s", o->root, (unsigned)o->net.port,
             o->ro ? " (read-only)" : "");
    return AXL_OK;
}

static int
serve_teardown(
    void *user
)
{
    (void)user;
    axl_9p_server_free(m_server);
    m_server = NULL;
    return AXL_OK;
}

AXL_SERVICE_DRIVER(serve9p_service);

#endif /* AXL_SERVICE_BUILD_DRIVER */
