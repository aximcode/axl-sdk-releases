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

/* Whitespace-split a UCS-2 command line into argv.
 * Handles double-quoted tokens (including spaces) and backslash-escaped
 * quotes. On success sets *out_argc/*out_argv (caller owns the array and
 * each string); on failure returns -1 and leaves them untouched.
 *
 * UEFI shells pass LoadOptions as the full command line including the
 * program name at argv[0], so this matches the EFI_SHELL_PARAMETERS_PROTOCOL
 * convention — no renumbering needed. */
static int
_tokenize_load_options(
    const unsigned short *src,
    size_t                src_words,
    int                  *out_argc,
    char               ***out_argv)
{
    if (src == NULL || src_words == 0) { return -1; }

    /* Count tokens with a two-pass scan. */
    int    n_tokens = 0;
    bool   in_quote = false;
    bool   in_token = false;
    for (size_t i = 0; i < src_words && src[i] != 0; i++) {
        unsigned short c = src[i];
        if (!in_quote && (c == ' ' || c == '\t')) {
            if (in_token) { in_token = false; }
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            if (!in_token) { in_token = true; n_tokens++; }
            continue;
        }
        if (!in_token) { in_token = true; n_tokens++; }
    }
    if (n_tokens == 0) { return -1; }

    char **argv = (char **)axl_calloc(n_tokens + 1, sizeof (char *));
    if (argv == NULL) { return -1; }

    /* Second pass: extract each token into a UCS-2 scratch buffer, then
     * convert to UTF-8. Scratch holds one token; tokens are bounded by
     * src_words so this is safe. */
    unsigned short *scratch = (unsigned short *)axl_calloc(
        src_words + 1, sizeof (unsigned short));
    if (scratch == NULL) { axl_free(argv); return -1; }

    int    argv_i = 0;
    size_t w = 0;
    in_quote = false;
    in_token = false;
    for (size_t i = 0; i < src_words && src[i] != 0; i++) {
        unsigned short c = src[i];
        if (!in_quote && (c == ' ' || c == '\t')) {
            if (in_token) {
                scratch[w] = 0;
                argv[argv_i++] = axl_ucs2_to_utf8(scratch);
                if (argv[argv_i - 1] == NULL) {
                    argv[argv_i - 1] = axl_strdup("");
                }
                w = 0;
                in_token = false;
            }
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            if (!in_token) { in_token = true; }
            continue;  /* don't include the quote itself */
        }
        if (!in_token) { in_token = true; }
        scratch[w++] = c;
    }
    if (in_token) {
        scratch[w] = 0;
        argv[argv_i++] = axl_ucs2_to_utf8(scratch);
        if (argv[argv_i - 1] == NULL) {
            argv[argv_i - 1] = axl_strdup("");
        }
    }
    axl_free(scratch);

    argv[argv_i] = NULL;
    *out_argc = argv_i;
    *out_argv = argv;
    return 0;
}

void
_axl_args_init(void *image_handle)
{
    mArgc = 0;
    mArgv = NULL;

    /* UEFI spec 9.1: every loaded image receives an EFI_LOADED_IMAGE_PROTOCOL
     * with LoadOptions set to whatever the loader passed in. For shell-
     * invoked apps this is the raw command-line string the user typed
     * (as UCS-2). We parse it ourselves instead of relying on
     * EFI_SHELL_PARAMETERS_PROTOCOL — the shell-params protocol is
     * Shell 2.0-specific and not universally published (e.g. Dell's
     * firmware shell doesn't install it for cross-volume invocations,
     * and it's entirely absent in BDS/bootloader contexts). Parsing
     * LoadOptions is one code path that works everywhere. */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_GUID li_guid = gEfiLoadedImageProtocolGuid;
    EFI_STATUS status = gBS->HandleProtocol(
        (EFI_HANDLE)image_handle, &li_guid, (void **)&li);

    if (!EFI_ERROR(status) && li != NULL
        && li->LoadOptions != NULL
        && li->LoadOptionsSize >= sizeof (unsigned short))
    {
        const unsigned short *opts = (const unsigned short *)li->LoadOptions;
        size_t opts_words = li->LoadOptionsSize / sizeof (unsigned short);
        int    argc_out = 0;
        char **argv_out = NULL;
        if (_tokenize_load_options(opts, opts_words, &argc_out, &argv_out) == 0) {
            mArgc = argc_out;
            mArgv = argv_out;
        }
    }

    /* Fallback: no LoadOptions at all (e.g. a DXE driver invoked via BDS
     * with no arg payload). Give the app a sane argc=1 / argv[0]="app". */
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
