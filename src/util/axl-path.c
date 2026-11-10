/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-path.c
    Path manipulation: basename, dirname, extension, join, resolve, cwd.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-path.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-app.h>
#include "axl-image-internal.h"   /* _axl_app_image_anchor */
AXL_LOG_DOMAIN("path");

/* Components axl_path_resolve keeps at once; more is AXL_ERR, never cut. */
#define MAX_COMPONENTS  256

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

/* axl_path_resolve's right-to-left walk: ".." still to cancel, and the
   components kept so far. With @c out set it also writes them, each
   ending at @c wpos. */
typedef struct {
    size_t  skip;    /* ".." not yet matched by a component to their left */
    size_t  depth;   /* components kept */
    size_t  bytes;   /* their bytes, separators not counted */
    char   *out;     /* NULL: measure only */
    size_t  wpos;    /* write end, when writing */
} PathWalk;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void path_walk(PathWalk *w, const char *src);

static int
is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* Walk @p src's components RIGHT TO LEFT into @p w: "." is dropped, ".."
   cancels the next component to its left, any other component is kept --
   counted and, when @p w->out is set, written ending at @p w->wpos with a
   '/' before it unless it is the leftmost kept one (@p w->depth counts
   down to it). Right to left, a ".." is resolved by what lies to its LEFT,
   exactly as the left-to-right stack resolved it, without holding any
   component: no stack array, no allocation. */
static void
path_walk(PathWalk *w, const char *src)
{
    size_t end = axl_strlen(src);
    while (end > 0) {
        if (is_sep(src[end - 1])) {
            end--;
            continue;
        }
        size_t start = end;
        while (start > 0 && !is_sep(src[start - 1])) {
            start--;
        }
        const char *comp = src + start;
        size_t      clen = end - start;
        end = start;

        if (clen == 1 && comp[0] == '.') {
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            w->skip++;
            continue;
        }
        if (w->skip > 0) {
            w->skip--;
            continue;
        }
        if (w->out == NULL) {
            w->depth++;
            w->bytes += clen;
        } else {
            w->wpos -= clen;
            axl_memcpy(w->out + w->wpos, comp, clen);
            if (--w->depth > 0) {
                w->out[--w->wpos] = '/';
            }
        }
    }
}

/**
 * Internal: find pointer to basename within path (non-allocating).
 * Used by axl_path_get_basename and axl_path_extension.
 */
static const char *
find_basename(const char *path)
{
    const char *last;
    const char *p;

    last = path;
    for (p = path; *p != '\0'; p++) {
        if (is_sep(*p)) {
            last = p + 1;
        }
    }

    return last;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

char *
axl_path_get_basename(const char *path)
{
    if (path == NULL) {
        return NULL;
    }

    return axl_strdup(find_basename(path));
}

char *
axl_path_get_dirname(const char *path)
{
    const char *p;
    const char *last_sep;
    size_t len;
    char *result;
    size_t i;

    if (path == NULL) {
        return NULL;
    }

    last_sep = NULL;
    for (p = path; *p != '\0'; p++) {
        if (is_sep(*p)) {
            last_sep = p;
        }
    }

    if (last_sep == NULL) {
        return axl_strdup(".");
    }

    len = (size_t)(last_sep - path);
    /* The separator IS the directory when nothing but a filesystem root
       precedes it: "/foo" -> "/", and "fs0:\foo" -> "fs0:\". A bare "fs0:"
       names the volume, not a directory UEFI can open, and a caller that
       concatenates rather than joins would build "fs0:name". */
    if (len == 0 || path[len - 1] == ':') {
        len++;
    }

    result = axl_malloc(len + 1);
    if (result == NULL) {
        axl_debug("path allocation failed");
        return NULL;
    }

    for (i = 0; i < len; i++) {
        result[i] = path[i];
    }
    result[len] = '\0';

    return result;
}

const char *
axl_path_extension(const char *path)
{
    const char *base;
    const char *dot;
    const char *p;

    if (path == NULL) {
        return NULL;
    }

    base = find_basename(path);
    dot = NULL;

    for (p = base; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    if (dot == NULL || dot == base) {
        return NULL;
    }

    return dot + 1;
}

char *
axl_path_companion(const char *anchor, const char *name)
{
    if (anchor == NULL || name == NULL) {
        return NULL;
    }
    char *dir = axl_path_get_dirname(anchor);
    if (dir == NULL) {
        return NULL;
    }
    char *result = axl_path_join(dir, name);
    axl_free(dir);
    return result;
}

/* Helper: returns true if @p path names an existing readable file. */
static bool
path_file_exists(const char *path)
{
    AxlFsEntry fi;
    return path != NULL && axl_file_info(path, &fi) == AXL_OK && !axl_fs_entry_is_dir(&fi);
}

int
axl_path_search(const char *search_list, const char *name, char **out_path)
{
    if (out_path != NULL) {
        *out_path = NULL;
    }
    if (search_list == NULL || name == NULL || out_path == NULL) {
        return AXL_ERR;
    }

    for (const char *seg = search_list; *seg != '\0'; ) {
        const char *end = seg;
        while (*end != '\0' && *end != ';') {
            end++;
        }
        size_t seg_len = (size_t)(end - seg);
        if (seg_len > 0) {   /* skip empty entries (e.g. leading/trailing ';') */
            char *dir = axl_malloc(seg_len + 1);
            if (dir == NULL) {
                return AXL_ERR;
            }
            axl_memcpy(dir, seg, seg_len);
            dir[seg_len] = '\0';

            char *full = axl_path_join(dir, name);
            axl_free(dir);
            if (full == NULL) {
                return AXL_ERR;
            }
            if (path_file_exists(full)) {
                *out_path = full;   /* first match wins */
                return AXL_OK;
            }
            axl_free(full);
        }
        seg = (*end == ';') ? end + 1 : end;
    }
    return AXL_NOT_FOUND;
}

char *
axl_resolve_data_file(const char *override_path, const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    /* 1. Explicit override wins. */
    if (path_file_exists(override_path)) {
        return axl_strdup(override_path);
    }

    /* 2. Companion file beside the running binary. Prefer the image's
       canonical FILEPATH (from EFI_LOADED_IMAGE_PROTOCOL) over argv[0]:
       argv[0] is whatever the shell typed and may be a bare basename
       (e.g. startup.nsh's `app.efi` without a `cd \` prefix), in which
       case axl_path_companion gives a cwd-relative ./<name> that won't
       resolve. The image-path source reflects where UEFI actually
       loaded the binary from, so it works regardless of shell cwd or
       how the script wrote the invocation. Fall through to argv[0]
       when no image path is available (synthetic load contexts). The
       image-side value is the ANCHOR, not axl_app_image_path: a
       buffer-loaded driver image has no file of its own (the public
       accessor correctly reports NULL) but its data files live beside the
       launcher that loaded it, which is what the anchor names. */
    const char *anchors[2] = { _axl_app_image_anchor(), axl_app_argv0() };
    for (int i = 0; i < 2; i++) {
        if (anchors[i] == NULL) {
            continue;
        }
        char *companion = axl_path_companion(anchors[i], name);
        if (path_file_exists(companion)) {
            return companion;
        }
        axl_free(companion);
    }

    /* 3. Bare name (current working directory). */
    if (path_file_exists(name)) {
        return axl_strdup(name);
    }

    return NULL;
}

char *
axl_path_join(const char *dir, const char *name)
{
    size_t dir_len;
    size_t name_len;
    int need_sep;
    size_t total;
    char *result;
    size_t pos;
    size_t i;

    if (dir == NULL || name == NULL) {
        return NULL;
    }

    dir_len = axl_strlen(dir);
    name_len = axl_strlen(name);
    need_sep = (dir_len > 0 && !is_sep(dir[dir_len - 1]));

    /* Pick the separator to insert based on the dir's existing style:
       if it carries a backslash (UEFI / Windows convention) or a
       drive/volume colon (e.g. "fs0:"), keep the path Windows-style
       with `\\` so shell OpenFileByName accepts it (UEFI shell
       silently rejects "fs0:/foo"). Otherwise default to POSIX `/`. */
    char sep = '/';
    for (i = 0; i < dir_len; i++) {
        if (dir[i] == '\\' || dir[i] == ':') {
            sep = '\\';
            break;
        }
    }

    total = dir_len + (need_sep ? 1 : 0) + name_len;
    result = axl_malloc(total + 1);
    if (result == NULL) {
        axl_debug("path allocation failed");
        return NULL;
    }

    pos = 0;
    for (i = 0; i < dir_len; i++) {
        result[pos++] = dir[i];
    }
    if (need_sep) {
        result[pos++] = sep;
    }
    for (i = 0; i < name_len; i++) {
        result[pos++] = name[i];
    }
    result[pos] = '\0';

    return result;
}

// ---------------------------------------------------------------------------
// axl_path_resolve
// ---------------------------------------------------------------------------

int
axl_path_resolve(
    const char *base,
    const char *relative,
    char       *out,
    size_t      size)
{
    if (base == NULL || relative == NULL || out == NULL || size == 0) {
        return AXL_ERR;
    }
    /* Two passes right to left over relative, then (unless relative is
       absolute) base: one measures, one writes into @p out sized exactly.
       Components come straight from the inputs, so a long path resolves
       whole or fails -- never cut -- and nothing is allocated: this runs
       inside firmware callbacks (a provider's Open and SetInfo), where
       neither a large stack frame nor an out-of-memory failure is welcome. */
    bool absolute = is_sep(relative[0]);
    bool has_root = absolute || is_sep(base[0]);

    PathWalk w = { 0 };
    path_walk(&w, relative);
    if (!absolute) {
        path_walk(&w, base);
    }
    if (w.skip > 0 && has_root) {
        return AXL_ERR;   /* ".." above the root */
    }
    if (w.depth > MAX_COMPONENTS) {
        return AXL_ERR;
    }
    if (w.depth == 0) {
        /* The root itself, or (no root, everything cancelled) ".". */
        if (size < 2) {
            return AXL_ERR;
        }
        out[0] = has_root ? '/' : '.';
        out[1] = '\0';
        return AXL_OK;
    }
    size_t len = (has_root ? 1u : 0u) + w.bytes + (w.depth - 1u);
    if (len + 1u > size) {
        return AXL_ERR;   /* does not fit whole */
    }
    w.skip = 0;
    w.out  = out;
    w.wpos = len;
    path_walk(&w, relative);
    if (!absolute) {
        path_walk(&w, base);
    }
    if (has_root) {
        out[0] = '/';
    }
    out[len] = '\0';
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_path_build_uefi
// ---------------------------------------------------------------------------

int
axl_path_build_uefi(
    const char *volume,
    const char *subpath,
    char       *out,
    size_t      size)
{
    int   n;
    char *p;

    if (volume == NULL || subpath == NULL || out == NULL || size == 0) {
        return AXL_ERR;
    }

    n = axl_snprintf(out, size, "%s:%s", volume, subpath);
    if (n < 0 || (size_t)n >= size) {
        return AXL_ERR;
    }

    /* Convert forward slashes to backslashes */
    for (p = out; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Working directory
// ---------------------------------------------------------------------------

char *
axl_get_current_dir(void)
{
    const unsigned short *wide;

    wide = axl_backend_shell_getcwd();
    if (wide == NULL) {
        return NULL;
    }

    return axl_ucs2_to_utf8(wide);
}

int
axl_chdir(const char *path)
{
    unsigned short *wide;
    int rc;

    if (path == NULL) {
        return AXL_ERR;
    }

    wide = axl_utf8_to_ucs2(path);
    if (wide == NULL) {
        return AXL_ERR;
    }

    rc = axl_backend_shell_chdir(wide);
    axl_free(wide);
    return rc;
}
