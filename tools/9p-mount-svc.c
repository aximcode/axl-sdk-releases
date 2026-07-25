/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-mount-svc.c - the `9p mount` service, dual-compiled.
 *
 * With -DAXL_SERVICE_BUILD_DRIVER this file IS 9p-mount-dxe.efi: a
 * resident DXE driver whose setup() brings a NIC online, connects to the
 * remote 9P server and publishes it as a UEFI fsN: volume. Without it, the
 * same file contributes only the descriptor + descs + opts that
 * 9p-cmd-mount.c hands to axl_service_start_embedded - so the launcher
 * does not link the fs-provider bridge or the mount machinery
 * (axl_9p_mount, axl_fs_provider_publish, mount_setup/mount_teardown). The
 * 9P client itself (axl_9p_connect) IS linked into 9p.efi regardless -
 * 9p-cmd-file.c needs it for ls/get/put - so this gating is narrower than
 * serve's (verified with `nm` on the built launcher).
 *
 * Residency is the point: the driver owns the connection, so the volume
 * outlives the `9p mount` command and the Shell can read it at the prompt.
 *
 * The provider's callbacks run synchronously in the CALLER's context (a
 * Shell `type`, another app's axl_file_get_contents), not on the driver
 * loop - the loop only exists because AxlService gives every driver one.
 * Hence there is no supervise loop and no tick work to do, which is why
 * driver_tick_ms below is an order of magnitude lazier than serve's: serve
 * accepts connections on its loop, mount does nothing there.
 *
 * Coherence note: the volume this publishes is CLOSE-TO-OPEN consistent,
 * the guarantee the rest of AXL's file layer promises - a freshly opened
 * handle sees current contents. Two images reading one tree are not
 * coherent WHILE open, by contract.
 *
 * The descriptor is the cross-binary ABI: one definition, two images.
 * See 9p-mount-svc.h.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-mount-svc.h"

AXL_LOG_DOMAIN("9p-mount");

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

#ifdef AXL_SERVICE_BUILD_DRIVER

static int
mount_setup(
    AxlLoop *loop,   ///< the driver's event loop (unused; see the file header)
    void    *user    ///< &g_mount9p_opts, decoded from LoadOptions
);

static int
mount_teardown(
    void *user   ///< &g_mount9p_opts (unused)
);

#endif /* AXL_SERVICE_BUILD_DRIVER */

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

Mount9pOpts g_mount9p_opts;

const AxlConfigDesc mount9p_descs[] = {
    AXL_9P_NET_CFG_DESCS(Mount9pOpts),
    { "host",  AXL_CFG_STRING, "",
      "Remote 9P server address",
      offsetof(Mount9pOpts, host),  sizeof(const char *) },
    { "aname", AXL_CFG_STRING, "/",
      "Exported tree to attach",
      offsetof(Mount9pOpts, aname), sizeof(const char *) },
    { "ro",    AXL_CFG_BOOL,   "false",
      "Publish the volume read-only",
      offsetof(Mount9pOpts, ro),    sizeof(bool) },
    { 0 }
};

/* Defined in both images. The setup/teardown MEMBERS are gated, not just
   their functions - a launcher-side reference to mount_setup would drag
   the fs-provider bridge and the mount machinery into 9p.efi (the 9P
   client itself is already there for ls/get/put; see the file header),
   which is the whole point of the split. Leaving them NULL launcher-side
   is safe: see the matching note in 9p-serve-svc.c. */
const AxlService mount9p_service = {
    .name           = "9p-mount",
    .opts_descs     = mount9p_descs,
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = mount_setup,
    .teardown       = mount_teardown,
#endif
    .user           = &g_mount9p_opts,
    .driver_tick_ms = 200,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static Axl9pClient *m_client;
static void        *m_volume;

// ---------------------------------------------------------------------------
// Service lifecycle (driver image only)
// ---------------------------------------------------------------------------

static int
mount_setup(
    AxlLoop *loop,
    void    *user
)
{
    Mount9pOpts *o = (Mount9pOpts *)user;

    (void)loop;   /* the provider is synchronous; nothing runs on the loop */

    if (o->host == NULL || o->host[0] == '\0') {
        axl_error("no server address");
        return AXL_ERR;
    }
    if (axl_net_init_from_opts(&o->net, 10) != AXL_OK) {
        axl_error("could not bring a NIC online");
        return AXL_ERR;
    }
    if (axl_9p_connect(o->host, o->net.port, "", o->aname, &m_client)
        != AXL_OK) {
        axl_error("cannot connect to %s:%u", o->host, (unsigned)o->net.port);
        return AXL_ERR;
    }
    if (axl_9p_mount(m_client, o->ro, &m_volume) != AXL_OK) {
        axl_error("cannot publish the volume");
        axl_9p_disconnect(m_client);
        m_client = NULL;
        return AXL_ERR;
    }
    axl_info("mounted %s:%u%s", o->host, (unsigned)o->net.port,
             o->ro ? " (read-only)" : "");
    return AXL_OK;
}

static int
mount_teardown(
    void *user
)
{
    (void)user;

    /* Unmount BEFORE disconnecting: axl_9p_mount borrows the client, and
       the volume's still-open handles are clunked over that connection.
       The reverse order would clunk over a closed socket. */
    int rc = axl_9p_unmount(m_volume);
    if (rc != AXL_OK) {
        axl_warning("unmount reported a failure; disconnecting anyway");
    }
    m_volume = NULL;
    axl_9p_disconnect(m_client);
    m_client = NULL;
    /* Deliberately return AXL_OK regardless of rc above, NOT a bare "ignore
       the error" - _axl_service_driver_unload_stub ORs a non-OK teardown rc
       into the EFI_STATUS it hands back to gBS->UnloadImage precisely so a
       failed teardown does not get absorbed into a "successful" unload
       (axl-service.c's own comment on that OR). Propagating rc here would
       trip that, but the unload stub uninstalls this driver's marker
       protocol (what axl_service_is_running/`9p status` key off) BEFORE
       returning to the firmware - by the time gBS->UnloadImage sees a
       non-EFI_SUCCESS return and (per the UEFI spec) leaves the image
       resident, the marker is already gone. `9p status` would then report
       "stopped" for an image that is still occupying memory: a worse lie
       than the one this discards. axl_9p_unmount's only non-OK returns are
       "token already dead" / "not an SFS handle" (axl-9p-mount.c), neither
       reachable from this single, non-reentrant teardown call - so today
       this trade costs nothing in practice, but it is not accidental, and a
       future non-degenerate failure mode should be weighed against the
       same "resident-but-reports-stopped" outcome before changing this. */
    return AXL_OK;
}

AXL_SERVICE_DRIVER(mount9p_service);

#endif /* AXL_SERVICE_BUILD_DRIVER */
