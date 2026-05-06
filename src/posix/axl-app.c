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
static char  *mImagePath;  /* UTF-8 of LoadedImage->FilePath, NULL if unknown */

// ---------------------------------------------------------------------------
// Internal: decode LoadedImage->FilePath device-path chain into a
// UTF-8 path string. UEFI image-load implementations split the path
// into one MEDIA_FILEPATH node per slash-separated component (or
// occasionally collapse to a single node containing the whole path);
// we walk the chain and concatenate, inserting a backslash separator
// between adjacent components. Used as the canonical source for
// "where the .efi was loaded from" by axl_app_image_path.
// argv[0] (mArgv[0]) reflects whatever the shell typed, which is
// often a basename — the device-path source is reliable regardless.
// ---------------------------------------------------------------------------

#define EFI_DP_TYPE_MEDIA      0x04
#define EFI_DP_SUBTYPE_FILE    0x04
#define EFI_DP_TYPE_END        0x7F
/* Defensive cap on device-path traversal — mirrors AXL_DP_MAX_NODES
   in src/util/axl-sys.c. UEFI device paths are typically <16 nodes;
   a malformed chain with header Length < 4 could otherwise loop
   forever (EFI_DP_NEXT advances by Length). */
#define MAX_DP_NODES           64u

static char *
_decode_image_filepath(EFI_DEVICE_PATH_PROTOCOL *dp)
{
    if (dp == NULL) {
        return NULL;
    }

    /* Pass 1: count UCS-2 chars across all FILEPATH nodes (excluding
       terminating NULs inside each node). The accumulated string also
       needs (n_nodes - 1) separators if we have to insert them. */
    size_t total_chars = 0;
    size_t n_nodes     = 0;
    EFI_DEVICE_PATH_PROTOCOL *node = dp;
    for (unsigned hops = 0;
         hops < MAX_DP_NODES && EFI_DP_TYPE(node) != EFI_DP_TYPE_END;
         hops++, node = EFI_DP_NEXT(node))
    {
        /* Bound malformed Length=0 nodes early — EFI_DP_NEXT advances
           by Length, so 0 would loop forever even within MAX_DP_NODES
           if the hop counter weren't there to backstop. */
        if (EFI_DP_LENGTH(node) < 4) {
            break;
        }
        if (EFI_DP_TYPE(node)    != EFI_DP_TYPE_MEDIA
            || EFI_DP_SUBTYPE(node) != EFI_DP_SUBTYPE_FILE)
        {
            continue;
        }
        size_t node_len = EFI_DP_LENGTH(node);
        if (node_len <= 4) {
            continue;
        }
        size_t node_chars = (node_len - 4) / 2;
        const unsigned short *p =
            (const unsigned short *)((const uint8_t *)node + 4);
        /* Strip trailing NUL if present. */
        while (node_chars > 0 && p[node_chars - 1] == 0) {
            node_chars--;
        }
        total_chars += node_chars;
        n_nodes++;
    }
    if (total_chars == 0) {
        return NULL;
    }

    size_t buf_chars = total_chars + n_nodes /* separators + NUL */;
    unsigned short *ucs2 = axl_malloc(buf_chars * sizeof(unsigned short));
    if (ucs2 == NULL) {
        return NULL;
    }
    size_t pos = 0;
    node = dp;
    for (unsigned hops = 0;
         hops < MAX_DP_NODES && EFI_DP_TYPE(node) != EFI_DP_TYPE_END;
         hops++, node = EFI_DP_NEXT(node))
    {
        if (EFI_DP_LENGTH(node) < 4) {
            break;
        }
        if (EFI_DP_TYPE(node)    != EFI_DP_TYPE_MEDIA
            || EFI_DP_SUBTYPE(node) != EFI_DP_SUBTYPE_FILE)
        {
            continue;
        }
        size_t node_len = EFI_DP_LENGTH(node);
        if (node_len <= 4) {
            continue;
        }
        size_t node_chars = (node_len - 4) / 2;
        const unsigned short *p =
            (const unsigned short *)((const uint8_t *)node + 4);
        while (node_chars > 0 && p[node_chars - 1] == 0) {
            node_chars--;
        }
        if (node_chars == 0) {
            continue;
        }
        /* Insert a separator between adjacent components if neither
           side already carries one. */
        if (pos > 0
            && ucs2[pos - 1] != '\\' && ucs2[pos - 1] != '/'
            && p[0] != '\\' && p[0] != '/')
        {
            ucs2[pos++] = '\\';
        }
        for (size_t i = 0; i < node_chars; i++) {
            ucs2[pos++] = p[i];
        }
    }
    ucs2[pos] = 0;

    char *utf8 = axl_ucs2_to_utf8(ucs2);
    axl_free(ucs2);
    return utf8;
}

/* Prepend the shell volume mapping (e.g. "fs0:") for @p device_handle
   to @p file_path. Without this, axl_app_image_path returns just the
   FILEPATH suffix (e.g. "\app.efi") whose dirname is "\\" — the shell
   resolves "\\<sidecar>" against its current directory, not against
   the volume the image was loaded from. Adding the volume prefix
   makes the lookup volume-absolute.

   Returns a new UTF-8 string on success (caller frees), or NULL if
   the EFI_SHELL_PROTOCOL is unavailable, the device handle has no
   device-path protocol installed, or no shell mapping covers the
   handle. The caller falls back to the prefix-less @p file_path
   in that case. */
static char *
_prepend_volume_mapping(void *device_handle, const char *file_path)
{
    if (device_handle == NULL || file_path == NULL) {
        return NULL;
    }

    /* EFI_SHELL_PROTOCOL is only present when running under the
       shell (startup.nsh, interactive Shell.efi, etc.). DXE drivers
       and BDS contexts skip this path — the caller's argv[0]
       fallback handles those. */
    EFI_SHELL_PROTOCOL *shell = NULL;
    EFI_GUID            shell_guid = gEfiShellProtocolGuid;
    if (EFI_ERROR(gBS->LocateProtocol(&shell_guid, NULL,
                                      (void **)&shell))
        || shell == NULL || shell->GetMapFromDevicePath == NULL)
    {
        return NULL;
    }

    /* Look up the device-path on the loaded-image's device handle.
       Required to feed GetMapFromDevicePath. */
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    EFI_GUID                  dp_guid = gEfiDevicePathProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)device_handle,
                                      &dp_guid, (void **)&dp))
        || dp == NULL)
    {
        return NULL;
    }

    /* GetMapFromDevicePath advances *dp past the matched volume
       portion. We don't care about the post-call position — pass a
       local copy so the LoadedImage's stored device-path stays
       unaffected. */
    EFI_DEVICE_PATH_PROTOCOL *dp_iter = dp;
    const unsigned short    *mapping = shell->GetMapFromDevicePath(&dp_iter);
    if (mapping == NULL) {
        return NULL;
    }

    char *map_utf8 = axl_ucs2_to_utf8(mapping);
    if (map_utf8 == NULL) {
        return NULL;
    }
    /* GetMapFromDevicePath returns ALL mapping names that resolve to
       this device handle, semicolon-separated (e.g.
       "FS0:;F0a65535a:;BLK0:"). The shell's OpenFileByName accepts
       any one of them as a volume prefix; trim to the first so the
       joined path is shaped like "FS0:\app.efi" rather than the
       multi-alias soup. The first mapping is always the friendliest
       (FSn: > BLKn: > device-path-derived alias). */
    for (char *p = map_utf8; *p != '\0'; p++) {
        if (*p == ';') {
            *p = '\0';
            break;
        }
    }
    /* Concatenate "<map>" + file_path. The mapping ends with ':'
       (e.g. "fs0:") and the FILEPATH typically starts with '\\',
       so the joined form "fs0:\\app.efi" is a valid shell-resolvable
       absolute path. */
    size_t map_len = axl_strlen(map_utf8);
    size_t fp_len  = axl_strlen(file_path);
    char  *joined  = axl_malloc(map_len + fp_len + 1);
    if (joined == NULL) {
        axl_free(map_utf8);
        return NULL;
    }
    axl_memcpy(joined,           map_utf8,  map_len);
    axl_memcpy(joined + map_len, file_path, fp_len);
    joined[map_len + fp_len] = '\0';
    axl_free(map_utf8);
    return joined;
}

// ---------------------------------------------------------------------------
// Internal: called by src/runtime/axl-runtime.c
// ---------------------------------------------------------------------------

/* Whitespace-split a UCS-2 command line into argv.
 * Handles double-quoted tokens (including spaces) and backslash-escaped
 * quotes. On success sets out_argc/out_argv (caller owns the array and
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
     * Shell 2.0-specific and not universally published (e.g. some OEM
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

    /* Capture the canonical image-load path from LoadedImage->FilePath
       — orthogonal to argv[0]. argv[0] is whatever the shell typed,
       which is often a basename (e.g. startup.nsh's
       'fs0:\app.efi' may surface as just 'app.efi' depending on the
       shell). The device-path source is reliable regardless of how
       the shell was invoked, which makes it the right anchor for
       sidecar discovery via axl_path_companion.

       Two stages: first the FILEPATH-derived suffix (e.g. "\app.efi"),
       then prepend the shell mapping for LoadedImage->DeviceHandle
       (e.g. "fs0:") so the resulting path is volume-absolute and
       resolves regardless of the shell's current directory. Without
       the prefix, the FILEPATH alone is root-relative — its dirname
       lookup lands on "\\" which the shell resolves against cwd, not
       against the volume the .efi was actually loaded from. This is
       why a `cd \` in startup.nsh used to be required for sidecar
       discovery; the prefix makes it unnecessary. */
    if (!EFI_ERROR(status) && li != NULL && li->FilePath != NULL) {
        mImagePath = _decode_image_filepath(
            (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
        if (mImagePath != NULL && li->DeviceHandle != NULL) {
            char *with_volume =
                _prepend_volume_mapping(li->DeviceHandle, mImagePath);
            if (with_volume != NULL) {
                axl_free(mImagePath);
                mImagePath = with_volume;
            }
            /* prepend-failure path leaves mImagePath at the FILEPATH
               suffix; better than nothing — the argv[0] fallback in
               axl_resolve_data_file still has a chance. */
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
    if (mImagePath != NULL) {
        axl_free(mImagePath);
        mImagePath = NULL;
    }
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

// ---------------------------------------------------------------------------
// Public — axl-app.h
// ---------------------------------------------------------------------------

const char *
axl_app_argv0(void)
{
    if (mArgc == 0 || mArgv == NULL) {
        return NULL;
    }
    return mArgv[0];
}

const char *
axl_app_image_path(void)
{
    return mImagePath;
}
