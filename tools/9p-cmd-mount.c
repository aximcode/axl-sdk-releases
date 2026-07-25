/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-mount.c - the `mount` and `umount` verbs.
 *
 * mount: fills g_mount9p_opts from AxlArgs (the launcher and the driver
 * agree on the field layout via mount9p_descs, so this manual fill matches
 * what AXL_SERVICE_DRIVER decodes from LoadOptions on the other side),
 * then hands an AxlServiceDeploy to axl_service_start_embedded, which
 * loads the embedded 9p-mount-dxe.efi. The driver connects and publishes
 * the volume, then stays resident so the fsN: outlives this command -
 * which is the entire point of the verb.
 *
 * umount: axl_service_stop unloads the driver image, whose teardown
 * unmounts the volume and disconnects. Idempotent.
 *
 * The server address is `host[:port]`, the same form ls/get/put take -
 * axl9p_split_host_port (9p-common.c) is the one splitter all four share.
 * mount ALSO has a --port flag, so the two are ordered: an inline ":port"
 * wins, and --port supplies the port for an address that names none. The
 * more specific form wins, and `9p mount 10.0.0.5` with no port at all
 * lands on --port's own default, the 9P well-known 564.
 *
 * Naming the published volume is the launcher's job even though the DRIVER
 * publishes it: axl_9p_mount hands back an opaque token, not a name, and
 * that token lives in the other image. Volumes are global, though, so the
 * launcher can snapshot axl_volume_enumerate around the deploy and resolve
 * the handle that appeared. See mount_report_volume.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-mount-svc.h"

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/* Snapshot capacity for the before/after volume diff. Far above any real
   firmware's volume count; a run that fills it is reported as "could not
   resolve", never as a guessed name (see mount_report_volume). */
#define MOUNT_VOL_MAX  32

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/* Embedded 9p-mount-dxe.efi blob - spliced in by the Makefile's
   EMBED_BLOB(mount9p_dxe, ...). */
AXL_EMBED_DECLARE(mount9p_dxe);

// ---------------------------------------------------------------------------
// Flags and positionals - non-static; 9p.c builds the verb tree from them
// ---------------------------------------------------------------------------

const AxlArgDesc axl9p_mount_flags[] = {
    AXL_9P_NET_ARG_NIC,
    AXL_9P_NET_ARG_PORT,
    { .name = "aname", .type = AXL_ARG_STRING, .default_value = "/",
      .help = "Exported tree to attach" },
    { .name = "ro",    .type = AXL_ARG_BOOL,
      .help = "Publish the volume read-only" },
    { 0 }
};

const AxlArgDesc axl9p_mount_positional[] = {
    { .name = "host", .type = AXL_ARG_STRING, .required = true,
      .help = "server address as host or host:port" },
    { 0 }
};

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

/// @brief Is @p handle one of the @p n volumes in @p vols?
/// @return true if a descriptor in @p vols carries @p handle.
static bool
mount_volume_known(
    const AxlVolume *vols,     ///< snapshot to search
    size_t           n,        ///< entries in @p vols
    const void      *handle    ///< firmware handle to look for
);

/// @brief Print which fsN: the just-deployed driver published, if it can.
static void
mount_report_volume(
    const char      *host,      ///< server address, echoed in every branch
    const AxlVolume *before,    ///< pre-deploy snapshot, NULL when unavailable
    size_t           n_before   ///< entries in @p before
);

/// @brief Build the deploy descriptor: the service plus the embedded image.
/// @return the descriptor, by value.
static AxlServiceDeploy
mount_deploy(
    void
);

// ---------------------------------------------------------------------------
// Volume reporting
// ---------------------------------------------------------------------------

static bool
mount_volume_known(
    const AxlVolume *vols,
    size_t           n,
    const void      *handle
)
{
    for (size_t i = 0; i < n; i++) {
        if (vols[i].handle == handle) {
            return true;
        }
    }
    return false;
}

/* The diff keys on the firmware HANDLE, not on AxlVolume::name: the name is
   the shell's alias, which is exactly the thing being discovered here, and
   its documented fallback is a synthesized positional "fsN" that can collide
   with a mapped volume's alias. Handles are identities and cannot.

   The name itself then comes from axl_volume_map_name, which returns the
   shell's REAL alias or a clean failure - never a plausible-but-wrong index.
   That distinction is the whole value of printing a name: a human is going
   to type it. */
static void
mount_report_volume(
    const char      *host,
    const AxlVolume *before,
    size_t           n_before
)
{
    AxlVolume   after[MOUNT_VOL_MAX];
    size_t      n_after   = 0;
    bool        found     = false;
    const void *published = NULL;
    char        name[16];

    if (before == NULL
        || axl_volume_enumerate(after, MOUNT_VOL_MAX, &n_after) != AXL_OK
        || n_after >= MOUNT_VOL_MAX) {
        axl_printf("9p: mounted %s (could not resolve the new volume's name; "
                   "run `map -r` to find it)\n", host);
        return;
    }
    for (size_t i = 0; i < n_after; i++) {
        if (!mount_volume_known(before, n_before, after[i].handle)) {
            found     = true;
            published = after[i].device_path;
            break;
        }
    }
    /* Defensive, not reached by any path known today. By the time this runs,
       axl_service_start_embedded already returned AXL_OK, and per
       axl-service.c's DriverEntry that only happens after mount_setup's
       axl_9p_mount call has registered a brand-new simple-fs handle - so
       the diff above is guaranteed to find it. The two triggers an earlier
       version of this comment cited are both unreachable: a second mount of
       an already-mounted server never gets here at all (axl_service_is_
       running short-circuits axl9p_mount_handler before the "before"
       snapshot is even taken), and a firmware with no free fsN: slot still
       creates and registers the handle (publish_shell_map's mapping step is
       best-effort - see its docstring in axl-fs-provider.c); that case
       surfaces at the axl_volume_map_name failure below, not here. Kept as
       a guard against a future change to either registration path making
       this reachable, and against axl_protocol_enumerate itself
       misreporting between the two snapshots. */
    if (!found) {
        axl_printf("9p: mounted %s (no new volume appeared)\n", host);
        return;
    }
    /* AxlVolume::device_path is documented NULL on handles that don't
       publish one at all (axl-fs.h) - distinct from "no new volume
       appeared" above: a volume DID appear, it just has nothing to hand
       axl_volume_map_name. Reporting it as "no new volume appeared" would
       claim the opposite of what happened. */
    if (published == NULL) {
        axl_printf("9p: mounted %s (the new volume has no device path; "
                   "see `dh` for its handle)\n", host);
        return;
    }
    if (axl_volume_map_name(published, name, sizeof(name)) != AXL_OK) {
        axl_printf("9p: mounted %s (the new volume has no shell alias yet; "
                   "run `map -r`)\n", host);
        return;
    }
    axl_printf("9p: mounted %s as %s:\n", host, name);
}

// ---------------------------------------------------------------------------
// Verb handlers
// ---------------------------------------------------------------------------

static AxlServiceDeploy
mount_deploy(
    void
)
{
    AxlServiceDeploy d = {
        .service         = &mount9p_service,
        .driver_blob     = AXL_EMBED_DATA(mount9p_dxe),
        .driver_blob_len = AXL_EMBED_SIZE(mount9p_dxe),
        .driver_name     = "9p-mount-dxe.efi",
    };
    return d;
}

int
axl9p_mount_handler(
    AxlArgs *a
)
{
    const AxlServiceDeploy d = mount_deploy();
    AxlVolume              before[MOUNT_VOL_MAX];
    size_t                 n_before = 0;
    bool                   snapped;
    const char            *spec     = axl_args_get_string(a, "host");
    /* g_mount9p_opts.host points AT this buffer. Same lifetime rule the
       axl_args_get_string fields below already live under: every read of it
       (the serialize pass inside start_embedded, then mount_report_volume)
       happens before this handler returns. */
    char                   host[AXL_9P_HOST_MAX];
    /* Seeds axl9p_split_host_port's IN/OUT port with the parsed --port
       (itself defaulted to AXL_9P_PORT_DEFAULT), so an inline ":port" in
       the address OVERRIDES --port and an address without one keeps it.
       Safe to narrow: axl9p_mount_flags declares .min = 1, .max = 65535 on
       "port", so axl_args_run already rejected anything outside uint16_t
       range before this handler ran - the cast can't truncate. */
    uint16_t               port     = (uint16_t)axl_args_get_uint(a, "port");

    if (!axl9p_split_host_port(spec, host, sizeof(host), &port)) {
        axl_printerr("9p: bad server address '%s' (want host or host:port)\n",
                     spec != NULL ? spec : "");
        return 1;
    }

    /* Populate the shared opts struct BEFORE start_embedded serializes it.
       axl_args_get_string's pointers stay valid until this handler returns,
       and the serialize pass happens inside the call below - so the copy
       must not be deferred past it. */
    g_mount9p_opts.net.nic_index = axl_args_get_uint(a, "nic");
    g_mount9p_opts.net.port      = port;
    g_mount9p_opts.host          = host;
    g_mount9p_opts.aname         = axl_args_get_string(a, "aname");
    g_mount9p_opts.ro            = axl_args_get_bool(a, "ro");

    if (axl_service_is_running(&d)) {
        /* The options just parsed into g_mount9p_opts above never reach the
           running driver - the resident instance keeps whatever it was
           deployed with. Say so explicitly rather than letting a
           remounting `mount` call read as a success: reporting 0 here is
           correct (an already-published volume is not an error), but the
           message must not imply the new flags took effect. */
        axl_printf("9p: already mounted - the running mount was not "
                   "reconfigured; run `9p umount` then `9p mount` again to "
                   "change its options\n");
        return 0;
    }
    /* Snapshot before the deploy, so the volume the driver publishes is the
       one that appears between the two enumerations. */
    snapped = (axl_volume_enumerate(before, MOUNT_VOL_MAX, &n_before) == AXL_OK
               && n_before < MOUNT_VOL_MAX);
    if (axl_service_start_embedded(&d) != AXL_OK) {
        axl_printerr("9p: could not start the mount driver\n");
        return 1;
    }
    mount_report_volume(g_mount9p_opts.host, snapped ? before : NULL, n_before);
    return 0;
}

int
axl9p_umount_handler(
    AxlArgs *a
)
{
    /* The stop SEQUENCE lives in axl9p_stop_service (shared with
       `serve-stop`); only the wording is this verb's. */
    static const Axl9pStopMsgs msgs = {
        .idle = "9p: not mounted\n",
        .fail = "9p: could not unmount\n",
        .done = "9p: unmounted\n",
    };

    (void)a;

    return axl9p_stop_service(&mount9p_service, &msgs);
}
