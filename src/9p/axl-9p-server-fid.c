/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server-fid.c
    Axl9pServer per-connection fid table + the qid and path-safety helpers.

    Split out of axl-9p-server.c (see axl-9p-server-internal.h for the
    three-way seam). Nothing here touches the transport, and the only wire
    codec use is s9p_qid's fixed 13-byte qid encode; otherwise this is the
    name-resolution layer the handlers in axl-9p-server-ops.c resolve a
    client's fids and path components through, and the one place that
    decides a walk cannot escape the export root. It also owns the per-fid
    OPEN state (an AxlDir* for an open directory fid, an AxlFileView* and/or
    a writable AxlStream* for an open file fid) and the single release path
    -- s9p_fid_release -- that every fid-dropping site in the server routes
    through. It used to carry view-staleness bookkeeping too; AxlFileView
    makes that best effort itself now (src/fs/axl-file-gen.h), for writes
    from anywhere in this PE image rather than only this server's own. The
    guarantee on the wire is close-to-open, not coherence -- see
    src/9p/README.md.
**/

#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-path.h>
#include <axl/axl-fs.h>
#include <axl/axl-file-view.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>
#include "axl-9p-internal.h"
#include "axl-9p-server-internal.h"

AXL_LOG_DOMAIN("9p");

// ---------------------------------------------------------------------------
// Fid table
// ---------------------------------------------------------------------------

/* Linear scan over AXL_9P_MAX_FIDS (128) entries; simple and fast enough --
   a 9P session realistically has a handful of fids live at once, and this
   is never a hot loop (once per T-message, not per byte). */
S9pFid *
s9p_fid_get(S9pConn *conn, uint32_t fid)
{
    for (size_t i = 0; i < AXL_9P_MAX_FIDS; i++) {
        if (conn->fids[i].used && conn->fids[i].fid == fid) {
            return &conn->fids[i];
        }
    }
    return NULL;
}

/* Allocate a free table slot for `fid` bound to `path`. Returns NULL (slot
   NOT allocated) if `fid` is already in use, the table is full, or `path`
   doesn't fit S9pFid.path -- callers turn any of those into Rlerror. */
S9pFid *
s9p_fid_alloc(S9pConn *conn, uint32_t fid, const char *path, bool is_dir)
{
    if (s9p_fid_get(conn, fid) != NULL) {
        return NULL;
    }
    for (size_t i = 0; i < AXL_9P_MAX_FIDS; i++) {
        S9pFid *f = &conn->fids[i];
        if (f->used) {
            continue;
        }
        if (axl_strlcpy(f->path, path, sizeof(f->path)) >= sizeof(f->path)) {
            return NULL;   /* path too long to store safely -- slot left free */
        }
        f->used   = true;
        f->fid    = fid;
        f->is_dir = is_dir;
        /* A fresh binding owns nothing yet. Every release path already
           zeroes these, so this is belt-and-braces -- but "a recycled slot
           carries no handle" is exactly the kind of invariant that is true
           until one release path forgets, and this is the one place that
           can state it locally rather than depend on all of them. */
        f->open       = false;
        f->view       = NULL;
        f->wstream    = NULL;
        f->dir        = NULL;
        f->dir_pos    = 0;
        return f;
    }
    return NULL;   /* table full */
}

/* Close whatever the fid holds open and clear the open-state fields, leaving
   the binding intact. Every handle is NULL-safe to close and every field is
   cleared unconditionally, so this is idempotent -- which matters because
   Tlopen calls it on a fid that has usually never been opened.

   Only Tlopen and Trename call this directly (re-opening a fid must not leak
   the first set of handles but must keep the fid bound to its path; a rename
   must let go of the firmware handles before the node moves under them);
   everything that DROPS a fid goes through s9p_fid_release. */
bool
s9p_fid_close_open(S9pFid *f)
{
    if (f == NULL) {
        return true;
    }
    bool ok = true;
    axl_file_view_close(f->view);
    /* Flush the write stream EXPLICITLY. axl_fclose is not a durability
       point -- it drains the AXL-side buffer through stream_drain and never
       calls the stream's flush -- and the firmware close under it cannot
       report anything, since EFI_FILE_PROTOCOL.Close is specified to return
       only EFI_SUCCESS. Without this a client that wrote and clunked with no
       Tfsync in between was answered Rclunk for bytes a full volume or
       write-protected media had dropped. */
    if (f->wstream != NULL && axl_fflush(f->wstream) != AXL_OK) {
        ok = false;
    }
    axl_fclose(f->wstream);
    axl_dir_close(f->dir);
    f->view       = NULL;
    f->wstream    = NULL;
    f->dir        = NULL;
    f->dir_pos    = 0;
    f->open       = false;
    return ok;
}

/* Give a FILE fid the handles its access mode calls for. See the header for
   the contract. */
bool
s9p_fid_open_file(S9pConn *conn, S9pFid *f, uint32_t accmode)
{
    /* Write stream FIRST, view second, and the order matters for O_RDWR.
       axl_fopen in a write mode bumps the file's generation at open (the
       mode carries CREATE, so the open itself can change the namespace).
       Opening the view first therefore left it one generation behind
       before the fid had served a single request, and its first Tread paid
       a full re-sync -- frames dropped, stream closed and reopened, file
       re-stat'd -- for a write that had not happened. Opening it after the
       stream costs nothing and starts it in step. */
    if (accmode != P9_ACC_RDONLY) {
        /* "w" is READ|WRITE|CREATE and, unlike POSIX, does NOT truncate --
           axl_fopen maps straight onto the UEFI open modes (see
           axl-stream-file.c). That is exactly what a 9P write fid wants:
           Twrite places bytes at an explicit offset, and truncation is a
           separate request (Tlopen's O_TRUNC, Tsetattr's size). */
        f->wstream = axl_fopen(f->path, "w");
        if (f->wstream == NULL) {
            return false;
        }
    }
    if (accmode != P9_ACC_WRONLY) {
        f->view = axl_file_view_open_cached(f->path, conn->srv->page_cache);
        if (f->view == NULL) {
            /* Don't leak the write stream opened above. Its flush answer is
               must-check, and here it is real: an O_RDWR open that got its
               stream and then failed on the view has a stream that was
               never written, so a flush failure is a media problem worth
               naming rather than swallowing. */
            if (!s9p_fid_close_open(f)) {
                axl_warning("9p: flush failed closing a fid whose view "
                            "would not open");
            }
            return false;
        }
    }
    f->open = true;
    return true;
}

/* Re-open the handles a fid held before a s9p_fid_close_open that has since
   turned out to be unnecessary. See the header for the contract. */
void
s9p_fid_restore_open(S9pConn *conn, S9pFid *f, bool had_view, bool had_wstream,
                     bool had_dir)
{
    if (f == NULL) {
        return;
    }
    if (had_dir) {
        f->dir  = axl_dir_open(f->path);
        f->open = (f->dir != NULL);
    } else if (had_view || had_wstream) {
        uint32_t accmode = had_view ? (had_wstream ? P9_ACC_RDWR
                                                   : P9_ACC_RDONLY)
                                    : P9_ACC_WRONLY;
        if (!s9p_fid_open_file(conn, f, accmode)) {
            axl_warning("9p: could not restore fid %u's handles after a "
                        "failed operation -- it must be re-opened",
                        (unsigned)f->fid);
        }
    }
}

/* Close `f`'s open state and free its slot. Deliberately the ONE teardown
   path: the three sites that drop a fid -- Tclunk, the Tversion session
   reset, and connection reap -- previously each open-coded a memset, which
   was correct only while a fid owned nothing but a path string. Routing all
   three here is what stops the next handler that gives a fid something to
   own from leaking it down whichever path its author didn't think of. */
bool
s9p_fid_release(S9pFid *f)
{
    if (f == NULL) {
        return true;
    }
    bool ok = s9p_fid_close_open(f);
    axl_memset(f, 0, sizeof(*f));
    return ok;
}

/* Release `fid`'s slot. No-op if `fid` isn't in the table (callers that need
   to distinguish "unknown fid" from "clunked" check s9p_fid_get first). */
bool
s9p_fid_clunk(S9pConn *conn, uint32_t fid)
{
    return s9p_fid_release(s9p_fid_get(conn, fid));
}

/* Drop every fid -- Tversion (re)starts the session and implicitly clunks
   all outstanding fids from any prior negotiation, and s9p_reap tears the
   whole table down. Walks the table rather than one blanket memset because
   a fid may hold an open AxlFileView or AxlDir: a client that renegotiates
   the msize in the middle of a directory walk (or simply disconnects
   mid-transfer) would otherwise leak every one of them. */
void
s9p_fid_reset_all(S9pConn *conn)
{
    for (size_t i = 0; i < AXL_9P_MAX_FIDS; i++) {
        /* Nobody to tell: Tversion has already discarded the session and
           s9p_reap has no peer left to answer. The handles still have to
           come down, which is what this call is for. */
        uint32_t id = conn->fids[i].fid;
        if (!s9p_fid_release(&conn->fids[i])) {
            axl_warning("9p: fid %u's write stream could not be flushed on "
                        "session teardown", (unsigned)id);
        }
    }
}

// ---------------------------------------------------------------------------
// qid + path-safety helpers
// ---------------------------------------------------------------------------

/* FNV-1a over a NUL-terminated path -- see s9p_qid for why this is what
   backs qid.path. */
static uint64_t
s9p_fnv1a(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;   /* FNV-1a 64-bit offset basis */
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= (uint64_t)*p;
        h *= 0x100000001b3ULL;            /* FNV-1a 64-bit prime */
    }
    return h;
}

/* Build a 9P qid (type[1] version[4] path[8], AXL_9P_QID_LEN bytes) for the
   node at `path` described by `e`. version is always 0 (not tracked in
   v1). path is an FNV-1a hash of the export path string -- a v1 limitation
   documented here and in the phase-4 plan: it is NOT an inode number, so a
   rename changes qid.path (two qids for "the same" underlying file across a
   rename won't match) and, in principle, two distinct paths could collide.
   Acceptable for a single-server session where paths are what fids/clients
   actually key off; a future revision could source this from a real AxlFs
   inode/index if one becomes available. */
void
s9p_qid(const AxlFsEntry *e, const char *path, uint8_t out[AXL_9P_QID_LEN])
{
    Axl9pWriter w;
    axl_9p_w_init(&w, out, AXL_9P_QID_LEN);
    axl_9p_w_u8(&w, axl_fs_entry_is_dir(e) ? 0x80u : 0x00u);
    axl_9p_w_u32(&w, 0);
    axl_9p_w_u64(&w, s9p_fnv1a(path));
}

/* True if `comp` is safe to treat as one simple path segment: non-empty, not
   truncated against `cap` (axl_9p_r_str's return value is the ACTUAL on-wire
   length; if that's >= cap, the copy was truncated and `comp` is not the
   real component), no embedded NUL (a wire string with a NUL before its
   declared end copies cleanly but desyncs strlen(comp) from wire_n), and
   free of BOTH path separators AxlFs recognizes -- '/' AND '\\' (see
   is_sep() in axl-path.c). Rejecting only '/' would leave a hole: a
   component like "..\\..\\secret" contains no '/' but axl_path_join/
   axl_file_info would still treat the embedded '\\' as a real separator,
   smuggling a multi-level escape past a slash-only check. */
bool
s9p_comp_is_safe(const char *comp, size_t wire_n, size_t cap)
{
    return wire_n > 0 && wire_n < cap
        && axl_strlen(comp) == wire_n
        && axl_strchr(comp, '/') == NULL
        && axl_strchr(comp, '\\') == NULL;
}

/* Join `base` (a path already known to be within the export root) + `name`
   (a single component already validated by s9p_comp_is_safe) into out[cap].
   Delegates to axl_path_join so the POSIX '/' vs UEFI volume "fs0:\\"
   separator convention is handled exactly the way the rest of AxlFs handles
   it. Returns false (out left undefined) if the joined path doesn't fit
   `cap`, or on allocation failure. */
bool
s9p_path_append(char *out, size_t cap, const char *base, const char *name)
{
    char *joined = axl_path_join(base, name);
    if (joined == NULL) {
        return false;
    }
    size_t n = axl_strlcpy(out, joined, cap);
    axl_free(joined);
    return n < cap;
}

/* Move `path` up one directory using axl_path_get_dirname (separator-aware,
   so it works for both "/export/sub" and "fs0:\\sub" root conventions).
   Refuses to go above `root` (the server's export root, already normalized
   at axl_9p_server_new time to carry no trailing separator -- see that
   function's comment -- so it textually matches a dirname() result at the
   top of the tree). Returns false (leaving `path` untouched) if `path`
   already equals `root`, or if the result doesn't fit `cap`. */
bool
s9p_path_up(const char *root, char *path, size_t cap)
{
    if (axl_strcmp(path, root) == 0) {
        return false;   /* already at the export root -- ".." would escape it */
    }
    char *parent = axl_path_get_dirname(path);
    if (parent == NULL) {
        return false;
    }
    size_t n = axl_strlcpy(path, parent, cap);
    axl_free(parent);
    return n < cap;
}
