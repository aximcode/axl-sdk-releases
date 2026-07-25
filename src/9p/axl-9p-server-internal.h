/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server-internal.h
    Private header shared between the Axl9pServer implementation files.

    Not a public header, and deliberately separate from axl-9p-internal.h:
    that one is the codec + CLIENT state, and is included by the unit tests
    and by axl-9p-codec.c, which have no business seeing the server's
    connection pool. The split is:

      axl-9p-server.c       transport -- new/listen/free, the conn pool,
                            accept, the recv/send callbacks, the framing
                            pump, buffer growth, reap.
      axl-9p-server-ops.c   s9p_dispatch (the routing table + the
                            read-only gate) and the session / navigation
                            / metadata handlers -- Tversion, Tattach,
                            Twalk, Tclunk, Tgetattr. Nothing here moves
                            file data or changes the tree.
      axl-9p-server-io-ops.c
                            everything that goes THROUGH an open fid
                            handle -- Tlopen, Tread, Twrite, Treaddir,
                            Tfsync -- plus the pure wire-arithmetic
                            helpers those handlers bound a hostile
                            offset/count/cursor with.
      axl-9p-server-ns-ops.c
                            everything that changes the NAMESPACE or a
                            node's metadata by path -- Tlcreate, Tmkdir,
                            Tremove, Trename, Tsetattr.
      axl-9p-server-fid.c   the per-connection fid table + the qid and
                            path-safety helpers the handlers resolve
                            names with (no transport; the only codec use
                            is s9p_qid's fixed 13-byte encode).

    The ops seam is by EFFECT, not by message number: "reads a handle"
    versus "rewrites the tree" is the same line the read-only gate draws,
    so a handler's file already tells you whether s9p_type_is_mutating
    must list it.
**/

#ifndef AXL_9P_SERVER_INTERNAL_H
#define AXL_9P_SERVER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <axl/axl-9p.h>            /* Axl9pServer, AxlLoop */
#include <axl/axl-fs.h>            /* AxlFsEntry, AxlDir */
#include <axl/axl-file-view.h>     /* AxlFileView, AxlPageCache */
#include <axl/axl-stream.h>        /* AxlStream */
#include <axl/axl-tcp.h>           /* AxlTcp */
#include "axl-9p-internal.h"       /* Axl9pReader, Axl9pWriter, AXL_9P_QID_LEN */

// ---------------------------------------------------------------------------
// Server tunables
// ---------------------------------------------------------------------------

#define AXL_9P_SERVER_DEFAULT_PORT       564u   ///< the registered 9P port
#define AXL_9P_SERVER_DEFAULT_MAX_CONNS  8u     ///< fixed conn-pool size

#define AXL_9P_MAX_FIDS  128u   ///< per-connection fid table cap

/* Capacity of every export-path buffer in the server -- S9pFid.path and the
   scratch buffers handlers build a child path in. ONE constant, so a
   handler cannot build a name into a buffer larger than the fid it is
   destined for and discover the mismatch as a silent truncation. */
#define AXL_9P_PATH_MAX  512u

/* msize negotiation ceiling (Tversion): generous but bounded, per the
   phase-4 plan's "cap generous e.g. 64-128 KiB". rbuf/txbuf grow to the
   negotiated value (capped here), never shrink. */
#define AXL_9P_SERVER_MAX_MSIZE  (128u * 1024u)

/* The ONE shared AxlFileView page cache every open file fid reads through,
   sized once per server rather than per fid (see axl_9p_server_new). Total
   resident file data is PAGE_SIZE * PAGE_FRAMES == 1 MiB no matter how many
   fids, or how many connections, a client opens -- a per-fid frame pool
   would let 8 conns x 128 fids multiply that budget by a thousand.

   Page size is matched to how a 9P client actually reads: it asks in
   msize-minus-11 chunks, so between AXL_9P_MSIZE (8 KiB default, chunk
   ~8181 B) and AXL_9P_SERVER_MAX_MSIZE (128 KiB, chunk ~131061 B). At 64
   KiB a max-msize chunk spans two or three pages -- near-zero read
   amplification -- while a default-msize chunk faults one page in and then
   serves the next seven reads from it, which is exactly the sequential
   pattern a mount(8) read produces. The cost is a single 8 KiB random read
   in a large file touching 64 KiB, which is the right trade for a
   sequential-dominated workload over a FAT driver whose per-read overhead
   dwarfs the extra bytes. */
#define AXL_9P_SERVER_PAGE_SIZE    (64u * 1024u)   ///< AxlFileView frame size
#define AXL_9P_SERVER_PAGE_FRAMES  16u             ///< resident frames, server-wide

/* Ceiling on how far ONE Tsetattr(size) may GROW a file.

   Growing is not a metadata update: axl_file_truncate documents that the
   EDK2-derived FAT driver physically writes the added region out as zeros,
   in chunks, and UEFI file I/O is synchronous. This server is a single
   AxlLoop, so a grow services nothing else -- no other connection, no
   accept, no timer -- for its whole duration. A Tsetattr size comes
   straight off the wire, so an unbounded one is a one-message denial of
   service: "grow to 4 GiB" is eleven bytes on the wire and minutes of dead
   loop, and FAT would accept it right up to the volume filling.

   16 MiB is chosen as the largest single stall worth accepting: on the slow
   end of the media this runs against (a few MB/s over USB) that is a few
   seconds, versus tens of minutes for the 4 GiB FAT maximum. A client that
   genuinely wants a larger file is not blocked, it is merely made to ASK in
   bounded steps -- each repeat returns to the loop in between, which is the
   property that matters. Shrinks are unbounded: they move no data.

   No free-space check accompanies this: AxlFs exposes no volume-free-space
   API (checked -- axl-fs.h has label/enumerate/map only), so the bound is a
   fixed ceiling rather than a fraction of what is left. A grow that fits
   the bound but not the volume still fails, in axl_file_truncate, as
   ENOSPC-shaped AXL_ERR. */
#define AXL_9P_SERVER_MAX_GROW  (16u * 1024u * 1024u)

// ---------------------------------------------------------------------------
// Linux errno vocabulary for Rlerror bodies
// ---------------------------------------------------------------------------

/* Shared by every handler file, which is why these live here rather than in
   whichever .c first needed one. The phase-4 plan fixes the vocabulary and
   the whole of it is spelled out, including the codes no handler currently
   reaches for -- the point of a fixed vocabulary is that the next handler
   picks from it instead of inventing a number. The two values NOT on the
   plan's list are marked, per its "add one deliberately and say so" rule. */
#define P9_EPERM     1u
#define P9_ENOENT    2u
#define P9_EIO       5u
#define P9_EBADF     9u
#define P9_ENOMEM    12u
#define P9_EACCES    13u
#define P9_EEXIST    17u
#define P9_EXDEV     AXL_9P_EXDEV   ///< NOT on the plan's list -- see s9p_handle_trename.
                                    ///< Aliases the shared definition in
                                    ///< axl-9p-internal.h: the client's
                                    ///< copy-then-unlink fallback keys off the
                                    ///< same number this handler sends.
#define P9_ENOTDIR   20u
#define P9_EISDIR    21u
#define P9_EINVAL    22u
#define P9_EFBIG     27u   ///< NOT on the plan's list -- see s9p_setattr_size_ok
#define P9_ENOSPC    28u
#define P9_EROFS     30u
#define P9_ENOTEMPTY 39u
#define P9_EPROTO    71u

// ---------------------------------------------------------------------------
// Linux open(2) flags, as they arrive in Tlopen/Tlcreate
// ---------------------------------------------------------------------------

#define P9_O_ACCMODE  3u        ///< mask selecting the access mode below
#define P9_ACC_RDONLY 0u
#define P9_ACC_WRONLY 1u
#define P9_ACC_RDWR   2u
#define P9_O_TRUNC    0x200u    ///< empty the file as part of the open

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct S9pConn S9pConn;

struct Axl9pServer {
    AxlLoop      *loop;
    char          root[256];
    bool          read_only;
    AxlTcp       *listener;
    uint16_t      port;
    bool          running;
    S9pConn      *conns;
    size_t        max_conns;
    AxlPageCache *page_cache;   ///< shared by every open file fid; outlives them all
};

/* One entry in a connection's fid table. `path` is always an absolute AxlFs
   path anchored under the server's (normalized -- see the root-stripping
   comment in axl_9p_server_new) export root; s9p_path_append/s9p_path_up
   are the only ways it's ever extended, which is what keeps it from ever
   escaping the root.

   OPEN STATE (everything below `path`) is owned by the fid and set up by
   Tlopen (or, for a file it just created, Tlcreate): `view` and/or
   `wstream` for a file fid, `dir` + `dir_pos` for a directory fid, never
   both kinds at once. It must be released through s9p_fid_release /
   _close_open from EVERY path that drops a fid -- Tclunk, Tremove, the
   Tversion session reset, connection reap, and a second Tlopen on the same
   fid. `dir_pos` is how many entries have been pulled off `dir`, which is
   what lets a Treaddir continue an open directory walk instead of reopening
   it per request.

   `view` and `wstream` are independent because the 9P access mode is:
   O_RDONLY gets a view, O_WRONLY a stream, O_RDWR both. Each is one open
   firmware handle held for the fid's lifetime -- the same exposure the
   read path already had, and the alternative (open per Twrite) would cost
   one file open per msize-sized chunk, i.e. hundreds per megabyte written.

   Keeping `view` across writes is reasonable because AxlFileView makes a
   best effort to notice writes made through AXL in this PE image: it
   re-stats before serving when the file it reads was written (see
   src/fs/axl-file-gen.h). The server used to carry that itself, marking
   views per-Axl9pServer after each mutation -- which left every writer
   OUTSIDE this server invisible, including a second Axl9pServer exporting
   the same root. Nothing here has to mark anything now.

   Note the guarantee this does NOT create: the export is close-to-open
   consistent, not coherent. A fid already open when a foreign image writes
   the file will not see it, and Tlopen is what a client re-runs to be
   sure. See src/9p/README.md. */
typedef struct {
    bool         used;
    uint32_t     fid;
    bool         is_dir;
    bool         open;         ///< Tlopen has succeeded on this fid
    char         path[AXL_9P_PATH_MAX];
    AxlFileView *view;         ///< file fid: page-cached reader (NULL otherwise)
    AxlStream   *wstream;      ///< file fid: positional writer (NULL otherwise)
    AxlDir      *dir;          ///< directory fid: live iterator (NULL otherwise)
    uint64_t     dir_pos;      ///< entries already pulled off `dir`
} S9pFid;

/* Per-connection state. rbuf accumulates raw bytes received from the socket
   and is reassembled into complete size-prefixed 9P messages by the framing
   loop in s9p_pump; rcap is its capacity and rlen the bytes currently
   buffered at its front. msize is the negotiated max message size (fixed at
   AXL_9P_MSIZE until Tversion negotiates it and grows rbuf/txbuf to match,
   see s9p_conn_grow_buffers) and is also the hard bound on an ACCEPTED frame
   -- the invariant msize <= rcap && msize <= txcap holds from accept onward.
   txbuf holds the single in-flight reply; tx_busy says a send is actually
   in flight over it, which is what enforces the one-request-in-flight
   invariant (see s9p_send_reply / s9p_pump). rx_armed says an async recv is
   outstanding over rbuf: the pump never double-arms one, and because it
   only ever arms once the drain has run dry, rx_armed is provably false
   throughout any dispatch -- which is what makes rbuf's compaction and
   Tversion's realloc safe. draining is the s9p_pump reentrancy guard.
   fids is the fixed-cap fid table (AXL_9P_MAX_FIDS); a
   client that tries to open more fids than that gets Rlerror(ENOMEM) rather
   than an unbounded table. */
struct S9pConn {
    bool          active;
    AxlTcp       *sock;
    Axl9pServer  *srv;
    uint32_t      msize;
    uint8_t      *rbuf;
    size_t        rcap;
    size_t        rlen;
    uint8_t      *txbuf;
    size_t        txcap;
    bool          tx_busy;
    bool          rx_armed;
    bool          draining;
    char          client_addr[46];
    S9pFid        fids[AXL_9P_MAX_FIDS];
};

// ---------------------------------------------------------------------------
// axl-9p-server.c -- transport
// ---------------------------------------------------------------------------

/* Close the connection's socket, free its receive/reply buffers, and reset
   the slot to inactive. Idempotent; see the definition for the callback
   re-entrancy rules and for what Task 4/5 must release before it. */
void
s9p_reap(S9pConn *conn);

/* Begin a reply in conn->txbuf: check that no send is in flight over the
   buffer and only THEN start the encode (axl_9p_msg_begin against
   conn->txbuf/conn->txcap). Returns whether @a conn SURVIVED -- false means
   the check failed, the connection has been reaped, and @a w was never
   initialized, so the caller must return false immediately without touching
   conn or @a w.

   This is the ONLY sanctioned way to start writing conn->txbuf. Calling
   axl_9p_msg_begin against it directly reintroduces the hole this closes:
   s9p_send_reply's guard runs after the caller has already scribbled into a
   buffer axl_tcp_send_async may still be borrowing, so it can detect and
   contain that bug but never prevent it. Beginning the encode behind the
   same check prevents the first byte.

   Composes with s9p_send_reply's own check by construction: that check is a
   pure predicate (see s9p_txbuf_check), so a reply that goes through
   reply_begin -> w_* -> send_reply tests the same condition twice on the
   same connection and never double-claims anything. */
AXL_WARN_UNUSED bool
s9p_reply_begin(
    S9pConn     *conn,   ///< connection whose txbuf to encode into
    Axl9pWriter *w,      ///< [out] writer initialized over conn->txbuf
    uint8_t      type,   ///< 9P message type of the reply
    uint16_t     tag     ///< tag of the request being answered
);

/* Finish and send a reply built in conn->txbuf via s9p_reply_begin +
   axl_9p_w_*. Returns whether @a conn SURVIVED the send: false means it was
   reaped and every field of it is now zeroed (rbuf/txbuf freed, fid table
   cleared, slot free for the next client), so the caller must not touch
   conn, its buffers, or an Axl9pReader over rbuf again. Must-check because
   "the send is the last thing a handler does" is otherwise convention only,
   and a handler that sends inside a loop breaks it silently.

   PRECONDITION -- READ THIS BEFORE WRITING A HANDLER THAT SENDS MORE THAN
   ONCE (Tread/Treaddir): conn->txbuf may only be encoded into while NO send
   is in flight over it, i.e. while !conn->tx_busy. A true return does NOT
   mean txbuf is free again: it means the connection survived, while
   axl_tcp_send_async is still BORROWING txbuf until s9p_on_send fires. So

       for (...) {
           if (!s9p_send_reply(conn, &w)) return false;
           if (!s9p_reply_begin(conn, &w, ...)) return false;     // WRONG
       }

   overwrites a buffer the firmware Transmit is reading. The correct shape
   is one reply per dispatch: return after the send and let s9p_on_send
   resume the pump. Both s9p_reply_begin and s9p_send_reply reap the
   connection with a warning if entered with tx_busy set -- and because the
   check now runs BEFORE the encode as well as before the send, the second
   iteration above is refused at reply_begin, with txbuf still intact. */
AXL_WARN_UNUSED bool
s9p_send_reply(
    S9pConn     *conn,   ///< connection whose txbuf holds the encoded reply
    Axl9pWriter *w       ///< writer the reply was built with
);

/* Build + send an Rlerror(tag, ecode). Returns what s9p_send_reply returns,
   and is must-check for the same reason. Carries the same !conn->tx_busy
   precondition, and enforces it BEFORE its own encode rather than
   inheriting the delegated check. */
AXL_WARN_UNUSED bool
s9p_send_rlerror(
    S9pConn *conn,    ///< connection to reply on
    uint16_t tag,     ///< tag of the request being answered
    uint32_t ecode    ///< Linux errno to report (P9_* in axl-9p-server-ops.c)
);

/* Grow conn->rbuf and conn->txbuf together to at least @a new_size (never
   shrinks), rebasing @a r onto the possibly-moved rbuf. Returns true only
   if BOTH buffers reached new_size; see the definition for why a partial or
   refused grow is always safe, and for why the reader is a required
   parameter rather than an optional one. */
AXL_WARN_UNUSED bool
s9p_conn_grow_buffers(
    S9pConn     *conn,      ///< connection whose buffers to grow
    uint32_t     new_size,  ///< target capacity in bytes
    Axl9pReader *r          ///< cursor over rbuf to rebase, or NULL if the caller holds none
);

// ---------------------------------------------------------------------------
// axl-9p-server-ops.c -- dispatch + session/navigation/metadata handlers
// ---------------------------------------------------------------------------

/* Decode the 9P header at the front of conn->rbuf and run the handler for
   it. The message is ALWAYS the first @a len bytes of conn->rbuf -- taking
   a length rather than a (base, len) pair makes that an invariant of the
   signature rather than of the caller, which is what lets a handler rebase
   its reader after a realloc moves rbuf. Returns whether @a conn survived
   the message (see s9p_send_reply); s9p_pump stops draining on false. */
AXL_WARN_UNUSED bool
s9p_dispatch(
    S9pConn *conn,   ///< connection holding the framed message
    size_t   len     ///< length of that message, from the framing loop
);

/* True if a 9P message type can change the exported tree or a node in it,
   and must therefore be refused with Rlerror(EROFS) on a read-only export.
   The list is exactly Tlcreate/Twrite/Tmkdir/Tremove/Trename/Tsetattr.

   Tlopen is NOT on it even though it can truncate: its refusal has to be
   finer-grained (a read-only OPEN of a file on a read-only export is
   perfectly legal and must still succeed), so it makes its own check on
   the access mode -- see s9p_handle_tlopen. Tfsync is not on it either: it
   writes nothing of its own, and POSIX permits fsync on a descriptor
   opened read-only.

   A predicate rather than a case list inlined into s9p_dispatch's switch
   so it can be unit-tested against every type the dispatcher knows,
   including the ones that must NOT be gated -- "the gate covers every
   mutating type" is otherwise a claim no test can make before the Task-6
   live-socket harness exists. */
AXL_WARN_UNUSED bool
s9p_type_is_mutating(
    uint8_t type   ///< 9P message type byte from the wire
);

// ---------------------------------------------------------------------------
// axl-9p-server-io-ops.c -- handlers that work through an open fid handle
// ---------------------------------------------------------------------------

/* Tlopen: fid[4] flags[4] -> Rlopen: qid[13] iounit[4]. */
AXL_WARN_UNUSED bool
s9p_handle_tlopen(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Tread: fid[4] offset[8] count[4] -> Rread: count[4] data[count]. */
AXL_WARN_UNUSED bool
s9p_handle_tread(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Treaddir: fid[4] offset[8] count[4] -> Rreaddir: dcount[4] data[dcount]. */
AXL_WARN_UNUSED bool
s9p_handle_treaddir(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Twrite: fid[4] offset[8] count[4] data[count] -> Rwrite: count[4]. */
AXL_WARN_UNUSED bool
s9p_handle_twrite(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Tfsync: fid[4] datasync[4] -> Rfsync (empty body). */
AXL_WARN_UNUSED bool
s9p_handle_tfsync(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* The pieces of Tread/Twrite/Treaddir arithmetic that are pure functions of
   wire-controlled values. Factored out of the handlers and exported (rather
   than left static) so they can be unit-tested for the hostile cases --
   offsets past EOF, 64-bit offsets no size_t index could hold, made-up
   dirent cursors, a byte count larger than the frame that carries it, a name
   that doesn't fit the reply -- none of which the test suite can reach
   through the handlers themselves until the Task-6 live-socket harness
   exists. */

/* Clamp a wire-supplied Tread (@a offset, @a count) against the file size
   and the space left in the reply. Returns the number of bytes to read --
   0 at or past EOF, which is how 9P spells "end of file" -- and, on a
   non-zero return, writes the offset NARROWED to size_t through @a
   out_offset. The narrowing is safe by construction rather than by
   assertion: a non-zero return implies offset < file_size, and file_size is
   already a size_t, so no information can be lost however large the 64-bit
   wire value was. */
size_t
s9p_read_clamp(
    uint64_t  offset,      ///< byte offset from the wire (unbounded)
    uint32_t  count,       ///< byte count from the wire (unbounded)
    size_t    file_size,   ///< current size of the file being read
    size_t    room,        ///< bytes the reply can still carry
    size_t   *out_offset   ///< [out] narrowed offset; 0 when the return is 0
);

/* Decide whether a wire-supplied Twrite (@a offset, @a count) may be
   executed, and narrow the offset. Answers TWO independently hostile
   values, and is the ONLY thing standing between `data` and a read past
   the receive buffer:

     - @a count is a client-supplied length for a payload that is supposed
       to follow it INSIDE the same frame. The framing loop guarantees the
       frame is complete, not that its count field is honest, so a count of
       0xFFFFFFFF in a 30-byte frame is one arithmetic slip away from
       handing axl_pwrite a pointer into whatever follows rbuf. @a avail is
       what the frame actually still carries (7+4+8+4 = 23 header bytes
       consumed, so `r->len - r->pos` at the call site), and a count larger
       than that is refused outright rather than clamped: a short frame is
       a protocol violation, and silently writing fewer bytes than the
       client believes it sent would corrupt the file it is writing.
     - @a offset is 64-bit and unbounded. It is narrowed only after proving
       that @a offset + @a count still fits a size_t, so the pair can be
       handed to a size_t-indexed positional write without wrapping.

   Returns false with *@a out_offset zeroed when either check fails; the
   caller answers Rlerror(EINVAL) and must not dereference the payload. A
   @a count of 0 is legal (an empty Twrite is answered Rwrite(0)). */
AXL_WARN_UNUSED bool
s9p_write_bounds(
    uint64_t  offset,      ///< byte offset from the wire (unbounded)
    uint32_t  count,       ///< byte count from the wire (unbounded)
    size_t    avail,       ///< payload bytes actually left in the received frame
    size_t   *out_offset   ///< [out] narrowed offset; 0 when the return is false
);

/* Decide whether a wire-supplied Twrite may be executed given how far past
   the end of the file it reaches.

   The SIBLING of s9p_setattr_size_ok, and it exists for exactly the same
   reason: a positional write is allowed to land past EOF, and the UEFI
   write that results grows the file and has the EDK2 FAT driver physically
   zero-fill the gap (see axl_file_truncate's docstring -- the same
   O(size) synchronous fill, reached through a different call). So
   `Twrite(offset = 0xFFFFFFFE, count = 1)` is TWENTY-FOUR bytes on the wire
   that stall this server's single loop while it writes 4 GiB and fills the
   volume. Bounding Tsetattr alone would have left the cheaper and more
   damaging half of the same hazard wide open.

   The bound is the same one, from the same constant: the write may reach at
   most AXL_9P_SERVER_MAX_GROW bytes beyond the current end of the file. A
   write entirely inside the file grows nothing and is always allowed.
   Refusal is Rlerror(EFBIG), matching Tsetattr's answer to the identical
   condition. Total over every input: both additions are guarded against
   wrapping before they are performed. */
AXL_WARN_UNUSED bool
s9p_write_grow_ok(
    uint64_t offset,    ///< byte offset from the wire (already frame-checked)
    uint32_t count,     ///< byte count from the wire (already frame-checked)
    uint64_t cur_size   ///< the file's current length
);

/* True when s9p_write_grow_ok's answer for (@a offset, @a count) actually
   DEPENDS on the file's current length, i.e. when the caller has to go and
   measure it. False means the answer is the same for every possible
   `cur_size`, so the caller passes 0 and skips the measurement entirely.

   That is not an optimization bolted on beside the bound -- it is a
   property OF the bound, which is why it lives here as a predicate over the
   same two inputs rather than as an `if` inside the handler:

     - if offset + count overflows, s9p_write_grow_ok refuses on its
       overflow guard before cur_size is ever read;
     - if offset + count <= AXL_9P_SERVER_MAX_GROW, it is allowed for EVERY
       cur_size -- either the write ends within the file (allowed outright)
       or it grows it by at most (offset + count), which is already inside
       the ceiling.

   The payoff is that an ordinary sequential append to any file under 16 MiB
   -- every write axl_9p_write_file makes at that size, i.e. the common case
   -- costs no size query at all, while a write that reaches past the
   ceiling still pays for an exact, never-cached answer. */
AXL_WARN_UNUSED bool
s9p_write_grow_needs_size(
    uint64_t offset,   ///< byte offset from the wire
    uint32_t count     ///< byte count from the wire
);

/* Decide whether a wire-supplied Tsetattr size may be handed to
   axl_file_truncate. A shrink or a no-op is always allowed -- it moves no
   data. A GROW is allowed only up to AXL_9P_SERVER_MAX_GROW bytes beyond
   the current length; see that constant for why an unbounded grow is a
   one-message denial of service against a single-threaded loop. Total over
   every uint64 pair: the subtraction is only ever performed on the branch
   that has already proved it cannot underflow. */
AXL_WARN_UNUSED bool
s9p_setattr_size_ok(
    uint64_t new_size,   ///< length requested by the wire (unbounded)
    uint64_t cur_size    ///< the file's current length
);

/* Map a wire-supplied Treaddir cursor to a resume position. Record 0 is the
   synthetic ".", record 1 the synthetic "..", record k >= 2 the (k-2)'th
   entry axl_dir_read yields; each record carries dirent offset k+1, and a
   client resumes by handing back the last offset it received. So a cursor
   of N means "N records already delivered" and decomposes into which of the
   two synthetic records are still owed and how many real entries precede
   the resume point. Total function: every uint64 cursor, including ones no
   reply ever emitted, maps to some position rather than faulting. */
void
s9p_readdir_resume(
    uint64_t  cursor,       ///< dirent offset from the wire (0 = start of directory)
    bool     *out_dot,      ///< [out] the "." record is still owed
    bool     *out_dotdot,   ///< [out] the ".." record is still owed
    uint64_t *out_skip      ///< [out] real directory entries preceding the resume point
);

/* Append one Rreaddir record -- qid[13] offset[8] type[1] name[s] -- to
   @a w. Returns false, having written NOTHING and left @a w->overflow
   clear, if the record does not fit in @a room or in the writer: that is
   the ordinary end-of-fill case, not an error, so the caller stops at this
   record boundary and lets the client re-ask from the last offset it did
   emit. A truncated record must never reach the wire.

   PRECONDITION: strlen(@a name) < 0xFFFF, so the name is encodable as a 9P
   string[s]. Every caller sources @a name from AxlFsEntry.name (char[256])
   or from a "." / ".." literal, so this holds by construction. It is a
   precondition rather than a checked case deliberately: the ONLY thing this
   function can say with a false return is "didn't fit", which the fill loop
   answers by stopping for good -- so an unencodable name reported that way
   would silently truncate the whole directory listing rather than skip one
   entry. A checked variant would have to return a third outcome the caller
   could `continue` past; there is no caller that needs one. */
AXL_WARN_UNUSED bool
s9p_dirent_put(
    Axl9pWriter   *w,       ///< writer positioned at the end of the record data
    size_t         room,    ///< record-data bytes the reply can still carry
    const uint8_t *qid,     ///< AXL_9P_QID_LEN-byte qid of the entry
    uint64_t       offset,  ///< this record's dirent cursor (record index + 1)
    uint8_t        dtype,   ///< Linux DT_* kind (DT_DIR 4, DT_REG 8)
    const char    *name     ///< entry basename (UTF-8, NUL-terminated)
);

// ---------------------------------------------------------------------------
// axl-9p-server-ns-ops.c -- handlers that rewrite the namespace / metadata
// ---------------------------------------------------------------------------

/* Tlcreate: dfid[4] name[s] flags[4] mode[4] gid[4] -> Rlcreate: qid[13]
   iounit[4]. Per 9P2000.L, dfid is REBOUND to the newly created open file. */
AXL_WARN_UNUSED bool
s9p_handle_tlcreate(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Tmkdir: dfid[4] name[s] mode[4] gid[4] -> Rmkdir: qid[13]. */
AXL_WARN_UNUSED bool
s9p_handle_tmkdir(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Tremove: fid[4] -> Rremove (empty body). Clunks the fid either way. */
AXL_WARN_UNUSED bool
s9p_handle_tremove(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Trename: fid[4] dfid[4] name[s] -> Rrename (empty body). */
AXL_WARN_UNUSED bool
s9p_handle_trename(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* Tsetattr: fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8]
   atime_nsec[8] mtime_sec[8] mtime_nsec[8] -> Rsetattr (empty body). */
AXL_WARN_UNUSED bool
s9p_handle_tsetattr(
    S9pConn     *conn,   ///< connection to reply on
    Axl9pReader *r,      ///< reader positioned at the request body
    uint16_t     tag     ///< tag of the request being answered
);

/* True if the directory at @a path holds no entries other than the on-disk
   "." and ".." a UEFI FAT directory carries. Lets Tremove answer a non-empty
   rmdir with ENOTEMPTY, which is what a Linux client needs to distinguish
   "you must empty this first" from a real I/O failure -- axl_dir_rmdir
   collapses both into AXL_ERR. Returns false when @a path cannot be opened
   as a directory at all, which the caller treats the same as "not safe to
   remove". */
AXL_WARN_UNUSED bool
s9p_dir_is_empty(
    const char *path   ///< directory path under the export root
);

// ---------------------------------------------------------------------------
// axl-9p-server-fid.c -- fid table + qid/path helpers
// ---------------------------------------------------------------------------

/* Look up @a fid in the connection's table. Returns NULL if unbound. */
S9pFid *
s9p_fid_get(
    S9pConn *conn,   ///< connection whose table to search
    uint32_t fid     ///< client-chosen fid
);

/* Allocate a free table slot for @a fid bound to @a path. Returns NULL
   (slot NOT allocated) if @a fid is already in use, the table is full, or
   @a path doesn't fit S9pFid.path. */
S9pFid *
s9p_fid_alloc(
    S9pConn    *conn,    ///< connection whose table to allocate in
    uint32_t    fid,     ///< client-chosen fid to bind
    const char *path,    ///< absolute AxlFs path under the export root (copied)
    bool        is_dir   ///< true if @a path is a directory
);

/* Close whatever the fid holds OPEN -- its AxlFileView, its write AxlStream
   or its AxlDir -- and clear the open-state fields, leaving the binding
   (fid/path/is_dir) intact. The primitive under s9p_fid_release; called on
   its own by Tlopen, which must not leak a previous open when a client
   re-opens the same fid but must keep the fid bound, and by Trename, which
   has to let go of the firmware handles before the node underneath them
   moves. NULL-safe, and idempotent on an already-closed fid.

   Returns whether everything the fid held came down CLEANLY -- false means
   a write stream's final flush failed, so bytes the client already had
   acknowledged are not on the media. The handles are released either way
   (there is nothing to retry with); the answer exists so the caller can
   tell its peer, which is the only chance the protocol offers. */
AXL_WARN_UNUSED bool
s9p_fid_close_open(S9pFid *f);   ///< fid whose open state to close (NULL-safe)

/* Give a FILE fid the handles its 9P access mode calls for: a page-cached
   AxlFileView for read access, a positional AxlStream for write access,
   both for O_RDWR. Sets f->open on success. Returns false, having released
   whatever it had already opened, if any handle the mode requires could not
   be had -- the caller answers Rlerror and the fid stays bound but
   unopened. Does NOT close a previous open; callers reach it through
   s9p_fid_close_open first (Tlopen) or on a fid that has never been opened
   (Tlcreate). */
AXL_WARN_UNUSED bool
s9p_fid_open_file(
    S9pConn *conn,     ///< connection whose server owns the shared page cache
    S9pFid  *f,        ///< bound file fid to give handles to
    uint32_t accmode   ///< 9P/Linux O_ACCMODE: 0 rdonly, 1 wronly, 2 rdwr
);

/* Re-open the handles @a f held before a s9p_fid_close_open that turned out
   to be unnecessary -- the rollback partner of that call, for a handler that
   has to release the firmware's grip on a node BEFORE attempting something
   that can then fail (Trename drops its handles first -- see the note there
   for why, which is NOT that the firmware refuses the move). Restores the
   same KIND of open the fid had, from the three
   booleans the caller captured beforehand; a fid that held nothing is left
   holding nothing.

   Best-effort by nature: the reopen can itself fail, and there is nothing
   better to do about that than log and leave the fid bound-but-closed, so
   the caller has nothing to check. A restored directory fid restarts its
   iterator at position 0 -- a renamed directory mid-Treaddir is pathological
   enough that re-walking is the honest answer. */
void
s9p_fid_restore_open(
    S9pConn *conn,         ///< connection whose server owns the shared page cache
    S9pFid  *f,            ///< fid to re-open (NULL-safe)
    bool     had_view,     ///< it held a read view
    bool     had_wstream,  ///< it held a write stream
    bool     had_dir       ///< it held a directory iterator
);

/* Close @a f's open state and free its slot. THE teardown path for a fid:
   every site that drops one goes through this or through
   s9p_fid_reset_all, so a future handler cannot add owned state that only
   some of them release. NULL-safe. Returns s9p_fid_close_open's answer --
   false when a write stream's final flush failed. */
AXL_WARN_UNUSED bool
s9p_fid_release(S9pFid *f);   ///< fid to release (NULL-safe)

/* Release @a fid's slot. No-op (and true) if @a fid isn't in the table.
   Returns false when the fid's write stream could not be flushed -- see
   s9p_fid_close_open; Tclunk turns that into Rlerror(EIO). */
AXL_WARN_UNUSED bool
s9p_fid_clunk(
    S9pConn *conn,   ///< connection whose table to release from
    uint32_t fid     ///< fid to release
);

/* Drop every fid, releasing whatever each holds open. Both Tversion --
   which (re)starts the session, and may do so in the middle of a live
   directory walk -- and s9p_reap go through this. */
void
s9p_fid_reset_all(S9pConn *conn);   ///< connection whose table to clear

/* Build a 9P qid (type[1] version[4] path[8]) for the node at @a path
   described by @a e. */
void
s9p_qid(
    const AxlFsEntry *e,                     ///< stat of the node
    const char       *path,                  ///< its export path (hashed into qid.path)
    uint8_t           out[AXL_9P_QID_LEN]    ///< [out] encoded qid
);

/* True if @a comp is safe to treat as one simple path segment: non-empty,
   not truncated against @a cap, no embedded NUL, and free of BOTH path
   separators AxlFs recognizes. */
AXL_WARN_UNUSED bool
s9p_comp_is_safe(
    const char *comp,     ///< NUL-terminated component copied off the wire
    size_t      wire_n,   ///< its ACTUAL on-wire length, from axl_9p_r_str
    size_t      cap       ///< capacity of the buffer @a comp was copied into
);

/* Join @a base + @a name into out[cap] via axl_path_join. Returns false
   (@a out left undefined) if the result doesn't fit, or on allocation
   failure. */
AXL_WARN_UNUSED bool
s9p_path_append(
    char       *out,    ///< [out] joined path
    size_t      cap,    ///< capacity of @a out
    const char *base,   ///< path already known to be within the export root
    const char *name    ///< single component already passed s9p_comp_is_safe
);

/* Move @a path up one directory, refusing to go above @a root. Returns
   false (leaving @a path untouched) if @a path already equals @a root, or
   if the result doesn't fit @a cap. */
AXL_WARN_UNUSED bool
s9p_path_up(
    const char *root,   ///< export root, normalized by axl_9p_server_new
    char       *path,   ///< [in,out] path to move up
    size_t      cap     ///< capacity of @a path
);

#endif /* AXL_9P_SERVER_INTERNAL_H */
