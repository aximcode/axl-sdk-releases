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
       shared one-send-in-flight path and wedges the loop. send_ptr/total/off
       track progress across chunks; the user callback fires once, after the
       final chunk. */
    AxlTcpCallback  on_send;
    void           *send_data;
    AxlSourceId     send_source;
    AxlSourceId     send_cancel_source;
    const uint8_t  *send_ptr;       /* caller's buffer (borrowed until done) */
    size_t          send_total;     /* total bytes to send */
    size_t          send_off;       /* bytes confirmed sent so far */
    EFI_TCP4_IO_TOKEN           tx_token;
    EFI_TCP4_TRANSMIT_DATA      tx_data;
    EFI_TCP4_FRAGMENT_DATA      tx_frag;
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

/* True if an async send is currently in flight on @p sock (i.e. a prior
   axl_tcp_send_async has not yet completed). axl_tcp_send_async is strictly
   one-send-in-flight; callers that encrypt before sending (axl_tls_write_async)
   must check this BEFORE doing irreversible work so they don't advance the TLS
   sequence number for a record that would then be rejected and dropped. */
bool axl_tcp_send_in_flight(const AxlTcp *sock);

#endif /* AXL_TCP_INTERNAL_H */
