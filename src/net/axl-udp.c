/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-udp.c
    UDP datagram sockets — ported from SoftBMC UdpUtil.c.

    Lifecycle: service binding → create child → configure → send/recv → close.
    Blocking I/O uses poll-wait with 1ms intervals. Async receive integrates
    with AxlLoop via raw event sources.
**/

#include "axl-net-internal.h"
#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-net.h>

AXL_LOG_DOMAIN("udp");

#define UDP_SEND_TIMEOUT_US  (2 * 1000 * 1000)  /* 2 seconds */

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

struct AxlUdpSocket {
    EFI_SERVICE_BINDING_PROTOCOL  *sb;
    EFI_HANDLE                     sb_handle;
    EFI_HANDLE                     udp_handle;
    EFI_UDP4_PROTOCOL             *udp4;

    /* Async receive state */
    AxlLoop                       *loop;
    uint32_t                       loop_source;
    AxlUdpRecvCallback             on_recv;
    void                          *on_recv_data;
    EFI_UDP4_COMPLETION_TOKEN      rx_token;
    EFI_EVENT                      rx_event;
};

// ---------------------------------------------------------------------------
// Service binding helpers
// ---------------------------------------------------------------------------

static int
find_udp_service_binding(
    AxlUdpSocket *sock
    )
{
    EFI_GUID sb_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    size_t count = 0;

    EFI_STATUS status = axl_bs()->LocateHandleBuffer(
        ByProtocol, &sb_guid, NULL, &count, &handles);
    if (status != 0 || count == 0) {
        if (handles) axl_bs()->FreePool(handles);
        return -1;
    }

    sock->sb_handle = handles[0];
    axl_bs()->FreePool(handles);

    EFI_GUID proto_guid = EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID;
    status = axl_bs()->OpenProtocol(
        sock->sb_handle, &proto_guid, (void **)&sock->sb,
        gImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

    return (status == 0) ? AXL_OK : AXL_ERR;
}

static int
create_udp_child(
    AxlUdpSocket *sock
    )
{
    EFI_STATUS status = sock->sb->CreateChild(sock->sb, &sock->udp_handle);
    if (status != 0) {
        return -1;
    }

    EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
    status = axl_bs()->OpenProtocol(
        sock->udp_handle, &udp_guid, (void **)&sock->udp4,
        gImageHandle, sock->udp_handle,
        EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

    if (status != 0) {
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
        sock->udp_handle = NULL;
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

int
axl_udp_open(
    AxlUdpSocket **out,
    uint16_t       local_port
    )
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;

    AxlUdpSocket *sock = axl_new(AxlUdpSocket);
    if (sock == NULL) {
        return AXL_ERR;
    }

    if (find_udp_service_binding(sock) != 0) {
        axl_warning("no UDP4 service binding found");
        axl_free(sock);
        return AXL_ERR;
    }

    if (create_udp_child(sock) != 0) {
        axl_warning("failed to create UDP4 child");
        axl_free(sock);
        return AXL_ERR;
    }

    /* Configure — use DHCP-assigned IP, specified local port */
    EFI_UDP4_CONFIG_DATA cfg;
    axl_memset(&cfg, 0, sizeof(cfg));
    cfg.AcceptBroadcast    = 0;
    cfg.AcceptPromiscuous  = 0;
    cfg.AcceptAnyPort      = 0;
    cfg.AllowDuplicatePort = 0;
    cfg.TimeToLive         = 64;
    cfg.TypeOfService      = 0;
    cfg.DoNotFragment      = 0;
    cfg.ReceiveTimeout     = 0;
    cfg.TransmitTimeout    = 0;
    cfg.UseDefaultAddress  = 1;
    cfg.StationPort        = local_port;

    EFI_STATUS status = axl_efi_call(sock->udp4->Configure, 2,
                                     sock->udp4, &cfg);
    if (status != 0) {
        axl_warning("UDP4 configure failed: 0x%llx",
                   (unsigned long long)status);
        EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
        axl_bs()->CloseProtocol(sock->udp_handle, &udp_guid,
                                gImageHandle, sock->udp_handle);
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
        axl_free(sock);
        return AXL_ERR;
    }

    axl_debug("opened (port=%u)", (unsigned)local_port);
    *out = sock;
    return AXL_OK;
}

void
axl_udp_close(
    AxlUdpSocket *sock
    )
{
    if (sock == NULL) {
        return;
    }

    axl_udp_recv_stop(sock);

    if (sock->udp4 != NULL) {
        axl_efi_call(sock->udp4->Configure, 2, sock->udp4, NULL);
        EFI_GUID udp_guid = EFI_UDP4_PROTOCOL_GUID;
        axl_bs()->CloseProtocol(sock->udp_handle, &udp_guid,
                                gImageHandle, sock->udp_handle);
    }
    if (sock->sb != NULL && sock->udp_handle != NULL) {
        sock->sb->DestroyChild(sock->sb, sock->udp_handle);
    }

    axl_free(sock);
}

// ---------------------------------------------------------------------------
// Public API: blocking send
// ---------------------------------------------------------------------------

int
axl_udp_send(
    AxlUdpSocket        *sock,
    const AxlIPv4Address *dest,
    uint16_t              port,
    const void           *data,
    size_t                len
    )
{
    if (sock == NULL || sock->udp4 == NULL || data == NULL || len == 0) {
        return AXL_ERR;
    }

    EFI_UDP4_SESSION_DATA session;
    axl_memset(&session, 0, sizeof(session));
    axl_memcpy(&session.DestinationAddress, dest, 4);
    session.DestinationPort = port;

    EFI_UDP4_FRAGMENT_DATA frag;
    frag.FragmentLength = (uint32_t)len;
    frag.FragmentBuffer = (void *)data;

    EFI_UDP4_TRANSMIT_DATA tx;
    axl_memset(&tx, 0, sizeof(tx));
    tx.UdpSessionData  = &session;
    tx.GatewayAddress  = NULL;
    tx.DataLength      = (uint32_t)len;
    tx.FragmentCount   = 1;
    tx.FragmentTable[0] = frag;

    EFI_EVENT event = NULL;
    EFI_STATUS status = axl_bs()->CreateEvent(0, 0, NULL, NULL, &event);
    if (status != 0) {
        return AXL_ERR;
    }

    EFI_UDP4_COMPLETION_TOKEN token;
    axl_memset(&token, 0, sizeof(token));
    token.Event  = event;
    token.Status = EFI_ABORTED;
    token.Packet.TxData = &tx;

    status = axl_efi_call(sock->udp4->Transmit, 2, sock->udp4, &token);
    if (status != 0) {
        axl_bs()->CloseEvent(event);
        return AXL_ERR;
    }

    if (_axl_udp_wait(sock->udp4, event, UDP_SEND_TIMEOUT_US) != 0) {
        axl_efi_call(sock->udp4->Cancel, 2, sock->udp4, &token);
        axl_bs()->CloseEvent(event);
        return AXL_ERR;
    }

    int rc = (token.Status == 0) ? AXL_OK : AXL_ERR;
    axl_bs()->CloseEvent(event);
    return rc;
}

// ---------------------------------------------------------------------------
// Public API: blocking send-receive
// ---------------------------------------------------------------------------

int
axl_udp_sendrecv(
    AxlUdpSocket        *sock,
    const AxlIPv4Address *dest,
    uint16_t              port,
    const void           *tx_data,
    size_t                tx_len,
    void                 *rx_buf,
    size_t                rx_size,
    size_t               *rx_len,
    size_t                timeout_ms
    )
{
    if (sock == NULL || sock->udp4 == NULL ||
        tx_data == NULL || tx_len == 0 ||
        rx_buf == NULL || rx_size == 0 || rx_len == NULL) {
        return AXL_ERR;
    }

    *rx_len = 0;

    /* --- Transmit phase --- */
    int rc = axl_udp_send(sock, dest, port, tx_data, tx_len);
    if (rc != AXL_OK) {
        return AXL_ERR;
    }

    /* --- Receive phase --- */
    EFI_EVENT rx_event = NULL;
    EFI_STATUS status = axl_bs()->CreateEvent(0, 0, NULL, NULL, &rx_event);
    if (status != 0) {
        return AXL_ERR;
    }

    EFI_UDP4_COMPLETION_TOKEN rx_token;
    axl_memset(&rx_token, 0, sizeof(rx_token));
    rx_token.Event  = rx_event;
    rx_token.Status = EFI_ABORTED;
    rx_token.Packet.RxData = NULL;

    status = axl_efi_call(sock->udp4->Receive, 2, sock->udp4, &rx_token);
    if (status != 0) {
        axl_bs()->CloseEvent(rx_event);
        return AXL_ERR;
    }

    if (_axl_udp_wait(sock->udp4, rx_event, timeout_ms * 1000) != 0) {
        axl_efi_call(sock->udp4->Cancel, 2, sock->udp4, &rx_token);
        axl_bs()->CloseEvent(rx_event);
        return AXL_ERR;
    }

    if (rx_token.Status != 0 || rx_token.Packet.RxData == NULL) {
        axl_bs()->CloseEvent(rx_event);
        return AXL_ERR;
    }

    /* Copy fragment data to caller's buffer */
    EFI_UDP4_RECEIVE_DATA *rx = rx_token.Packet.RxData;
    size_t total = 0;
    for (uint32_t i = 0; i < rx->FragmentCount; i++) {
        size_t frag_len = rx->FragmentTable[i].FragmentLength;
        size_t copy_len = (total + frag_len <= rx_size)
            ? frag_len : (rx_size - total);
        if (copy_len > 0) {
            axl_memcpy((uint8_t *)rx_buf + total,
                       rx->FragmentTable[i].FragmentBuffer, copy_len);
        }
        total += copy_len;
        if (total >= rx_size) {
            break;
        }
    }
    *rx_len = total;

    /* Return buffer to firmware */
    axl_bs()->SignalEvent(rx->RecycleSignal);
    axl_bs()->CloseEvent(rx_event);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public API: async receive (loop-integrated)
// ---------------------------------------------------------------------------

static bool
on_udp_recv_event(void *data)
{
    AxlUdpSocket *sock = (AxlUdpSocket *)data;

    axl_efi_call(sock->udp4->Poll, 1, sock->udp4);

    if (sock->rx_token.Status != 0 || sock->rx_token.Packet.RxData == NULL) {
        /* Error — stop receiving */
        return AXL_SOURCE_REMOVE;
    }

    EFI_UDP4_RECEIVE_DATA *rx = sock->rx_token.Packet.RxData;

    /* Gather fragments into a contiguous buffer */
    size_t total = 0;
    for (uint32_t i = 0; i < rx->FragmentCount; i++) {
        total += rx->FragmentTable[i].FragmentLength;
    }

    void *buf = NULL;
    if (total <= 2048) {
        /* Stack buffer for small datagrams */
        uint8_t stack_buf[2048];
        size_t pos = 0;
        for (uint32_t i = 0; i < rx->FragmentCount; i++) {
            axl_memcpy(stack_buf + pos,
                       rx->FragmentTable[i].FragmentBuffer,
                       rx->FragmentTable[i].FragmentLength);
            pos += rx->FragmentTable[i].FragmentLength;
        }

        AxlIPv4Address from;
        axl_memcpy(&from, &rx->UdpSession.SourceAddress, 4);
        uint16_t from_port = rx->UdpSession.SourcePort;

        axl_bs()->SignalEvent(rx->RecycleSignal);

        if (sock->on_recv != NULL) {
            sock->on_recv(sock, stack_buf, total,
                          &from, from_port, sock->on_recv_data);
        }
    } else {
        buf = axl_malloc(total);
        if (buf != NULL) {
            size_t pos = 0;
            for (uint32_t i = 0; i < rx->FragmentCount; i++) {
                axl_memcpy((uint8_t *)buf + pos,
                           rx->FragmentTable[i].FragmentBuffer,
                           rx->FragmentTable[i].FragmentLength);
                pos += rx->FragmentTable[i].FragmentLength;
            }

            AxlIPv4Address from;
            axl_memcpy(&from, &rx->UdpSession.SourceAddress, 4);
            uint16_t from_port = rx->UdpSession.SourcePort;

            axl_bs()->SignalEvent(rx->RecycleSignal);

            if (sock->on_recv != NULL) {
                sock->on_recv(sock, buf, total,
                              &from, from_port, sock->on_recv_data);
            }
            axl_free(buf);
        } else {
            axl_bs()->SignalEvent(rx->RecycleSignal);
        }
    }

    /* Re-arm receive */
    axl_memset(&sock->rx_token, 0, sizeof(sock->rx_token));
    sock->rx_token.Event  = sock->rx_event;
    sock->rx_token.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData = NULL;

    EFI_STATUS status = axl_efi_call(sock->udp4->Receive, 2,
                                     sock->udp4, &sock->rx_token);
    if (status != 0) {
        return AXL_SOURCE_REMOVE;
    }

    return AXL_SOURCE_CONTINUE;
}

int
axl_udp_recv_start(
    AxlUdpSocket      *sock,
    AxlLoop           *loop,
    AxlUdpRecvCallback cb,
    void              *data
    )
{
    if (sock == NULL || sock->udp4 == NULL || loop == NULL || cb == NULL) {
        return AXL_ERR;
    }

    if (sock->loop != NULL) {
        axl_warning("recv already started");
        return AXL_ERR;
    }

    /* Create event for receive signaling */
    EFI_STATUS status = axl_bs()->CreateEvent(0, 0, NULL, NULL,
                                              &sock->rx_event);
    if (status != 0) {
        return AXL_ERR;
    }

    sock->loop         = loop;
    sock->on_recv      = cb;
    sock->on_recv_data = data;

    axl_memset(&sock->rx_token, 0, sizeof(sock->rx_token));
    sock->rx_token.Event  = sock->rx_event;
    sock->rx_token.Status = EFI_ABORTED;
    sock->rx_token.Packet.RxData = NULL;

    status = axl_efi_call(sock->udp4->Receive, 2,
                          sock->udp4, &sock->rx_token);
    if (status != 0) {
        axl_bs()->CloseEvent(sock->rx_event);
        sock->rx_event = NULL;
        sock->loop = NULL;
        return AXL_ERR;
    }

    sock->loop_source = axl_loop_add_event(
        loop, sock->rx_event, on_udp_recv_event, sock);

    return AXL_OK;
}

void
axl_udp_recv_stop(
    AxlUdpSocket *sock
    )
{
    if (sock == NULL || sock->loop == NULL) {
        return;
    }

    if (sock->loop_source != 0) {
        axl_loop_remove_source(sock->loop, sock->loop_source);
        sock->loop_source = 0;
    }

    axl_efi_call(sock->udp4->Cancel, 2, sock->udp4, &sock->rx_token);

    if (sock->rx_event != NULL) {
        axl_bs()->CloseEvent(sock->rx_event);
        sock->rx_event = NULL;
    }

    sock->loop         = NULL;
    sock->on_recv      = NULL;
    sock->on_recv_data = NULL;
}
