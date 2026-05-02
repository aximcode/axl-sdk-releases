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
AXL_LOG_DOMAIN("path");

#define MAX_COMPONENTS  64

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int
is_sep(char c)
{
    return c == '/' || c == '\\';
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
    if (len == 0) {
        /* Path starts with separator, e.g. "/foo" -> "/" */
        result = axl_malloc(2);
        if (result == NULL) {
            axl_warning("path allocation failed");
            return NULL;
        }
        result[0] = path[0];
        result[1] = '\0';
        return result;
    }

    result = axl_malloc(len + 1);
    if (result == NULL) {
        axl_warning("path allocation failed");
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
    AxlFileInfo fi;
    return path != NULL && axl_file_info(path, &fi) == 0 && !fi.is_dir;
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

    /* 2. Companion file beside the running binary. */
    char *companion = axl_path_companion(axl_app_argv0(), name);
    if (path_file_exists(companion)) {
        return companion;
    }
    axl_free(companion);

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

    total = dir_len + (need_sep ? 1 : 0) + name_len;
    result = axl_malloc(total + 1);
    if (result == NULL) {
        axl_warning("path allocation failed");
        return NULL;
    }

    pos = 0;
    for (i = 0; i < dir_len; i++) {
        result[pos++] = dir[i];
    }
    if (need_sep) {
        result[pos++] = '/';
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
    /* Component stack — pointers into the work buffer */
    const char *stack[MAX_COMPONENTS];
    size_t      stack_len[MAX_COMPONENTS];
    int         depth = 0;
    const char *src;
    char        work[512];
    size_t      wpos = 0;

    if (base == NULL || relative == NULL || out == NULL || size == 0) {
        return -1;
    }

    /*
     * Build a combined path in work[]. If relative is absolute,
     * use it alone; otherwise prepend base + "/".
     */
    if (is_sep(relative[0])) {
        /* Absolute relative — use directly */
        for (size_t i = 0; relative[i] != '\0' && wpos < sizeof(work) - 1; i++) {
            work[wpos++] = relative[i];
        }
    } else {
        /* Prepend base */
        for (size_t i = 0; base[i] != '\0' && wpos < sizeof(work) - 1; i++) {
            work[wpos++] = base[i];
        }
        if (wpos > 0 && !is_sep(work[wpos - 1]) && wpos < sizeof(work) - 1) {
            work[wpos++] = '/';
        }
        for (size_t i = 0; relative[i] != '\0' && wpos < sizeof(work) - 1; i++) {
            work[wpos++] = relative[i];
        }
    }
    work[wpos] = '\0';

    /*
     * Parse components: split on '/' and '\', resolve "." and "..".
     */
    src = work;

    /* Skip leading separator (remember we have an absolute path) */
    bool has_root = false;
    if (is_sep(*src)) {
        has_root = true;
        src++;
    }

    while (*src != '\0') {
        /* Skip consecutive separators */
        if (is_sep(*src)) {
            src++;
            continue;
        }

        /* Find end of component */
        const char *comp = src;
        while (*src != '\0' && !is_sep(*src)) {
            src++;
        }
        size_t clen = (size_t)(src - comp);
        if (*src != '\0') {
            src++;  /* skip separator */
        }

        if (clen == 1 && comp[0] == '.') {
            continue;  /* skip "." */
        }

        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (depth > 0) {
                depth--;
            } else if (has_root) {
                return -1;  /* underflow past root */
            }
            continue;
        }

        if (depth >= MAX_COMPONENTS) {
            return -1;  /* too many components */
        }
        stack[depth] = comp;
        stack_len[depth] = clen;
        depth++;
    }

    /*
     * Reconstruct the normalized path into out[].
     */
    size_t opos = 0;

    if (has_root) {
        if (opos >= size) {
            return -1;
        }
        out[opos++] = '/';
    }

    for (int i = 0; i < depth; i++) {
        if (i > 0) {
            if (opos >= size) {
                return -1;
            }
            out[opos++] = '/';
        }
        if (opos + stack_len[i] >= size) {
            return -1;
        }
        for (size_t j = 0; j < stack_len[i]; j++) {
            char ch = stack[i][j];
            out[opos++] = is_sep(ch) ? '/' : ch;
        }
    }

    /* Empty result (no root, all ".." consumed) */
    if (opos == 0) {
        if (size < 2) {
            return -1;
        }
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }

    if (opos >= size) {
        return -1;
    }
    out[opos] = '\0';
    return 0;
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
        return -1;
    }

    n = axl_snprintf(out, size, "%s:%s", volume, subpath);
    if (n < 0 || (size_t)n >= size) {
        return -1;
    }

    /* Convert forward slashes to backslashes */
    for (p = out; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }

    return 0;
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
        return -1;
    }

    wide = axl_utf8_to_ucs2(path);
    if (wide == NULL) {
        return -1;
    }

    rc = axl_backend_shell_chdir(wide);
    axl_free(wide);
    return rc;
}
