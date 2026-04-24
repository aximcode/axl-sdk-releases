/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-udp.h:
 *
 * UDP datagram sockets. Fire-and-forget send, request-response
 * send-receive, and async loop-integrated receive.
 *
 * @code
 * AxlUdpSocket *sock;
 * if (axl_udp_open(&sock, 0) == 0) {
 *     AxlIPv4Address dest;
 *     axl_ipv4_parse("192.168.1.100", &dest);
 *     axl_udp_send(sock, &dest, 514, msg, msg_len);
 *     axl_udp_close(sock);
 * }
 * @endcode
 */

#ifndef AXL_UDP_H
#define AXL_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-loop.h>          /* AxlLoop */
#include <axl/axl-inet-address.h>  /* AxlIPv4Address */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlUdpSocket AxlUdpSocket;

/**
 * @brief Open a UDP socket bound to a local port.
 *
 * Uses the NIC's DHCP-assigned or static IP address.
 * Pass 0 for @a local_port to use an ephemeral port.
 *
 * @return 0 on success, -1 if UDP4 stack is not available.
 */
int
axl_udp_open(
    AxlUdpSocket **sock,       ///< [out] receives socket handle
    uint16_t       local_port  ///< local port (0 = ephemeral)
);

/**
 * @brief Close a UDP socket and release resources. NULL-safe.
 */
void
axl_udp_close(
    AxlUdpSocket *sock  ///< socket to close
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlUdpSocket, axl_udp_close)
#endif

/**
 * @brief Send a UDP datagram. Blocking with 2-second timeout.
 *
 * Fire-and-forget — no response expected.
 *
 * @return 0 on success, -1 on error or timeout.
 */
int
axl_udp_send(
    AxlUdpSocket              *sock,  ///< socket
    const AxlIPv4Address *dest, ///< destination IPv4 address
    uint16_t                    port,  ///< destination port
    const void                 *data,  ///< payload
    size_t                      len    ///< payload length
);

/**
 * @brief Send a datagram and wait for a response.
 *
 * Sends @a tx_data, then polls for an incoming datagram up to
 * @a timeout_ms milliseconds. Useful for DNS queries, NTP, etc.
 *
 * @return 0 on success, -1 on error or timeout.
 */
int
axl_udp_sendrecv(
    AxlUdpSocket              *sock,      ///< socket
    const AxlIPv4Address *dest,     ///< destination IPv4 address
    uint16_t                    port,      ///< destination port
    const void                 *tx_data,   ///< request payload
    size_t                      tx_len,    ///< request length
    void                       *rx_buf,    ///< [out] response buffer
    size_t                      rx_size,   ///< response buffer capacity
    size_t                     *rx_len,    ///< [out] bytes received
    size_t                      timeout_ms ///< receive timeout in ms
);

/**
 * AxlUdpRecvCallback:
 *
 * Called when a datagram is received on an async-monitored socket.
 */
typedef void (*AxlUdpRecvCallback)(
    AxlUdpSocket              *sock,       ///< socket
    const void                *data,       ///< received payload
    size_t                     len,        ///< payload length
    const AxlIPv4Address *from,     ///< sender address
    uint16_t                   from_port,  ///< sender port
    void                      *user_data   ///< caller context
);

/**
 * @brief Start receiving datagrams asynchronously via the event loop.
 *
 * @a cb fires on each received datagram. Receiving continues until
 * axl_udp_recv_stop() is called or the socket is closed.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_udp_recv_start(
    AxlUdpSocket      *sock,  ///< socket
    AxlLoop    *loop,  ///< event loop
    AxlUdpRecvCallback cb,    ///< receive callback
    void              *data   ///< user data for callback
);

/**
 * @brief Stop async receive. No more callbacks after this returns.
 */
void
axl_udp_recv_stop(
    AxlUdpSocket *sock  ///< socket
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_UDP_H */
