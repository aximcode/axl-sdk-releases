/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-mount.c
    Bridge a live Axl9pClient onto an AxlFsProvider so a remote 9P share is
    published as a UEFI fsN: volume. Each open handle holds a persistent 9P
    fid (walked+opened on Open, clunked on Close); reads/writes are offset-
    based Tread/Twrite; directories iterate a buffered Treaddir entry list.
**/

#include <axl/axl-9p.h>
#include <axl/axl-fs.h>
#include <axl/axl-fs-provider.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-array.h>
#include "axl-9p-internal.h"

// Linux open flags used on the wire (9P2000.L passes host O_* values).
#define P9_O_RDONLY   0x0000u
#define P9_O_WRONLY   0x0001u
#define P9_O_RDWR     0x0002u
#define P9_O_CREAT    0x0040u
#define P9_O_TRUNC    0x0200u

/* Per-publication context (AxlFsProvider.backend_ctx). Owns nothing: the
   client is owned by the caller of axl_9p_mount and must outlive the mount. */
typedef struct {
    struct Axl9pClient *client;
    bool                read_only;
    AxlFsProvider       provider;   ///< the vtable, backend_ctx -> this
    void               *volume;     ///< publish handle (set by axl_9p_mount)
} Mount9pCtx;

/* Per-open-handle state. */
struct AxlFsProviderFile {
    struct Axl9pClient *client;
    uint32_t   fid;
    uint64_t   offset;      ///< current byte (file) or entry-cursor is separate
    bool       is_dir;
    bool       read_only;
    char       path[512];   ///< absolute server path (for rename source, root check)
    /* Directory iteration: entries buffered from Treaddir, served one per read_dir. */
    AxlArray  *dents;       ///< AxlArray of AxlFsEntry, or NULL until first read_dir
    size_t     dent_pos;    ///< next index into dents
    uint64_t   dir_off;     ///< 9P dirent offset cursor for the next Treaddir
};

static AxlFsStatus
errno_to_fs(void)
{
    /* c->last_errno DOES carry the Rlerror errno now (axl_9p_rename's EXDEV
       fallback in axl-9p-client.c reads it) -- this call site just does not
       consult it, mapping every failure to the generic IO code instead. A
       finer per-errno mapping (ENOENT -> NOT_FOUND, EACCES -> ACCESS_DENIED,
       etc., per spec §9) is no longer blocked by the codec; it is simply a
       separate change from this one. */
    return AXL_FS_ERR_IO;
}

// --- open --------------------------------------------------------------------

static AxlFsStatus
mount_open(void *backend_ctx, const char *utf8_path, unsigned mode,
           unsigned attributes, AxlFsProviderFile **out, bool *out_is_dir)
{
    Mount9pCtx *m = (Mount9pCtx *)backend_ctx;
    if (m->read_only && (mode & (AXL_FS_OPEN_WRITE | AXL_FS_OPEN_CREATE))) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }

    AxlFsProviderFile *f = axl_calloc(1, sizeof(*f));
    if (f == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->client    = m->client;
    f->read_only = m->read_only;
    f->fid       = AXL_9P_NOFID;
    size_t plen  = axl_strlen(utf8_path);
    if (plen >= sizeof(f->path)) {
        axl_free(f);
        return AXL_FS_ERR_INVALID;
    }
    axl_memcpy(f->path, utf8_path, plen + 1);

    uint32_t fid = AXL_9P_NOFID;
    bool     exists = (axl_9p_client_walk(m->client, utf8_path, &fid) == AXL_OK);

    if (!exists) {
        if (!(mode & AXL_FS_OPEN_CREATE)) {
            axl_free(f);
            return AXL_FS_ERR_NOT_FOUND;
        }
        /* CREATE: walk to parent, then Tlcreate (file) or Tmkdir (dir). */
        char parent[512];
        const char *name = NULL;
        if (axl_9p_client_split_parent(utf8_path, parent, sizeof(parent), &name) != AXL_OK) {
            axl_free(f);
            return AXL_FS_ERR_INVALID;
        }
        uint32_t dfid = AXL_9P_NOFID;
        if (axl_9p_client_walk(m->client, parent, &dfid) != AXL_OK) {
            axl_free(f);
            return AXL_FS_ERR_NOT_FOUND;
        }
        if (attributes & AXL_FS_ATTR_DIRECTORY) {
            /* Tmkdir(dfid, name, mode=0755, gid=0) -> Rmkdir. */
            uint8_t req[512];
            Axl9pWriter w;
            axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TMKDIR, 0);
            axl_9p_w_u32(&w, dfid);
            axl_9p_w_str(&w, name);
            axl_9p_w_u32(&w, 0755);
            axl_9p_w_u32(&w, 0);
            int rc = w.overflow ? AXL_ERR
                   : axl_9p_transact(m->client, req, axl_9p_msg_finish(&w), AXL_9P_RMKDIR);
            axl_9p_client_clunk(m->client, dfid);
            if (rc != AXL_OK) {
                axl_free(f);
                return errno_to_fs();
            }
            /* Re-walk to the freshly created dir for the returned handle. */
            if (axl_9p_client_walk(m->client, utf8_path, &fid) != AXL_OK) {
                axl_free(f);
                return errno_to_fs();
            }
            f->fid = fid; f->is_dir = true; f->offset = 0;
            *out = f; *out_is_dir = true;
            return AXL_FS_OK;
        }
        /* Tlcreate turns dfid into the new open file. */
        if (axl_9p_client_lcreate(m->client, dfid, name,
                                  P9_O_RDWR | P9_O_CREAT, 0644) != AXL_OK) {
            axl_9p_client_clunk(m->client, dfid);
            axl_free(f);
            return errno_to_fs();
        }
        f->fid = dfid; f->is_dir = false; f->offset = 0;
        *out = f; *out_is_dir = false;
        return AXL_FS_OK;
    }

    /* Existing path: stat to learn dir-ness, then open (dir or file). */
    uint32_t st_mode = 0;
    if (axl_9p_client_getattr(m->client, fid, NULL, NULL, &st_mode) != AXL_OK) {
        axl_9p_client_clunk(m->client, fid);
        axl_free(f);
        return errno_to_fs();
    }
    bool is_dir = (st_mode & AXL_9P_S_IFDIR) != 0;
    /* Open write-capable only when the caller actually asked for write AND
       the mount isn't read-only -- NOT "every existing file, always RDWR".
       A real 9P server rejects a write-capable Tlopen with EACCES when the
       attach user lacks write on the file (mode 0444, another uid's file),
       so blindly requesting RDWR breaks reads of read-only content. */
    bool     want_write = (mode & AXL_FS_OPEN_WRITE) && !m->read_only;
    uint32_t oflags = (is_dir || !want_write) ? P9_O_RDONLY : P9_O_RDWR;
    if (axl_9p_client_lopen(m->client, fid, oflags) != AXL_OK) {
        axl_9p_client_clunk(m->client, fid);
        axl_free(f);
        return errno_to_fs();
    }
    f->fid = fid; f->is_dir = is_dir; f->offset = 0;
    *out = f; *out_is_dir = is_dir;
    return AXL_FS_OK;
}

// --- close -------------------------------------------------------------------

static AxlFsStatus
mount_close(AxlFsProviderFile *f)
{
    if (f == NULL) {
        return AXL_FS_OK;
    }
    if (f->fid != AXL_9P_NOFID) {
        axl_9p_client_clunk(f->client, f->fid);
    }
    if (f->dents != NULL) {
        axl_array_free(f->dents);
    }
    axl_free(f);
    return AXL_FS_OK;
}

// --- read (file bytes) -------------------------------------------------------

static AxlFsStatus
mount_read(AxlFsProviderFile *f, void *buf, size_t *inout_size)
{
    struct Axl9pClient *c = f->client;
    size_t want = *inout_size;
    uint32_t chunk = c->msize - 11;                 /* header(7) + count(4) */
    if (want > chunk) want = chunk;

    uint8_t req[24];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREAD, 0);
    axl_9p_w_u32(&w, f->fid);
    axl_9p_w_u64(&w, f->offset);
    axl_9p_w_u32(&w, (uint32_t)want);
    if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREAD) != AXL_OK) {
        return errno_to_fs();
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint32_t count = axl_9p_r_u32(&r);
    /* count > want is caught separately from the c->rbuf bound above: a
       compliant server never returns more than requested, but `buf` is the
       caller's fixed-size buffer (sized to the original `want`, not to
       c->rbuf) -- a server that lies here would overflow it, not c->rbuf. */
    if (r.error || (size_t)count + 11 > c->rlen || count > want) {
        return AXL_FS_ERR_IO;
    }
    if (count > 0) {
        axl_memcpy(buf, c->rbuf + 11, count);
        f->offset += count;
    }
    *inout_size = count;                            /* 0 = EOF */
    return AXL_FS_OK;
}

// --- read_dir ----------------------------------------------------------------

static AxlFsStatus
mount_fill_dents(AxlFsProviderFile *f)
{
    struct Axl9pClient *c = f->client;
    f->dents = axl_array_new(sizeof(AxlFsEntry));
    if (f->dents == NULL) {
        return AXL_FS_ERR_NO_MEMORY;
    }
    f->dir_off = 0;
    uint32_t chunk = c->msize - 11;
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREADDIR, 0);
        axl_9p_w_u32(&w, f->fid);
        axl_9p_w_u64(&w, f->dir_off);
        axl_9p_w_u32(&w, chunk);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREADDIR) != AXL_OK) {
            goto fail;
        }
        uint32_t dcount = 0;
        uint64_t last_off = 0;
        /* Rreaddir body starts at rbuf+7: count[4] then dcount bytes. */
        if (axl_9p_parse_readdir(c->rbuf + 7, c->rlen - 7, f->dents,
                                 &dcount, &last_off) != AXL_OK) {
            goto fail;
        }
        if (dcount == 0) {
            break;                                  /* end of directory */
        }
        f->dir_off = last_off;
        if (axl_array_len(f->dents) > AXL_9P_MAX_DIR_ENTRIES) {
            goto fail;
        }
    }
    f->dent_pos = 0;
    return AXL_FS_OK;

fail:
    /* Leave f->dents NULL on any failure so the next read_dir call actually
       retries the network fetch, instead of `dents != NULL` short-circuiting
       into silently serving this partial (truncated) listing as if it were
       complete. */
    axl_array_free(f->dents);
    f->dents = NULL;
    return AXL_FS_ERR_IO;
}

static AxlFsStatus
mount_read_dir(AxlFsProviderFile *f, AxlFsEntry *out, bool *out_end)
{
    if (f->dents == NULL) {
        AxlFsStatus s = mount_fill_dents(f);
        if (s != AXL_FS_OK) {
            return s;
        }
    }
    if (f->dent_pos >= axl_array_len(f->dents)) {
        *out_end = true;
        return AXL_FS_OK;
    }
    AxlFsEntry *e = (AxlFsEntry *)axl_array_get(f->dents, f->dent_pos++);
    *out = *e;
    *out_end = false;
    return AXL_FS_OK;
}

// --- write -------------------------------------------------------------------

static AxlFsStatus
mount_write(AxlFsProviderFile *f, const void *buf, size_t *inout_size)
{
    struct Axl9pClient *c = f->client;
    size_t want = *inout_size;
    uint32_t chunk = c->msize - 23;                 /* hdr(7)+fid(4)+off(8)+cnt(4) */
    if (want > chunk) want = chunk;

    Axl9pWriter w;
    axl_9p_msg_begin(&w, c->wbuf, sizeof(c->wbuf), AXL_9P_TWRITE, 0);
    axl_9p_w_u32(&w, f->fid);
    axl_9p_w_u64(&w, f->offset);
    axl_9p_w_u32(&w, (uint32_t)want);
    axl_9p_w_bytes(&w, buf, want);
    if (w.overflow
        || axl_9p_transact(c, c->wbuf, axl_9p_msg_finish(&w), AXL_9P_RWRITE) != AXL_OK) {
        return errno_to_fs();
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint32_t count = axl_9p_r_u32(&r);
    if (r.error) {
        return AXL_FS_ERR_IO;
    }
    if (count > want) {
        count = (uint32_t)want;   /* never trust a server reporting more than we sent */
    }
    f->offset += count;
    *inout_size = count;
    return AXL_FS_OK;
}

// --- seek --------------------------------------------------------------------

static AxlFsStatus
mount_seek(AxlFsProviderFile *f, uint64_t position)
{
    if (f->is_dir) {
        if (position != 0) {
            return AXL_FS_ERR_UNSUPPORTED;
        }
        /* Reset directory iteration. */
        if (f->dents != NULL) {
            axl_array_free(f->dents);
            f->dents = NULL;
        }
        f->dent_pos = 0;
        f->dir_off  = 0;
        return AXL_FS_OK;
    }
    if (position == (uint64_t)-1) {
        /* Seek to EOF: stat for size. */
        uint64_t size = 0;
        if (axl_9p_client_getattr(f->client, f->fid, &size, NULL, NULL) != AXL_OK) {
            return errno_to_fs();
        }
        f->offset = size;
        return AXL_FS_OK;
    }
    f->offset = position;
    return AXL_FS_OK;
}

// --- del ---------------------------------------------------------------------

static AxlFsStatus
mount_del(AxlFsProviderFile *f)
{
    if (f->read_only) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }
    struct Axl9pClient *c = f->client;
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREMOVE, 0);
    axl_9p_w_u32(&w, f->fid);
    int rc = axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREMOVE);
    /* Tremove clunks the fid server-side regardless of success; don't clunk
       again in close. */
    f->fid = AXL_9P_NOFID;
    return (rc == AXL_OK) ? AXL_FS_OK : errno_to_fs();
}

// --- get_info ----------------------------------------------------------------

static AxlFsStatus
mount_get_info(AxlFsProviderFile *f, AxlFsEntry *out)
{
    uint64_t size = 0, mtime = 0;
    uint32_t mode = 0;
    if (axl_9p_client_getattr(f->client, f->fid, &size, &mtime, &mode) != AXL_OK) {
        return errno_to_fs();
    }
    axl_memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version     = AXL_FS_ENTRY_VERSION;
    /* Basename from f->path (empty for root "/"). */
    const char *base = f->path;
    for (const char *p = f->path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    size_t blen = axl_strlen(base);
    if (blen >= sizeof(out->name)) blen = sizeof(out->name) - 1;
    axl_memcpy(out->name, base, blen);
    out->name[blen] = '\0';
    out->mtime_unix = mtime;
    if (mode & AXL_9P_S_IFDIR) {
        out->attributes = AXL_FS_ATTR_DIRECTORY;
        out->size = 0;
    } else {
        out->size = size;
    }
    return AXL_FS_OK;
}

// --- set_info (rename only in v1) -------------------------------------------

static AxlFsStatus
mount_set_info(AxlFsProviderFile *f, const AxlFsEntry *in)
{
    if (f->read_only) {
        return AXL_FS_ERR_WRITE_PROTECTED;
    }
    /* Only a basename change (rename within the same directory) is honored in
       v1; attribute-only SetInfo is accepted as a no-op. */
    const char *cur_base = f->path;
    for (const char *p = f->path; *p; p++) {
        if (*p == '/') cur_base = p + 1;
    }
    if (in->name[0] == '\0' || axl_strcmp(in->name, cur_base) == 0) {
        return AXL_FS_OK;                            /* no rename requested */
    }
    /* Build the destination path: parent(f->path) + "/" + in->name. */
    char parent[512];
    const char *ignore = NULL;
    if (axl_9p_client_split_parent(f->path, parent, sizeof(parent), &ignore) != AXL_OK) {
        return AXL_FS_ERR_INVALID;
    }
    char dst[512];
    int n = axl_snprintf(dst, sizeof(dst), "%s%s%s", parent,
                         (parent[axl_strlen(parent) - 1] == '/') ? "" : "/", in->name);
    if (n < 0 || (size_t)n >= sizeof(dst)) {
        return AXL_FS_ERR_INVALID;
    }
    if (axl_9p_rename(f->client, f->path, dst) != AXL_OK) {
        return errno_to_fs();
    }
    /* Track the new path so a later get_info reports the new basename. */
    size_t dlen = axl_strlen(dst);
    axl_memcpy(f->path, dst, dlen + 1);
    return AXL_FS_OK;
}

// --- vtable assembly ---------------------------------------------------------

static void
axl_9p_build_provider(struct Axl9pClient *c, bool read_only,
                      AxlFsProvider *out, Mount9pCtx *ctx)
{
    ctx->client    = c;
    ctx->read_only = read_only;
    ctx->volume    = NULL;

    AxlFsProvider p = {
        .struct_size   = sizeof(AxlFsProvider),
        .version       = AXL_FS_PROVIDER_VERSION,
        .open          = mount_open,
        .close         = mount_close,
        .read          = mount_read,
        .read_dir      = mount_read_dir,
        .write         = read_only ? NULL : mount_write,
        .seek          = mount_seek,
        .del           = read_only ? NULL : mount_del,
        .flush         = NULL,                       /* no-op: writes are synchronous */
        .get_info      = mount_get_info,
        .set_info      = read_only ? NULL : mount_set_info,
        .volume_info   = NULL,
        .default_label = "9p",
        .backend_ctx   = ctx,
    };
    *out = p;
}

// --- public mount / unmount ---------------------------------------------------

/* A stable, mount-specific vendor GUID (distinct per provider kind so device
   paths from concurrent mounts don't collide with other providers). */
static const AxlGuid AXL_9P_MOUNT_GUID = {
    0x9b2f7a10, 0x4c3d, 0x4e58,
    { 0xa1, 0x6e, 0x39, 0x50, 0x7b, 0x2c, 0x11, 0x9d }
};

int
axl_9p_mount(Axl9pClient *c, bool read_only, void **out_volume)
{
    if (c == NULL || out_volume == NULL) {
        return AXL_ERR;
    }
    Mount9pCtx *ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return AXL_ERR;
    }
    axl_9p_build_provider((struct Axl9pClient *)c, read_only, &ctx->provider, ctx);

    void *handle = NULL;
    if (axl_fs_provider_publish(&ctx->provider, &AXL_9P_MOUNT_GUID, &handle) != AXL_OK) {
        axl_free(ctx);
        return AXL_ERR;
    }
    ctx->volume = handle;
    *out_volume = ctx;                 /* token = the ctx (owns the provider vtable) */
    return AXL_OK;
}

int
axl_9p_unmount(void *volume)
{
    if (volume == NULL) {
        return AXL_OK;
    }
    Mount9pCtx *ctx = (Mount9pCtx *)volume;
    int rc = axl_fs_provider_unpublish(ctx->volume);
    axl_free(ctx);
    return rc;
}
