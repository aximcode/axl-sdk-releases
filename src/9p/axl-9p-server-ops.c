/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server-ops.c
    Axl9pServer dispatch + the session / navigation / metadata handlers.

    Split out of axl-9p-server.c, then split again along the ops seam (see
    axl-9p-server-internal.h). What is left here is s9p_dispatch -- the
    routing table every T-message enters through -- and the handlers that
    neither move file data nor change the tree: Tversion, Tattach, Twalk,
    Tclunk, Tgetattr. The handlers that work through an open fid handle live
    in axl-9p-server-io-ops.c, and the ones that rewrite the namespace in
    axl-9p-server-ns-ops.c.

    Every handler answers exactly one request and returns whether the
    connection survived its reply -- see s9p_send_reply in axl-9p-server.c
    for why that answer is must-check rather than assumed.
**/

#include <axl/axl-str.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>
#include "axl-9p-internal.h"
#include "axl-9p-server-internal.h"

AXL_LOG_DOMAIN("9p");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* Rwalk's fixed prefix: size[4] type[1] tag[2] nwqid[2]. Everything after it
   is qid[13] * nwqid, so (msize - this) / AXL_9P_QID_LEN is the most
   components one Twalk can be answered with. */
#define AXL_9P_RWALK_HDR_LEN  9u

/* Tgetattr's request_mask/valid P9_GETATTR_BASIC bitmask -- the same value
   axl_9p_client_getattr (axl-9p-client.c) requests; this server always
   fills (and reports valid for) exactly this basic field set. */
#define P9_GETATTR_BASIC  0x00000000000007ffULL

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

/* Tversion: msize[4] version[s] -> Rversion: msize[4] version[s].
   Negotiates msize down to min(client, AXL_9P_SERVER_MAX_MSIZE), growing
   rbuf/txbuf to match (never shrinking). TWO things get the session refused
   with Rversion(client_msize, "unknown") -- the spec's "I can't speak this"
   reply, after which a compliant client gives up:
     - a version the client didn't spell "9P2000...";
     - an msize below AXL_9P_MIN_MSIZE. There is no legal way to answer that
       one: the spec forbids replying with a LARGER msize than requested, and
       accepting the client's value would leave conn->msize at (possibly) 0,
       which underflows every later "msize minus header" read/write chunk
       computation into a huge count. Refusing is the only correct answer, so
       it shares the single refusal path rather than inventing a second.
   Echoing client_msize in the refusal is safe by construction (never larger
   than what the client asked for) and leaves conn->msize untouched at the
   last value the connection successfully negotiated -- which is never 0 and
   never below the floor, since it starts at AXL_9P_MSIZE. Either way,
   Tversion (re)starts the session: every fid from a prior negotiation is
   dropped first.

   Like every handler, returns whether `conn` survived its reply (see
   s9p_send_reply). */
AXL_WARN_UNUSED static bool
s9p_handle_tversion(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t client_msize = axl_9p_r_u32(r);
    char     version[32];
    axl_9p_r_str(r, version, sizeof(version));
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    s9p_fid_reset_all(conn);

    uint32_t negotiated = 0;
    if (!axl_str_has_prefix(version, "9P2000")
        || !axl_9p_negotiate_msize(client_msize, AXL_9P_SERVER_MAX_MSIZE,
                                   &negotiated)) {
        Axl9pWriter w;
        if (!s9p_reply_begin(conn, &w, AXL_9P_RVERSION, tag)) {
            return false;
        }
        axl_9p_w_u32(&w, client_msize);
        axl_9p_w_str(&w, "unknown");
        return s9p_send_reply(conn, &w);
    }

    if (negotiated > conn->rcap || negotiated > conn->txcap) {
        /* Passing `r` is what keeps this handler's cursor valid across the
           axl_realloc that may move rbuf -- the grow rebases it (see that
           function). */
        if (!s9p_conn_grow_buffers(conn, negotiated, r)) {
            axl_warning("9p: msize grow to %u failed (allocation failure)",
                       (unsigned)negotiated);
        }
    }
    /* Whether or not the grow above fully succeeded, never report/use a
       msize bigger than either buffer can actually hold. Both start at
       AXL_9P_MSIZE and only ever grow, so this can never push the result
       back under AXL_9P_MIN_MSIZE. */
    size_t cap = (conn->rcap < conn->txcap) ? conn->rcap : conn->txcap;
    if (negotiated > cap) {
        negotiated = (uint32_t)cap;
    }
    conn->msize = negotiated;

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RVERSION, tag)) {
        return false;
    }
    axl_9p_w_u32(&w, negotiated);
    axl_9p_w_str(&w, "9P2000.L");
    return s9p_send_reply(conn, &w);
}

/* Tattach: fid[4] afid[4] uname[s] aname[s] n_uname[4] -> Rattach: qid[13].
   v1 is single-tenant: every attach binds `fid` to the whole export root
   regardless of aname/uname/n_uname (read but otherwise ignored -- there is
   only one tree to attach to). If the root itself fails to stat (e.g. an
   empty/edge-case export), still attach with a synthetic directory qid
   rather than failing the mount outright. */
AXL_WARN_UNUSED static bool
s9p_handle_tattach(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid = axl_9p_r_u32(r);
    char     uname[64];
    char     aname[256];
    /* afid / uname / aname / n_uname are read purely to advance the wire
       cursor (and so a truncated body still trips r->error below) -- v1 has
       exactly one tree and no auth, so their values carry no meaning here. */
    axl_9p_r_u32(r);                            /* afid */
    axl_9p_r_str(r, uname, sizeof(uname));
    axl_9p_r_str(r, aname, sizeof(aname));
    axl_9p_r_u32(r);                            /* n_uname */
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    if (s9p_fid_get(conn, fid) != NULL) {
        /* fid already in use */
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    AxlFsEntry root_entry;
    bool have_root = (axl_file_info(conn->srv->root, &root_entry) == AXL_OK);
    bool root_is_dir = !have_root || axl_fs_entry_is_dir(&root_entry);

    if (s9p_fid_alloc(conn, fid, conn->srv->root, root_is_dir) == NULL) {
        return s9p_send_rlerror(conn, tag, P9_ENOMEM);
    }

    uint8_t qidbuf[AXL_9P_QID_LEN];
    if (have_root) {
        s9p_qid(&root_entry, conn->srv->root, qidbuf);
    } else {
        AxlFsEntry synthetic = {0};
        synthetic.attributes = AXL_FS_ATTR_DIRECTORY;
        s9p_qid(&synthetic, conn->srv->root, qidbuf);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RATTACH, tag)) {
        return false;
    }
    axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
    return s9p_send_reply(conn, &w);
}

/* Twalk: fid[4] newfid[4] nwname[2] wname[s]* -> Rwalk: nwqid[2] qid[13]*.
   Resolves each component from fid's path in turn (rejecting ".." past the
   root, a non-existent component, or a walk into a non-directory) and
   returns a qid per successfully-walked component.

   Binding follows walk(5) exactly: newfid is affected ONLY by a walk that
   completed every component (nwqid == nwname). A partial walk is not a wire
   error -- it still replies Rwalk with the qids actually walked -- but it
   leaves newfid unbound, so a client must not (and ours no longer does)
   clunk it afterwards. A fully-failed walk (zero of nwname > 0 resolved) is
   Rlerror(ENOENT).

   nwname is bounded by what the reply can carry within the negotiated msize
   rather than by a fixed element cap: our own axl_9p_client_walk sends every
   component of a path in ONE Twalk without batching, so any arbitrary cap
   (Plan 9's traditional MAXWELEM of 16, say) would make a deep path fail
   against our own server. The qids stream straight into the reply as they
   are walked, so nothing needs a fixed-size scratch array either. */
AXL_WARN_UNUSED static bool
s9p_handle_twalk(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid    = axl_9p_r_u32(r);
    uint32_t newfid = axl_9p_r_u32(r);
    uint16_t nwname = axl_9p_r_u16(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    /* conn->msize is never below AXL_9P_MIN_MSIZE (512), so this can neither
       underflow nor land on a useless zero bound. */
    if (nwname > (conn->msize - AXL_9P_RWALK_HDR_LEN) / AXL_9P_QID_LEN) {
        /* reply couldn't fit msize */
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    S9pFid *src = s9p_fid_get(conn, fid);
    if (src == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (newfid == fid && nwname != 0) {
        /* v1 restriction (phase-4 plan): walk-in-place only allowed as the
           zero-component "clone fid" case. */
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    if (newfid != fid && s9p_fid_get(conn, newfid) != NULL) {
        /* newfid already in use */
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    char cur[sizeof(src->path)];
    axl_strlcpy(cur, src->path, sizeof(cur));

    AxlFsEntry cur_entry = {0};
    if (nwname > 0 && axl_file_info(cur, &cur_entry) != AXL_OK) {
        /* fid's own node no longer resolves (e.g. removed after being
           walked to) -- nothing can walk from here. */
        return s9p_send_rlerror(conn, tag, P9_ENOENT);
    }

    /* Build the reply as the walk proceeds: nwqid[2] is reserved here and
       back-patched once the loop settles on a count. Safe to start writing
       txbuf now -- s9p_reply_begin has established that no send is in flight
       over it -- and every error exit below simply re-begins the buffer as
       an Rlerror, which is likewise safe only because nothing has been sent
       in between. */
    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RWALK, tag)) {
        return false;
    }
    size_t   nwqid_pos = w.len;
    uint16_t nwqid     = 0;
    axl_9p_w_u16(&w, 0);

    for (uint16_t i = 0; i < nwname; i++) {
        char   comp[256];
        size_t wire_n = axl_9p_r_str(r, comp, sizeof(comp));
        if (r->error) {
            /* Truncated request body -- nothing has been bound yet, so it's
               safe to hard-error the whole message rather than reply with a
               partial walk built on garbage. */
            return s9p_send_rlerror(conn, tag, P9_EINVAL);
        }
        if (!s9p_comp_is_safe(comp, wire_n, sizeof(comp))) {
            break;   /* malformed component -- ordinary partial-walk failure */
        }

        char next[sizeof(cur)];
        if (axl_strcmp(comp, "..") == 0) {
            axl_strlcpy(next, cur, sizeof(next));
            if (!s9p_path_up(conn->srv->root, next, sizeof(next))) {
                break;   /* would escape the export root */
            }
        } else if (axl_strcmp(comp, ".") == 0) {
            axl_strlcpy(next, cur, sizeof(next));
        } else {
            if (!axl_fs_entry_is_dir(&cur_entry)) {
                break;   /* can't walk into a non-directory */
            }
            if (!s9p_path_append(next, sizeof(next), cur, comp)) {
                break;   /* path too long */
            }
        }

        AxlFsEntry next_entry;
        if (axl_file_info(next, &next_entry) != AXL_OK) {
            break;   /* component doesn't exist (or vanished mid-walk) */
        }
        axl_strlcpy(cur, next, sizeof(cur));
        cur_entry = next_entry;

        uint8_t qidbuf[AXL_9P_QID_LEN];
        s9p_qid(&cur_entry, cur, qidbuf);
        axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
        nwqid++;
    }

    if (nwqid == 0 && nwname > 0) {
        return s9p_send_rlerror(conn, tag, P9_ENOENT);
    }

    /* walk(5): "If the first element cannot be walked ... an error is
       returned. Otherwise ... newfid will represent the same file as fid
       only if nwqid == nwname." A partial walk leaves newfid untouched. */
    if (nwqid == nwname) {
        bool is_dir = (nwname == 0) ? src->is_dir
                                    : axl_fs_entry_is_dir(&cur_entry);
        if (newfid == fid) {
            axl_strlcpy(src->path, cur, sizeof(src->path));
            src->is_dir = is_dir;
        } else if (s9p_fid_alloc(conn, newfid, cur, is_dir) == NULL) {
            return s9p_send_rlerror(conn, tag, P9_ENOMEM);
        }
    }

    axl_9p_w_patch_u16(&w, nwqid_pos, nwqid);
    return s9p_send_reply(conn, &w);
}

/* Tclunk: fid[4] -> Rclunk (empty body). Unknown fid -> Rlerror(EBADF). */
AXL_WARN_UNUSED static bool
s9p_handle_tclunk(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    if (s9p_fid_get(conn, fid) == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    /* The fid is released either way -- 9P clunks it even when the reply is
       an error, and the client considers it gone the moment it asks. But a
       client that wrote and clunked WITHOUT an explicit Tfsync has no other
       moment to learn its bytes never reached the media, so a failed final
       flush is answered Rlerror(EIO) rather than a mute Rclunk. */
    if (!s9p_fid_clunk(conn, fid)) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RCLUNK, tag)) {
        return false;
    }
    return s9p_send_reply(conn, &w);
}

/* Tgetattr: fid[4] request_mask[8] -> Rgetattr: valid[8] qid[13] mode[4]
   uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8]
   atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8]
   btime_sec[8] btime_nsec[8] gen[8] data_version[8] (153-byte fixed body --
   8+13+4+4+4 plus fifteen 8-byte fields; matches both 9P2000.L's 160-byte
   Rgetattr and axl_9p_client_getattr's read order exactly, and measured on
   the wire by test-9p-server-qemu.sh's `GETATTR body=153`). Only size/mtime are
   tracked by AxlFsEntry; everything else this server doesn't track is
   reported as zero, with `valid` masked down to P9_GETATTR_BASIC so a
   client can tell what's real. */
AXL_WARN_UNUSED static bool
s9p_handle_tgetattr(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid          = axl_9p_r_u32(r);
    uint64_t request_mask = axl_9p_r_u64(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    AxlFsEntry e;
    if (axl_file_info(f->path, &e) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_ENOENT);
    }

    uint8_t  qidbuf[AXL_9P_QID_LEN];
    s9p_qid(&e, f->path, qidbuf);
    uint32_t st_mode = axl_fs_entry_is_dir(&e) ? 0040755u : 0100644u;
    uint64_t blocks  = (e.size + 511u) / 512u;

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RGETATTR, tag)) {
        return false;
    }
    axl_9p_w_u64(&w, request_mask & P9_GETATTR_BASIC);   /* valid */
    axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
    axl_9p_w_u32(&w, st_mode);
    axl_9p_w_u32(&w, 0);        /* uid */
    axl_9p_w_u32(&w, 0);        /* gid */
    axl_9p_w_u64(&w, 1);        /* nlink */
    axl_9p_w_u64(&w, 0);        /* rdev */
    axl_9p_w_u64(&w, e.size);
    axl_9p_w_u64(&w, 512);      /* blksize */
    axl_9p_w_u64(&w, blocks);
    axl_9p_w_u64(&w, 0); axl_9p_w_u64(&w, 0);   /* atime_sec, atime_nsec */
    axl_9p_w_u64(&w, e.mtime_unix); axl_9p_w_u64(&w, 0);   /* mtime_sec, mtime_nsec */
    axl_9p_w_u64(&w, 0); axl_9p_w_u64(&w, 0);   /* ctime_sec, ctime_nsec */
    axl_9p_w_u64(&w, 0); axl_9p_w_u64(&w, 0);   /* btime_sec, btime_nsec */
    axl_9p_w_u64(&w, 0); axl_9p_w_u64(&w, 0);   /* gen, data_version */
    return s9p_send_reply(conn, &w);
}

// ---------------------------------------------------------------------------
// s9p_dispatch -- decode the header and reply
// ---------------------------------------------------------------------------

/* True if a message type can change the exported tree or a node in it. See
   the header for why Tlopen and Tfsync are deliberately absent, and why
   this is a testable predicate rather than a case list inlined below. */
bool
s9p_type_is_mutating(uint8_t type)
{
    switch (type) {
    case AXL_9P_TLCREATE:
    case AXL_9P_TWRITE:
    case AXL_9P_TMKDIR:
    case AXL_9P_TREMOVE:
    case AXL_9P_TRENAME:
    case AXL_9P_TSETATTR:
        return true;
    default:
        return false;
    }
}

/* Every 9P2000.L message type this server implements now has a handler; the
   blanket Rlerror(EPROTO) is left for the types it does not (Tauth,
   Tsymlink, Tlink, Tstatfs, Txattr*, ...).

   The message is ALWAYS the first `len` bytes of conn->rbuf -- taking a
   length rather than a (base, len) pair makes that an invariant of the
   signature rather than of the caller, which is what lets a handler rebase
   its reader after a realloc moves rbuf (see s9p_handle_tversion).

   Returns whether `conn` survived the message -- every path either replies
   (and propagates s9p_send_reply's answer up from the handler) or reaps.
   s9p_pump stops draining on false; see s9p_send_reply for why that answer
   is must-check rather than re-derived from conn->active. */
bool
s9p_dispatch(S9pConn *conn, size_t len)
{
    Axl9pReader r;
    uint32_t    size;
    uint8_t     type;
    uint16_t    tag;

    axl_9p_r_init(&r, conn->rbuf, len);
    if (!axl_9p_msg_header(&r, &size, &type, &tag)) {
        /* Unreachable given the framing loop's invariant (len >= 7 is
           checked before dispatch is ever called) -- defense in depth. */
        s9p_reap(conn);
        return false;
    }

    /* The read-only gate, HERE rather than in each handler: one place that
       cannot be forgotten by the next handler someone adds, and it fires
       before a single filesystem call -- before the name is even resolved,
       so a --ro export cannot be probed for what exists by watching which
       errno comes back. */
    if (conn->srv->read_only && s9p_type_is_mutating(type)) {
        return s9p_send_rlerror(conn, tag, P9_EROFS);
    }

    switch (type) {
    case AXL_9P_TVERSION:
        return s9p_handle_tversion(conn, &r, tag);
    case AXL_9P_TATTACH:
        return s9p_handle_tattach(conn, &r, tag);
    case AXL_9P_TWALK:
        return s9p_handle_twalk(conn, &r, tag);
    case AXL_9P_TCLUNK:
        return s9p_handle_tclunk(conn, &r, tag);
    case AXL_9P_TGETATTR:
        return s9p_handle_tgetattr(conn, &r, tag);
    case AXL_9P_TLOPEN:
        return s9p_handle_tlopen(conn, &r, tag);
    case AXL_9P_TREAD:
        return s9p_handle_tread(conn, &r, tag);
    case AXL_9P_TREADDIR:
        return s9p_handle_treaddir(conn, &r, tag);
    case AXL_9P_TWRITE:
        return s9p_handle_twrite(conn, &r, tag);
    case AXL_9P_TFSYNC:
        return s9p_handle_tfsync(conn, &r, tag);
    case AXL_9P_TLCREATE:
        return s9p_handle_tlcreate(conn, &r, tag);
    case AXL_9P_TMKDIR:
        return s9p_handle_tmkdir(conn, &r, tag);
    case AXL_9P_TREMOVE:
        return s9p_handle_tremove(conn, &r, tag);
    case AXL_9P_TRENAME:
        return s9p_handle_trename(conn, &r, tag);
    case AXL_9P_TSETATTR:
        return s9p_handle_tsetattr(conn, &r, tag);
    default:
        return s9p_send_rlerror(conn, tag, P9_EPROTO);
    }
}
