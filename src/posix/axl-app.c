/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-app.c
    POSIX shim: convert UEFI shell parameters into a C-style argc/argv.
    Called by the runtime module (src/runtime/axl-runtime.c) during
    _axl_init; the produced argv is released by _axl_args_free during
    _axl_cleanup.
**/

#include "../backend/axl-backend.h"
#include "axl-app-internal.h"
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("app");

// ---------------------------------------------------------------------------
// Saved args
// ---------------------------------------------------------------------------

static int    mArgc;
static char **mArgv;

// ---------------------------------------------------------------------------
// Internal: called by src/runtime/axl-runtime.c
// ---------------------------------------------------------------------------

void
_axl_args_init(void *image_handle)
{
    mArgc = 0;
    mArgv = NULL;

    EFI_SHELL_PARAMETERS_PROTOCOL  *params = NULL;
    EFI_GUID    params_guid = gEfiShellParametersProtocolGuid;
    EFI_STATUS  status;

    status = gBS->HandleProtocol(
        (EFI_HANDLE)image_handle, &params_guid, (void **)&params);

    if (!EFI_ERROR(status) && params != NULL && params->Argc > 0) {
        size_t            shell_argc = params->Argc;
        unsigned short  **shell_argv = (unsigned short **)params->Argv;

        mArgv = (char **)axl_calloc(shell_argc + 1, sizeof (char *));
        if (mArgv == NULL) {
            axl_error("argv alloc failed for %zu args", shell_argc);
        } else {
            for (size_t i = 0; i < shell_argc; i++) {
                mArgv[i] = axl_ucs2_to_utf8(shell_argv[i]);
                if (mArgv[i] == NULL) {
                    mArgv[i] = axl_strdup("");
                }
            }
            mArgc = (int)shell_argc;
            mArgv[mArgc] = NULL;
        }
    }

    /* Fallback: no shell params (e.g. driver invoked via BDS) -- give
     * the app a sane argc=1 / argv[0]="app" so it can start. */
    if (mArgv == NULL) {
        mArgv = (char **)axl_calloc(2, sizeof (char *));
        if (mArgv != NULL) {
            mArgv[0] = axl_strdup("app");
            mArgv[1] = NULL;
            mArgc = 1;
        }
    }
}

void
_axl_args_free(void)
{
    if (mArgv == NULL) {
        return;
    }
    for (int i = 0; i < mArgc; i++) {
        axl_free(mArgv[i]);
    }
    axl_free(mArgv);
    mArgv = NULL;
    mArgc = 0;
}

// ---------------------------------------------------------------------------
// Public (called by CRT0 via the _axl_get_args prototype in axl.h)
// ---------------------------------------------------------------------------

void
_axl_get_args(int *argc, char ***argv)
{
    if (argc != NULL) {
        *argc = mArgc;
    }
    if (argv != NULL) {
        *argv = mArgv;
    }
}
