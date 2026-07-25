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
#include <axl/axl-string.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("app");

/* Longest volume name we join a path onto ("FS0", "fs12", a SetMap alias),
   NUL included. axl_backend_volume_name_for_handle fails rather than truncate,
   so an over-long alias degrades to the prefix-less path, not a wrong one. */
#define AXL_APP_VOLUME_NAME_MAX  64u

// ---------------------------------------------------------------------------
// Saved args
// ---------------------------------------------------------------------------

static int    mArgc;
static char **mArgv;
static char  *mImagePath;   /* UTF-8 path THIS image was loaded from; NULL for a
                             * synthetic load context (buffer load) that has no
                             * such file -- see axl_app_image_path's contract. */
static void  *mImageDeviceHandle; /* The volume THIS image was loaded from
                             * (EFI_HANDLE), or NULL for a synthetic load
                             * context. Re-resolved to a name per call by
                             * axl_app_boot_path -- see _axl_init_image_path. */
static char  *mImageAnchor; /* Nearest ancestor image (this one, else up the
                             * ParentHandle chain) that WAS loaded from a file.
                             * The directory anchor for sidecar discovery; NOT
                             * a claim about where THIS image came from. */

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

/* Prepend the volume name (e.g. "FS0:") for @p device_handle to @p file_path.
   Without this, axl_app_image_path returns just the FILEPATH suffix (e.g.
   "\app.efi") whose dirname is "\\" — the shell resolves "\\<sidecar>" against
   its current directory, not against the volume the image was loaded from.
   Adding the volume prefix makes the lookup volume-absolute.

   The name comes from axl_backend_volume_name_for_handle, so all three
   contexts are covered by one call: the modern shell's own alias, the old EFI
   1.x shell's map, and — for a BDS boot option, where no shell exists at all —
   the positional fs<n> that the backend's shell-free file path resolves.

   Returns a new UTF-8 string on success (caller frees), or NULL when the
   handle names no filesystem volume. The caller falls back to the prefix-less
   @p file_path in that case. */
char *
_axl_prepend_volume_mapping(void *device_handle, const char *file_path)
{
    if (device_handle == NULL || file_path == NULL) {
        return NULL;
    }

    char vol[AXL_APP_VOLUME_NAME_MAX];
    if (axl_backend_volume_name_for_handle(device_handle, vol, sizeof(vol))
        != AXL_OK)
    {
        return NULL;
    }

    /* Concatenate "<vol>:" + file_path. FILEPATH device-path nodes usually
       carry the leading "\\" (giving "FS0:\app.efi" after the join), but when
       the shell loaded the image via a cwd-relative name (e.g. startup.nsh:
       `fs0:app.efi` rather than `fs0:\app.efi`) the firmware encodes the
       FILEPATH as a bare basename. In that case the unsweetened concatenation
       would produce "FS0:app.efi" — which the shell accepts as a load target
       but breaks `axl_path_get_dirname`, since there's no separator between
       volume and filename. That in turn breaks `axl_path_companion`-based
       sidecar discovery: dirname returns empty and the joined companion is a
       bare name with no volume anchor.

       Insert a `\` separator when the tail doesn't carry one. The colon
       ending the volume does NOT count as a separator for dirname/basename
       purposes — sidecar discovery's path math expects an explicit slash. */
    bool fp_starts_sep = (file_path[0] == '\\' || file_path[0] == '/');
    return axl_asprintf("%s:%s%s", vol, fp_starts_sep ? "" : "\\", file_path);
}

/* Decode ONE image's FilePath into a volume-prefixed UTF-8 path, but only
   when that path actually names a file the image was loaded from.

   The gate is `DeviceHandle != NULL`. A firmware file load always records the
   volume the file came from; a synthetic load context does not. AXL itself
   creates such a context: `axl_driver_load_buffer` /
   `axl_driver_ensure_with_embedded`'s embedded step call
   `LoadImage(SourceBuffer=...)`, which leaves FilePath NULL, and AXL then
   SYNTHESIZES a `MemoryMapped(...)/FilePath("\\name")` device path so the
   aarch64 shell can render the handle (see driver_apply_image_identity).
   Decoding that synthesized node yields a volume-less `"\\name"` that names
   no real file -- and, when the caller passed no name, borrows the
   LAUNCHER's basename, so it names an entirely unrelated file. There is no
   volume to resolve it against, which is exactly what DeviceHandle == NULL
   is telling us.

   Returns a heap-owned UTF-8 string, or NULL when this image has no file. */
static char *
_decode_own_image_path(EFI_LOADED_IMAGE_PROTOCOL *li)
{
    if (li == NULL || li->FilePath == NULL || li->DeviceHandle == NULL) {
        return NULL;
    }
    char *decoded = _axl_decode_image_filepath(
        (EFI_DEVICE_PATH_PROTOCOL *)li->FilePath);
    if (decoded == NULL) {
        return NULL;
    }
    char *with_volume = _axl_prepend_volume_mapping(li->DeviceHandle, decoded);
    if (with_volume != NULL) {
        axl_free(decoded);
        return with_volume;
    }
    /* Volume-mapping unavailable (no Shell protocol, BDS context, etc.):
       fall back to the FILEPATH suffix. It's cwd-relative but the caller's
       argv[0] path still has a chance, and step-3 (bare-name-in-cwd) in the
       sidecar resolver doesn't need a prefix. The image still genuinely came
       from that file -- only the volume label is missing. */
    return decoded;
}

/* Walk up LoadedImage->ParentHandle until an image that WAS loaded from a
   file, and return that file's path. This is the anchor for sidecar
   discovery, not a claim about the current image: a buffer-loaded driver
   embedded into a launcher inherits the LAUNCHER's directory, which is
   where its data files actually live.

   Depth-bounded at 8 levels to defend against pathological parent
   chains; real chains are typically depth 1 (shell -> app) or 2
   (shell -> launcher -> embedded driver).

   Returns a heap-owned UTF-8 string, or NULL when no image in the chain has
   a file of its own. */
static EFI_LOADED_IMAGE_PROTOCOL *
_loaded_image_for(EFI_HANDLE handle)
{
    if (handle == NULL) {
        return NULL;
    }
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_GUID li_guid = gEfiLoadedImageProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol(handle, &li_guid, (void **)&li))) {
        return NULL;
    }
    return li;
}

static char *
_capture_image_anchor(EFI_HANDLE handle)
{
    EFI_HANDLE current = handle;
    for (int depth = 0; depth < 8 && current != NULL; depth++) {
        EFI_LOADED_IMAGE_PROTOCOL *li = _loaded_image_for(current);
        if (li == NULL) {
            return NULL;
        }
        char *own = _decode_own_image_path(li);
        if (own != NULL) {
            return own;
        }
        /* The firmware sets ParentHandle to the image that called LoadImage;
           for an embedded driver that's the launcher. */
        current = (EFI_HANDLE)li->ParentHandle;
    }
    return NULL;
}

/* Public-internal: drivers reach this from `axl_driver_init` (their
   CRT path skips `_axl_args_init`). See header for semantics. */
void
_axl_init_image_path(void *image_handle)
{
    if (mImageAnchor != NULL) {
        return;
    }

    /* Depth 0 answers "what file is THIS image", which is the public
       accessor's contract. The anchor question is a superset -- it accepts
       an ancestor's file -- so it reuses the same decode via the walk. */
    EFI_LOADED_IMAGE_PROTOCOL *li = _loaded_image_for((EFI_HANDLE)image_handle);
    mImagePath   = _decode_own_image_path(li);
    mImageAnchor = (mImagePath != NULL)
                   ? mImagePath
                   : _capture_image_anchor((EFI_HANDLE)image_handle);

    /* Kept so axl_app_boot_path can re-derive the volume name per call rather
       than slicing the prefix off the string captured here. The name is not
       fixed for the life of the image: a `map -r` re-letters a shell's map,
       and with no shell a volume that goes away (or a disconnect/reconnect
       cycle) renumbers the positional fs<n> of everything after it. Only the
       handle is stable, so that is what we keep. */
    mImageDeviceHandle = (li != NULL) ? li->DeviceHandle : NULL;
}

/* Public-internal: the sidecar-discovery anchor. See axl-image-internal.h. */
const char *
_axl_app_image_anchor(void)
{
    return mImageAnchor;
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

    /* Capture the canonical image-load path + the sidecar anchor. Shared
       between apps (this path) and drivers (axl_driver_init). */
    _axl_init_image_path(image_handle);

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
    /* mImageAnchor either IS mImagePath or is a separate allocation; free
       each exactly once. */
    if (mImageAnchor != NULL && mImageAnchor != mImagePath) {
        axl_free(mImageAnchor);
    }
    mImageAnchor = NULL;
    if (mImagePath != NULL) {
        axl_free(mImagePath);
        mImagePath = NULL;
    }
    /* Firmware-owned, nothing to free — but it must not outlive the capture
       it belongs to, or a re-init would keep the stale handle. */
    mImageDeviceHandle = NULL;
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
        /* No file this image came from, so no volume to hang a sibling off.
           Synthetic (buffer) load contexts land here — see
           axl_app_image_path's contract. */
        return AXL_ERR;
    }

    /* Re-resolve the volume name from the handle rather than slicing the
       prefix off mImagePath: the name a volume answers to can change during
       the run (a `map -r`; a volume removed ahead of this one in the
       no-shell positional order), and this call is the one that promises a
       path that resolves NOW. */
    char vol[AXL_APP_VOLUME_NAME_MAX];
    if (axl_backend_volume_name_for_handle(mImageDeviceHandle,
                                           vol, sizeof(vol)) != AXL_OK)
    {
        /* Nothing names this volume — network boot, a RAM disk with no
           source volume. Refuse rather than silently producing a path that
           won't resolve. */
        return AXL_ERR;
    }

    /* Skip leading separators in relative_path so the joined path
       has exactly one backslash between prefix and tail.
       axl_fs accepts forward slashes too, but the canonical UEFI
       form is backslash; we emit that. */
    const char *rel = relative_path;
    while (*rel == '\\' || *rel == '/') {
        rel++;
    }

    /* out = <volume-name> + ':' + '\\' + <rel> */
    int n = axl_snprintf(out, out_size, "%s:\\%s", vol, rel);
    if (n < 0 || (size_t)n >= out_size) {
        return AXL_ERR;
    }
    return AXL_OK;
}
