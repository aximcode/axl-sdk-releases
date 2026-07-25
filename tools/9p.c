/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p.c - 9P2000.L client and server for the UEFI Shell: the verb tree.
 *
 * A thin launcher over <axl/axl-9p.h>. Two shapes of verb:
 *
 *   One-shot   ls / get / put - a synchronous Axl9pClient session per
 *              invocation. Handlers in 9p-cmd-file.c.
 *
 *   Resident   serve / mount - deploy an embedded DXE driver via
 *              AxlService so the export (or the fsN: volume) outlives the
 *              command. Handlers in 9p-cmd-serve.c / 9p-cmd-mount.c; the
 *              services themselves in 9p-{serve,mount}-svc.c, each also
 *              compiled into its own driver image.
 *
 * Same file split as axl-webfs (src/app/main.c + cmd-*.c + the
 * dual-compiled service sources), which is this tool's template.
 */

#include <axl.h>

#include "9p-common.h"
#include "9p-mount-svc.h"
#include "9p-serve-svc.h"

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

/// @brief `9p status` - report which resident 9P services are loaded.
/// @return 0 always; this verb reports, it does not gate.
static int
verb_status(
    AxlArgs *a   ///< parsed `status` verb arguments (unused)
);

// ---------------------------------------------------------------------------
// The verb tree
// ---------------------------------------------------------------------------

static const AxlArgsNode verbs[] = {
    { .name = "ls",  .help = "List a directory on a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_ls_positional,
      .handler = axl9p_ls_handler },
    { .name = "get", .help = "Read a file from a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_get_positional,
      .handler = axl9p_get_handler },
    { .name = "put", .help = "Write a local file to a 9P server",
      .flags = axl9p_file_flags, .positionals = axl9p_put_positional,
      .handler = axl9p_put_handler },
    { .name = "serve",      .help = "Export a local subtree over 9P (resident)",
      .flags = axl9p_serve_flags, .positionals = axl9p_serve_positional,
      .handler = axl9p_serve_handler },
    { .name = "serve-stop", .help = "Unload the resident 9P server",
      .handler = axl9p_serve_stop_handler },
    { .name = "mount",      .help = "Mount a remote 9P export as fsN: (resident)",
      .flags = axl9p_mount_flags, .positionals = axl9p_mount_positional,
      .handler = axl9p_mount_handler },
    { .name = "umount",     .help = "Unmount the resident 9P volume",
      .handler = axl9p_umount_handler },
    { .name = "status",     .help = "Report the resident services",
      .handler = verb_status },
    { 0 }
};

// ---------------------------------------------------------------------------
// Implementations
// ---------------------------------------------------------------------------

/* axl9p_report_service's bool return is deliberately discarded: status
   prints, it does not branch. */
static int
verb_status(
    AxlArgs *a
)
{
    (void)a;
    axl9p_report_service("9p-serve", &serve9p_service);
    axl9p_report_service("9p-mount", &mount9p_service);
    return 0;
}

AXL_TOOL_MAIN(9p)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "9p",
        .help        = "9P2000.L client and server",
        .help_prolog =
            "Speaks 9P2000.L over TCP. The one-shot verbs (ls/get/put) open "
            "a session, act, and exit. Every verb that names a server "
            "(ls/get/put/mount) takes it as host[:port]; the port defaults "
            "to 564, and for `mount` - which also has --port - an inline "
            ":port wins. `serve` deploys the export as a resident "
            "driver: `--detach` starts it and returns to the shell "
            "immediately, so the export outlives the command and "
            "`serve-stop` is what ends it; without `--detach` the command "
            "supervises the driver in the foreground until Ctrl-C. `mount` "
            "is the mirror image: it deploys a resident driver that "
            "publishes a remote export as a UEFI fsN: volume - it always "
            "returns to the shell, printing the name it published, and "
            "`umount` is what ends it.",
        .help_epilog =
            "Examples:\n"
            "  9p ls 10.0.0.5 /\n"
            "  9p get 10.0.0.5:5640 /hello.txt fs0:\\hello.txt\n"
            "  9p put fs0:\\log.txt 10.0.0.5 /log.txt\n"
            "  9p serve fs0:\\pub --port 5640 --detach\n"
            "  9p mount 10.0.0.5:5640\n"
            "  9p mount 10.0.0.5 --port 5640\n"
            "  9p status\n"
            "  9p serve-stop\n"
            "  9p umount",
        .verbs       = verbs,
    });
}
