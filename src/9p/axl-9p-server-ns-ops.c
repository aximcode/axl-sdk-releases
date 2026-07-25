/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server-ns-ops.c
    Axl9pServer handlers that rewrite the NAMESPACE or a node's metadata.

    Split out of axl-9p-server-ops.c (see axl-9p-server-internal.h for the
    seam): Tlcreate, Tmkdir, Tremove, Trename, Tsetattr. Everything here
    resolves a wire-supplied NAME against a fid's directory and then changes
    the tree; nothing here moves file data (that is
    axl-9p-server-io-ops.c). This is exactly the set the read-only gate
    refuses, which is why s9p_type_is_mutating lists these five plus Twrite
    and nothing else.

    Every name that arrives here goes through the SAME two guards a Twalk
    component does -- s9p_comp_is_safe (non-empty, not truncated, no
    embedded NUL, and free of BOTH separators AxlFs recognizes) and
    s9p_path_append (which builds only downward from a path already known
    to be inside the export root). There is deliberately no second,
    parallel check: a client cannot reach a path a walk could not have
    reached.

    Every handler answers exactly one request and returns whether the
    connection survived its reply -- see s9p_send_reply in axl-9p-server.c
    for why that answer is must-check rather than assumed.
**/

#include <axl/axl-str.h>
#include <axl/axl-path.h>
#include <axl/axl-mem.h>
#include <axl/axl-fs.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>
#include "axl-9p-internal.h"
#include "axl-9p-server-internal.h"

AXL_LOG_DOMAIN("9p");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* Tsetattr `valid` bits (9P2000.L). SIZE (0x8) is the only one this server
   acts on; MODE (0x1), UID (0x2), GID (0x4) and the ATIME/MTIME bits above
   them are accepted as no-ops -- see s9p_handle_tsetattr for why. */
#define P9_SETATTR_SIZE   0x00000008u

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Resolve `dfid` + a wire-supplied `name` into a full export path.

   The one funnel every namespace handler enters through, so the guard set
   is stated once: the fid must exist, must still BE a directory (checked
   against the fresh stat the caller then reuses -- a fid walked to a
   directory that has since become a file must not be created into), and
   the name must survive s9p_comp_is_safe and fit S9pFid.path.

   On failure writes the Rlerror code to answer with through `out_ecode`
   and returns false, so the caller is a single `return
   s9p_send_rlerror(...)` rather than five scattered ones with five chances
   to pick a wrong errno. */
AXL_WARN_UNUSED static bool
s9p_resolve_child(S9pConn *conn, uint32_t dfid, const char *name,
                  size_t name_wire_n, size_t name_cap, char *out,
                  size_t out_cap, uint32_t *out_ecode)
{
    *out_ecode = P9_EIO;

    S9pFid *d = s9p_fid_get(conn, dfid);
    if (d == NULL) {
        *out_ecode = P9_EBADF;
        return false;
    }
    AxlFsEntry de;
    if (axl_file_info(d->path, &de) != AXL_OK) {
        *out_ecode = P9_ENOENT;
        return false;
    }
    if (!axl_fs_entry_is_dir(&de)) {
        *out_ecode = P9_ENOTDIR;
        return false;
    }
    if (!s9p_comp_is_safe(name, name_wire_n, name_cap)) {
        *out_ecode = P9_EINVAL;
        return false;
    }
    if (axl_strcmp(name, ".") == 0 || axl_strcmp(name, "..") == 0) {
        /* Legal Twalk components, never legal names to CREATE or rename
           ONTO: both already name a node that exists, so honouring them
           would aim a mutation at a directory the client only meant to
           traverse. s9p_comp_is_safe deliberately passes them (the
           root-escape guard is s9p_path_up's job, on the walk path), so
           they are refused here, where the field means "name of a node
           being made" rather than "step of a path". */
        *out_ecode = P9_EINVAL;
        return false;
    }
    if (!s9p_path_append(out, out_cap, d->path, name)) {
        *out_ecode = P9_EINVAL;   /* result too long to store safely */
        return false;
    }
    return true;
}

bool
s9p_dir_is_empty(const char *path)
{
    AxlDir *dir = axl_dir_open(path);
    if (dir == NULL) {
        return false;
    }
    bool       empty = true;
    AxlFsEntry e;
    while (axl_dir_read(dir, &e)) {
        /* A UEFI FAT directory carries real on-disk "." and ".." entries.
           Counting them would make EVERY directory look non-empty and turn
           rmdir into a permanent ENOTEMPTY. */
        if (axl_strcmp(e.name, ".") == 0 || axl_strcmp(e.name, "..") == 0) {
            continue;
        }
        empty = false;
        break;
    }
    axl_dir_close(dir);
    return empty;
}

bool
s9p_setattr_size_ok(uint64_t new_size, uint64_t cur_size)
{
    if (new_size <= cur_size) {
        return true;   /* a shrink or a no-op moves no data */
    }
    return (new_size - cur_size) <= (uint64_t)AXL_9P_SERVER_MAX_GROW;
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

/* Tlcreate: dfid[4] name[s] flags[4] mode[4] gid[4] -> Rlcreate: qid[13]
   iounit[4].

   Per 9P2000.L, Tlcreate REBINDS dfid: the fid that named the parent
   directory comes back naming the newly created, already-open file. That is
   why the parent path is captured before anything is created -- after the
   rebind there is no fid left pointing at the directory -- and why the
   rebind is staged and committed rather than done in place: a failed
   request must leave dfid meaning what it meant before, and must not leave
   the created file behind either.

   An existing name is always EEXIST, i.e. O_EXCL is effectively always
   applied. That is not a shortcut: a Linux client only sends Tlcreate after
   a lookup came back negative, so reaching this handler with the name
   already taken means either a race or a client inventing requests, and
   silently opening the existing file is the wrong answer to both. Our own
   axl_9p_write_file agrees -- it walks first and only creates when the walk
   failed.

   `mode` and `gid` are read and ignored: AxlFs has no POSIX permission or
   ownership model to apply them to, and reporting success for a mode that
   was never set is the lesser lie than refusing every create. */
bool
s9p_handle_tlcreate(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t dfid = axl_9p_r_u32(r);
    char     name[256];
    size_t   wire_n = axl_9p_r_str(r, name, sizeof(name));
    uint32_t flags  = axl_9p_r_u32(r);
    axl_9p_r_u32(r);                            /* mode -- no POSIX model */
    axl_9p_r_u32(r);                            /* gid  -- no POSIX model */
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    char     path[AXL_9P_PATH_MAX];
    uint32_t ecode = P9_EIO;
    if (!s9p_resolve_child(conn, dfid, name, wire_n, sizeof(name), path,
                           sizeof(path), &ecode)) {
        return s9p_send_rlerror(conn, tag, ecode);
    }

    AxlFsEntry existing;
    if (axl_file_info(path, &existing) == AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EEXIST);
    }

    /* axl_fopen("w") is READ|WRITE|CREATE against a name proved absent
       above, so this both creates the file and yields the handle the fid
       will keep. Failing here is a create failure, not an open failure --
       a full volume, a read-only medium, or a name FAT cannot represent. */
    AxlStream *created = axl_fopen(path, "w");
    if (created == NULL) {
        /* A full volume, a physically read-only medium and a name FAT
           cannot represent all arrive here as the same NULL, so EIO is the
           only answer that isn't a guess. ENOSPC would be the more useful
           one when it is true and a misdiagnosis when it is not. */
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }
    axl_fclose(created);

    AxlFsEntry e;
    if (axl_file_info(path, &e) != AXL_OK) {
        axl_file_delete(path);
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    S9pFid *d = s9p_fid_get(conn, dfid);
    if (d == NULL) {
        axl_file_delete(path);
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }

    /* PREPARE, then COMMIT. Everything fallible happens against a scratch
       fid, and dfid is only rebound once nothing can fail -- because a
       failed 9P request must leave the fid it names UNAFFECTED, and this
       one rebinds dfid from "the parent directory" to "the new file". A
       handler that mutated first and errored second would hand the client
       back an Rlerror while silently changing what its fid meant, so a
       retry would operate on something else entirely. The scratch fid
       carries only the path s9p_fid_open_file needs; the table is never
       touched until the commit. */
    S9pFid staged;
    axl_memset(&staged, 0, sizeof(staged));
    if (axl_strlcpy(staged.path, path, sizeof(staged.path))
            >= sizeof(staged.path)) {
        /* Unreachable -- `path` was built into a buffer of the same size --
           but checked before the create is committed rather than after. */
        axl_file_delete(path);
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    /* A create is a write by definition, so a create that asked for no
       write access still gets a writable stream -- otherwise the very next
       Twrite on the rebound fid would answer EBADF. */
    uint32_t accmode = flags & P9_O_ACCMODE;
    if (accmode == P9_ACC_RDONLY) {
        accmode = P9_ACC_RDWR;
    }
    if (!s9p_fid_open_file(conn, &staged, accmode)) {
        /* Undo the create too: the client is being told this request
           failed, so it must not leave a zero-byte file behind. */
        axl_file_delete(path);
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    /* Commit. Nothing below can fail: `d` names a DIRECTORY, and a
       directory fid never holds a write stream, so its close has nothing
       to flush. */
    if (!s9p_fid_close_open(d)) {
        axl_warning("9p: flush failed closing the directory fid Tlcreate "
                    "reuses");
    }
    axl_strlcpy(d->path, staged.path, sizeof(d->path));
    d->is_dir     = false;
    d->view       = staged.view;
    d->wstream    = staged.wstream;
    d->open       = true;

    uint8_t qidbuf[AXL_9P_QID_LEN];
    s9p_qid(&e, path, qidbuf);

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RLCREATE, tag)) {
        return false;
    }
    axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
    axl_9p_w_u32(&w, 0);   /* iounit: none of our own -- chunk at msize */
    return s9p_send_reply(conn, &w);
}

/* Tmkdir: dfid[4] name[s] mode[4] gid[4] -> Rmkdir: qid[13].

   Unlike Tlcreate this does NOT rebind dfid -- mkdir(2) leaves the parent
   descriptor alone, and 9P2000.L follows it.

   axl_dir_mkdir is deliberately idempotent (it succeeds on an existing
   directory), which is the wrong answer for a client that needs to tell
   "I made it" from "it was already there", so an existing name is rejected
   with EEXIST before the call rather than after. */
bool
s9p_handle_tmkdir(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t dfid = axl_9p_r_u32(r);
    char     name[256];
    size_t   wire_n = axl_9p_r_str(r, name, sizeof(name));
    axl_9p_r_u32(r);                            /* mode -- no POSIX model */
    axl_9p_r_u32(r);                            /* gid  -- no POSIX model */
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    char     path[AXL_9P_PATH_MAX];
    uint32_t ecode = P9_EIO;
    if (!s9p_resolve_child(conn, dfid, name, wire_n, sizeof(name), path,
                           sizeof(path), &ecode)) {
        return s9p_send_rlerror(conn, tag, ecode);
    }

    AxlFsEntry existing;
    if (axl_file_info(path, &existing) == AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EEXIST);
    }
    if (axl_dir_mkdir(path) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    AxlFsEntry e;
    if (axl_file_info(path, &e) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    uint8_t qidbuf[AXL_9P_QID_LEN];
    s9p_qid(&e, path, qidbuf);

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RMKDIR, tag)) {
        return false;
    }
    axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
    return s9p_send_reply(conn, &w);
}

/* Tremove: fid[4] -> Rremove (empty body).

   Tremove ALWAYS clunks the fid, on success and on failure alike -- our own
   axl_9p_remove relies on that and deliberately does not clunk afterwards,
   so a path that skipped the clunk would leave the client's fid allocator
   and the server's table permanently out of step.

   The clunk happens BEFORE the delete, not after: the fid may be holding an
   open AxlFileView or write AxlStream on the very node being removed, and a
   UEFI FAT driver will not delete a file another handle still has open.
   Releasing first is what makes "open it, write it, remove it" work.

   The export root is refused outright: a client that attached and
   immediately sent Tremove would otherwise ask the server to delete the
   volume it is exporting. */
bool
s9p_handle_tremove(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }

    /* Everything needed after the clunk, copied out first: `f` is dead the
       moment the slot is released. */
    char path[sizeof(f->path)];
    axl_strlcpy(path, f->path, sizeof(path));

    if (axl_strcmp(path, conn->srv->root) == 0) {
        /* Still clunks -- Tremove always does. An unflushed write stream is
           moot here: the reply is already an error and nothing was removed. */
        if (!s9p_fid_clunk(conn, fid)) {
            axl_warning("9p: unflushed write stream on a refused Tremove");
        }
        return s9p_send_rlerror(conn, tag, P9_EPERM);
    }

    AxlFsEntry e;
    bool have  = (axl_file_info(path, &e) == AXL_OK);
    bool isdir = have && axl_fs_entry_is_dir(&e);

    /* An unflushed write stream on the fid being REMOVED is not worth
       reporting: the file it would have reached is about to stop existing,
       so there is no data to preserve and no durability claim to correct. */
    if (!s9p_fid_clunk(conn, fid)) {
        axl_debug("9p: unflushed write stream on a fid being removed");
    }
    if (!have) {
        return s9p_send_rlerror(conn, tag, P9_ENOENT);
    }
    if (isdir) {
        /* axl_dir_rmdir collapses "not empty" into the same AXL_ERR as a
           real I/O failure, and a Linux client genuinely needs the two
           apart -- rmdir(2) reporting EIO where ENOTEMPTY belongs sends it
           hunting for a broken filesystem. */
        if (!s9p_dir_is_empty(path)) {
            return s9p_send_rlerror(conn, tag, P9_ENOTEMPTY);
        }
        if (axl_dir_rmdir(path) != AXL_OK) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
    } else if (axl_file_delete(path) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RREMOVE, tag)) {
        return false;
    }
    return s9p_send_reply(conn, &w);
}

/* Trename: fid[4] dfid[4] name[s] -> Rrename (empty body).

   SAME-DIRECTORY ONLY. A rename whose target parent differs from the
   source's is answered EXDEV, which is NOT on the phase-4 plan's errno list
   and is added here deliberately, for two reasons that point the same way:

     - AxlFs cannot do it atomically. axl_file_rename refuses a
       cross-directory rename outright (most UEFI FAT drivers cannot move a
       file between directories via SetFileInfo), and the only fallback,
       axl_file_move, is a whole-file COPY. That copy is unbounded,
       synchronous, and single-threaded -- the same one-message stall that
       forced a ceiling on Tsetattr's grow, except a rename has no natural
       ceiling to apply.
     - EXDEV is precisely what a client already knows how to handle.
       mv(1) and every rename(2) caller falls back to copy-then-unlink on
       it, which comes back as ordinary bounded Twrites the loop can
       interleave. Refusing with EXDEV is therefore not a missing feature;
       it moves the copy to the side of the wire that can afford it.

   The fid's open handles are dropped before the rename and NOT reopened
   after, so a renamed fid is left bound-but-closed and must be re-Tlopen'd.

   Not because the firmware refuses the move with a live handle -- it does
   not, and our own suite proves it: CASE43 asserts that a Trename SUCCEEDS
   while ANOTHER connection holds the same file open. The reason is that a
   handle which survives the rename keeps naming the moved node while the
   fid's stored path names where the node used to be, so the two disagree
   about what the fid refers to. Dropping the handles makes the fid say one
   thing. A client that renames a file it is actively writing is doing
   something unusual; one that renames a closed file -- which is every
   mv(1) -- is unaffected. */
bool
s9p_handle_trename(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid  = axl_9p_r_u32(r);
    uint32_t dfid = axl_9p_r_u32(r);
    char     name[256];
    size_t   wire_n = axl_9p_r_str(r, name, sizeof(name));
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (axl_strcmp(f->path, conn->srv->root) == 0) {
        return s9p_send_rlerror(conn, tag, P9_EPERM);   /* not the export root */
    }

    char     newpath[AXL_9P_PATH_MAX];
    uint32_t ecode = P9_EIO;
    if (!s9p_resolve_child(conn, dfid, name, wire_n, sizeof(name), newpath,
                           sizeof(newpath), &ecode)) {
        return s9p_send_rlerror(conn, tag, ecode);
    }
    if (axl_strcmp(newpath, f->path) == 0) {
        /* Renaming a node onto itself: nothing to do, and letting it reach
           axl_file_rename would trip the destination-exists check below on
           the node's own name. */
        Axl9pWriter self;
        if (!s9p_reply_begin(conn, &self, AXL_9P_RRENAME, tag)) {
            return false;
        }
        return s9p_send_reply(conn, &self);
    }

    /* Same-parent check. axl_path_get_dirname is separator-aware, so this
       works for both the "/export/sub" and "fs0:\\sub" root conventions. */
    char *src_parent = axl_path_get_dirname(f->path);
    char *dst_parent = axl_path_get_dirname(newpath);
    bool  same_dir   = (src_parent != NULL && dst_parent != NULL
                        && axl_strcmp(src_parent, dst_parent) == 0);
    axl_free(src_parent);
    axl_free(dst_parent);
    if (!same_dir) {
        return s9p_send_rlerror(conn, tag, P9_EXDEV);
    }

    AxlFsEntry existing;
    if (axl_file_info(newpath, &existing) == AXL_OK) {
        /* axl_file_rename would fail on a taken name anyway; answering
           EEXIST rather than EIO says WHY. POSIX rename overwrites, but
           overwriting through this server would mean an unlogged delete the
           client never asked for. */
        return s9p_send_rlerror(conn, tag, P9_EEXIST);
    }

    /* Let go of the firmware handles before the node moves, so the fid is
       not left holding a handle on the moved node while its stored path
       still names where the node used to be (see the header note: the
       firmware does NOT refuse the rename -- CASE43 proves it completes
       one with another connection's handle live). Remember WHICH handles
       it held, so a rename that then fails can put the fid back the way it
       was rather than leaving it closed for an operation that never
       happened. */
    bool had_view    = (f->view != NULL);
    bool had_wstream = (f->wstream != NULL);
    bool had_dir     = (f->dir != NULL);
    bool flushed     = s9p_fid_close_open(f);

    if (!flushed) {
        /* Writes the client already had acknowledged are not on the media.
           Renaming now would bury that under a successful Rrename, so this
           takes the same "the operation did not happen" exit the failed
           rename below does. */
        s9p_fid_restore_open(conn, f, had_view, had_wstream, had_dir);
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }
    if (axl_file_rename(f->path, newpath) != AXL_OK) {
        s9p_fid_restore_open(conn, f, had_view, had_wstream, had_dir);
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }
    if (axl_strlcpy(f->path, newpath, sizeof(f->path)) >= sizeof(f->path)) {
        /* Unreachable: newpath was built into a buffer of the same size.
           Kept so the fid can never be left naming a path it isn't at. */
        if (!s9p_fid_clunk(conn, fid)) {
            axl_warning("9p: unflushed write stream on an over-long rename");
        }
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RRENAME, tag)) {
        return false;
    }
    return s9p_send_reply(conn, &w);
}

/* Tsetattr: fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8]
   atime_nsec[8] mtime_sec[8] mtime_nsec[8] -> Rsetattr (empty body).

   Only ATTR_SIZE is honored. mode/uid/gid are accepted as no-ops because
   AxlFs has no POSIX permission or ownership model, and the times are
   accepted as no-ops because the SDK exposes no mtime setter -- a Linux
   client expects ATTR_SIZE to bump mtime/ctime and it will NOT, since
   axl_file_truncate documents that it writes the file's existing times back
   unchanged. That is a documented v1 divergence, not something this reply
   claims otherwise: Rsetattr carries no field asserting what was set.

   The size itself is bounded before it reaches axl_file_truncate. See
   s9p_setattr_size_ok and AXL_9P_SERVER_MAX_GROW: a wire-supplied grow is
   O(size) synchronous zero-fill on the FAT driver, and this server is one
   loop, so an unbounded one is a denial of service costing the client
   eleven bytes. An over-bound request is EFBIG -- also not on the plan's
   list, and chosen because it is exactly what truncate(2) returns for a
   length beyond what the filesystem will grant, so a client reports "File
   too large" rather than a misleading EIO or EPERM.

   ENOENT / EISDIR / EROFS are recovered by pre-calling axl_file_info, since
   axl_file_truncate collapses every failure into AXL_ERR. Volume-full and
   I/O error genuinely cannot be told apart from here, so both surface as
   EIO -- the documented lie in this handler. */
bool
s9p_handle_tsetattr(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid   = axl_9p_r_u32(r);
    uint32_t valid = axl_9p_r_u32(r);
    axl_9p_r_u32(r);                            /* mode -- no POSIX model */
    axl_9p_r_u32(r);                            /* uid  -- no POSIX model */
    axl_9p_r_u32(r);                            /* gid  -- no POSIX model */
    uint64_t size = axl_9p_r_u64(r);
    axl_9p_r_u64(r); axl_9p_r_u64(r);           /* atime_sec, atime_nsec */
    axl_9p_r_u64(r); axl_9p_r_u64(r);           /* mtime_sec, mtime_nsec */
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }

    if ((valid & P9_SETATTR_SIZE) != 0u) {
        AxlFsEntry e;
        if (axl_file_info(f->path, &e) != AXL_OK) {
            return s9p_send_rlerror(conn, tag, P9_ENOENT);
        }
        if (axl_fs_entry_is_dir(&e)) {
            return s9p_send_rlerror(conn, tag, P9_EISDIR);
        }
        if (axl_fs_entry_is_read_only(&e)) {
            /* The FILE carries the read-only attribute -- that is EACCES,
               the way truncate(2) reports an unwritable file. EROFS is
               reserved for the whole export being read-only, which the
               dispatch gate answers before this handler ever runs. */
            return s9p_send_rlerror(conn, tag, P9_EACCES);
        }
        if (!s9p_setattr_size_ok(size, e.size)) {
            return s9p_send_rlerror(conn, tag, P9_EFBIG);
        }
        if (size != e.size) {
            if (axl_file_truncate(f->path, size) != AXL_OK) {
                /* A full volume and a backing-store failure are
                   indistinguishable here -- axl_file_truncate returns the
                   same AXL_ERR for both -- so both are reported as EIO. */
                return s9p_send_rlerror(conn, tag, P9_EIO);
            }
        }
    }
    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RSETATTR, tag)) {
        return false;
    }
    return s9p_send_reply(conn, &w);
}
