/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-serve-fs.c
    Generic axl-fs-backed WebDAV ops — the glue between
    axl_http_server_add_webdav and <axl/axl-fs.h>, so a consumer serving
    a mounted volume doesn't hand-write the ~13 AxlWebDavOps callbacks.

    Each callback maps a relative WebDAV path onto a path within the
    AxlFsRoot's base directory (traversal-contained) and calls the
    matching axl-fs primitive. Streaming read uses AxlFileView; streaming
    write uses AxlFileWriter; COPY is the library's replace transaction
    and its iterative, handle-tracked tree copy.
**/

#include "axl-http-server-internal.h"
#include "../fs/axl-fs-internal.h"
#include <axl/axl-fs.h>
#include <axl/axl-file-view.h>
#include <axl/axl-path.h>
#include <axl/axl-rng.h>
#include <axl/axl-time.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

AXL_LOG_DOMAIN("serve-fs");

#define FS_PATH_MAX        512
#define FS_READ_FRAMES     8       /* AxlFileView LRU frames for GET */
/* A PUT writes its body to a temp sibling of the target named
   "~AP" + five upper-case hex digits + ".TMP": a valid 8.3 name in upper
   case, so FAT stores it with no long name and no alias -- its one
   spelling is this one, in any letter case. */
#define FS_PUT_TEMP_FMT      "~AP%05X.TMP"
#define FS_PUT_TEMP_MASK     0xFFFFFu
#define FS_PUT_TEMP_TRIES    64
/* PUTs in flight at once, process-wide: one per WebDAV mount. */
#define FS_PUT_INFLIGHT_MAX  32

struct AxlFsRoot {
    char     root[FS_PATH_MAX];   /* base path, trailing separator stripped */
    uint32_t flags;
};

/* The temp path of every PUT in flight, process-wide (it is the filesystem
   that is shared, not a mount: two mounts can serve one directory). A live
   temp is not a resource: never listed, copied, served or written by a
   request. It is recognised by its BASENAME -- an 8.3 name that is unique
   among live temps and has no alias -- because the directory it sits in
   can be spelled many ways (an 8.3 alias of any directory on the way). A
   temp-shaped file NOT here -- left by a power loss -- is an ordinary
   file. Points into each PUT's own FsWriteCtx; no allocation. */
static const char *fs_put_inflight_paths[FS_PUT_INFLIGHT_MAX];

/* Where a PUT IN PLACE is writing, process-wide: the file a write in place
   is filling is busy for EVERY mount (two mounts can serve one directory)
   -- another PUT to it is AXL_BUSY (503), and a DELETE of it or a MOVE or
   COPY that would replace or remove it is AXL_BUSY (423 Locked). Unlike a
   temp it stays a visible resource. Keyed on the volume and the STORED
   path -- each component's name as the filesystem keeps it, read back the
   way axl_file_replace decides identity -- so an 8.3 alias or another
   letter case of the file or of any directory on the way is the same key. */
typedef struct {
    bool  used;
    void *volume;
    char  canon[AXL_FS_REL_MAX];
} FsBusy;

static FsBusy fs_inplace_busy[FS_PUT_INFLIGHT_MAX];

/* Is a write in place busy at @p fspath -- or, with @p below, anywhere
   under it (a directory a DELETE, MOVE or COPY would take with it)?
   AXL_OK when not, AXL_BUSY when it is, AXL_IO_ERROR when a write in place
   is busy on @p fspath's volume and @p fspath cannot be resolved to its
   stored names -- it cannot be told apart from the busy file, and must
   neither pass nor be told to retry. The stored names are read only when
   the volume has a busy entry: the common case costs no I/O. */
static int
fs_busy_at(
    const char *fspath,
    bool        below
    )
{
    void *vol = NULL;
    char  rel[AXL_FS_REL_MAX];
    bool  any = false;
    bool  located = axl_fs_path_locate(fspath, &vol, rel, sizeof(rel)) == AXL_OK;
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        any = any || (fs_inplace_busy[i].used
                      && (!located || fs_inplace_busy[i].volume == vol));
    }
    if (!any) {
        return AXL_OK;
    }
    char canon[AXL_FS_REL_MAX];
    int  rc = located ? axl_fs_path_canonical(fspath, &vol, canon, sizeof(canon)) : AXL_ERR;
    if (rc == AXL_NOT_FOUND || rc == AXL_NOT_DIR) {
        return AXL_OK;   /* not there: neither the busy file nor holding it */
    }
    if (rc != AXL_OK) {
        return AXL_IO_ERROR;
    }
    size_t n = axl_strlen(canon);
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        const FsBusy *b = &fs_inplace_busy[i];
        if (!b->used || b->volume != vol) {
            continue;
        }
        if (axl_strcmp(b->canon, canon) == 0) {
            return AXL_BUSY;
        }
        if (below && (n == 0 || (axl_strncmp(b->canon, canon, n) == 0 && b->canon[n] == '\\'))) {
            return AXL_BUSY;
        }
    }
    return AXL_OK;
}

/* Mark @p fspath busy, under its stored names. The slot's index; -1 when no
   slot is free (AXL_BUSY: retry), -2 when the path cannot be resolved
   (AXL_IO_ERROR). */
static int
fs_busy_begin(
    const char *fspath
    )
{
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        FsBusy *b = &fs_inplace_busy[i];
        if (!b->used) {
            if (axl_fs_path_canonical(fspath, &b->volume, b->canon, sizeof(b->canon)) != AXL_OK) {
                return -2;
            }
            b->used = true;
            return (int)i;
        }
    }
    return -1;
}

static void
fs_busy_end(
    int slot
    )
{
    if (slot >= 0 && (size_t)slot < FS_PUT_INFLIGHT_MAX) {
        fs_inplace_busy[slot].used = false;
    }
}

// ---------------------------------------------------------------------------
// Path mapping — relative WebDAV path -> contained filesystem path
// ---------------------------------------------------------------------------

/* The basename of a mapped path: what follows its last '\\' or ':'. */
static const char *
fs_basename(
    const char *fspath
    )
{
    const char *base = fspath;
    for (const char *p = fspath; *p != '\0'; p++) {
        if (*p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    return base;
}

/* Whether the @p len bytes at @p name are a live temp's basename -- any
   live temp but the one at @p except (NULL: any). Case-folded, as FAT
   matches names. */
static bool
fs_put_name_live(
    const char *name,
    size_t      len,
    const char *except
    )
{
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        const char *t = fs_put_inflight_paths[i];
        if (t == NULL || t == except || t[0] == '\0') {
            continue;
        }
        const char *b = fs_basename(t);
        if (axl_strlen(b) == len && axl_strncasecmp(b, name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether ANY component of @p fspath is a live temp's name (other than
   the one at @p except): the temp itself, reached through whatever
   spelling of its directory. */
static bool
fs_put_inflight(
    const char *fspath,
    const char *except
    )
{
    const char *start = fspath;
    for (const char *p = fspath; ; p++) {
        if (*p == '\\' || *p == '/' || *p == ':' || *p == '\0') {
            if (p > start && fs_put_name_live(start, (size_t)(p - start), except)) {
                return true;
            }
            if (*p == '\0') {
                return false;
            }
            start = p + 1;
        }
    }
}

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
fs_map_any(
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
    } else if (n > 0 && out[n - 1] == ':') {
        /* The mount root of a mount at a bare volume ("FS0:"): the root
           directory is "FS0:\" -- "FS0:" alone opens nothing on FAT. */
        if (n + 1 >= size) {
            return AXL_ERR;
        }
        out[n++] = '\\';
    }
    out[n] = '\0';
    return AXL_OK;
}

/* fs_map_any for a request: a PUT's live temp is not a resource, so it maps
   to nothing. */
static int
fs_map(
    const AxlFsRoot *root,
    const char      *rel,
    char            *out,
    size_t           size
    )
{
    if (fs_map_any(root, rel, out, size) != AXL_OK || fs_put_inflight(out, NULL)) {
        return AXL_ERR;
    }
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
        if (fs_put_name_live(e.name, axl_strlen(e.name), NULL)) {
            continue;   /* a live PUT's temp */
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
    int busy = fs_busy_at(fspath, true);
    if (busy != AXL_OK) {
        return busy;   /* a PUT is writing it in place: 423; unresolvable: 500 */
    }
    if (axl_file_is_dir(fspath)) {
        return axl_dir_rmdir(fspath);
    }
    return axl_file_delete(fspath);
}

/* The replace flags for what a MOVE or COPY may not do, decided by the
   replace transaction by identity -- the names the filesystem stores --
   before anything changes: replace a DIFFERENT existing entry when the
   client sent Overwrite: F or the mount is no-overwrite
   (AXL_FS_REPLACE_NO_CLOBBER: AXL_EXISTS, 412), or replace a different
   existing collection on a mount that refuses DELETE
   (AXL_FS_REPLACE_NO_DIR_DEST: AXL_DENIED, 403) -- an overwrite must not
   wipe a tree DELETE may not. The source under another spelling (its 8.3
   alias, or another letter case on case-insensitive FAT) is no such
   destination; on a case-sensitive volume a.txt and A.txt are two. */
static uint32_t
fs_transfer_guard(
    const AxlFsRoot *r, bool overwrite
    )
{
    uint32_t g = 0;
    if (!overwrite || (r->flags & AXL_SERVE_FS_NO_OVERWRITE) != 0u) {
        g |= AXL_FS_REPLACE_NO_CLOBBER;
    }
    if ((r->flags & AXL_SERVE_FS_NO_DELETE) != 0u) {
        g |= AXL_FS_REPLACE_NO_DIR_DEST;
    }
    return g;
}

/* Map a MOVE or COPY's two request paths. A destination that maps to
   nothing (a live PUT's temp) is AXL_NOT_FOUND, so it is a 409 like any
   destination that cannot be reached. A destination a PUT is writing in
   place -- or, for a MOVE (@p moves), a source -- is AXL_BUSY (423). */
static int
fs_transfer_map(
    const AxlFsRoot *r, const char *src, const char *dst, bool moves,
    char *s, char *d, size_t size
    )
{
    if ((r->flags & AXL_SERVE_FS_READONLY) != 0u) {
        return AXL_DENIED;
    }
    if (fs_map(r, src, s, size) != AXL_OK || fs_map(r, dst, d, size) != AXL_OK) {
        return AXL_NOT_FOUND;
    }
    int busy = fs_busy_at(d, true);
    if (busy == AXL_OK && moves) {
        busy = fs_busy_at(s, true);
    }
    return busy;   /* AXL_BUSY 423, AXL_IO_ERROR 500 */
}

// --- streaming write (PUT) ---
//
// A PUT is atomic: the body goes to a temp sibling of the target, and the
// target is replaced only once the temp is complete and flushed -- the
// model Apache's mod_dav_fs, nginx and lighttpd follow. Until then the old
// file is untouched, so a PUT that does not complete (the client hangs up,
// a chunk cannot be written, the Content-Digest does not match) costs
// nothing. The replace sets the old file ASIDE before the temp takes its
// name and deletes the aside last: FAT cannot rename over a name, and
// deleting first would lose the old file to a rename that then failed. A
// PUT replaces a FILE only -- whatever is at the target when it commits is
// checked again there, and a collection is never removed. The price is
// room for both files for a moment.
//
// A mount can opt out of that price for an existing file
// (AXL_SERVE_FS_PUT_IN_PLACE*): the file is truncated and the body
// streamed into it, with no temp. Every refusal is made first, exactly as
// for the atomic path; what is given up is the old content of a PUT that
// does not complete, which is then reported, never hidden.

typedef struct {
    AxlFileWriter   *w;
    const AxlFsRoot *r;
    bool             in_place;             /* writing the target itself */
    int              busy;                 /* its fs_inplace_busy slot, -1 none */
    uint64_t         length;               /* stated body length, or UNKNOWN */
    uint64_t         written;              /* bytes write_chunk stored */
    char             temp[FS_PATH_MAX];    /* where the body is written */
    char             target[FS_PATH_MAX];  /* what it replaces on commit */
} FsWriteCtx;

/* The next temp name's number. Seeded once per boot from the RNG (or the
   clock), with no allocation, so a temp a power loss left behind is
   unlikely to be the first name a later boot tries -- and if it is, the
   exclusive open moves on. */
static uint32_t
fs_put_temp_number(void)
{
    static bool     seeded;
    static uint32_t next;

    if (!seeded) {
        if (axl_rng_bytes(&next, sizeof(next)) != AXL_OK) {
            next = (uint32_t)axl_time_get_us();
        }
        seeded = true;
    }
    return (next++) & FS_PUT_TEMP_MASK;
}

/* Record @p c's temp as a live PUT's: hidden and refused from now until
   fs_put_inflight_end. AXL_BUSY when FS_PUT_INFLIGHT_MAX are in flight. */
static int
fs_put_inflight_begin(
    const FsWriteCtx *c
    )
{
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        if (fs_put_inflight_paths[i] == NULL) {
            fs_put_inflight_paths[i] = c->temp;
            return AXL_OK;
        }
    }
    return AXL_BUSY;
}

static void
fs_put_inflight_end(
    const FsWriteCtx *c
    )
{
    for (size_t i = 0; i < FS_PUT_INFLIGHT_MAX; i++) {
        if (fs_put_inflight_paths[i] == c->temp) {
            fs_put_inflight_paths[i] = NULL;
        }
    }
}

/* Create @p c->temp beside the target, opened exclusive so it can never be
   a file that already exists. Only a name already taken is retried -- any
   other failure fails every name alike: AXL_NOT_FOUND when the parent
   collection does not exist (409), AXL_IO_ERROR for the server's own
   failure (500), AXL_ERR for a temp path too long. */
static int
fs_put_temp_open(
    FsWriteCtx *c
    )
{
    const char *base = fs_basename(c->target);
    size_t      dlen = (size_t)(base - c->target);

    for (unsigned tries = 0; tries < FS_PUT_TEMP_TRIES; tries++) {
        AxlFsEntry e;
        char       name[16];
        axl_snprintf(name, sizeof(name), FS_PUT_TEMP_FMT,
                     (unsigned)fs_put_temp_number());
        if (fs_put_name_live(name, axl_strlen(name), NULL)
            || axl_strcasecmp(name, base) == 0) {
            continue;   /* live temps' names stay unique */
        }
        int n = axl_snprintf(c->temp, sizeof(c->temp), "%.*s%s",
                             (int)dlen, c->target, name);
        if (n <= 0 || n >= (int)sizeof(c->temp)) {
            return AXL_ERR;
        }
        c->w = axl_file_writer_open(c->temp, AXL_FILE_WRITER_EXCL);
        if (c->w != NULL) {
            return AXL_OK;
        }
        if (axl_file_info(c->temp, &e) != AXL_OK) {
            /* Not a name clash. A parent collection that is not there (or
               is a file) is the client's: 409. Anything else -- out of
               memory, a volume that will not create the file -- is the
               server's own failure: 500, not a 409 blaming the client. */
            char  canon[AXL_FS_REL_MAX];
            void *vol = NULL;
            /* The temp's own path resolves (it is absent) only when every
               directory on the way exists. */
            int prc = axl_fs_path_canonical(c->temp, &vol, canon, sizeof(canon));
            return (prc == AXL_NOT_FOUND || prc == AXL_NOT_DIR) ? AXL_NOT_FOUND : AXL_IO_ERROR;
        }
    }
    return AXL_IO_ERROR;   /* every name tried was taken */
}

/* Remove the temp THROUGH THE WRITER'S OWN HANDLE -- never by path, which
   by now could name a file that is not this PUT's. What cannot be removed
   is logged by name; once this PUT is over it is an ordinary file
   (fs_put_inflight_end). */
static void
fs_put_temp_discard(
    FsWriteCtx *c
    )
{
    if (axl_file_writer_discard(c->w) != AXL_OK) {
        axl_warning("PUT: the temp file '%s' could not be removed",
                    c->temp);
    }
    c->w = NULL;
}

/* What a PUT may not replace, checked at the open and again at the commit
   (the target can change while the body streams): another PUT's live temp
   -- AXL_DENIED (403), as a request naming one is refused; a file another
   PUT is writing in place, through any mount -- AXL_BUSY (503); a collection --
   AXL_IS_DIR (409); on a no-overwrite mount, any existing entry --
   AXL_EXISTS (409); a read-only file -- AXL_DENIED (403). On AXL_OK,
   @p old_size (may be NULL) is the existing file's length, or UINT64_MAX
   when nothing is there. */
static int
fs_put_target_check(
    const FsWriteCtx *c,
    uint64_t         *old_size
    )
{
    AxlFsEntry e;
    if (old_size != NULL) {
        *old_size = UINT64_MAX;
    }
    if (fs_put_inflight(c->target, c->temp)) {
        return AXL_DENIED;
    }
    int busy = fs_busy_at(c->target, false);
    if (busy != AXL_OK) {
        return busy;   /* written in place through any mount: 503; unresolvable: 500 */
    }
    if (axl_file_info(c->target, &e) != AXL_OK) {
        return AXL_OK;   /* nothing there */
    }
    if (axl_fs_entry_is_dir(&e)) {
        return AXL_IS_DIR;
    }
    if ((c->r->flags & AXL_SERVE_FS_NO_OVERWRITE) != 0u) {
        return AXL_EXISTS;
    }
    if ((e.attributes & AXL_FS_ATTR_READ_ONLY) != 0u) {
        return AXL_DENIED;
    }
    if (old_size != NULL) {
        *old_size = e.size;
    }
    return AXL_OK;
}

/* @p bytes in whole allocation units of @p unit. */
static uint64_t
fs_units(
    uint64_t bytes,
    uint64_t unit
    )
{
    return (bytes / unit) + ((bytes % unit) != 0u ? 1u : 0u);
}

/* The name @p fspath's entry is STORED under, read through a handle on it
   -- the long name, however the path spelled it. false when nothing is
   there or the name cannot be read whole. */
static bool
fs_stored_name(
    const char *fspath,
    char       *out,
    size_t      cap
    )
{
    unsigned short *wide = axl_utf8_to_ucs2(fspath);
    AxlFileHandle   h    = NULL;
    bool            ok   = false;
    if (wide != NULL
        && axl_backend_file_open(wide, AXL_FILE_MODE_READ, 0, &h) == AXL_OK) {
        ok = axl_fs_handle_name(h, out, cap) == AXL_FS_NAME_WHOLE;
        axl_backend_file_close(&h);
    }
    axl_free(wide);
    return ok;
}

/* How a PUT of @p c->length bytes over what is at the target (@p old_size,
   UINT64_MAX for nothing) is written, by the mount's flags and the
   volume's free space: sets c->in_place, or refuses AXL_NO_RESOURCES (507)
   for a body the volume cannot hold either way -- decided before anything
   is touched. Only an existing file goes in place. A volume that cannot
   say how much room it has, or a body of unknown length, decides nothing:
   no refusal, and IF_SHORT stays atomic. */
static int
fs_put_plan(
    FsWriteCtx *c,
    uint64_t    old_size
    )
{
    bool exists = old_size != UINT64_MAX;
    bool always = (c->r->flags & AXL_SERVE_FS_PUT_IN_PLACE) != 0u;
    bool short_ok = (c->r->flags & AXL_SERVE_FS_PUT_IN_PLACE_IF_SHORT) != 0u;

    c->in_place = exists && always;
    AxlFsVolumeFacts vf;
    if (c->length == AXL_WEBDAV_LENGTH_UNKNOWN
        || axl_fs_volume_facts(c->target, &vf) != AXL_OK
        || vf.free_bytes == UINT64_MAX || vf.block_size == 0) {
        return AXL_OK;
    }
    uint64_t unit = vf.block_size;
    uint64_t body = fs_units(c->length, unit);
    uint64_t free_units = vf.free_bytes / unit;
    if (c->in_place) {
        /* Truncating gives the old file's units back first. */
        return (body > free_units + fs_units(old_size, unit)) ? AXL_NO_RESOURCES : AXL_OK;
    }
    /* The temp, then the commit: the body, plus the directory room the
       temp's own entry and the commit's replace take -- counted as
       axl_file_replace's pre-flight counts it, so a long name's entries
       are not taken for one unit and the whole body uploaded only for the
       commit to be refused 507. */
    char     stored[AXL_FS_NAME_MAX];
    uint64_t dir = (uint64_t)axl_fs_name_entries("~AP00000.TMP") * 32u
                   + axl_fs_replace_dir_bytes(fs_basename(c->target),
                                              (exists && fs_stored_name(c->target, stored,
                                                                        sizeof(stored)))
                                              ? stored : (exists ? fs_basename(c->target)
                                                                 : NULL));
    if (body + fs_units(dir, unit) <= free_units) {
        return AXL_OK;
    }
    if (exists && short_ok && body <= free_units + fs_units(old_size, unit)) {
        c->in_place = true;
        /* log-level: not a success -- the PUT gave up atomicity, which
           neither the client nor the caller can learn from any status */
        axl_info("PUT in place: '%s' (%llu bytes): the volume has %llu bytes "
                 "free, too few for a second copy", c->target,
                 (unsigned long long)c->length, (unsigned long long)vf.free_bytes);
        return AXL_OK;
    }
    /* Atomic: refused only when the body alone cannot fit. */
    return (body > free_units) ? AXL_NO_RESOURCES : AXL_OK;
}

static int
fs_write_open(
    void *user, const char *path, uint64_t length, void **out_ctx, bool *in_place
    )
{
    AxlFsRoot *r = (AxlFsRoot *)user;

    if (out_ctx == NULL || (r->flags & AXL_SERVE_FS_READONLY) != 0u) {
        return AXL_ERR;
    }
    FsWriteCtx *c = axl_calloc(1, sizeof(*c));
    if (c == NULL) {
        return AXL_IO_ERROR;   /* the server's own failure: 500, not the client's 409 */
    }
    c->r      = r;
    c->length = length;
    c->busy   = -1;
    uint64_t old_size = UINT64_MAX;
    int rc = fs_map_any(r, path, c->target, sizeof(c->target));
    /* Refuse up front what the commit would refuse after the whole body
       had been written -- and, in place, what must be refused before the
       file is truncated. */
    if (rc == AXL_OK) {
        rc = fs_put_target_check(c, &old_size);
    }
    if (rc == AXL_OK) {
        rc = fs_put_plan(c, old_size);
    }
    if (rc == AXL_OK && c->in_place) {
        /* The target itself, under whatever spelling the PUT used: the
           entry keeps its stored name. No temp, no in-flight slot. */
        c->busy = fs_busy_begin(c->target);
        c->w    = (c->busy >= 0) ? axl_file_writer_open(c->target, 0) : NULL;
        rc      = (c->w != NULL) ? AXL_OK
                  : (c->busy >= 0) ? AXL_IO_ERROR   /* the open failed: 500 */
                  : (c->busy == -1) ? AXL_BUSY : AXL_IO_ERROR;
        if (rc != AXL_OK) {
            fs_busy_end(c->busy);
        }
    } else if (rc == AXL_OK) {
        /* Claim the in-flight slot BEFORE the temp exists, so no request
           can see the temp between its creation and its registration. */
        rc = fs_put_inflight_begin(c);
        if (rc == AXL_OK) {
            rc = fs_put_temp_open(c);   /* 409 for a missing parent, else 500 */
            if (rc != AXL_OK) {
                fs_put_inflight_end(c);
            }
        }
    }
    if (rc != AXL_OK) {
        axl_free(c);
        return rc;
    }
    *in_place = c->in_place;
    *out_ctx  = c;
    return AXL_OK;
}

static int
fs_write_chunk(
    void *ctx, const void *data, size_t len
    )
{
    FsWriteCtx *c = (FsWriteCtx *)ctx;
    int         rc = axl_file_writer_write(c->w, data, len);
    if (rc == AXL_OK) {
        c->written += len;
    }
    return rc;
}

/* End an in-place PUT. A commit flushes and closes: AXL_OK once the body
   is on the volume. A discard cannot restore the old content -- it is
   gone since the open -- so it keeps what was written, says so in an
   error, and returns AXL_ERR, which tells the handler the resource holds
   it. */
static int
fs_put_in_place_close(
    FsWriteCtx *c,
    bool        aborted
    )
{
    /* The close flushes, and fails when the flush (or an earlier write)
       did: that is the commit's verdict. */
    int rc = (axl_file_writer_close(c->w) == AXL_OK && !aborted) ? AXL_OK : AXL_ERR;
    c->w = NULL;
    /* What the file holds now is what the log reports: a write that failed
       may have stored part of its chunk before failing, so the count of
       successful writes would understate it. */
    AxlFsEntry e;
    uint64_t   stored = (axl_file_info(c->target, &e) == AXL_OK) ? e.size : c->written;
    if (!aborted && rc != AXL_OK) {
        axl_error("PUT in place: '%s' is partially written: its final flush "
                  "failed; its old content is gone", c->target);
    } else if (aborted && c->length != AXL_WEBDAV_LENGTH_UNKNOWN
               && stored == c->length) {
        axl_error("PUT in place: '%s' holds the whole body (%llu bytes), which "
                  "the PUT did not accept; its old content is gone", c->target,
                  (unsigned long long)stored);
    } else if (aborted && c->length != AXL_WEBDAV_LENGTH_UNKNOWN) {
        axl_error("PUT in place: '%s' is partially written (%llu of %llu "
                  "bytes); its old content is gone", c->target,
                  (unsigned long long)stored, (unsigned long long)c->length);
    } else if (aborted) {
        axl_error("PUT in place: '%s' is partially written (%llu bytes of a "
                  "chunked body); its old content is gone", c->target,
                  (unsigned long long)stored);
    }
    return rc;
}


/* Put the complete temp in the target's place: axl_file_replace with no
   flags -- rename(2) -- so an existing FILE is set aside, the temp renamed
   into place and the aside removed last, and a collection at the target is
   AXL_IS_DIR with nothing changed: a PUT never removes a tree. The PUT's
   own checks come first, again: the target can have changed while the body
   streamed. */
static int
fs_put_commit(
    const FsWriteCtx *c
    )
{
    int rc = fs_put_target_check(c, NULL);
    if (rc != AXL_OK) {
        return rc;
    }
    /* A target that exists is replaced under the name it is STORED under,
       read through a handle on it: a PUT addressed through an 8.3 alias
       (or another letter case) changes the content, not the name. */
    char name[AXL_FS_NAME_MAX];
    if (fs_stored_name(c->target, name, sizeof(name))
        && axl_strcmp(name, fs_basename(c->target)) != 0) {
        char        stored[FS_PATH_MAX];
        const char *base = fs_basename(c->target);
        int         n    = axl_snprintf(stored, sizeof(stored), "%.*s%s",
                                        (int)(base - c->target), c->target, name);
        if (n <= 0 || n >= (int)sizeof(stored)) {
            return AXL_ERR;
        }
        return axl_file_replace(c->temp, stored, 0, NULL);
    }
    return axl_file_replace(c->temp, c->target, 0, NULL);
}

/* After a commit that reported failure: is the body at the target after
   all? Asked through the writer's own handle -- the temp read back under
   any name but its own was renamed, so it is the resource now, and a
   discard would delete what a client may already read. A name that cannot
   be read is treated the same way: nothing is deleted that cannot be
   identified as this PUT's temp. */
static bool
fs_put_landed_anyway(
    const FsWriteCtx *c,
    char             *name,
    size_t            cap
    )
{
    if (axl_file_writer_name(c->w, name, cap) != AXL_FS_NAME_WHOLE) {
        name[0] = '\0';   /* no name to report: "may be there" */
        return true;
    }
    return axl_strcasecmp(name, fs_basename(c->temp)) != 0;
}

static int
fs_write_close(
    void *ctx, bool aborted
    )
{
    FsWriteCtx *c  = (FsWriteCtx *)ctx;
    int         rc = AXL_OK;

    if (c->in_place) {
        rc = fs_put_in_place_close(c, aborted);
        fs_busy_end(c->busy);
        axl_free(c);
        return rc;
    }
    /* Flush with the handle still open: AXL_OK means the temp's bytes are
       on the volume, which is what makes replacing the target with it
       safe -- and the handle is what a discard deletes through. */
    if (!aborted) {
        rc = axl_file_writer_flush(c->w);
        if (rc != AXL_OK) {
            axl_warning("PUT: the final flush of '%s' failed; the target is "
                        "unchanged", c->target);
        } else {
            rc = fs_put_commit(c);
            char now[AXL_FS_NAME_MAX];
            if (rc != AXL_OK && fs_put_landed_anyway(c, now, sizeof(now))) {
                if (now[0] != '\0') {
                    axl_error("PUT: the commit to '%s' reported a failure, but "
                              "the body is there as '%s'; it is kept",
                              c->target, now);
                } else {
                    axl_error("PUT: the commit to '%s' reported a failure, and "
                              "the body may be there (its name cannot be read); "
                              "it is kept", c->target);
                }
                if (axl_file_writer_close(c->w) != AXL_OK) {
                    axl_warning("PUT: closing '%s' failed", c->target);
                }
                c->w = NULL;
            } else if (rc != AXL_OK) {
                axl_warning("PUT: '%s' could not replace '%s'; the target is "
                            "unchanged", c->temp, c->target);
            }
        }
    }
    if (c->w == NULL) {
        /* kept above: the commit landed after all */
    } else if (aborted || rc != AXL_OK) {
        fs_put_temp_discard(c);   /* aborted, or not committed */
    } else if (axl_file_writer_close(c->w) != AXL_OK) {
        /* Already flushed and in place: a close cannot unsay that. */
        axl_warning("PUT: closing '%s' after it replaced its target failed",
                    c->target);
    }
    fs_put_inflight_end(c);
    axl_free(c);
    return aborted ? AXL_OK : rc;
}

/* MOVE: axl_file_replace with COPY | TREE, after the mount's policy.
   Identity is the filesystem's: a destination that is the source under
   another spelling (its 8.3 alias) is RESULT_SAME, answered 403 as
   RFC 4918 9.9.4 answers a MOVE onto itself; one differing in letter case
   only renames (RESULT_RECASED). Everything the library refuses, it
   refuses before anything changes, and a replace it cannot finish puts
   both entries back. */
static int
fs_move(
    void *user, const char *src, const char *dst, bool overwrite
    )
{
    AxlFsRoot           *r = (AxlFsRoot *)user;
    char                 s[FS_PATH_MAX];
    char                 d[FS_PATH_MAX];
    AxlFileReplaceResult res = AXL_FILE_REPLACE_RESULT_CREATED;

    int rc = fs_transfer_map(r, src, dst, true, s, d, sizeof(s));
    if (rc == AXL_OK) {
        rc = axl_fs_replace_guarded(s, d, AXL_FILE_REPLACE_COPY | AXL_FILE_REPLACE_TREE
                                          | fs_transfer_guard(r, overwrite), &res);
    }
    if (rc == AXL_OK && res == AXL_FILE_REPLACE_RESULT_SAME) {
        return AXL_INVALID;   /* one entry: nothing to move */
    }
    return rc;
}

/* A live PUT's temp is no part of a COPY: the library's tree copy asks
   this of every entry below the source. */
static bool
fs_copy_skip(
    const char *name
    )
{
    return fs_put_name_live(name, axl_strlen(name), NULL);
}

/* COPY: the replace transaction with KEEP_SOURCE -- it decides every
   refusal first (a COPY onto the source itself, or into its own subtree, is
   AXL_INVALID before any copying) and sets an existing destination aside;
   then the library copies the source (a Depth: 0 COPY of a collection
   makes the collection only), iteratively and with Ctrl-C checked, and a
   copy that fails removes exactly what it created; the transaction then
   removes the aside, or puts it back. */
static int
fs_copy(
    void *user, const char *src, const char *dst, bool overwrite, int depth
    )
{
    AxlFsRoot      *r = (AxlFsRoot *)user;
    char            s[FS_PATH_MAX];
    char            d[FS_PATH_MAX];
    AxlFsReplaceTxn txn;

    int rc = fs_transfer_map(r, src, dst, false, s, d, sizeof(s));
    if (rc == AXL_OK) {
        rc = axl_fs_replace_begin(s, d, AXL_FILE_REPLACE_TREE
                                        | AXL_FS_REPLACE_KEEP_SOURCE
                                        | ((depth == 0) ? AXL_FS_REPLACE_SHALLOW : 0u)
                                        | fs_transfer_guard(r, overwrite),
                                  &txn);
    }
    if (rc != AXL_OK) {
        return rc;
    }
    /* depth == -1 (infinity) copies contents; depth == 0 is the
       collection only. The handler rejects Depth: 1 before here. */
    rc = axl_fs_replace_copy(&txn, fs_copy_skip);
    axl_fs_replace_end(&txn, rc == AXL_OK);
    return rc;
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
    /* MOVE and COPY decide identity in the replace transaction
       (fs_transfer_guard): a case-variant destination is refused only when
       it is another entry. */
    .caps         = AXL_WEBDAV_CAP_IDENTITY,
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
