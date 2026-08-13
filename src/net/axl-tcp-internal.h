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
    const uint8_t      *buf;        /* BORROWED from the caller until done */
    size_t              len;
    /* EDK2: RemainDataLen — bytes not yet confirmed on the wire. The active
       send walks it down one bounded Transmit at a time (TCP_SEND_CHUNK_MAX),
       so buf + (len - remaining) is the next chunk. A PARTIAL POSIX send()
       retires part of a token the same way, which is where AxlTcp is going
       (design §3.7). */
    size_t              remaining;
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
    EFI_TCP4_IO_TOKEN           rx_token;
    EFI_TCP4_RECEIVE_DATA       rx_data;
    void           *recv_buf;
    size_t          recv_capacity;  /* buffer capacity — preserved across re-arms */
    size_t          recv_size;      /* bytes received last fire (see axl_tcp_recv_get_size) */
    /* send. A single async send is chunk-chained over the transport: the
       caller's buffer is submitted as a sequence of bounded Transmits
       (TCP_SEND_CHUNK_MAX each) so no one Transmit is unbounded — an
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
    /* Set while callbacks are being delivered. finalize_sock must not free the
       socket underneath that — it records free_deferred and the delivery does
       the free once the last callback has returned. */
    bool                        send_draining;
    bool                        free_deferred;
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

/* Retire every pending send — the ACTIVE one and everything queued behind it —
   with @p status, then run their callbacks before returning. Port of EDK2
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
void axl_tcp_send_flush          (AxlTcp *sock, AxlStatus status);

/* Run the retired-send callbacks queued on @p sock. The loop's deferred-work
   queue calls this; axl_tcp_send_flush calls it directly. */
void axl_tcp_send_drain_done     (AxlTcp *sock);

/* True when @p sock already has an accepted send that has not been retired —
   on the wire or waiting behind one. The async API does not care (it queues);
   axl_tcp_send does, because it cannot outlive its own call. */
bool axl_tcp_send_pending        (const AxlTcp *sock);

/* Abandon the send identified by (@p cb, @p cb_data) — wherever it sits: on the
   wire, on the queue, or retired and waiting for its drain. It is dropped
   WITHOUT its callback, because the only caller is the one context that can
   know the callback would be worse than silence: axl_tcp_send, whose token
   points at its own stack frame and at an ephemeral loop it is about to free.
   Every other abandonment is a cancel (AXL_CANCELLED) or a close
   (axl_tcp_send_flush).

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

#endif /* AXL_TCP_INTERNAL_H */
