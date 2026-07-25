/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-serve-fs.c
    Generic axl-fs-backed WebDAV ops — the glue between
    axl_http_server_add_webdav and <axl/axl-fs.h>, so a consumer serving
    a mounted volume doesn't hand-write the ~13 AxlWebDavOps callbacks.

    Each callback maps a relative WebDAV path onto a path within the
    AxlFsRoot's base directory (traversal-contained) and calls the
    matching axl-fs primitive. Streaming read uses AxlFileView; streaming
    write uses AxlFileWriter; COPY streams read->write (recursive for
    deep collection copies).
**/

#include "axl-http-server-internal.h"
#include <axl/axl-fs.h>
#include <axl/axl-file-view.h>
#include <axl/axl-path.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("serve-fs");

#define FS_PATH_MAX        512
#define FS_COPY_BUF        4096
#define FS_COPY_MAX_DEPTH  64      /* recursion bound for deep COPY */
#define FS_READ_FRAMES     8       /* AxlFileView LRU frames for GET */

struct AxlFsRoot {
    char     root[FS_PATH_MAX];   /* base path, trailing separator stripped */
    uint32_t flags;
};

// ---------------------------------------------------------------------------
// Path mapping — relative WebDAV path -> contained filesystem path
// ---------------------------------------------------------------------------

/* Resolve a WebDAV request path (rooted at "/") to an absolute fs path
   within root->root. Returns AXL_ERR on any ".." that would escape the
   mount (or on truncation), so the server can never touch a file
   outside root->root.

   The containment trick: normalize @p rel against "/" FIRST — since rel
   is itself rooted at "/", axl_path_resolve treats it as absolute,
   resolves "." / "..", and returns AXL_ERR if a ".." underflows past
   "/". The normalized result therefore stays within "/", and prepending
   root->root keeps it within root->root. (Resolving rel directly
   against root->root would NOT contain it: axl_path_resolve ignores the
   base for an absolute relative arg.) */
static int
fs_map(
    const AxlFsRoot *root,
    const char      *rel,
    char            *out,
    size_t           size
    )
{
    char   norm[FS_PATH_MAX];
    size_t n = 0;

    if (root == NULL || rel == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (axl_path_resolve("/", rel, norm, sizeof(norm)) != AXL_OK) {
        return AXL_ERR;   /* ".." escape, or too long */
    }

    /* out = root + norm, norm separators ('/') flipped to UEFI '\'. The
       leading "/" of norm becomes the separator joining root and the
       first component. norm == "/" (the mount root) appends nothing. */
    for (const char *p = root->root; *p != '\0'; p++) {
        if (n + 1 >= size) {
            return AXL_ERR;
        }
        out[n++] = *p;
    }
    if (!(norm[0] == '/' && norm[1] == '\0')) {
        for (const char *p = norm; *p != '\0'; p++) {
            if (n + 1 >= size) {
                return AXL_ERR;
            }
            out[n++] = (*p == '/') ? '\\' : *p;
        }
    }
    out[n] = '\0';
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// MIME type by extension (compact; mirrors the set_static map)
// ---------------------------------------------------------------------------

static const char *
fs_mime(
    const char *path
    )
{
    const char *ext = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            ext = p;
        }
    }
    if (ext == NULL) {
        return "application/octet-stream";
    }
    if (axl_strcmp(ext, ".html") == 0 || axl_strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (axl_strcmp(ext, ".css") == 0)  { return "text/css"; }
    if (axl_strcmp(ext, ".js") == 0)   { return "application/javascript"; }
    if (axl_strcmp(ext, ".json") == 0) { return "application/json"; }
    if (axl_strcmp(ext, ".txt") == 0)  { return "text/plain"; }
    if (axl_strcmp(ext, ".xml") == 0)  { return "application/xml"; }
    if (axl_strcmp(ext, ".png") == 0)  { return "image/png"; }
    if (axl_strcmp(ext, ".jpg") == 0 || axl_strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (axl_strcmp(ext, ".gif") == 0)  { return "image/gif"; }
    if (axl_strcmp(ext, ".svg") == 0)  { return "image/svg+xml"; }
    if (axl_strcmp(ext, ".ico") == 0)  { return "image/x-icon"; }
    if (axl_strcmp(ext, ".pdf") == 0)  { return "application/pdf"; }
    if (axl_strcmp(ext, ".iso") == 0)  { return "application/octet-stream"; }
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// AxlWebDavOps callbacks
// ---------------------------------------------------------------------------

static int
fs_list_dir(
    void *user, const char *path, AxlFsEntry *out, size_t max, size_t *count
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if (out == NULL || count == NULL) {
        return AXL_ERR;
    }
    *count = 0;
    if (fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }

    AxlDir *d = axl_dir_open(fspath);
    if (d == NULL) {
        return AXL_ERR;
    }
    size_t     n = 0;
    AxlFsEntry e;
    while (n < max && axl_dir_read(d, &e)) {
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        out[n++] = e;
    }
    axl_dir_close(d);
    *count = n;
    return AXL_OK;
}

static int
fs_stat(
    void *user, const char *path, AxlFsEntry *out
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if (out == NULL) {
        return AXL_ERR;
    }
    /* The mount root ("/") is the served directory itself; some firmware
       won't stat a bare volume path, so synthesize the directory entry. */
    if (path != NULL && path[0] == '/' && path[1] == '\0') {
        axl_memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(*out);
        out->attributes  = AXL_FS_ATTR_DIRECTORY;
        return AXL_OK;
    }
    if (fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_file_info(fspath, out);
}

// --- streaming read (GET) ---

typedef struct {
    AxlFileView *view;
    size_t       offset;
} FsReadCtx;

static int
fs_read_open(
    void *user, const char *path, uint64_t offset, void **out_ctx
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if (out_ctx == NULL || fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }
    AxlFileView *v = axl_file_view_open(fspath, 0, FS_READ_FRAMES);
    if (v == NULL) {
        return AXL_ERR;
    }
    /* PIN the view for the life of the response. The Content-Length (and
       any Range bounds) went out on the wire from the stat that preceded
       this open; a view that grew or shrank mid-body would make the
       framing a lie -- too few bytes hangs the client, too many corrupt
       the next response on a keep-alive connection. HTTP's freshness unit
       is the response, and this open is where it is taken. */
    axl_file_view_set_pinned(v, true);
    FsReadCtx *c = axl_calloc(1, sizeof(*c));
    if (c == NULL) {
        axl_file_view_close(v);
        return AXL_ERR;
    }
    c->view   = v;
    c->offset = (size_t)offset;
    *out_ctx  = c;
    return AXL_OK;
}

static int
fs_read_chunk(
    void *ctx, void *buf, size_t buf_size, size_t *bytes_read
    )
{
    FsReadCtx *c = (FsReadCtx *)ctx;
    if (c == NULL || bytes_read == NULL) {
        return AXL_ERR;
    }
    size_t n = axl_file_view_read(c->view, c->offset, buf, buf_size);
    c->offset  += n;
    *bytes_read = n;   /* n == 0 signals EOF to the streamer */
    return AXL_OK;
}

static void
fs_read_close(
    void *ctx
    )
{
    FsReadCtx *c = (FsReadCtx *)ctx;
    if (c == NULL) {
        return;
    }
    axl_file_view_close(c->view);
    axl_free(c);
}

// --- streaming write (PUT) ---

static int
fs_write_open(
    void *user, const char *path, void **out_ctx
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if (out_ctx == NULL || (r->flags & AXL_SERVE_FS_READONLY) != 0u) {
        return AXL_ERR;
    }
    if (fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t wflags = 0;
    if ((r->flags & AXL_SERVE_FS_NO_OVERWRITE) != 0u) {
        wflags |= AXL_FILE_WRITER_EXCL;   /* refuse to replace an existing file */
    }
    AxlFileWriter *w = axl_file_writer_open(fspath, wflags);
    if (w == NULL) {
        return AXL_ERR;
    }
    *out_ctx = w;
    return AXL_OK;
}

static int
fs_write_chunk(
    void *ctx, const void *data, size_t len
    )
{
    return axl_file_writer_write((AxlFileWriter *)ctx, data, len);
}

static int
fs_write_close(
    void *ctx, bool aborted
    )
{
    /* On a clean EOF or a mid-upload abort we close the same way; an
       aborted PUT leaves the partial file (the next PUT truncates it), so a
       non-OK close on the abort path is expected — and the caller ignores
       our return there. On a clean EOF the status is load-bearing: it is
       the only signal that the bytes did NOT reach the volume, and the SDK
       turns it into 500 rather than 201. */
    int close_rc = axl_file_writer_close((AxlFileWriter *)ctx);
    if (!aborted && close_rc != AXL_OK) {
        axl_warning("upload: final flush/close failed - stored file is incomplete");
    }
    return close_rc;
}

// --- lifecycle (MKCOL / DELETE / MOVE / COPY) ---

static int
fs_mkdir(
    void *user, const char *path
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if ((r->flags & AXL_SERVE_FS_READONLY) != 0u
        || fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_dir_mkdir(fspath);
}

static int
fs_remove(
    void *user, const char *path
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       fspath[FS_PATH_MAX];

    if ((r->flags & (AXL_SERVE_FS_READONLY | AXL_SERVE_FS_NO_DELETE)) != 0u
        || fs_map(r, path, fspath, sizeof(fspath)) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_file_is_dir(fspath)) {
        return axl_dir_rmdir(fspath);
    }
    return axl_file_delete(fspath);
}

static int
fs_move(
    void *user, const char *src, const char *dst, bool overwrite
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       s[FS_PATH_MAX];
    char       d[FS_PATH_MAX];

    if ((r->flags & AXL_SERVE_FS_READONLY) != 0u
        || fs_map(r, src, s, sizeof(s)) != AXL_OK
        || fs_map(r, dst, d, sizeof(d)) != AXL_OK) {
        return AXL_ERR;
    }
    /* Refuse to clobber an existing destination when the client asked
       not to overwrite OR the mount forbids it — axl_file_move replaces
       unconditionally, so the check has to live here. */
    if (!overwrite || (r->flags & AXL_SERVE_FS_NO_OVERWRITE) != 0u) {
        AxlFsEntry e;
        if (axl_file_info(d, &e) == AXL_OK) {
            return AXL_ERR;
        }
    }
    return axl_file_move(s, d);
}

/* Stream-copy one file, src/dst already mapped fs paths. */
static int
fs_copy_file(
    const char *srcfs, const char *dstfs
    )
{
    AxlFileView *v = axl_file_view_open(srcfs, 0, FS_READ_FRAMES);
    if (v == NULL) {
        return AXL_ERR;
    }
    /* PIN the source. The loop below is written against ONE length taken
       up front, so a source that moved mid-copy would produce a hybrid
       file either way -- the pin makes that contract explicit instead of
       implicit. It also keeps the copy off a pathological path: writing
       the destination is itself a write, and a destination whose name
       keys to the same generation slot as the source (a COPY that keeps
       the filename, which is most of them) would otherwise have the
       source view re-stat and re-open once per buffer. */
    axl_file_view_set_pinned(v, true);
    AxlFileWriter *w = axl_file_writer_open(dstfs, 0);
    if (w == NULL) {
        axl_file_view_close(v);
        return AXL_ERR;
    }
    size_t sz  = axl_file_view_size(v);
    size_t off = 0;
    int    rc  = AXL_OK;
    char   buf[FS_COPY_BUF];
    while (off < sz) {
        size_t n = axl_file_view_read(v, off, buf, sizeof(buf));
        if (n == 0) {
            /* We are still short of the file size, so a 0-byte read is
               a fault, not EOF — fail rather than silently truncate. */
            rc = AXL_ERR;
            break;
        }
        if (axl_file_writer_write(w, buf, n) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
        off += n;
    }
    if (axl_file_writer_close(w) != AXL_OK) {
        rc = AXL_ERR;
    }
    axl_file_view_close(v);
    return rc;
}

/* Recursively copy srcfs -> dstfs (both mapped). deep: copy children
   for a directory; depth bounds recursion. */
static int
fs_copy_tree(
    const char *srcfs, const char *dstfs, bool deep, int depth
    )
{
    if (depth <= 0) {
        return AXL_ERR;
    }
    if (!axl_file_is_dir(srcfs)) {
        return fs_copy_file(srcfs, dstfs);
    }
    if (axl_dir_mkdir(dstfs) != AXL_OK) {
        return AXL_ERR;
    }
    if (!deep) {
        return AXL_OK;   /* Depth: 0 — collection itself, no contents */
    }
    AxlDir *d = axl_dir_open(srcfs);
    if (d == NULL) {
        return AXL_ERR;
    }
    int        rc = AXL_OK;
    AxlFsEntry e;
    while (axl_dir_read(d, &e)) {
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        char cs[FS_PATH_MAX];
        char cd[FS_PATH_MAX];
        int  ls = axl_snprintf(cs, sizeof(cs), "%s\\%s", srcfs, e.name);
        int  ld = axl_snprintf(cd, sizeof(cd), "%s\\%s", dstfs, e.name);
        if (ls <= 0 || ls >= (int)sizeof(cs)
            || ld <= 0 || ld >= (int)sizeof(cd)) {
            rc = AXL_ERR;   /* build failure or truncation */
            break;
        }
        if (fs_copy_tree(cs, cd, deep, depth - 1) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
    }
    axl_dir_close(d);
    return rc;
}

static int
fs_copy(
    void *user, const char *src, const char *dst, bool overwrite, int depth
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;
    char       s[FS_PATH_MAX];
    char       d[FS_PATH_MAX];

    if ((r->flags & AXL_SERVE_FS_READONLY) != 0u
        || fs_map(r, src, s, sizeof(s)) != AXL_OK
        || fs_map(r, dst, d, sizeof(d)) != AXL_OK) {
        return AXL_ERR;
    }
    if (!overwrite || (r->flags & AXL_SERVE_FS_NO_OVERWRITE) != 0u) {
        AxlFsEntry e;
        if (axl_file_info(d, &e) == AXL_OK) {
            return AXL_ERR;
        }
    }
    /* depth == -1 (infinity) copies contents; depth == 0 is the
       collection only. The handler rejects Depth: 1 before here. */
    return fs_copy_tree(s, d, depth != 0, FS_COPY_MAX_DEPTH);
}

static const char *
fs_content_type(
    void *user, const char *path
    )
{
    (void)user;
    return fs_mime(path);
}

// ---------------------------------------------------------------------------
// The shared ops table (full read-write; flags applied by serve_fs)
// ---------------------------------------------------------------------------

static const AxlWebDavOps fs_ops = {
    .list_dir     = fs_list_dir,
    .stat         = fs_stat,
    .read_open    = fs_read_open,
    .read_chunk   = fs_read_chunk,
    .read_close   = fs_read_close,
    .write_open   = fs_write_open,
    .write_chunk  = fs_write_chunk,
    .write_close  = fs_write_close,
    .mkdir        = fs_mkdir,
    .remove       = fs_remove,
    .move         = fs_move,
    .copy         = fs_copy,
    .content_type = fs_content_type,
};

const AxlWebDavOps *
axl_fs_webdav_ops(void)
{
    return &fs_ops;
}

// ---------------------------------------------------------------------------
// AxlFsRoot + serve_fs
// ---------------------------------------------------------------------------

AxlFsRoot *
axl_fs_root_new(
    const char *fs_root,
    uint32_t    flags
    )
{
    if (fs_root == NULL || fs_root[0] == '\0') {
        return NULL;
    }
    AxlFsRoot *r = axl_calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    /* Copy the base path, stripping a trailing separator so fs_map can
       always join with one. */
    size_t i = 0;
    for (; fs_root[i] != '\0' && i + 1 < sizeof(r->root); i++) {
        r->root[i] = fs_root[i];
    }
    if (fs_root[i] != '\0') {            /* didn't fit */
        axl_free(r);
        return NULL;
    }
    while (i > 0 && (r->root[i - 1] == '/' || r->root[i - 1] == '\\')) {
        i--;
    }
    r->root[i] = '\0';
    r->flags   = flags;
    return r;
}

void
axl_fs_root_free(
    AxlFsRoot *root
    )
{
    axl_free(root);
}

int
axl_http_server_serve_fs(
    AxlHttpServer *s,
    const char    *prefix,
    const char    *fs_root,
    uint32_t       flags,
    uint32_t       auth_flags
    )
{
    if (s == NULL || prefix == NULL || fs_root == NULL) {
        return AXL_ERR;
    }

    AxlFsRoot *root = axl_fs_root_new(fs_root, flags);
    if (root == NULL) {
        return AXL_ERR;
    }

    /* Copy the shared table and drop the mutating callbacks the flags
       forbid — a NULL op makes the handler answer 405 for that verb. */
    AxlWebDavOps ops = fs_ops;
    if ((flags & AXL_SERVE_FS_READONLY) != 0u) {
        ops.write_open  = NULL;
        ops.write_chunk = NULL;
        ops.write_close = NULL;
        ops.mkdir       = NULL;
        ops.remove      = NULL;
        ops.move        = NULL;
        ops.copy        = NULL;
    }
    if ((flags & AXL_SERVE_FS_NO_DELETE) != 0u) {
        ops.remove = NULL;
    }

    if (axl_http_server_add_webdav_auth(s, prefix, &ops, root,
                                        auth_flags) != AXL_OK) {
        axl_fs_root_free(root);
        return AXL_ERR;
    }

    /* add_webdav appended the mount; mark its user_data server-owned so
       axl_http_server_free releases the root we allocated. */
    if (s->webdav_ctx_count > 0) {
        s->webdav_ctxs[s->webdav_ctx_count - 1]->user_data_free =
            (void (*)(void *))axl_fs_root_free;
    }
    return AXL_OK;
}
