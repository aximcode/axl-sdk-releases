/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-cmd-serve.c - the `serve` and `serve-stop` verbs.
 *
 * serve: fills g_serve9p_opts from AxlArgs (the launcher and the driver
 * agree on the field layout via serve9p_descs, so this manual fill matches
 * what AXL_SERVICE_DRIVER decodes from LoadOptions on the other side),
 * then hands an AxlServiceDeploy to axl_service_start_embedded, which
 * loads the embedded 9p-serve-dxe.efi. The export runs as a resident DXE
 * driver until `serve-stop` (or `unload -n 9p-serve-dxe.efi`).
 *
 * serve-stop: axl_service_stop resolves the running image by the
 * service's name-derived GUID and unloads it. Idempotent.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-serve-svc.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/* Embedded 9p-serve-dxe.efi blob - spliced in by the Makefile's
   EMBED_BLOB(serve9p_dxe, ...). */
AXL_EMBED_DECLARE(serve9p_dxe);

// ---------------------------------------------------------------------------
// Flags and positionals - non-static; 9p.c builds the verb tree from them
// ---------------------------------------------------------------------------

const AxlArgDesc axl9p_serve_flags[] = {
    AXL_9P_NET_ARG_NIC,
    AXL_9P_NET_ARG_PORT,
    { .name = "ro",                        .type = AXL_ARG_BOOL,
      .help = "Export read-only" },
    { .name = "detach", .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Start the driver and return to the shell" },
    { 0 }
};

const AxlArgDesc axl9p_serve_positional[] = {
    { .name = "root", .type = AXL_ARG_STRING, .default_value = "fs0:\\",
      .help = "AxlFs subtree to export" },
    { 0 }
};

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

/// @brief Build the deploy descriptor: the service plus the embedded image.
/// @return the descriptor, by value.
static AxlServiceDeploy
serve_deploy(
    void
);

// ---------------------------------------------------------------------------
// Verb handlers
// ---------------------------------------------------------------------------

static AxlServiceDeploy
serve_deploy(
    void
)
{
    AxlServiceDeploy d = {
        .service         = &serve9p_service,
        .driver_blob     = AXL_EMBED_DATA(serve9p_dxe),
        .driver_blob_len = AXL_EMBED_SIZE(serve9p_dxe),
        .driver_name     = "9p-serve-dxe.efi",
    };
    return d;
}

int
axl9p_serve_handler(
    AxlArgs *a
)
{
    const AxlServiceDeploy d = serve_deploy();

    /* Populate the shared opts struct BEFORE start_embedded serializes it.
       axl_args_get_string's pointers stay valid until this handler returns,
       and the serialize pass happens inside the call below - so the copy
       must not be deferred past it. */
    g_serve9p_opts.net.nic_index = axl_args_get_uint(a, "nic");
    /* Safe to narrow: axl9p_serve_flags declares .min = 1, .max = 65535 on
       "port", so axl_args_run already rejected anything outside uint16_t
       range before this handler ran - the cast below can't truncate. */
    g_serve9p_opts.net.port      = (uint16_t)axl_args_get_uint(a, "port");
    g_serve9p_opts.root          = axl_args_get_string(a, "root");
    g_serve9p_opts.ro            = axl_args_get_bool(a, "ro");

    if (axl_service_is_running(&d)) {
        /* The options just parsed into g_serve9p_opts above never reach the
           running driver - the resident instance keeps whatever it was
           deployed with. Say so explicitly rather than letting a
           reconfiguring `serve` call read as a success: reporting 0 here is
           correct (an already-running export is not an error), but the
           message must not imply the new flags took effect. */
        axl_printf("9p: already serving - the running export was not "
                   "reconfigured; run `9p serve-stop` then `9p serve` "
                   "again to change its options\n");
        return 0;
    }
    if (axl_service_start_embedded(&d) != AXL_OK) {
        axl_printerr("9p: could not start the serve driver\n");
        return 1;
    }
    axl_printf("9p: serving %s on port %u\n", g_serve9p_opts.root,
               (unsigned)g_serve9p_opts.net.port);
    if (axl_args_get_bool(a, "detach")) {
        return 0;
    }
    /* axl_service_supervise already returns a process-exit-shaped int
       (0 clean / 1 otherwise), not an AXL_* status - pass it straight
       through rather than re-translating it. */
    return axl_service_supervise(&d);
}

int
axl9p_serve_stop_handler(
    AxlArgs *a
)
{
    /* The stop SEQUENCE lives in axl9p_stop_service (shared with `umount`);
       only the wording is this verb's. */
    static const Axl9pStopMsgs msgs = {
        .idle = "9p: not serving\n",
        .fail = "9p: could not stop the serve driver\n",
        .done = "9p: stopped serving\n",
    };

    (void)a;

    return axl9p_stop_service(&serve9p_service, &msgs);
}
