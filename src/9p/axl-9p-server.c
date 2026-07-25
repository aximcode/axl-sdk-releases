/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-9p-server.c
    Axl9pServer -- async 9P2000.L server exporting an AxlFs subtree over TCP.

    This file is the TRANSPORT half: listen on the caller's AxlLoop, accept
    clients into a fixed connection pool, drive each connection's recv via
    axl_tcp_recv_async, reassemble the byte stream into complete 9P messages,
    send the replies, and reap. It mirrors src/net/axl-http-server.c's shape.
    The message handlers live in axl-9p-server-ops.c and the fid table +
    path/qid helpers in axl-9p-server-fid.c; axl-9p-server-internal.h holds
    the shared S9pConn/S9pFid state and describes the seam.

    Two borrowed-address invariants are load-bearing here and are enforced by
    construction rather than by convention -- s9p_send_reply's must-check
    "conn still alive" answer, and s9p_conn_grow_buffers rebasing the
    caller's reader across the realloc that may move rbuf. See those two
    functions before changing the tx_busy / rx_armed state machine.
**/

#include <axl/axl-9p.h>
#include <axl/axl-tcp.h>
#include <axl/axl-loop.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <axl/axl-page-cache.h>
#include "axl-9p-internal.h"
#include "axl-9p-server-internal.h"

AXL_LOG_DOMAIN("9p");

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void s9p_pump(S9pConn *conn);
static bool s9p_on_recv(AxlTcp *sock, AxlStatus status, void *data);
static void s9p_reap_impl(S9pConn *conn, bool abortive);

// ---------------------------------------------------------------------------
// s9p_reap -- close + free a connection slot, idempotent
// ---------------------------------------------------------------------------

/* Close the connection's socket, free its receive/reply buffers, and reset
   the slot to inactive. Safe to call more than once (no-op once inactive)
   and safe to call from inside s9p_on_recv / s9p_on_send / s9p_dispatch:
   those callbacks always return false, so the loop never touches sock/conn
   again after a false return (see the AxlTcpCallback closing-from-callback
   rules in axl-tcp.h). A fid can own an open AxlFileView or AxlDir, so the
   table is released through s9p_fid_reset_all BEFORE the blanket
   axl_memset below -- which would otherwise drop the last reference to
   every handle a client had open when it disconnected mid-transfer.

   @p abortive selects the socket teardown, and mirrors do_reset_connection
   in axl-http-conn.c:

   - false (s9p_reap, every LIVE-server path -- peer closed, recv/send
     error, malformed frame, Tclunk of the last fid): a graceful FIN. The
     server keeps running and keeps pumping the loop, so the deferred
     finalize completes normally on a later tick and the un-ACKed tail of
     the last reply still reaches the client.
   - true (axl_9p_server_free only): a RST with a synchronous, loop-free
     finalize. The whole server is going away and the caller is about to
     free the loop these deferred closes would have been registered
     against -- exactly the "caller-owned event source still active" leak
     axl_loop_free reports, and one more firmware PCB holding the port. */
static void
s9p_reap_impl(S9pConn *conn, bool abortive)
{
    if (conn == NULL || !conn->active) {
        return;
    }

    axl_debug("9p: connection %s closed", conn->client_addr);

    s9p_fid_reset_all(conn);
    if (conn->sock != NULL) {
        if (abortive) {
            axl_tcp_close(conn->sock, AXL_TEARDOWN_RESET);
        } else {
            axl_tcp_close(conn->sock, AXL_TEARDOWN_GRACEFUL);
        }
    }
    axl_free(conn->rbuf);
    axl_free(conn->txbuf);

    /* Preserve the server back-pointer across the reset, matching
       reset_connection's pattern in axl-http-conn.c. */
    Axl9pServer *srv = conn->srv;
    axl_memset(conn, 0, sizeof(*conn));
    conn->srv = srv;
}

void
s9p_reap(S9pConn *conn)
{
    s9p_reap_impl(conn, false);
}

// ---------------------------------------------------------------------------
// s9p_on_send -- async send completion for a reply built in conn->txbuf
// ---------------------------------------------------------------------------

/* Return value is ignored for send per AxlTcpCallback's contract (send is
   one-shot); still checked here because tcp-echo-server.c's on_echo_sent
   establishes the idiom of reaping on a failed send (peer likely gone).
   txbuf is free again once this fires, so this is also where a connection
   whose drain paused on tx_busy resumes -- s9p_pump picks the framing loop
   back up at the next buffered message. Reaping (here or anywhere) never
   routes back through this callback: axl_tcp_close removes the send's loop
   source without invoking it, so an inactive conn can only be seen if a
   caller reaped between arming and completion; the guard below covers it. */
static bool
s9p_on_send(AxlTcp *sock, AxlStatus status, void *data)
{
    S9pConn *conn = (S9pConn *)data;
    (void)sock;

    if (!conn->active) {
        return true;
    }
    conn->tx_busy = false;
    if (status != AXL_OK) {
        s9p_reap(conn);
        return true;
    }
    s9p_pump(conn);
    return true;
}

// ---------------------------------------------------------------------------
// s9p_send_reply / s9p_send_rlerror -- finish + send a reply built in txbuf
// ---------------------------------------------------------------------------

/* Guard on the entry of the encode path (s9p_reply_begin) AND of the send
   path (s9p_send_reply): conn->txbuf may only be touched while no send is in
   flight over it. tx_busy set here is a SERVER bug, not a client one --
   s9p_pump gates dispatch on !tx_busy and every handler sends exactly once,
   so the only way to arrive here twice is a handler that encodes or sends
   inside a loop (the Tread/Treaddir shape) without waiting for s9p_on_send.
   Unreachable today; it exists so that mistake ends as a named warning and a
   closed connection instead of a silent second Transmit over a buffer the
   first one is still reading. Reaping is the right answer either way: the
   peer has just been sent, or is being sent, bytes that no longer match what
   this server intended, so the stream cannot be trusted to resync.

   A PURE PREDICATE, deliberately: despite guarding a resource it never sets
   tx_busy, and it must stay that way. One reply legitimately tests it more
   than once -- s9p_reply_begin checks before the encode and s9p_send_reply
   again before the send, and s9p_send_rlerror reaches both through
   reply_begin -- so a real claim/release pair would make the second test
   fail and self-reap the connection on EVERY reply. "Check" not "claim" is
   the whole contract; do not finish the abstraction a claim/release name
   would imply. */
AXL_WARN_UNUSED static bool
s9p_txbuf_check(S9pConn *conn)
{
    if (!conn->tx_busy) {
        return true;
    }
    axl_warning("9p: reply encoded while a send was still in flight over "
                "txbuf (server bug) -- closing connection %s",
                conn->client_addr);
    s9p_reap(conn);
    return false;
}

/* Begin a reply in conn->txbuf. The ONLY sanctioned way to start writing
   that buffer: the check runs ahead of axl_9p_msg_begin, so a handler that
   tries to build a second reply while the first is still being transmitted
   is refused with txbuf still intact -- where s9p_send_reply's identical
   check could only ever notice after the damage. See the header for the
   full contract; false means `conn` has been reaped and `w` was never
   initialized. */
bool
s9p_reply_begin(S9pConn *conn, Axl9pWriter *w, uint8_t type, uint16_t tag)
{
    if (!s9p_txbuf_check(conn)) {
        return false;
    }
    axl_9p_msg_begin(w, conn->txbuf, conn->txcap, type, tag);
    return true;
}

/* Finish and send a reply already built into conn->txbuf via s9p_reply_begin
   + axl_9p_w_*. conn->txbuf is dedicated to the single in-flight reply, and
   axl_tcp_send_async BORROWS it until s9p_on_send fires (axl-tcp.h: "the
   buffer must stay valid until the callback fires"). Nothing may therefore
   rewrite -- or, worse, axl_realloc -- txbuf while tx_busy is set. That is
   not left to a client behaving itself: a client is free to pipeline two
   requests into one TCP segment, and the framing loop would otherwise
   dispatch the second while the first reply is still draining. s9p_pump
   enforces the invariant instead, refusing to dispatch while tx_busy is
   set and resuming from s9p_on_send, and s9p_txbuf_check backstops both.

   PRECONDITION: !conn->tx_busy. Violating it is a server bug and is
   answered by reaping the connection -- see s9p_txbuf_check.

   Returns whether `conn` SURVIVED the send -- false means it was reaped
   (encode overflow, or the transport refused the send) and every field of
   it is now zeroed: rbuf and txbuf are freed, the fid table is cleared and
   the slot is free for the next client. A caller that touches conn, its
   buffers, or an Axl9pReader over rbuf after a false return is reading
   freed memory. The result is AXL_WARN_UNUSED precisely so that check
   cannot be forgotten: "the send is the last thing a handler does" was
   convention, and a handler that sends inside a loop (Tread/Treaddir) is
   one edit away from breaking it silently. Handlers propagate the answer
   up through s9p_dispatch to s9p_pump, which stops draining on false. */
bool
s9p_send_reply(S9pConn *conn, Axl9pWriter *w)
{
    if (!s9p_txbuf_check(conn)) {
        return false;
    }

    size_t len = axl_9p_msg_finish(w);

    if (w->overflow) {
        /* Every handler sizes its reply against conn->txcap (== conn->msize
           once negotiated), so this should be unreachable in practice; kept
           as a hard backstop so a future handler bug can never smuggle a
           wire write past the end of txbuf. */
        axl_warning("9p: reply encode overflow (unreachable)");
        s9p_reap(conn);
        return false;
    }

    /* Set BEFORE arming so the flag is already true if the completion were
       ever to fire from inside axl_tcp_send_async (it does not today -- the
       completion runs off a loop source -- but s9p_on_send clearing a flag
       this call had not set yet would be a silent trap for whoever changes
       that). */
    conn->tx_busy = true;
    if (axl_tcp_send_async(conn->sock, conn->txbuf, len, conn->srv->loop,
                           NULL, s9p_on_send, conn) != AXL_OK) {
        s9p_reap(conn);
        return false;
    }
    return true;
}

/* Build + send an Rlerror(tag, ecode) reply. Returns what s9p_send_reply
   returns: false once `conn` has been reaped, and must-check for the same
   reason.

   Goes through s9p_reply_begin like every other reply, which is what keeps
   this path -- the one place that both encodes and sends, and the
   most-travelled path in the file -- from writing its 11 bytes into a
   borrowed buffer before any check has run. */
bool
s9p_send_rlerror(S9pConn *conn, uint16_t tag, uint32_t ecode)
{
    Axl9pWriter w;
    if (!s9p_reply_begin(conn, &w, AXL_9P_RLERROR, tag)) {
        return false;
    }
    axl_9p_w_u32(&w, ecode);
    return s9p_send_reply(conn, &w);
}

// ---------------------------------------------------------------------------
// s9p_conn_grow_buffers -- grow rbuf/txbuf together to a negotiated msize
// ---------------------------------------------------------------------------

/* Grow both conn->rbuf and conn->txbuf to at least new_size bytes (never
   shrinks -- a no-op if both are already big enough). axl_realloc leaves the
   original block untouched and returns NULL on allocation failure (it does
   NOT free on failure), so a failed grow here never corrupts or loses either
   buffer -- the caller (s9p_handle_tversion) re-clamps the negotiated msize
   to whatever capacity actually resulted, covering both the "rbuf grew but
   txbuf didn't" partial-failure case and total failure. Returns true only if
   BOTH buffers reached new_size.

   Refuses outright while EITHER buffer is lent to the transport, because
   axl_realloc would free the very block the firmware is reading from or
   writing into: txbuf is borrowed by axl_tcp_send_async until s9p_on_send
   fires, and rbuf is borrowed by axl_tcp_recv_async until s9p_on_recv
   fires. s9p_pump already guarantees no handler runs with either
   outstanding (it won't dispatch while tx_busy, and it only arms a recv
   once the drain has run dry) -- this is the structural backstop for both,
   so neither rule can be quietly broken by a future caller.

   @a r is the caller's cursor over rbuf, and growing rbuf is an
   axl_realloc that MAY MOVE it -- so this rebases @a r onto whatever
   allocation rbuf ends up at (realloc preserved the bytes it holds, and
   the framing loop always dispatches from the front of rbuf, so the
   cursor's position stays meaningful). The reader is a REQUIRED parameter,
   not an optional convenience: knowing to re-point the cursor after a grow
   used to live in the one caller that happened to have one, which made a
   dangling read one new handler away. A caller that genuinely holds no
   cursor over rbuf passes NULL and says so at the call site -- there is
   deliberately no second, rebase-free entry point to reach for. */
bool
s9p_conn_grow_buffers(S9pConn *conn, uint32_t new_size, Axl9pReader *r)
{
    bool ok = false;

    if (!conn->tx_busy && !conn->rx_armed) {
        ok = true;
        if (new_size > conn->rcap) {
            uint8_t *nrbuf = axl_realloc(conn->rbuf, new_size);
            if (nrbuf != NULL) {
                conn->rbuf = nrbuf;
                conn->rcap = new_size;
            } else {
                ok = false;
            }
        }
        if (new_size > conn->txcap) {
            uint8_t *ntxbuf = axl_realloc(conn->txbuf, new_size);
            if (ntxbuf != NULL) {
                conn->txbuf = ntxbuf;
                conn->txcap = new_size;
            } else {
                ok = false;
            }
        }
    }

    /* Unconditional, so the post-condition is simply "@a r points at the
       live rbuf when this returns" -- no caller has to work out which exit
       paths moved it (the refusal and allocation-failure paths did not). */
    if (r != NULL) {
        r->buf = conn->rbuf;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// s9p_arm_recv -- issue (or re-issue) the async recv
// ---------------------------------------------------------------------------

/* Receives directly into conn->rbuf at the current write offset, sized to
   the remaining capacity -- mirroring axl-http-conn.c's arm_transport_recv,
   deliberately NOT the "return true re-arms with the same buffer" idiom
   axl_tcp_recv_async also supports: that re-arm reuses the exact (buf, size)
   pair from the call that started it, which cannot track rlen as it grows
   across partial-frame accumulation and shrinks after each post-dispatch
   memmove. Explicitly re-issuing with an updated pointer/size each time --
   and always returning false from the recv callback -- sidesteps that
   entirely; on_conn_data (axl-http-conn.c) never returns true either. */
static void
s9p_arm_recv(S9pConn *conn)
{
    if (axl_tcp_recv_async(conn->sock, conn->rbuf + conn->rlen,
                           conn->rcap - conn->rlen, conn->srv->loop,
                           NULL, s9p_on_recv, conn) != AXL_OK) {
        s9p_reap(conn);
        return;
    }
    conn->rx_armed = true;
}

// ---------------------------------------------------------------------------
// s9p_pump -- message framing loop + recv re-arm
// ---------------------------------------------------------------------------

/* Consume as many complete size-prefixed messages as are buffered at the
   front of conn->rbuf, dispatching each in turn and compacting the
   remainder down after each one; then make sure the connection is still
   listening. Called from the recv completion (fresh bytes arrived) and from
   the send completion (txbuf freed up, so a paused drain can continue).

   ONE REQUEST IN FLIGHT (phase-4 plan, Global Constraints) is enforced HERE,
   not assumed of the client: a client is free to pipeline (say) Tclunk and
   Tversion into a single TCP segment, and dispatching the second while the
   first reply is still draining would rewrite -- and, via Tversion's msize
   grow, axl_realloc and FREE -- the txbuf that axl_tcp_send_async has
   borrowed. So the loop stops as soon as tx_busy is set, leaving the
   remaining bytes buffered (compacted, so the next message is at rbuf[0]),
   and s9p_on_send resumes it once txbuf is free again. Neither path can
   lose or re-dispatch a byte: rlen only ever shrinks by exactly the frame
   that was dispatched, and the pause happens BEFORE the dispatch, never
   between the dispatch and the compaction. A pause also leaves the RECV
   unarmed, for the symmetric reason -- see the re-arm condition at the
   bottom of this function.

   The `draining` guard makes the resume path safe even if a send ever
   completed synchronously from inside axl_tcp_send_async (it does not
   today -- the completion runs off a loop source): the nested call returns
   immediately and the already-running loop, seeing tx_busy cleared, just
   carries on. Without it that shape would recurse once per buffered
   message, and its re-arm step would hand the transport an rbuf offset that
   the outer loop's next compaction invalidates.

   A hostile client can never grow rlen past rcap: any frame whose claimed
   size exceeds the negotiated msize (itself always <= rcap) is rejected
   before its bytes are ever required to be present. */
static void
s9p_pump(S9pConn *conn)
{
    if (conn->draining) {
        return;
    }
    conn->draining = true;

    while (!conn->tx_busy) {
        if (conn->rlen < 4) {
            break;   /* need the size prefix */
        }

        uint32_t frame_size = (uint32_t)conn->rbuf[0]
                            | ((uint32_t)conn->rbuf[1] << 8)
                            | ((uint32_t)conn->rbuf[2] << 16)
                            | ((uint32_t)conn->rbuf[3] << 24);
        /* Bound against the NEGOTIATED msize, not merely the buffer: a frame
           bigger than the msize both sides agreed on is a protocol violation
           whose stream can no longer be trusted to resync, so it is reaped
           like any other framing error rather than answered with an
           Rlerror. */
        if (frame_size < 7 || frame_size > conn->msize) {
            s9p_reap(conn);   /* malformed / oversized */
            return;
        }
        if (conn->rlen < frame_size) {
            break;   /* partial message; wait for more bytes */
        }

        if (!s9p_dispatch(conn, frame_size)) {
            /* Dispatch reaped the connection (e.g. a failed reply send).
               conn is now a zeroed slot -- rbuf is freed and rlen is 0, so
               computing `rlen - frame_size` below would underflow into a
               bogus memmove length against a NULL rbuf. Must bail here
               (draining is zeroed by the reap along with everything else). */
            return;
        }

        size_t rest = conn->rlen - frame_size;
        if (rest > 0) {
            axl_memmove(conn->rbuf, conn->rbuf + frame_size, rest);
        }
        conn->rlen = rest;
    }

    conn->draining = false;

    /* Re-arm only when the drain has genuinely run dry -- NOT while it is
       merely paused on tx_busy. axl_tcp_recv_async hands rbuf + rlen
       straight to the firmware (axl-tcp-async.c: FragmentTable[0].
       FragmentBuffer = buf) and the driver writes there whenever bytes
       arrive, so a recv armed across a pause would be pointing at an offset
       the resumed drain is about to invalidate: the compaction below moves
       the remaining bytes down (the driver then writes over live data and
       `conn->rlen += n` counts bytes that landed elsewhere -- a re-dispatch
       of an already-handled message), and a Tversion msize grow reallocs
       rbuf outright, leaving the firmware writing into a freed block.
       Gating on tx_busy also makes rx_armed provably false during any
       dispatch, which is what makes that realloc safe by construction.

       This cannot strand a connection: tx_busy is only ever set by
       s9p_send_reply, which either armed a send whose completion calls
       s9p_on_send (clear + pump, and this pump runs again) or reaped. The
       resumed drain ends on rlen < 4 / a partial frame with tx_busy clear
       and arms then. rx_armed guards against double-arming and
       rlen < rcap against a zero-length recv; a full rbuf always holds a
       complete frame (frame_size <= msize <= rcap), so the drain always
       frees space to arm into. */
    if (!conn->tx_busy && !conn->rx_armed && conn->rlen < conn->rcap) {
        s9p_arm_recv(conn);
    }
}

// ---------------------------------------------------------------------------
// s9p_on_recv -- async recv completion
// ---------------------------------------------------------------------------

/* Appends the newly received bytes to conn->rbuf at rlen and pumps. Always
   returns false: the re-arm is explicit (see s9p_arm_recv's comment). */
static bool
s9p_on_recv(AxlTcp *sock, AxlStatus status, void *data)
{
    S9pConn *conn = (S9pConn *)data;

    if (!conn->active) {
        return false;
    }
    conn->rx_armed = false;   /* this completion consumed the armed recv */

    if (status != AXL_OK) {
        s9p_reap(conn);
        return false;
    }

    size_t n = axl_tcp_recv_get_size(sock);
    if (n == 0) {
        s9p_reap(conn);   /* peer closed */
        return false;
    }

    conn->rlen += n;
    s9p_pump(conn);
    return false;
}

// ---------------------------------------------------------------------------
// s9p_on_accept -- async accept completion
// ---------------------------------------------------------------------------

/* The two REJECTION paths below (buffer-alloc failure, pool full) stay
   GRACEFUL, matching axl-http-server.c's equivalents. They fire on a server
   that is still running and still pumping its loop, so the deferred finalize
   completes on a later tick like any other connection drop -- and they leave
   the LISTEN port untouched, which is the only port a stranded child could
   hold hostage here. Only the two teardown paths (the !running branch, and
   axl_9p_server_free) need the abortive close. */
static bool
s9p_on_accept(AxlTcp *client, AxlStatus status, void *data)
{
    Axl9pServer *srv = (Axl9pServer *)data;

    if (status != AXL_OK || client == NULL) {
        return true;   /* per-accept error -- keep listening */
    }
    if (!srv->running) {
        /* Abortive, unlike the two rejection paths below: `running` goes
           false only in axl_9p_server_free, so this client is being
           discarded by a server whose loop is about to be freed. A FIN
           here would defer its finalize onto that loop -- the same
           stranded source and stranded firmware PCB the listener's own
           RESET exists to prevent. Nothing was ever sent on this socket,
           so the RST discards nothing. */
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return false;   /* server torn down -- stop accepting */
    }

    for (size_t i = 0; i < srv->max_conns; i++) {
        S9pConn *conn = &srv->conns[i];
        if (conn->active) {
            continue;
        }

        conn->rbuf = axl_calloc(1, AXL_9P_MSIZE);
        conn->txbuf = (conn->rbuf != NULL) ? axl_calloc(1, AXL_9P_MSIZE) : NULL;
        if (conn->rbuf == NULL || conn->txbuf == NULL) {
            axl_warning("9p: connection buffer alloc failed, rejecting client");
            axl_free(conn->rbuf);
            axl_free(conn->txbuf);
            conn->rbuf = NULL;
            conn->txbuf = NULL;
            axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
            return true;   /* keep accepting -- next client may find a slot */
        }

        conn->active = true;
        conn->sock   = client;
        conn->msize  = AXL_9P_MSIZE;
        conn->rcap   = AXL_9P_MSIZE;
        conn->rlen   = 0;
        conn->txcap  = AXL_9P_MSIZE;

        uint16_t remote_port = 0;
        axl_tcp_get_remote_addr(client, conn->client_addr,
                                sizeof(conn->client_addr), &remote_port);
        axl_debug("9p: accepted connection from %s:%u", conn->client_addr,
                 (unsigned)remote_port);

        s9p_arm_recv(conn);
        return true;   /* keep accepting more clients */
    }

    axl_debug("9p: no free connection slots, rejecting client");
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_9p_server_new(
    AxlLoop      *loop,
    const char   *root,
    bool          read_only,
    Axl9pServer **out
    )
{
    /* An EMPTY root is rejected alongside NULL: it would silently void the
       path-safety design rather than fail loudly. With root == "",
       axl_path_join("", "foo") is "foo", axl_path_get_dirname("foo") is ".",
       and the axl_strcmp(path, root) guard in s9p_path_up never matches --
       so ".." would walk right out of the "export" (which is anchored
       nowhere in the first place). */
    if (loop == NULL || root == NULL || root[0] == '\0' || out == NULL) {
        return AXL_ERR;
    }

    Axl9pServer *s = axl_calloc(1, sizeof(*s));
    if (s == NULL) {
        return AXL_ERR;
    }
    if (axl_strlcpy(s->root, root, sizeof(s->root)) >= sizeof(s->root)) {
        axl_free(s);
        return AXL_ERR;   /* root path too long to store safely */
    }
    /* Strip a single trailing separator ("fs0:\" -> "fs0:", "/export/" ->
       "/export"), unless root IS just that one separator (the bare
       filesystem/volume root, e.g. "/"). Every path this server ever
       builds goes through axl_path_join (Twalk, s9p_path_append), which
       never emits a bare trailing separator, and axl_path_get_dirname
       (s9p_path_up) never returns one either -- so without this
       normalization, a root configured with a trailing separator (the
       conventional way to write a whole-volume root) would never
       textually match a path walked back up to it, defeating the ".."
       root-escape guard entirely (every ".." at the top level would look
       like an escape attempt, or worse, silently succeed one level too
       far, depending on how the comparison skewed). */
    size_t root_len = axl_strlen(s->root);
    if (root_len > 1
        && (s->root[root_len - 1] == '/' || s->root[root_len - 1] == '\\')) {
        s->root[root_len - 1] = '\0';
    }

    s->loop      = loop;
    s->read_only = read_only;
    s->max_conns = AXL_9P_SERVER_DEFAULT_MAX_CONNS;
    s->conns     = axl_calloc(s->max_conns, sizeof(S9pConn));
    if (s->conns == NULL) {
        axl_free(s);
        return AXL_ERR;
    }

    /* ONE page cache for the whole server, allocated up front (the pool is
       eager, so a read never fails for want of a frame mid-transfer) and
       shared by every file fid's AxlFileView via axl_file_view_open_cached.
       That is what bounds total resident file data at PAGE_SIZE *
       PAGE_FRAMES regardless of how many files a client -- or eight
       clients -- hold open at once. Failing server_new outright is the
       honest answer if it can't be had: the alternative, per-view private
       pools, is exactly the unbounded budget this exists to prevent. */
    s->page_cache = axl_page_cache_new_shared(AXL_9P_SERVER_PAGE_SIZE,
                                              AXL_9P_SERVER_PAGE_FRAMES);
    if (s->page_cache == NULL) {
        axl_free(s->conns);
        axl_free(s);
        return AXL_ERR;
    }
    for (size_t i = 0; i < s->max_conns; i++) {
        s->conns[i].srv = s;
    }

    *out = s;
    return AXL_OK;
}

int
axl_9p_server_listen(
    Axl9pServer *s,
    uint16_t     port
    )
{
    if (s == NULL) {
        return AXL_ERR;
    }
    if (s->listener != NULL) {
        return AXL_ERR;   /* already listening */
    }

    uint16_t p = (port == 0) ? AXL_9P_SERVER_DEFAULT_PORT : port;
    if (axl_tcp_listen(p, &s->listener) != AXL_OK) {
        return AXL_ERR;
    }

    s->port    = p;
    s->running = true;
    if (axl_tcp_accept_async(s->listener, s->loop, NULL, s9p_on_accept, s) != AXL_OK) {
        /* Abortive: axl_tcp_listen already BOUND the port, so a graceful
           close here would hand the caller an AXL_ERR it cannot act on --
           a retry (or a second server on the same port) would fail
           Configure() with EFI_INVALID_PARAMETER until the deferred close
           finalized, and EFI_TCP4 has no SO_REUSEADDR. There is no client
           and no in-flight data at this point, so the RST costs nothing
           and the port is free on return. */
        axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
        s->listener = NULL;
        s->running  = false;
        return AXL_ERR;
    }

    axl_info("9p: server listening on port %u", (unsigned)p);
    return AXL_OK;
}

void
axl_9p_server_free(
    Axl9pServer *s
    )
{
    if (s == NULL) {
        return;
    }

    s->running = false;

    /* EXHAUSTIVE, port-releasing teardown -- the shape axl-http-server.c's
       server_free_impl arrived at (commits 94a603b5 / f558bc5d / 9f960b7b),
       reached here for the same reason: freeing a 9P server has no "keep
       serving" variant, and every caller (the resident 9p-serve driver's
       teardown, the selftests) then frees or abandons the loop these
       sockets were registered against.
       A GRACEFUL listener close DEFERS its finalize, so the pending
       axl_tcp_accept_async source outlives this call and axl_loop_free
       reports "caller-owned event source ... still active" -- while the
       EFI_TCP4 child stays bound and the next listen on that port fails
       Configure() with EFI_INVALID_PARAMETER (no SO_REUSEADDR).
       AXL_TEARDOWN_RESET on the LISTENER is what covers the three
       port-holder categories the app cannot reach itself: the listener's
       own child, the firmware accept backlog, and any already-deferred
       closes of connections accepted from it (axl-tcp-sync.c PHASE 0).
       The fourth -- this server's LIVE connections -- is the abortive
       s9p_reap_impl below. Listener first, connections second, matching
       the HTTP server. */
    if (s->listener != NULL) {
        axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
    }

    if (s->conns != NULL) {
        for (size_t i = 0; i < s->max_conns; i++) {
            if (s->conns[i].active) {
                s9p_reap_impl(&s->conns[i], true);
            }
        }
        axl_free(s->conns);
    }

    /* AFTER every conn is reaped, and therefore after every fid's
       AxlFileView has been closed: axl_file_view_close only drops that
       view's frames back into the pool, so the cache has to outlive the
       last view that borrows it. */
    axl_page_cache_free(s->page_cache);
    axl_free(s);
}
