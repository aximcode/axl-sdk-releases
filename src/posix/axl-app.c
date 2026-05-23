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

char *
_axl_decode_image_filepath(EFI_DEVICE_PATH_PROTOCOL *dp)
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
char *
_axl_prepend_volume_mapping(void *device_handle, const char *file_path)
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
       (e.g. "fs0:"). FILEPATH device-path nodes usually carry the
       leading "\\" (giving "fs0:\app.efi" after the join), but when
       the shell loaded the image via a cwd-relative name (e.g.
       startup.nsh: `fs0:app.efi` rather than `fs0:\app.efi`) the
       firmware encodes the FILEPATH as a bare basename. In that
       case the unsweetened concatenation would produce
       "fs0:app.efi" — which the shell accepts as a load target but
       breaks `axl_path_get_dirname`, since there's no separator
       between volume and filename. That in turn breaks
       `axl_path_companion`-based sidecar discovery: dirname returns
       empty and the joined companion is a bare name with no
       volume anchor.

       Insert a `\` separator when neither side carries one. This
       only fires on the bare-basename case; absolute FILEPATHs are
       unchanged. */
    size_t map_len = axl_strlen(map_utf8);
    size_t fp_len  = axl_strlen(file_path);
    /* Insert a '\' between map and file_path unless one side
       already carries a separator. The colon at the end of the map
       (e.g. "fs0:") does NOT count as a separator for
       dirname/basename purposes — sidecar discovery's path math
       expects an explicit slash. */
    bool map_ends_sep = map_len > 0
        && (map_utf8[map_len - 1] == '\\' || map_utf8[map_len - 1] == '/');
    bool fp_starts_sep = fp_len > 0
        && (file_path[0] == '\\' || file_path[0] == '/');
    int  need_sep = (map_ends_sep || fp_starts_sep) ? 0 : 1;
    size_t total = map_len + (need_sep ? 1 : 0) + fp_len;
    char  *joined  = axl_malloc(total + 1);
    if (joined == NULL) {
        axl_free(map_utf8);
        return NULL;
    }
    size_t pos = 0;
    axl_memcpy(joined + pos, map_utf8, map_len);
    pos += map_len;
    if (need_sep) {
        joined[pos++] = '\\';
    }
    axl_memcpy(joined + pos, file_path, fp_len);
    pos += fp_len;
    joined[pos] = '\0';
    axl_free(map_utf8);
    return joined;
}

/* Walk LoadedImage->FilePath into a volume-prefixed UTF-8 path; if
   the handle has no FilePath, recurse into LoadedImage->ParentHandle.
   The parent walk is what makes buffer-loaded images (drivers
   embedded into a launcher via `axl_driver_ensure_with_embedded`'s
   step-4) inherit a sidecar-discovery anchor — the firmware sets
   `FilePath = NULL` on `LoadImage(SourceBuffer=...)`, but the
   ParentHandle still points back to the launcher whose own FilePath
   IS set.

   Depth-bounded at 8 levels to defend against pathological parent
   chains; real chains are typically depth 1 (shell → app) or 2
   (shell → launcher → embedded driver).

   Returns a heap-owned UTF-8 string, or NULL when no ancestor in
   the chain has a usable FilePath. */
static char *
_capture_image_path(EFI_HANDLE handle)
{
    if (handle == NULL) {
        return NULL;
    }

    EFI_HANDLE current = handle;
    for (int depth = 0; depth < 8 && current != NULL; depth++) {
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        EFI_GUID li_guid = gEfiLoadedImageProtocolGuid;
        EFI_STATUS st = gBS->HandleProtocol(current, &li_guid,
                                            (void **)&li);
        if (EFI_ERROR(st) || li == NULL) {
            return NULL;
        }
        if (li->FilePath != NULL) {
            char *decoded = _axl_decode_image_filepath(
                (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
            if (decoded == NULL) {
                return NULL;
            }
            if (li->DeviceHandle != NULL) {
                char *with_volume = _axl_prepend_volume_mapping(
                    li->DeviceHandle, decoded);
                if (with_volume != NULL) {
                    axl_free(decoded);
                    return with_volume;
                }
                /* Volume-mapping unavailable (no Shell protocol, BDS
                   context, etc.): fall back to the FILEPATH suffix.
                   It's cwd-relative but the caller's argv[0] path
                   still has a chance, and step-3 (bare-name-in-cwd)
                   in the sidecar resolver doesn't need a prefix. */
            }
            return decoded;
        }
        /* No FilePath on this image — walk up. The firmware sets
           ParentHandle to the image that called LoadImage; for an
           embedded driver that's the launcher. */
        current = (EFI_HANDLE)li->ParentHandle;
    }
    return NULL;
}

/* Public-internal: drivers reach this from `axl_driver_init` (their
   CRT path skips `_axl_args_init`). See header for semantics. */
void
_axl_init_image_path(void *image_handle)
{
    if (mImagePath != NULL) {
        return;
    }
    mImagePath = _capture_image_path((EFI_HANDLE)image_handle);
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

    /* Capture the canonical image-load path. Shared between apps
       (this path) and drivers (axl_driver_init); see _capture_image_path. */
    mImagePath = _capture_image_path((EFI_HANDLE)image_handle);

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

int
axl_app_boot_path(
    const char *relative_path,
    char       *out,
    size_t      out_size)
{
    if (relative_path == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    if (mImagePath == NULL) {
        return AXL_ERR;
    }

    /* Find the ':' marking the end of the volume label in
       mImagePath. The decoded form is "<vol>:<path>" where <vol>
       is "fs0" / "fs1" / ... — anything before the first ':' is
       the prefix we want to keep, plus the ':' itself. */
    const char *colon = axl_strchr(mImagePath, ':');
    if (colon == NULL) {
        /* Image path has no volume prefix — happens on network
           boot, RAM-disk-with-no-source-volume, etc. Refuse rather
           than silently producing a path that won't resolve. */
        return AXL_ERR;
    }
    size_t prefix_len = (size_t)(colon - mImagePath) + 1;  /* include ':' */

    /* Skip leading separators in relative_path so the joined path
       has exactly one backslash between prefix and tail.
       axl_fs accepts forward slashes too, but the canonical UEFI
       form is backslash; we emit that. */
    const char *rel = relative_path;
    while (*rel == '\\' || *rel == '/') {
        rel++;
    }

    /* out = <volume-prefix-with-colon> + '\\' + <rel> */
    int n = axl_snprintf(out, out_size, "%.*s\\%s",
                         (int)prefix_len, mImagePath, rel);
    if (n < 0 || (size_t)n >= out_size) {
        return AXL_ERR;
    }
    return AXL_OK;
}
