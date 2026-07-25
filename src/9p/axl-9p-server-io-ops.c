/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server-io-ops.c
    Axl9pServer handlers that work THROUGH an open fid handle.

    Split out of axl-9p-server-ops.c (see axl-9p-server-internal.h for the
    seam). Tlopen hands a fid the handle every later message here needs --
    an AxlDir iterator for a directory, a page-cached AxlFileView for a
    reader -- and Tread/Treaddir drive that handle. Task 5 adds Twrite and
    Tfsync here, over a writable AxlStream the same fid owns. Nothing in
    this file resolves a name or changes the shape of the tree; that is
    axl-9p-server-ns-ops.c.

    Everything a handler here computes from a wire-supplied offset, count or
    dirent cursor is factored into the pure helpers at the top of the file,
    because those are the values a hostile client controls and the only part
    of the server the unit suite can reach before the Task-6 live-socket
    harness exists.

    Every handler answers exactly one request and returns whether the
    connection survived its reply -- see s9p_send_reply in axl-9p-server.c
    for why that answer is must-check rather than assumed.
**/

#include <axl/axl-str.h>
#include <axl/axl-fs.h>
#include <axl/axl-file-view.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>
#include "axl-9p-internal.h"
#include "axl-9p-server-internal.h"

AXL_LOG_DOMAIN("9p");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/* Rread's and Rreaddir's fixed prefix: size[4] type[1] tag[2] count[4].
   Everything after it is payload, so msize - this is the most data one
   reply can carry. */
#define AXL_9P_RDATA_HDR_LEN  11u

/* Linux DT_* dirent kinds, the `type[1]` field of an Rreaddir record. */
#define P9_DT_DIR  4u
#define P9_DT_REG  8u

// ---------------------------------------------------------------------------
// Pure read/write/readdir arithmetic (see axl-9p-server-internal.h)
// ---------------------------------------------------------------------------

size_t
s9p_read_clamp(uint64_t offset, uint32_t count, size_t file_size, size_t room,
               size_t *out_offset)
{
    *out_offset = 0;
    if (offset >= (uint64_t)file_size) {
        /* At or past EOF -- 9P spells that as an Rread of count 0, not as an
           error. This comparison is also what makes the narrowing below
           safe: it is done in uint64 arithmetic (file_size widens, it never
           truncates offset), so however large the wire offset was, reaching
           the next line proves it is smaller than a value that already fits
           a size_t. */
        return 0;
    }
    size_t off   = (size_t)offset;
    size_t avail = file_size - off;
    size_t n     = count;          /* widening; count is uint32_t */

    if (n > avail) {
        n = avail;
    }
    if (n > room) {
        n = room;
    }
    if (n == 0) {
        /* Nothing will be read, so report no offset either -- "*out_offset
           is 0 whenever the return is 0" holds for EVERY zero result, not
           just the past-EOF one. A caller that reads the offset without
           first checking the count then gets a harmless 0 rather than a
           position it was never cleared to read from. */
        return 0;
    }
    *out_offset = off;
    return n;
}

bool
s9p_write_bounds(uint64_t offset, uint32_t count, size_t avail,
                 size_t *out_offset)
{
    *out_offset = 0;
    if ((uint64_t)count > (uint64_t)avail) {
        return false;   /* the frame does not carry the payload it claims */
    }
    if (offset > (uint64_t)SIZE_MAX - (uint64_t)count) {
        return false;   /* offset + count would wrap the index it becomes */
    }
    *out_offset = (size_t)offset;
    return true;
}

bool
s9p_write_grow_ok(uint64_t offset, uint32_t count, uint64_t cur_size)
{
    if (offset > UINT64_MAX - (uint64_t)count) {
        return false;   /* the write's end is not even representable */
    }
    uint64_t end = offset + (uint64_t)count;

    if (end <= cur_size) {
        return true;   /* entirely inside the file -- overwrites, grows nothing */
    }
    return (end - cur_size) <= (uint64_t)AXL_9P_SERVER_MAX_GROW;
}

bool
s9p_write_grow_needs_size(uint64_t offset, uint32_t count)
{
    if (offset > UINT64_MAX - (uint64_t)count) {
        /* s9p_write_grow_ok refuses on its own overflow guard, before it
           ever looks at cur_size -- so the answer is already settled. */
        return false;
    }
    /* At or below the ceiling the write is allowed for EVERY cur_size:
       either it ends inside the file, or it grows the file by at most
       (offset + count), which is itself within the ceiling. */
    return (offset + (uint64_t)count) > (uint64_t)AXL_9P_SERVER_MAX_GROW;
}

void
s9p_readdir_resume(uint64_t cursor, bool *out_dot, bool *out_dotdot,
                   uint64_t *out_skip)
{
    *out_dot    = (cursor < 1u);
    *out_dotdot = (cursor < 2u);
    *out_skip   = (cursor < 2u) ? 0u : cursor - 2u;
}

bool
s9p_dirent_put(Axl9pWriter *w, size_t room, const uint8_t *qid,
               uint64_t offset, uint8_t dtype, const char *name)
{
    size_t nlen = axl_strlen(name);
    /* qid[13] offset[8] type[1] namelen[2] name[nlen] */
    size_t need = AXL_9P_QID_LEN + 8u + 1u + 2u + nlen;

    /* Both bounds checked BEFORE the first byte is written, which is what
       makes a refusal a clean stop at a record boundary rather than a
       half-written record plus an overflow flag. */
    if (need > room || need > w->cap - w->len) {
        return false;
    }
    axl_9p_w_bytes(w, qid, AXL_9P_QID_LEN);
    axl_9p_w_u64(w, offset);
    axl_9p_w_u8(w, dtype);
    axl_9p_w_str(w, name);
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Current length of the file behind an ALREADY-OPEN write stream.

   Deliberately not axl_file_info: that opens the file again, and Twrite runs
   once per msize chunk, so a stat there would reintroduce exactly the
   per-chunk firmware open the fid's persistent handle exists to avoid
   (hundreds per megabyte). Seek-to-end on the live handle is two backend
   calls and no open. It is also always accurate, so it introduces no
   cached-length staleness of its own -- a cached size would have needed
   invalidating on every shrink anywhere in the server, and a size cache that
   went stale HIGH would have re-opened the very hole this bounds.

   Moving the position is harmless: axl_pwrite saves and restores it around
   every call (see file_pwrite in axl-stream-file.c), and nothing else in
   this server uses the stream's cursor. */
AXL_WARN_UNUSED static bool
s9p_wstream_size(AxlStream *s, uint64_t *out)
{
    if (axl_fseek(s, 0, AXL_SEEK_END) != AXL_OK) {
        return false;
    }
    int64_t n = axl_ftell(s);
    if (n < 0) {
        return false;
    }
    *out = (uint64_t)n;
    return true;
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

/* Tlopen: fid[4] flags[4] -> Rlopen: qid[13] iounit[4]. Gives the fid the
   open handles every later Tread/Twrite/Treaddir works through: an AxlDir
   iterator for a directory, a page-cached AxlFileView and/or a positional
   AxlStream for a file (s9p_fid_open_file picks by access mode). Which KIND
   is decided by a fresh stat rather than by the is_dir the walk recorded,
   since the node may have changed kind since it was walked to.

   The file is NEVER read whole. The view reads positionally through the
   server's one shared page cache (see AXL_9P_SERVER_PAGE_SIZE), so serving
   a 4 GiB file costs the same resident memory as serving a 4 KiB one.

   O_TRUNC is honored, because our own axl_9p_write_file opens an existing
   file with O_WRONLY|O_TRUNC and then writes from offset 0 -- without it,
   overwriting a long file with a short one would leave the old tail behind.
   It runs BEFORE the handles are opened so that neither the view's cached
   length nor the stream's position is ever derived from the pre-truncation
   file, and it needs no s9p_setattr_size_ok bound because truncating to
   zero is a pure shrink: it moves no data.

   iounit 0 tells the client "no I/O size of my own" -- it should chunk at
   msize - 11, which is exactly what our own axl_9p_read_file does.

   Re-opening an already-open fid is legal and simply replaces the handles;
   s9p_fid_close_open drops the previous set first so it cannot leak. */
bool
s9p_handle_tlopen(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid   = axl_9p_r_u32(r);
    uint32_t flags = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    uint32_t accmode = flags & P9_O_ACCMODE;
    if (conn->srv->read_only && accmode != P9_ACC_RDONLY) {
        /* Refused at open time, which is where a POSIX client expects to
           learn a filesystem is read-only -- not later, per write. This is
           why Tlopen is deliberately absent from s9p_type_is_mutating: the
           blanket gate would also refuse a legal read-only open. */
        return s9p_send_rlerror(conn, tag, P9_EROFS);
    }

    AxlFsEntry e;
    if (axl_file_info(f->path, &e) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_ENOENT);
    }

    /* A re-opened fid may still hold a write stream from its previous open.
       If that stream's final flush fails the client's earlier writes are
       gone, and answering Rlopen would hide it behind a fresh handle. */
    if (!s9p_fid_close_open(f)) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }
    f->is_dir = axl_fs_entry_is_dir(&e);
    if (f->is_dir) {
        if (accmode != P9_ACC_RDONLY) {
            return s9p_send_rlerror(conn, tag, P9_EISDIR);
        }
        f->dir = axl_dir_open(f->path);
        if (f->dir == NULL) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
        f->open = true;
    } else {
        if ((flags & P9_O_TRUNC) != 0u && accmode != P9_ACC_RDONLY
            && e.size != 0u) {
            if (axl_file_truncate(f->path, 0) != AXL_OK) {
                return s9p_send_rlerror(conn, tag, P9_EIO);
            }
            /* Re-stat so the qid and any later size decision describe the
               truncated file. Other fids' views need no help HERE:
               axl_file_truncate is an AXL write path, so an AxlFileView on
               this file in this image picks the change up by itself. That is
               best effort, not a guarantee we are relying on -- this handler
               only owes a correct qid for its OWN open, which the re-stat
               below supplies. */
            if (axl_file_info(f->path, &e) != AXL_OK) {
                return s9p_send_rlerror(conn, tag, P9_EIO);
            }
        }
        if (!s9p_fid_open_file(conn, f, accmode)) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
    }

    uint8_t qidbuf[AXL_9P_QID_LEN];
    s9p_qid(&e, f->path, qidbuf);

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RLOPEN, tag)) {
        return false;
    }
    axl_9p_w_bytes(&w, qidbuf, sizeof(qidbuf));
    axl_9p_w_u32(&w, 0);   /* iounit: none of our own -- chunk at msize */
    return s9p_send_reply(conn, &w);
}

/* Tread: fid[4] offset[8] count[4] -> Rread: count[4] data[count].
   Both offset and count come straight off the wire unbounded, so every
   decision about how many bytes are copied and where from is made by
   s9p_read_clamp (unit-tested against 64-bit offsets, offsets past EOF and
   counts larger than the reply) before a single byte moves. The clamp's
   `room` is the tighter of the negotiated msize and what is physically left
   in txbuf, so neither an msize that outran the buffer nor the reverse can
   put a byte past the end.

   The data is read STRAIGHT into the reply buffer via axl_9p_w_reserve --
   no staging copy, so a max-msize read moves 128 KiB once rather than
   twice. count[4] is written as a placeholder first and back-patched,
   because a short read (only reachable as a backing-store failure, since
   the clamp already bounded the request by the file size) has to be
   answered as EIO rather than as a truthful-but-silent short Rread that a
   client would read as EOF. */
bool
s9p_handle_tread(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid    = axl_9p_r_u32(r);
    uint64_t offset = axl_9p_r_u64(r);
    uint32_t count  = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL || !f->open) {
        /* Unbound, clunked, or walked-to but never opened -- all EBADF, and
           all reached before anything dereferences a handle. */
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (f->dir != NULL) {
        return s9p_send_rlerror(conn, tag, P9_EISDIR);
    }
    if (f->view == NULL) {
        /* Opened O_WRONLY: there is nothing to read through. */
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    /* A view caches the file length at ITS open and serves from cached
       pages, so a write that landed since would otherwise be clamped
       against a stale size and answered out of stale bytes. AxlFileView
       makes a best effort at that itself now, for writes from any source
       in this PE image -- this fid, another fid, another connection,
       another Axl9pServer over the same root, or any AXL code that is not
       9P at all. Asking here (rather than at the write) is what keeps a
       write-only fid, and a write loop that never reads back, paying
       nothing for it.

       Best effort is all it is: the export is close-to-open consistent
       (src/9p/README.md), so a foreign-image writer is invisible to this
       call and Tlopen is what a client re-runs to be sure.

       AXL_ERR means the file went away under the fid. The view SURVIVES
       that -- it reports size 0 and keeps answering AXL_ERR -- so every
       subsequent Tread on this fid gets the same EIO. It used to get EIO
       once and EBADF from then on, because the failed refresh left the fid
       open with no view. */
    if (axl_file_view_refresh(f->view) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RREAD, tag)) {
        return false;
    }
    size_t count_pos = w.len;
    axl_9p_w_u32(&w, 0);   /* count -- patched once the read settles */

    size_t room = (size_t)conn->msize - AXL_9P_RDATA_HDR_LEN;
    if (room > w.cap - w.len) {
        room = w.cap - w.len;
    }
    size_t off = 0;
    size_t n   = s9p_read_clamp(offset, count, axl_file_view_size(f->view),
                                room, &off);
    if (n > 0) {
        uint8_t *data = axl_9p_w_reserve(&w, n);
        if (data == NULL || axl_file_view_read(f->view, off, data, n) != n) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
    }
    axl_9p_w_patch_u32(&w, count_pos, (uint32_t)n);
    return s9p_send_reply(conn, &w);
}

/* Treaddir: fid[4] offset[8] count[4] -> Rreaddir: dcount[4] data[dcount],
   data being whole records of qid[13] offset[8] type[1] name[s]. dcount 0
   is end-of-directory.

   The reply always leads with synthetic "." and ".." (records 0 and 1) so a
   Linux mount sees the entries POSIX requires; the on-disk "." / ".."
   entries UEFI FAT directories carry are dropped, or every one would appear
   twice. Real entries follow in axl_dir_read order.

   `offset` is a record cursor, not a byte offset: record k carries offset
   k+1 and the client hands back the last offset it received, so a cursor of
   N means "N records already delivered" (s9p_readdir_resume decodes it, and
   is total over every uint64 a client might invent). AxlDir cannot seek, so
   the fid keeps the iterator open across requests and `dir_pos` records how
   far it has been pulled: a cursor at that position simply continues, one
   ahead of it skips forward, and one BEHIND it -- a client rewinding, or
   re-asking after a record that didn't fit -- reopens the directory and
   re-skips. That reopen is the price of a seekless iterator; it is correct
   in every case rather than fast in most, which is the right trade for a
   path a well-behaved sequential client never takes. */
bool
s9p_handle_treaddir(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid    = axl_9p_r_u32(r);
    uint64_t offset = axl_9p_r_u64(r);
    uint32_t count  = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL || !f->open) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (f->dir == NULL) {
        return s9p_send_rlerror(conn, tag, P9_ENOTDIR);
    }

    bool     want_dot    = false;
    bool     want_dotdot = false;
    uint64_t skip        = 0;
    s9p_readdir_resume(offset, &want_dot, &want_dotdot, &skip);

    if (skip < f->dir_pos) {
        axl_dir_close(f->dir);
        f->dir     = axl_dir_open(f->path);
        f->dir_pos = 0;
        if (f->dir == NULL) {
            /* The directory went away between requests. Drop the fid's open
               state so later messages answer EBADF ("this fid is no longer
               usable") rather than ENOTDIR, which would read as "you opened
               the wrong kind of thing" and send the client hunting for a bug
               it doesn't have. */
            if (!s9p_fid_close_open(f)) {
                /* A directory fid holds no write stream, so this cannot
                   fail -- and the reply below is already EIO either way. */
                axl_warning("9p: flush failed dropping a directory fid");
            }
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
    }
    /* Bounded by the directory's real length, never by the cursor's value:
       a made-up cursor of 2^64-1 just runs the iterator dry and yields an
       empty (end-of-directory) reply. */
    while (f->dir_pos < skip) {
        AxlFsEntry ignored;
        if (!axl_dir_read(f->dir, &ignored)) {
            break;
        }
        f->dir_pos++;
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RREADDIR, tag)) {
        return false;
    }
    size_t dcount_pos = w.len;
    axl_9p_w_u32(&w, 0);   /* dcount -- patched once the fill settles */

    size_t room = (size_t)conn->msize - AXL_9P_RDATA_HDR_LEN;
    if (room > (size_t)count) {
        room = count;
    }
    if (room > w.cap - w.len) {
        room = w.cap - w.len;
    }
    size_t  data_start = w.len;
    uint8_t qidbuf[AXL_9P_QID_LEN];

    if (f->dir_pos < skip) {
        goto done;   /* cursor past the end of the directory */
    }
    if (want_dot) {
        AxlFsEntry self = {0};
        if (axl_file_info(f->path, &self) != AXL_OK) {
            self.attributes = AXL_FS_ATTR_DIRECTORY;
        }
        s9p_qid(&self, f->path, qidbuf);
        if (!s9p_dirent_put(&w, room - (w.len - data_start), qidbuf, 1,
                            P9_DT_DIR, ".")) {
            goto done;
        }
    }
    if (want_dotdot) {
        char parent[sizeof(f->path)];
        axl_strlcpy(parent, f->path, sizeof(parent));
        if (!s9p_path_up(conn->srv->root, parent, sizeof(parent))) {
            /* At the export root, where s9p_path_up refuses to step above.
               It leaves `parent` untouched on a refusal (that IS its
               contract -- see the declaration), so this restore is a no-op
               today; it is kept as the explicit statement of what this
               branch wants, namely ".." resolving to the root itself, which
               is what a mount expects at the top of a tree. */
            axl_strlcpy(parent, f->path, sizeof(parent));
        }
        AxlFsEntry pe = {0};
        if (axl_file_info(parent, &pe) != AXL_OK) {
            pe.attributes = AXL_FS_ATTR_DIRECTORY;
        }
        s9p_qid(&pe, parent, qidbuf);
        if (!s9p_dirent_put(&w, room - (w.len - data_start), qidbuf, 2,
                            P9_DT_DIR, "..")) {
            goto done;
        }
    }
    for (;;) {
        AxlFsEntry de;
        if (!axl_dir_read(f->dir, &de)) {
            break;   /* end of directory */
        }
        /* Record index is dir_pos + 2 (past the two synthetic records), and
           a record's cursor is its index + 1. Advance BEFORE any skip below
           so the cursor stays a truthful description of the iterator. */
        uint64_t rec_off = f->dir_pos + 3u;
        f->dir_pos++;

        if (axl_strcmp(de.name, ".") == 0 || axl_strcmp(de.name, "..") == 0) {
            continue;   /* synthesized above -- don't emit the on-disk pair */
        }
        char full[sizeof(f->path)];
        if (!s9p_path_append(full, sizeof(full), f->path, de.name)) {
            continue;   /* no path to qid it by; skip, cursor still advances */
        }
        s9p_qid(&de, full, qidbuf);
        if (!s9p_dirent_put(&w, room - (w.len - data_start), qidbuf, rec_off,
                            axl_fs_entry_is_dir(&de) ? P9_DT_DIR : P9_DT_REG,
                            de.name)) {
            /* Stop at this record boundary -- a truncated record would
               desync the client's parse of the whole reply. The entry stays
               consumed and dir_pos stays truthful, so the client's next
               request (at the last offset actually emitted, which is behind
               dir_pos) takes the reopen path above and re-reaches it. */
            break;
        }
    }

done:
    axl_9p_w_patch_u32(&w, dcount_pos, (uint32_t)(w.len - data_start));
    return s9p_send_reply(conn, &w);
}

/* Twrite: fid[4] offset[8] count[4] data[count] -> Rwrite: count[4].

   THREE wire-controlled values and TWO independent bounds on them.

   `data` is the one that has already cost this file two Critical
   use-after-frees' worth of caution: it is not a buffer the client sent
   separately, it is the tail of the frame sitting in conn->rbuf, and
   `count` is the client's own claim about how long that tail is.
   s9p_write_bounds is what makes the claim safe -- it refuses a count
   larger than the frame ACTUALLY carries (`r->len - r->pos`, i.e.
   everything after the 7+4+8+4 = 23 header bytes) before the payload
   pointer is ever formed, and narrows the 64-bit offset only once offset +
   count is proved to fit a size_t.

   `offset` needs a SECOND, unrelated bound. Passing the first one only
   proves the write can be addressed, not that it is cheap: a positional
   write past EOF is legal, grows the file, and has the FAT driver
   physically zero-fill the gap, so `offset = 0xFFFFFFFE, count = 1` is 24
   bytes on the wire that stall this server's single loop for 4 GiB of
   writing and then leave the volume full. s9p_write_grow_ok applies the
   same AXL_9P_SERVER_MAX_GROW ceiling, and the same EFBIG, that Tsetattr
   applies to the identical hazard reached the other way.

   The write goes through axl_pwrite on the fid's own stream, which places
   bytes at an absolute offset without moving the stream position -- so a
   client is free to write its chunks out of order, and two fids on the same
   file do not fight over a shared cursor.

   A SHORT write is reported truthfully as Rwrite(n) rather than as an
   error: 9P lets a client re-send the remainder, and that is a better
   answer than failing a whole megabyte because one chunk was clipped. A
   zero-byte result for a non-zero request is the exception -- a client that
   loops on it would spin forever, so it is answered EIO. */
bool
s9p_handle_twrite(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid    = axl_9p_r_u32(r);
    uint64_t offset = axl_9p_r_u64(r);
    uint32_t count  = axl_9p_r_u32(r);
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL || !f->open) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (f->dir != NULL) {
        return s9p_send_rlerror(conn, tag, P9_EISDIR);
    }
    if (f->wstream == NULL) {
        /* Bound, open, but opened read-only -- POSIX spells a write on a
           read-only descriptor EBADF, not EACCES. */
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }

    size_t off = 0;
    if (!s9p_write_bounds(offset, count, r->len - r->pos, &off)) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }

    uint32_t wrote = 0;
    if (count > 0u) {
        /* SECOND bound, and a different hazard from the first: the frame
           check said the payload is really there, this one says the write
           will not run the loop for gigabytes. A positional write past EOF
           is legal and grows the file, physically zero-filling the gap, so
           an unbounded offset is the same denial of service Tsetattr's
           ceiling exists for -- reached by a 24-byte message instead of an
           11-byte field. Same ceiling, same errno. */
        /* The size query is skipped whenever the bound's answer cannot
           depend on it -- which is every write that stays within the
           ceiling, i.e. every chunk of an ordinary sequential append to a
           file under 16 MiB. cur_size stays 0 on that path, and 0 is not a
           guess: s9p_write_grow_needs_size only says "no" for inputs whose
           verdict is the same for every possible length. */
        uint64_t cur_size = 0;
        if (s9p_write_grow_needs_size(offset, count)
            && !s9p_wstream_size(f->wstream, &cur_size)) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
        if (!s9p_write_grow_ok(offset, count, cur_size)) {
            return s9p_send_rlerror(conn, tag, P9_EFBIG);
        }
        /* Only NOW is a pointer into the frame formed, and only over a
           range s9p_write_bounds has proved lies inside it. */
        const uint8_t *data = r->buf + r->pos;
        axl_ssize_t    n    = axl_pwrite(f->wstream, data, count, off);
        if (n <= 0) {
            return s9p_send_rlerror(conn, tag, P9_EIO);
        }
        wrote = (uint32_t)n;
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RWRITE, tag)) {
        return false;
    }
    axl_9p_w_u32(&w, wrote);
    return s9p_send_reply(conn, &w);
}

/* Tfsync: fid[4] datasync[4] -> Rfsync (empty body).

   Flushes the fid's write stream if it has one. A fid with no write stream
   -- a reader, or a directory -- is answered Rfsync without touching
   anything: there is no buffered state of ours behind it, and POSIX permits
   fsync on a read-only descriptor. Not gated by s9p_type_is_mutating for
   the same reason.

   An unbound fid, and a fid walked-to but never Tlopen'd, are both EBADF --
   the same gate Tread, Treaddir and Twrite apply. fsync(2) has no meaning
   without an open file description, and answering Rfsync for one was this
   handler disagreeing with its three siblings about what an unopened fid
   is.

   `datasync` is read to advance the cursor and then ignored: AxlStream has
   no data-only flush to distinguish it from a full one. */
bool
s9p_handle_tfsync(S9pConn *conn, Axl9pReader *r, uint16_t tag)
{
    uint32_t fid = axl_9p_r_u32(r);
    axl_9p_r_u32(r);                            /* datasync */
    if (r->error) {
        return s9p_send_rlerror(conn, tag, P9_EINVAL);
    }
    S9pFid *f = s9p_fid_get(conn, fid);
    if (f == NULL || !f->open) {
        return s9p_send_rlerror(conn, tag, P9_EBADF);
    }
    if (f->wstream != NULL && axl_fflush(f->wstream) != AXL_OK) {
        return s9p_send_rlerror(conn, tag, P9_EIO);
    }

    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RFSYNC, tag)) {
        return false;
    }
    return s9p_send_reply(conn, &w);
}
