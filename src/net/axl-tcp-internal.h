/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tcp-internal.h
    Private header shared between axl-tcp-sync.c and axl-tcp-async.c.
    Defines the AxlTcp struct (including async state fields) and the
    two EFI service-binding helpers used by both sides.

    Not a public header — do not include from outside src/net/.
**/

#ifndef AXL_TCP_INTERNAL_H
#define AXL_TCP_INTERNAL_H

#include "../backend/axl-backend.h"
#include <axl/axl-inet-address.h>
#include <axl/axl-tcp.h>
#include <axl/axl-loop.h>   /* AxlSourceId */

#define TCP_MAPPING_RETRIES   10                    // retries on EFI_NO_MAPPING
#define TCP_MAPPING_DELAY     (1000 * 1000)         // 1s between retries

/* Outbound send queue — port of EDK2 NetworkPkg/TcpDxe's socket-layer token
   queue (edk2-stable202511, 46548b1: Socket.h SOCK_TOKEN, SockImpl.c
   SockProcessSndToken / SockDataSent, SockInterface.c SockSend).
   See docs/AXL-Tcp-Queue-Design.md.

   EDK2 keeps two lists, SndTokenList (accepted, not yet handed down) and
   ProcessingSndTokenList (handed down, awaiting completion). AXL deliberately
   keeps ONE EFI token in flight and queues above it (design §2), so its
   "processing list" is the single send_active pointer below.

   EVERY accepted send owns a token, the active one included. That is what
   makes retirement allocation-free: a send is retired by moving its token to
   send_done, and the only allocation on the whole path happens in
   axl_tcp_send_async, where a failure is still the caller's to see. */
typedef struct AxlTcpToken {
    struct AxlTcpToken *next;       /* EDK2: LIST_ENTRY TokenList */
    struct AxlTcp      *sock;       /* EDK2: SOCKET *Sock — a cancelling token
                                       must find the queue it sits on */
    const uint8_t      *buf;        /* BORROWED from the caller until done;
                                       the firmware only ever sees a copy */
    size_t              len;
    /* Bytes handed to the firmware so far -- whole pieces, counted when each
       Transmit is accepted -- so buf + committed is the next piece. This is
       what axl_tcp_send_get_sent reports (BSD send()'s partial count): a
       piece once handed over cannot be recalled, so it counts even if the
       send is later cancelled or fails. EDK2's RemainDataLen is len minus
       this. */
    size_t              committed;
    /* Every piece's Transmit completed. False for a send retired while its
       last piece was still armed (a cancel of a fully-armed send), which the
       sync wrapper reports as a timeout rather than a success. */
    bool                finished;
    AxlTcpCallback      cb;
    void               *cb_data;
    AxlCancellable     *cancel;
    AxlLoop            *loop;       /* the SUBMITTER's loop, not sock->async_loop */
    AxlSourceId         cancel_source;  /* armed from accept to retirement */
    AxlStatus           status;     /* set at retirement, read by the delivery */
    /* Scheduled at retirement so this token's callback runs on the loop its
       OWN submitter chose. Per token, not per socket: two callers on two loops
       is exactly the case a shared handle gets wrong — it would run one
       caller's callback from the other's loop, or (if that loop has stopped)
       never run it at all. done_source is the fallback for a full defer ring. */
    uint32_t            done_defer;
    AxlSourceId         done_source;
} AxlTcpToken;

/* EDK2's SndBuffer.HighWater / .LowWater have NO analogue here, deliberately.
   They exist in EDK2 to choose submit-now vs buffer, because its processing
   list can hold several tokens at once. AXL keeps exactly ONE EFI token in
   flight (design §2), so that choice is already made: busy means queue. Neither
   is a rejection threshold in EDK2 either — SockSend buffers and returns
   EFI_SUCCESS rather than refusing (§3.2) — so there is nothing for a
   watermark to gate.

   An earlier cut defined both and used high_water to return AXL_BUSY. That was
   the deviation §3.2 documents; with it gone the constants had no reader, and
   an unread constant that looks like flow control is worse than none.

   The consequence, shared with EDK2 and accepted: the queue is unbounded
   against a peer that never drains. TCP_SEND_QUEUE_WARN_BYTES only makes that
   visible; it does not bound it. Whether a hard depth cap is wanted is design
   §7's open question. */
#define TCP_SEND_QUEUE_WARN_BYTES   (1024u * 1024u)

/* Transmit pacing: ONE per loop that has a send pending on it, shared by every
   socket whose active send belongs to that loop (and, for a moment after it
   retires, the socket's receive: AxlTcpPacing.linger_us). See the "transmit pacing"
   section of axl-tcp-async.c for why it exists. `idle` is the loop source that
   polls the members every iteration; it is 0 while the pacer has backed off
   (no send on this loop made progress for AxlTcpPacing.idle_backoff_ms), and the next
   progress re-adds it. The slot is free when `loop` is NULL. */
typedef struct AxlTcpPacer {
    AxlLoop            *loop;
    AxlSourceId         idle;
    uint64_t            progress_ms;    /* monotonic ms of the last progress */
    bool                progress_known; /* false: the clock could not be read */
    struct AxlTcp      *head;           /* members, linked through pace_next */
} AxlTcpPacer;

struct AxlTcp {
    EFI_TCP4_PROTOCOL           *tcp4;
    EFI_HANDLE                  tcp_handle;
    EFI_SERVICE_BINDING_PROTOCOL *tcp_sb;
    EFI_HANDLE                  sb_handle;
    bool                        is_listener;
    /* Identifies the listener a socket belongs to: for a listener, its own
       unique id; for an accepted connection, its accepting listener's id; 0 for
       a client socket. Lets an abortive listener teardown find and finalize the
       deferred (graceful, loop-owned) closes of its own children synchronously,
       scoped to this listener (children share the NIC's service binding with
       every other Tcp4 user, so sb_handle is too broad). A monotonic id, not a
       pointer, so a freed-then-reused listener address can never alias-match a
       stale child's deferred close. */
    uint64_t                    listener_id;

    /* async state. Each op gets its own cancel_source so independent
       cancellables can coexist (e.g. concurrent recv + send with
       different cancel tokens). Zero means "no cancellable observed".
       Caveat: cancelling a pending connect is terminal — on_connect_cancel
       calls axl_tcp_close(sock), which tears down every other armed
       op on the same sock. recv/send/accept cancels are NOT terminal
       and leave the sock/listener valid. */
    AxlLoop        *async_loop;
    /* axl_net_set_latency's list of live sockets (axl-net-latency.c): linked
       once the socket has a TCP instance, unlinked in finalize_sock before
       that instance goes. Changed only at TPL_CALLBACK. */
    struct AxlTcp  *lat_next;
    bool            lat_linked;
    /* accept */
    AxlTcpCallback  on_accept;
    void           *accept_data;
    AxlSourceId     accept_source;
    AxlSourceId     accept_cancel_source;
    EFI_TCP4_LISTEN_TOKEN       acc_token;
    /* recv */
    AxlTcpCallback  on_recv;
    void           *recv_data;
    AxlSourceId     recv_source;
    AxlSourceId     recv_cancel_source;
    /* The loop recv_source and recv_cancel_source are registered on, valid
       while either is set. Not async_loop: every op stamps that one (a send
       on another loop moves it), and source ids are unique across loops, so
       removing a receive's source from any other loop is a silent no-op
       that leaves it registered. */
    AxlLoop        *recv_loop;
    /* Counts for the "recv-stats" test point (_axl_tcp_test_point): the
       receives completed, the lingers begun (AxlTcpPacing.linger_us), and
       the receives that completed while one was in force -- each a request
       taken at once rather than at the firmware's next 10 ms timer. */
    uint32_t        stat_recvs;
    uint32_t        stat_lingers;
    uint32_t        stat_linger_recvs;
    EFI_TCP4_IO_TOKEN           rx_token;
    EFI_TCP4_RECEIVE_DATA       rx_data;
    void           *recv_buf;
    size_t          recv_capacity;  /* buffer capacity — preserved across re-arms */
    size_t          recv_size;      /* bytes received last fire (see axl_tcp_recv_get_size) */
    EFI_STATUS      recv_end;       /* how the last recv ended: EFI_SUCCESS while
                                       data flows, EFI_CONNECTION_FIN at an orderly
                                       close, the transport's error otherwise (a
                                       reset, a timeout); see _axl_tcp_recv_fin */
    /* send. A single async send is chunk-chained over the transport: the
       caller's buffer is submitted as a sequence of bounded Transmits
       (AxlTcpPacing.chunk_bytes each) so no one Transmit is unbounded — an
       unbounded Transmit to a client that can't drain it monopolizes the
       shared one-send-in-flight path and wedges the loop. The active token's
       `remaining` tracks progress across chunks; the user callback fires
       once, after the final chunk. */
    AxlSourceId     send_source;
    /* EDK2 ProcessingSndTokenList, of capacity one: the token whose bytes are
       with the firmware. NULL when the transport is idle. */
    AxlTcpToken                *send_active;
    EFI_TCP4_IO_TOKEN           tx_token;
    EFI_TCP4_TRANSMIT_DATA      tx_data;
    EFI_TCP4_FRAGMENT_DATA      tx_frag;
    /* Bytes handed to the firmware by the Transmit in flight, for the
       diagnostics -- the token's `committed` is what advances. */
    size_t                      tx_armed;
    /* AxlTcp's own copy of the piece in flight: the firmware is handed THIS,
       never the caller's buffer, so no caller memory is referenced once its
       callback has run and a Transmit never has to be cancelled to get a
       buffer back. At least one piece long; allocated on the first send,
       grown only between Transmits (one is in flight at a time), and freed
       in finalize_sock once the firmware has released the socket. */
    uint8_t                    *tx_bounce;
    size_t                      tx_bounce_cap;
    /* A Transmit is armed that no send owns any more: its send was
       cancelled (or its sync caller timed out or was interrupted) after the
       piece was handed over. EFI_TCP4.Cancel on a Transmit TcpDxe is still
       processing frees its token and leaves its data queued, which the next
       segmentation trips over (SockDataSent's ASSERT on DEBUG firmware), so
       AxlTcp never cancels one: the orphan goes out on its own, and the next
       send is armed only once its token has signalled. */
    bool                        tx_orphan;
    /* A Transmit on tx_token has been accepted and its completion not yet
       consumed -- whoever owns it (the active send or the orphan). The one
       fact finalize_sock needs: if the firmware's release FAILS while this is
       set, the firmware may still read tx_bounce, so the buffer is kept. */
    bool                        tx_inflight;
    /* The loop send_source sits on: the active send's loop, or -- while an
       orphan is waiting to complete -- the loop of the send queued behind it
       (none if nothing is queued). Kept separately from async_loop, which the
       recv/connect paths and the sync wrappers also rewrite. */
    AxlLoop                    *send_source_loop;
    /* Some send was retired with 0 < committed < len: the peer holds a
       message cut short. Never cleared -- a later close is always abortive,
       because a FIN would make the truncated stream look complete. */
    bool                        stream_torn;
    /* The count and completion of the send whose callback most recently
       started, or that axl_tcp_send most recently returned: what
       axl_tcp_send_get_sent reports and the sync wrapper maps to a status. */
    size_t                      send_last_sent;
    bool                        send_last_finished;
    /* Deferred sends — EDK2 SndTokenList. FIFO: promoted head-first from
       on_send_complete once the active send retires. send_queued_bytes counts
       what is accepted but not yet handed to the firmware. */
    AxlTcpToken                *send_queued_head;
    AxlTcpToken                *send_queued_tail;
    size_t                      send_queued_bytes;
    /* Retired sends awaiting their callbacks — the port of EDK2's
       SIGNAL_TOKEN, which is gBS->SignalEvent and therefore QUEUES the
       consumer's notify instead of calling it. Retirement moves a token here
       and schedules its delivery on the token's own loop, which runs it at the
       top of that loop's next iteration — so no consumer code ever runs inside
       the transport's own call stack. The list exists so axl_tcp_close can
       deliver what is still owed synchronously, because the loop may be freed
       the moment close returns.

       Each token's schedule is valid only while its loop lives: close the
       socket BEFORE freeing any loop it was used from, the same ordering every
       other loop source on this struct requires (axl-tcp.h). */
    AxlTcpToken                *send_done_head;
    AxlTcpToken                *send_done_tail;
    /* The pacer this socket is a member of while its active send is on the
       wire (NULL otherwise), and the next member of that pacer. Membership
       follows send_active: tcp_pace_update joins, moves or leaves. */
    AxlTcpPacer                *pacer;
    struct AxlTcp              *pace_next;
    /* Still a member after its last send retired, while a receive is armed
       on the same loop: until this monotonic time in us (0: not lingering).
       See AxlTcpPacing.linger_us. */
    uint64_t                    pace_linger_us;
    /* Set while callbacks are being delivered. finalize_sock must not free the
       socket underneath that — it records free_deferred and the delivery does
       the free once the last callback has returned. */
    bool                        send_draining;
    bool                        free_deferred;
    /* The firmware's release failed with a Transmit in flight
       (finalize_sock): TcpDxe may still write tx_token's status and read
       tx_data / tx_frag -- which live INSIDE this struct -- and tx_bounce.
       Neither free site may release the socket then; it is leaked whole. */
    bool                        firmware_holds;
    /* Set by tcp_close_impl when teardown starts. A socket is torn down once:
       a second close (typically from a callback the teardown itself fired) is
       a no-op, and new sends/receives are refused rather than armed against
       firmware state that is already released. */
    bool                        closed;
    /* connect */
    AxlTcpCallback  on_connect;
    void           *connect_data;
    AxlSourceId     connect_source;
    AxlSourceId     connect_cancel_source;
    EFI_TCP4_CONNECTION_TOKEN   conn_token;
};

/* EFI service-binding helpers shared between sync and async files.
   Defined in axl-tcp-sync.c. */

/* Walk every TCP4 service binding handle and pick one that's actually
   usable for the requested route:
     1. If `forced_source != NULL`, only a handle whose
        IP4Config2 InterfaceInfo.StationAddress matches is acceptable.
     2. Otherwise, skip handles with StationAddress == 0.0.0.0.
     3. Among the remaining, prefer one whose
        (StationAddress & SubnetMask) == (dest & SubnetMask).
     4. Fall back to the first valid handle if no subnet match.
   `forced_source == NULL` is the default "auto" path; pass an
   AxlIPv4Address to pin to a specific local interface.
   `dest` is the destination IPv4 — used only for subnet-match
   preference and may be NULL (degrades to "first valid"). */
EFI_STATUS
tcp_find_service_binding(
    const EFI_IPv4_ADDRESS         *dest,
    const EFI_IPv4_ADDRESS         *forced_source,
    EFI_SERVICE_BINDING_PROTOCOL  **sb,
    EFI_HANDLE                    *out_handle
);

EFI_STATUS
tcp_create_child(
    EFI_SERVICE_BINDING_PROTOCOL  *sb,
    EFI_HANDLE                    *child_handle,
    EFI_TCP4_PROTOCOL             **tcp4
);

/* Per-op teardown helpers exported from axl-tcp-async.c so the sync
   wrappers and axl_tcp_close can share the same cleanup logic. Each
   removes the completion source (if active), the cancel source (if
   registered), and — for accept/connect which don't persist the
   event across calls — closes the token's completion event. */

void axl_tcp_accept_drop_sources (AxlTcp *listener);
void axl_tcp_connect_drop_sources(AxlTcp *sock);
void axl_tcp_recv_drop_sources   (AxlTcp *sock);
void axl_tcp_send_drop_sources   (AxlTcp *sock);

/* Test seams (axl-test-net's linger tests): whether a transmit-pacer slot is
   bound to @p loop, and whether @p sock is a lingering pacer member. A slot
   bound to a loop the caller has freed is the use-after-free the linger rules
   exist to prevent. */
bool _axl_tcp_pacer_bound(const AxlLoop *loop);
bool _axl_tcp_lingering(const AxlTcp *sock);

/**
 * Test seam: true at @p point turns on a test-only behaviour. One point:
 *
 *   "recv-stats"  axl_tcp_close logs (INFO) the socket's receive counts
 *                 (stat_recvs, stat_lingers, stat_linger_recvs) if it
 *                 received at all -- how test-sshd-linger-qemu.sh measures
 *                 what AxlTcpPacing.linger_us promises, in the guest.
 *
 * The library's definition is WEAK and always false, so production carries
 * no trace of a test control; a test image links a strong one
 * (test/integration/sshd-test-hook.c). EVERY TEST SEAM IS NAMED
 * `*_test_point`: test-efi-pack-release-qemu.sh finds the seams in a shipped
 * image by that suffix alone, and fails one whose size is not the weak
 * always-false stub's.
 */
bool
_axl_tcp_test_point(
    const char *point
    );

/* Retire every pending send — the ACTIVE one and everything queued behind it —
   then run their callbacks before returning. Queued sends are always
   AXL_CANCELLED. The active send is AXL_CANCELLED in an abortive close; a
   @p graceful close (which only happens when the active send, if any, is
   fully armed) retires it AXL_OK, or with the status its last Transmit
   already completed with. Port of EDK2
   SockConnFlush, which runs SockFlushPendingToken over the processing list as
   well as SndTokenList (design §3.5).

   Called only from the close path, and synchronous for one reason: the caller's
   loop may be freed the moment axl_tcp_close returns, so a retirement left for
   the next tick would never be delivered. Everywhere else, retirement is
   deferred (see send_done_head).

   Safe against the socket being freed underneath it: a callback that closes
   this socket re-enters a close that is already torn down (`closed`), which
   returns without finalizing, and a finalize from an OUTER drain defers its
   free until the drain is done. */
void axl_tcp_send_flush          (AxlTcp *sock, bool graceful);

/* Run the retired-send callbacks queued on @p sock. The loop's deferred-work
   queue calls this; axl_tcp_send_flush calls it directly. */
void axl_tcp_send_drain_done     (AxlTcp *sock);

/* True when @p sock already has an accepted send that has not been retired —
   on the wire or waiting behind one. The async API does not care (it queues);
   axl_tcp_send does, because it cannot outlive its own call. An orphan piece
   is not a pending send: no caller owns it. */
bool axl_tcp_send_pending        (const AxlTcp *sock);

/* True when a close now would drop accepted bytes or end a truncated stream:
   a send is queued, the active send is only partly armed, or the stream is
   already torn. axl_tcp_close promotes a graceful close to abortive then. */
bool axl_tcp_send_close_drops    (const AxlTcp *sock);

/* Abandon the send identified by (@p cb, @p cb_data) — wherever it sits: on the
   wire, on the queue, or retired and waiting for its drain. It is dropped
   WITHOUT its callback, because the only caller is the one context that can
   know the callback would be worse than silence: axl_tcp_send, whose token
   points at its own stack frame and at an ephemeral loop it is about to free.
   Every other abandonment is a cancel (AXL_CANCELLED) or a close
   (axl_tcp_send_flush).

   A send on the wire is abandoned the way a cancel stops it: its armed piece
   is left to the firmware as the socket's orphan, never cancelled. The
   bytes it had committed are left in send_last_sent for axl_tcp_send to
   report.

   Returns true if a token was found. */
bool axl_tcp_send_drop_token     (AxlTcp *sock, AxlTcpCallback cb,
                                  void *cb_data);

/* Internal connect variant that also hands back the in-progress socket via
   @p out_pending (NULL OK). The public axl_tcp_connect_async_via wraps this
   with out_pending == NULL. The sync connect wrapper uses it to reach the
   socket's tcp4 so it can drive Poll() while its ephemeral loop blocks at a
   raised TPL (see tcp_sync_poll_tick in axl-tcp-sync.c). @p *out_pending is
   set only on the AXL_OK return; on any error the socket is already torn
   down, so it stays whatever the caller initialized it to (use NULL). */
int
axl_tcp_connect_async_via_ex(
    const char            *host,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data,
    AxlTcp               **out_pending
    );

/* Connect to an ALREADY-RESOLVED IPv4 address — the resolve-free core of
   axl_tcp_connect_async_via_ex. The async HTTP client calls this after its
   own axl_net_resolve_async so the whole request stays nest-free (the _via_ex
   form resolves synchronously, nesting an ephemeral loop). Same out_pending
   contract: set only on the AXL_OK return. */
int
axl_tcp_connect_addr_async(
    const AxlIPv4Address  *dest,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data,
    AxlTcp               **out_pending
    );

/* Did the socket's last recv end at an ORDERLY close -- the peer's FIN
   (EFI_CONNECTION_FIN)? false while data flows, and after a reset, a
   timeout, a cancel or any other transport error. A recv reports every
   ending as a failure (AXL_ERR to its callback); this tells the clean one
   apart, which a body framed by the close needs: only a FIN ends it. */
bool
_axl_tcp_recv_fin(
    const AxlTcp *sock
    );

// axl_net_set_latency hooks (axl-net-latency.c). link/unlink keep the list
// of live sockets the latency poll walks; activity is called wherever an
// operation is armed or completes, and keeps a WINDOW policy fast.
void _axl_net_latency_link(AxlTcp *sock);
void _axl_net_latency_unlink(AxlTcp *sock);
void _axl_net_latency_activity(void);

#endif /* AXL_TCP_INTERNAL_H */
