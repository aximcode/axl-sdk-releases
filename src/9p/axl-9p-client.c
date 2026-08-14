/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-client.c
    Synchronous 9P2000.L client over AxlTcp: connect, read_file, list.
**/

#include <axl/axl-9p.h>
#include "axl-9p-internal.h"
#include <axl/axl-tcp.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>     /* axl_memcpy */
#include <axl/axl-string.h>  /* AxlString accumulator for read_file */
#include <axl/axl-fs.h>      /* AxlFsEntry for axl_9p_list */
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("9p");

#define AXL_9P_IO_TIMEOUT_MS  5000u

/* O_WRONLY|O_TRUNC (Linux/9P2000.L numeric open flags -- no UEFI/libc
   dependency here, so spelled out rather than pulling in <fcntl.h>). */
#define AXL_9P_O_WRONLY_TRUNC 0x201u

/* Twrite's fixed fields are header(7) + fid(4) + offset(8) + count(4) = 23
   bytes; the rest of msize is available for the data chunk. */
#define AXL_9P_TWRITE_HDR_LEN 23u

/* axl_9p_list stats each non-directory entry with its own walk+getattr. One
   failure is a file that vanished between the Rreaddir and the walk -- skip
   it. But a dead session fails EVERY entry, and each failure costs up to two
   AXL_9P_IO_TIMEOUT_MS waits, so a large directory would stall for minutes
   and then hand back a listing whose sizes are all silently wrong. This many
   failures IN A ROW is taken as "the session is gone": stop and fail the
   whole call rather than report success for a listing that isn't one.

   Counts REMOTE failures only (STAT_ENTRY_REMOTE). A local join truncation
   never touched the wire, so it is no evidence either way -- see
   StatEntryResult. */
#define AXL_9P_LIST_STAT_FAIL_LIMIT 3u

/* Ceiling on the EXDEV copy-then-unlink fallback (rename_xdev_copy): the
   whole file is materialized in UEFI heap, so a cross-directory move of more
   than this is refused rather than attempted. */
#define AXL_9P_XDEV_COPY_MAX (32u * 1024u * 1024u)

/* Outcome of one stat_list_entry call. The two failures mean OPPOSITE
   things, so axl_9p_list's "the session is gone" counter must not see them
   as one: a name too long for the join buffer is a purely LOCAL limit that
   sends nothing and proves nothing about the connection, while a failed
   walk or Tgetattr IS the wire evidence that counter exists to accumulate.
   Folding them together let three consecutive long names abort an otherwise
   healthy listing under a diagnosis that was simply false. */
typedef enum {
    STAT_ENTRY_OK = 0,   ///< size filled (and type corrected if dtype lied)
    STAT_ENTRY_LOCAL,    ///< the child path did not fit; nothing was sent
    STAT_ENTRY_REMOTE    ///< the walk or the Tgetattr failed on the wire
} StatEntryResult;

/* Read exactly n bytes (sync recv returns partial). */
static int
recv_exact(AxlTcp *sock, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t chunk = n - got;
        if (axl_tcp_recv(sock, buf + got, &chunk, AXL_9P_IO_TIMEOUT_MS) != AXL_OK
            || chunk == 0) {
            return AXL_ERR;
        }
        got += chunk;
    }
    return AXL_OK;
}

int
axl_9p_transact(Axl9pClient *c, const uint8_t *req, size_t req_len,
                uint8_t expect_type)
{
    /* Both cleared up front, so EVERY exit path below leaves last_errno
       either freshly set by an Rlerror or zero. A caller that branches on it
       after a TRANSPORT failure (send/recv error, no reply at all) must not
       read a stale errno from three requests ago and take a fallback branch
       that makes no sense for the request that actually failed. */
    c->rlen       = 0;
    c->last_errno = 0;
    if (axl_tcp_send(c->sock, req, req_len, AXL_9P_IO_TIMEOUT_MS) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t hdr[7];
    if (recv_exact(c->sock, hdr, 7) != AXL_OK) {
        return AXL_ERR;
    }
    Axl9pReader hr;
    axl_9p_r_init(&hr, hdr, 7);
    uint32_t size; uint8_t type; uint16_t tag;
    axl_9p_msg_header(&hr, &size, &type, &tag);
    if (size < 7 || size > sizeof(c->rbuf)) {
        return AXL_ERR;
    }
    axl_memcpy(c->rbuf, hdr, 7);
    if (size > 7 && recv_exact(c->sock, c->rbuf + 7, size - 7) != AXL_OK) {
        return AXL_ERR;
    }
    c->rlen = size;
    if (type == AXL_9P_RLERROR) {
        Axl9pReader er;
        axl_9p_r_init(&er, c->rbuf + 7, size - 7);
        uint32_t ecode = axl_9p_r_u32(&er);
        c->last_errno  = ecode;
        axl_debug("9p: server error errno=%u", ecode);
        return AXL_ERR;
    }
    return (type == expect_type) ? AXL_OK : AXL_ERR;
}

int
axl_9p_connect(const char *host, uint16_t port, const char *uname,
               const char *aname, Axl9pClient **out)
{
    if (host == NULL || out == NULL) {
        return AXL_ERR;
    }
    if (uname == NULL) {
        uname = "";
    }
    if (aname == NULL || aname[0] == '\0') {
        aname = "/";
    }

    Axl9pClient *c = axl_new(Axl9pClient);
    if (c == NULL) {
        return AXL_ERR;
    }
    c->msize    = AXL_9P_MSIZE;
    c->root_fid = 0;
    c->next_fid = 1;

    if (axl_tcp_connect_timeout(host, port, NULL, 4000, &c->sock) != AXL_OK
        || c->sock == NULL) {
        axl_free(c);
        return AXL_ERR;
    }

    /* Tversion(msize, "9P2000.L") -> Rversion */
    uint8_t req[64];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TVERSION, 0xFFFF);
    axl_9p_w_u32(&w, AXL_9P_MSIZE);
    axl_9p_w_str(&w, "9P2000.L");
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RVERSION) != AXL_OK) {
        goto fail;
    }
    {   /* Rversion: msize[4] version[s] — require exact "9P2000.L". */
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t smsize = axl_9p_r_u32(&r);
        char ver[16];
        axl_9p_r_str(&r, ver, sizeof(ver));
        if (r.error || axl_strcmp(ver, "9P2000.L") != 0) {
            axl_warning("9p: server declined 9P2000.L (got '%s')", ver);
            goto fail;
        }
        if (smsize > 0 && smsize < c->msize) {
            c->msize = smsize;
        }
        if (c->msize < AXL_9P_MIN_MSIZE) {
            axl_warning("9p: server msize %u below floor %u", c->msize, AXL_9P_MIN_MSIZE);
            goto fail;
        }
    }

    /* Tattach(root_fid, NOFID, uname, aname, NONUNAME) -> Rattach */
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TATTACH, 0);
    axl_9p_w_u32(&w, c->root_fid);
    axl_9p_w_u32(&w, AXL_9P_NOFID);
    axl_9p_w_str(&w, uname);
    axl_9p_w_str(&w, aname);
    axl_9p_w_u32(&w, AXL_9P_NONUNAME);
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RATTACH) != AXL_OK) {
        goto fail;
    }

    *out = c;
    return AXL_OK;

fail:
    axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
    axl_free(c);
    return AXL_ERR;
}

void
axl_9p_disconnect(Axl9pClient *c)
{
    if (c == NULL) {
        return;
    }
    axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
    axl_free(c);
}

void
axl_9p_client_clunk(Axl9pClient *c, uint32_t fid)
{
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TCLUNK, 0);
    axl_9p_w_u32(&w, fid);
    axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RCLUNK);
}

/* Split an absolute '/'-path into components and Twalk root_fid -> new_fid.
   Returns AXL_OK with *out_fid a fresh fid pointing at path (a clone of root
   for "/" -- zero wname elements). Caller clunks *out_fid when done. */
int
axl_9p_client_walk(Axl9pClient *c, const char *path, uint32_t *out_fid)
{
    uint32_t new_fid = c->next_fid++;
    uint8_t  req[1024];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TWALK, 0);
    axl_9p_w_u32(&w, c->root_fid);
    axl_9p_w_u32(&w, new_fid);

    /* Count + emit path components (skip leading '/' and empty segments). */
    size_t nw_pos = w.len;          /* remember where nwname[2] goes */
    axl_9p_w_u16(&w, 0);            /* placeholder count, patched below */
    uint16_t nwname = 0;
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        const char *start = p;
        while (*p != '\0' && *p != '/') p++;
        size_t seg = (size_t)(p - start);
        if (seg == 0) {
            continue;
        }
        char comp[256];
        size_t clen = (seg < sizeof(comp) - 1) ? seg : sizeof(comp) - 1;
        axl_memcpy(comp, start, clen);
        comp[clen] = '\0';
        axl_9p_w_str(&w, comp);
        nwname++;
    }
    /* Patch nwname[2] little-endian. */
    req[nw_pos]     = (uint8_t)nwname;
    req[nw_pos + 1] = (uint8_t)(nwname >> 8);

    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RWALK) != AXL_OK) {
        return AXL_ERR;
    }
    /* Rwalk: nwqid[2] -- must equal nwname (full walk succeeded). A partial
       walk (0 < nwqid < nwname, e.g. a missing leaf in an existing dir) is
       NOT a wire error, but per walk(5) "newfid will represent the same file
       as fid only if nwqid == nwname": a walk the server could not complete
       leaves new_fid UNBOUND, so there is nothing to clean up here. Clunking
       it would just draw an Rlerror(EBADF) round-trip. */
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    uint16_t nwqid = axl_9p_r_u16(&r);
    if (r.error || nwqid != nwname) {
        return AXL_ERR;
    }
    *out_fid = new_fid;
    return AXL_OK;
}

/* Tlopen(fid, flags) -> Rlopen. flags: 0 = O_RDONLY. */
int
axl_9p_client_lopen(Axl9pClient *c, uint32_t fid, uint32_t flags)
{
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TLOPEN, 0);
    axl_9p_w_u32(&w, fid);
    axl_9p_w_u32(&w, flags);
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RLOPEN);
}

/* Tgetattr(fid, P9_GETATTR_BASIC) -> Rgetattr. Extracts size, mtime_sec, and
   the raw st_mode (caller tests S_IFDIR = 0040000). Any missing out-pointer is
   skipped. Returns AXL_ERR on wire error / underrun. */
int
axl_9p_client_getattr(struct Axl9pClient *c, uint32_t fid, uint64_t *out_size,
                      uint64_t *out_mtime, uint32_t *out_mode)
{
    uint8_t req[24];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TGETATTR, 0);
    axl_9p_w_u32(&w, fid);
    axl_9p_w_u64(&w, 0x000007ffULL);            /* P9_GETATTR_BASIC */
    if (w.overflow
        || axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RGETATTR) != AXL_OK) {
        return AXL_ERR;
    }
    Axl9pReader r;
    axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
    axl_9p_r_u64(&r);                             /* valid */
    for (int i = 0; i < 13; i++) axl_9p_r_u8(&r); /* qid[13] */
    uint32_t mode = axl_9p_r_u32(&r);
    axl_9p_r_u32(&r);                             /* uid */
    axl_9p_r_u32(&r);                             /* gid */
    axl_9p_r_u64(&r);                             /* nlink */
    axl_9p_r_u64(&r);                             /* rdev */
    uint64_t size = axl_9p_r_u64(&r);
    axl_9p_r_u64(&r);                             /* blksize */
    axl_9p_r_u64(&r);                             /* blocks */
    axl_9p_r_u64(&r);                             /* atime_sec */
    axl_9p_r_u64(&r);                             /* atime_nsec */
    uint64_t mtime = axl_9p_r_u64(&r);
    if (r.error) {
        return AXL_ERR;
    }
    if (out_size)  *out_size  = size;
    if (out_mtime) *out_mtime = mtime;
    if (out_mode)  *out_mode  = mode;
    return AXL_OK;
}

int
axl_9p_read_file(Axl9pClient *c, const char *path, AxlBytes **out)
{
    if (c == NULL || path == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (axl_9p_client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (axl_9p_client_lopen(c, fid, 0) != AXL_OK) {
        goto done;
    }

    /* Accumulate chunked Treads into a growable string buffer. */
    AxlString *acc = axl_string_new(NULL);
    uint64_t   offset = 0;
    uint32_t   chunk  = c->msize - 11;          /* header(7) + count(4) */
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREAD, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, chunk);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREAD) != AXL_OK) {
            axl_string_free(acc);
            goto done;
        }
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t count = axl_9p_r_u32(&r);
        /* Widen count to size_t BEFORE adding: count + 11 in uint32_t
           arithmetic can wrap for a huge server-supplied count, which would
           defeat this bounds check and let the append below run past the
           end of c->rbuf (Rread data lives at rbuf+11). */
        if (r.error || (size_t)count + 11 > c->rlen) {
            axl_string_free(acc);
            goto done;
        }
        if (count == 0) {
            break;                                          /* EOF */
        }
        axl_string_append_len(acc, (const char *)(c->rbuf + 11), count);
        offset += count;
        if (axl_string_len(acc) > AXL_9P_MAX_XFER_BYTES) {
            /* Hostile/buggy server streaming unbounded data -- hard error,
               not a size we should ever try to accumulate in one AxlBytes. */
            axl_string_free(acc);
            goto done;
        }
    }
    AxlBytes *b = axl_bytes_new(axl_string_str(acc), axl_string_len(acc));
    axl_string_free(acc);
    if (b == NULL) {
        goto done;        /* rc stays AXL_ERR */
    }
    *out = b;
    rc = AXL_OK;

done:
    axl_9p_client_clunk(c, fid);
    return rc;
}

/* Split an absolute "/a/b/c" into parent "/a/b" + name "c" (root-level "/c"
   yields parent "/"). Returns AXL_ERR if @p path has no '/' (not absolute)
   or ends in '/' (no basename). */
int
axl_9p_client_split_parent(const char *path, char *parent, size_t cap,
                            const char **name_out)
{
    const char *slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            slash = p;
        }
    }
    if (slash == NULL || slash[1] == '\0') {
        return AXL_ERR;                       /* not absolute, or no basename */
    }
    size_t plen = (size_t)(slash - path);
    if (plen == 0) {
        plen = 1;                             /* "/c" -> parent "/" */
    }
    if (plen + 1 > cap) {
        return AXL_ERR;
    }
    axl_memcpy(parent, path, plen);
    parent[plen] = '\0';
    *name_out = slash + 1;
    return AXL_OK;
}

/* Tlcreate(dfid, name, flags, mode, gid=0) -> Rlcreate. dfid becomes the
   newly created, open file. */
int
axl_9p_client_lcreate(Axl9pClient *c, uint32_t dfid, const char *name,
                      uint32_t flags, uint32_t mode)
{
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TLCREATE, 0);
    axl_9p_w_u32(&w, dfid);
    axl_9p_w_str(&w, name);
    axl_9p_w_u32(&w, flags);
    axl_9p_w_u32(&w, mode);
    axl_9p_w_u32(&w, 0);                        /* gid */
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RLCREATE);
}

int
axl_9p_write_file(Axl9pClient *c, const char *path, const void *buf, size_t len)
{
    if (c == NULL || path == NULL || (buf == NULL && len != 0)) {
        return AXL_ERR;
    }

    /* Resolve the write fid: truncate-open an existing file, else create it. */
    uint32_t fid;
    if (axl_9p_client_walk(c, path, &fid) == AXL_OK) {
        if (axl_9p_client_lopen(c, fid, AXL_9P_O_WRONLY_TRUNC) != AXL_OK) {
            axl_9p_client_clunk(c, fid);
            return AXL_ERR;
        }
    } else {
        char parent[512];
        const char *name = NULL;
        if (axl_9p_client_split_parent(path, parent, sizeof(parent), &name) != AXL_OK
            || axl_9p_client_walk(c, parent, &fid) != AXL_OK) {
            return AXL_ERR;
        }
        /* Tlcreate turns `fid` into the new open file. */
        if (axl_9p_client_lcreate(c, fid, name, AXL_9P_O_WRONLY_TRUNC, 0644) != AXL_OK) {
            axl_9p_client_clunk(c, fid);
            return AXL_ERR;
        }
    }

    /* Chunked Twrite(fid, offset, count, data). */
    int rc = AXL_OK;
    uint64_t offset = 0;
    size_t   remaining = len;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t maxchunk = c->msize - AXL_9P_TWRITE_HDR_LEN;
    while (remaining > 0) {
        uint32_t n = (remaining < maxchunk) ? (uint32_t)remaining : maxchunk;
        Axl9pWriter w;
        axl_9p_msg_begin(&w, c->wbuf, sizeof(c->wbuf), AXL_9P_TWRITE, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, n);
        axl_9p_w_bytes(&w, src, n);
        if (w.overflow
            || axl_9p_transact(c, c->wbuf, axl_9p_msg_finish(&w),
                               AXL_9P_RWRITE) != AXL_OK) {
            rc = AXL_ERR;
            break;
        }
        Axl9pReader r;
        axl_9p_r_init(&r, c->rbuf + 7, c->rlen - 7);
        uint32_t wrote = axl_9p_r_u32(&r);      /* Rwrite: count[4] */
        if (r.error || wrote == 0 || wrote > n) {
            rc = AXL_ERR;                        /* short/bad write */
            break;
        }
        offset    += wrote;
        src       += wrote;
        remaining -= wrote;
    }

    axl_9p_client_clunk(c, fid);
    return rc;
}

int
axl_9p_parse_readdir(const uint8_t *body, size_t body_len,
                     struct AxlArray *arr, uint32_t *out_dcount,
                     uint64_t *out_last_off)
{
    Axl9pReader r;
    axl_9p_r_init(&r, body, body_len);
    uint32_t dcount = axl_9p_r_u32(&r);
    if (r.error) {
        return AXL_ERR;
    }
    if (dcount == 0) {
        *out_dcount = 0;
        return AXL_OK;                /* end of directory */
    }
    if ((size_t)dcount > body_len - r.pos) {
        return AXL_ERR;               /* dcount claims more bytes than are present */
    }

    /* Each entry: qid[13] offset[8] type[1] name[s]. */
    size_t   start    = r.pos;
    uint64_t last_off = 0;
    while (r.pos - start < dcount && !r.error) {
        r.pos += AXL_9P_QID_LEN;                 /* skip qid */
        uint64_t next_off = axl_9p_r_u64(&r);    /* dirent offset cursor */
        uint8_t  dtype    = axl_9p_r_u8(&r);     /* DT_DIR=4, DT_REG=8 */
        AxlFsEntry e = {0};
        e.struct_size = sizeof(AxlFsEntry);
        e.version     = AXL_FS_ENTRY_VERSION;
        axl_9p_r_str(&r, e.name, sizeof(e.name));
        if (dtype == 4) {           /* DT_DIR */
            e.attributes |= AXL_FS_ATTR_DIRECTORY;
        }
        if (r.error) {
            return AXL_ERR;         /* entry ran past the declared dcount window */
        }
        /* Skip "." and ".." -- not surfaced as volume entries. */
        if (axl_strcmp(e.name, ".") != 0 && axl_strcmp(e.name, "..") != 0) {
            axl_array_append(arr, &e);
        }
        last_off = next_off;
    }
    if (r.error) {
        return AXL_ERR;
    }
    *out_dcount   = dcount;
    *out_last_off = last_off;
    return AXL_OK;
}

/* Join a directory path and a child name into an absolute path for a fresh
   Twalk ("/" + "hello.txt" -> "/hello.txt"; "/dir" + "a.txt" -> "/dir/a.txt").
   Used by axl_9p_list to getattr each entry. Returns false on truncation. */
static bool
join_child_path(const char *dir, const char *name, char *out, size_t cap)
{
    size_t dlen = axl_strlen(dir);
    bool   root = dlen == 0 || (dlen == 1 && dir[0] == '/');
    size_t need = (root ? 1 : dlen + 1) + axl_strlen(name) + 1;

    if (need > cap) {
        return false;
    }
    if (root) {
        out[0] = '/';
        axl_strlcpy(out + 1, name, cap - 1);
    } else {
        axl_strlcpy(out, dir, cap);
        axl_strlcat(out, "/", cap);
        axl_strlcat(out, name, cap);
    }
    return true;
}

/* Fill one listing entry's size with a fresh walk + Tgetattr (Rreaddir's wire
   dirent carries no size). Callers skip entries the dirent's dtype already
   marked as a directory -- see axl_9p_list -- but dtype is not authoritative:
   a server that reports DT_UNKNOWN (ftype-less XFS, some NFS re-exports
   behind diod/virtfs) leaves a real directory looking like a file here, so
   this also reclassifies from Rgetattr's st_mode and re-zeros the size the
   directory-skip exists to enforce. Leaves e->size at 0 on either failure,
   so a caller that keeps going never publishes a stale size. */
static StatEntryResult
stat_list_entry(
    Axl9pClient *c,      ///< connected session
    const char  *dir,    ///< directory being listed
    AxlFsEntry  *e       ///< [in,out] entry whose name is read, size filled,
                         ///< and attributes corrected if dtype lied
)
{
    char     child[512];
    uint32_t child_fid;

    if (!join_child_path(dir, e->name, child, sizeof(child))) {
        return STAT_ENTRY_LOCAL;
    }
    if (axl_9p_client_walk(c, child, &child_fid) != AXL_OK) {
        return STAT_ENTRY_REMOTE;
    }
    /* getattr publishes *out_size and *out_mode only after the whole
       Rgetattr body parses, so a failure here leaves e->size at its
       zero-initialized default. */
    uint32_t mode = 0;
    int rc = axl_9p_client_getattr(c, child_fid, &e->size, NULL, &mode);
    axl_9p_client_clunk(c, child_fid);
    if (rc != AXL_OK) {
        return STAT_ENTRY_REMOTE;
    }
    if ((mode & AXL_9P_S_IFDIR) != 0) {
        e->attributes |= AXL_FS_ATTR_DIRECTORY;
        e->size = 0;                 /* the same rule mount_get_info applies */
    }
    return STAT_ENTRY_OK;
}

int
axl_9p_list(Axl9pClient *c, const char *path, AxlArray **out)
{
    if (c == NULL || path == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (axl_9p_client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = AXL_ERR;
    if (axl_9p_client_lopen(c, fid, 0) != AXL_OK) {
        goto done;
    }

    AxlArray *arr = axl_array_new(sizeof(AxlFsEntry));
    uint64_t offset = 0;
    for (;;) {
        uint8_t req[24];
        Axl9pWriter w;
        axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREADDIR, 0);
        axl_9p_w_u32(&w, fid);
        axl_9p_w_u64(&w, offset);
        axl_9p_w_u32(&w, c->msize - 11);
        if (axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREADDIR) != AXL_OK) {
            axl_array_free(arr);
            goto done;
        }
        uint32_t dcount   = 0;
        uint64_t last_off = 0;
        if (axl_9p_parse_readdir(c->rbuf + 7, c->rlen - 7, arr, &dcount, &last_off) != AXL_OK) {
            /* Malformed Rreaddir (a lying dcount, or an entry that runs past
               it): a hard error, NOT EOF -- returning AXL_OK here would leave
               `offset` unchanged and the caller would replay the same bad
               request against the same offset forever. */
            axl_array_free(arr);
            goto done;
        }
        if (dcount == 0) {
            break;                     /* end of directory */
        }
        if (last_off <= offset) {
            /* last_off is server-controlled; if it doesn't strictly advance,
               replaying the same offset would hang forever against a
               malformed/hostile server. */
            axl_array_free(arr);
            goto done;
        }
        if (axl_array_len(arr) > AXL_9P_MAX_DIR_ENTRIES) {
            axl_array_free(arr);
            goto done;
        }
        offset = last_off;
    }

    /* Rreaddir's wire dirent is qid[13] offset[8] type[1] name[s] -- no
       size. AxlFsEntry.size promises "file size in bytes (0 for
       directories)" for every other producer (axl_file_info,
       axl_dir_read); fill it here with one Tgetattr per non-directory entry
       so axl_9p_list's entries honor the same contract. Directories are
       skipped outright: the contract says 0, and the dirent's DT_DIR bit
       (already in e->attributes) answered the question without a
       round-trip -- mount_get_info zeroes the same field for the same
       reason. An entry the dirent called non-directory still gets
       reclassified from Rgetattr's st_mode inside stat_list_entry, so a
       server that reports DT_UNKNOWN for a real directory doesn't leak a
       wrong type letter and a wrong nonzero size out of this call. */
    size_t stat_fails = 0;
    for (size_t i = 0; i < axl_array_len(arr); i++) {
        AxlFsEntry *e = (AxlFsEntry *)axl_array_get(arr, i);

        if (axl_fs_entry_is_dir(e)) {
            continue;
        }
        /* Only a REMOTE failure counts toward the limit, and a LOCAL one is
           NEUTRAL rather than a reset: a join truncation is a name this
           build cannot address, so it is neither evidence that the session
           died nor evidence that it lives. Three long names in a row must
           not abort a listing whose connection is fine; equally, they must
           not launder away two real wire failures around them. See
           StatEntryResult. */
        StatEntryResult sr = stat_list_entry(c, path, e);
        if (sr == STAT_ENTRY_LOCAL) {
            continue;
        }
        if (sr == STAT_ENTRY_OK) {
            stat_fails = 0;
            continue;
        }
        if (++stat_fails >= AXL_9P_LIST_STAT_FAIL_LIMIT) {
            /* The session, not one entry, is gone -- see the #define. */
            axl_array_free(arr);
            goto done;
        }
    }

    *out = arr;
    rc = AXL_OK;

done:
    axl_9p_client_clunk(c, fid);
    return rc;
}

int
axl_9p_mkdir(Axl9pClient *c, const char *path)
{
    if (c == NULL || path == NULL) {
        return AXL_ERR;
    }
    char parent[512];
    const char *name = NULL;
    if (axl_9p_client_split_parent(path, parent, sizeof(parent), &name) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t dfid;
    if (axl_9p_client_walk(c, parent, &dfid) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TMKDIR, 0);
    axl_9p_w_u32(&w, dfid);          /* dfid */
    axl_9p_w_str(&w, name);          /* name */
    axl_9p_w_u32(&w, 0755);          /* mode */
    axl_9p_w_u32(&w, 0);             /* gid  */
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RMKDIR);
    axl_9p_client_clunk(c, dfid);
    return rc;
}

int
axl_9p_remove(Axl9pClient *c, const char *path)
{
    if (c == NULL || path == NULL) {
        return AXL_ERR;
    }
    uint32_t fid;
    if (axl_9p_client_walk(c, path, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    uint8_t req[16];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TREMOVE, 0);
    axl_9p_w_u32(&w, fid);
    /* Tremove clunks `fid` server-side on BOTH success and error -- do NOT
       client_clunk it afterward (that would be a double-clunk / bad-fid). */
    return (w.overflow) ? AXL_ERR
         : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RREMOVE);
}

/* Copy-then-unlink for a cross-directory rename the server refused with
   EXDEV. This is what a POSIX client does, and the server's refusal is
   deliberate (a server-side move is an unbounded synchronous whole-file copy
   on its single loop), so the cost belongs on the client, where it blocks
   only its own caller.

   REFUSES an existing @p to outright, before any read or write. rename(2)'s
   permission to clobber a taken destination is only safe because the
   replacement is atomic; this fallback is not (a session drop between the
   truncating open and the last Twrite would leave a real destination
   zero-length or half-written), so it cannot inherit that permission. A
   caller that wants replace semantics removes the destination first -- at
   which point a failed copy demonstrably leaves nothing there, because they
   removed it.

   Bounded, and only for regular files:
     - a DIRECTORY is refused. Copy-then-unlink on a directory is a recursive
       tree walk with its own partial-failure semantics, which rename() does
       not have; answering AXL_ERR is honest, silently moving half a tree is
       not.
     - AXL_9P_XDEV_COPY_MAX caps it against the size getattr reports AND
       against what was actually read: the two are rechecked separately
       because a file can grow between the two calls, or a server can lie in
       Rgetattr, and the read path's own ceiling (AXL_9P_MAX_XFER_BYTES,
       256 MiB) is far looser than the 32 MiB this fallback promises.

   Failure semantics, stated because the compound operation can fail halfway:
   if the unlink fails after the copy succeeded, the destination is LEFT IN
   PLACE and AXL_ERR is returned -- the copy happened, the move did not, and
   the caller is told so rather than being handed a success for a source that
   still exists. */
static int
rename_xdev_copy(Axl9pClient *c, const char *from, const char *to)
{
    uint32_t existing_fid = 0;
    if (axl_9p_client_walk(c, to, &existing_fid) == AXL_OK) {
        axl_9p_client_clunk(c, existing_fid);
        axl_debug("9p: cross-directory rename refuses to overwrite the "
                  "existing destination %s", to);
        return AXL_ERR;
    }

    uint32_t  fid  = 0;
    uint64_t  size = 0;
    uint32_t  mode = 0;
    AxlBytes *data = NULL;

    if (axl_9p_client_walk(c, from, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = axl_9p_client_getattr(c, fid, &size, NULL, &mode);
    axl_9p_client_clunk(c, fid);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    if ((mode & AXL_9P_S_IFDIR) != 0) {
        axl_debug("9p: cross-directory rename of a directory is not "
                  "supported (server answered EXDEV)");
        return AXL_ERR;
    }
    if (size > AXL_9P_XDEV_COPY_MAX) {
        axl_debug("9p: cross-directory rename of %llu bytes exceeds the "
                  "%u-byte copy limit",
                  (unsigned long long)size,
                  (unsigned)AXL_9P_XDEV_COPY_MAX);
        return AXL_ERR;
    }
    if (axl_9p_read_file(c, from, &data) != AXL_OK) {
        return AXL_ERR;
    }
    size_t      len = 0;
    const void *buf = axl_bytes_get_data(data, &len);
    /* Re-check what was actually read, not just what Rgetattr claimed: the
       file can grow between the two calls, or a server can misreport its
       size, and axl_9p_read_file's own ceiling (256 MiB) is far looser than
       the 32 MiB this fallback promises. */
    if (len > AXL_9P_XDEV_COPY_MAX) {
        axl_debug("9p: cross-directory rename read %zu bytes, exceeding "
                  "the %u-byte copy limit; refusing the write",
                  len, (unsigned)AXL_9P_XDEV_COPY_MAX);
        axl_bytes_unref(data);
        return AXL_ERR;
    }
    rc = axl_9p_write_file(c, to, buf, len);
    axl_bytes_unref(data);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_9p_remove(c, from) != AXL_OK) {
        /* log-level: the status says "the rename failed", which is not what
           happened -- the copy SUCCEEDED and only the unlink did, so the file
           now exists at both paths. A caller acting on AXL_ERR alone will
           retry or report a failure and leave the duplicate behind; there is
           no return value that can express "half done, and here is which
           half". That is the partial-completion case warning is for. */
        axl_warning("9p: cross-directory rename copied %s to %s but could "
                    "not remove the source; both now exist", from, to);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_9p_rename(Axl9pClient *c, const char *from, const char *to)
{
    if (c == NULL || from == NULL || to == NULL) {
        return AXL_ERR;
    }
    char parent[512];
    const char *name = NULL;
    if (axl_9p_client_split_parent(to, parent, sizeof(parent), &name) != AXL_OK) {
        return AXL_ERR;
    }
    uint32_t fid, dfid;
    if (axl_9p_client_walk(c, from, &fid) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_9p_client_walk(c, parent, &dfid) != AXL_OK) {
        axl_9p_client_clunk(c, fid);
        return AXL_ERR;
    }
    uint8_t req[512];
    Axl9pWriter w;
    axl_9p_msg_begin(&w, req, sizeof(req), AXL_9P_TRENAME, 0);
    axl_9p_w_u32(&w, fid);           /* fid  (the node being renamed) */
    axl_9p_w_u32(&w, dfid);          /* dfid (new parent directory)   */
    axl_9p_w_str(&w, name);          /* name (new basename)           */
    int rc = (w.overflow) ? AXL_ERR
           : axl_9p_transact(c, req, axl_9p_msg_finish(&w), AXL_9P_RRENAME);
    /* Snapshot the errno BEFORE the clunks: each clunk is itself a transact,
       and every transact clears last_errno on entry, so reading it after them
       would always see 0 and the fallback would never fire. */
    uint32_t rename_errno = c->last_errno;
    /* Clunk BOTH fids before the fallback: it walks the same paths again and
       a server with a bounded fid table should not be made to hold four fids
       for one logical move. */
    axl_9p_client_clunk(c, dfid);
    axl_9p_client_clunk(c, fid);
    if (rc != AXL_OK && rename_errno == AXL_9P_EXDEV) {
        return rename_xdev_copy(c, from, to);
    }
    return rc;
}
