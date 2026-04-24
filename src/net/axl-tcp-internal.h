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
#include <axl/axl-tcp.h>

#define TCP_MAPPING_RETRIES   10                    // retries on EFI_NO_MAPPING
#define TCP_MAPPING_DELAY     (1000 * 1000)         // 1s between retries

struct AxlTcp {
    EFI_TCP4_PROTOCOL           *tcp4;
    EFI_HANDLE                  tcp_handle;
    EFI_SERVICE_BINDING_PROTOCOL *tcp_sb;
    EFI_HANDLE                  sb_handle;
    bool                        is_listener;

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
    uint32_t        accept_source;
    uint32_t        accept_cancel_source;
    EFI_TCP4_LISTEN_TOKEN       acc_token;
    /* recv */
    AxlTcpCallback  on_recv;
    void           *recv_data;
    uint32_t        recv_source;
    uint32_t        recv_cancel_source;
    EFI_TCP4_IO_TOKEN           rx_token;
    EFI_TCP4_RECEIVE_DATA       rx_data;
    void           *recv_buf;
    size_t          recv_capacity;  /* buffer capacity — preserved across re-arms */
    size_t          recv_size;      /* bytes received last fire (see axl_tcp_recv_get_size) */
    /* send */
    AxlTcpCallback  on_send;
    void           *send_data;
    uint32_t        send_source;
    uint32_t        send_cancel_source;
    EFI_TCP4_IO_TOKEN           tx_token;
    EFI_TCP4_TRANSMIT_DATA      tx_data;
    EFI_TCP4_FRAGMENT_DATA      tx_frag;
    /* connect */
    AxlTcpCallback  on_connect;
    void           *connect_data;
    uint32_t        connect_source;
    uint32_t        connect_cancel_source;
    EFI_TCP4_CONNECTION_TOKEN   conn_token;
};

/* EFI service-binding helpers shared between sync and async files.
   Defined in axl-tcp-sync.c. */

EFI_STATUS
tcp_find_service_binding(
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

#endif /* AXL_TCP_INTERNAL_H */
