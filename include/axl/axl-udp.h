/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-udp.h:
 *
 * UDP datagram sockets. Fire-and-forget send, request-response
 * send-receive, and async loop-integrated receive.
 *
 * @code
 * AxlUdp *sock;
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
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-loop.h>          /* AxlLoop */
#include <axl/axl-inet-address.h>  /* AxlIPv4Address */

typedef struct AxlCancellable AxlCancellable;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlUdp AxlUdp;

/**
 * @brief Open a UDP socket bound to a local port.
 *
 * Uses the NIC's DHCP-assigned or static IP address.
 * Pass 0 for @a local_port to use an ephemeral port.
 *
 * @return AXL_OK on success, AXL_ERR if UDP4 stack is not available.
 */
int
axl_udp_open(
    AxlUdp **sock,       ///< [out] receives socket handle
    uint16_t       local_port  ///< local port (0 = ephemeral)
);

/**
 * @brief Open a UDP socket with explicit source-NIC selection.
 *
 * Like axl_udp_open but adds optional pinning to a specific
 * local interface by station IP. When @p source_ip is non-NULL and
 * non-zero, walks the UDP4 service-binding handles and picks the
 * one whose IP4Config2 station address matches — useful when the
 * host has multiple NICs (e.g. an in-band BMC USB-NIC at
 * 169.254.1.0/24 alongside a regular ethernet) and the consumer
 * needs to control which interface emits the datagram.
 *
 * Pass @p source_ip = NULL or {0,0,0,0} for the auto-pick path
 * (same behavior as axl_udp_open). Mirrors the
 * axl_tcp_listen_via shape.
 *
 * @return AXL_OK on success, AXL_ERR on failure (including "no
 *     interface has station IP @p source_ip" when forced).
 */
int
axl_udp_open_via(
    AxlUdp          **sock,        ///< [out] receives socket handle
    uint16_t                local_port,  ///< local port (0 = ephemeral)
    const AxlIPv4Address   *source_ip    ///< NULL or zeros = auto-pick
);

/**
 * @brief Close a UDP socket and release resources. NULL-safe.
 */
void
axl_udp_close(
    AxlUdp *sock  ///< socket to close
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlUdp, axl_udp_close)
#endif

/**
 * @brief Query the local address of an open UDP socket.
 *
 * Useful in two cases:
 *   - After axl_udp_open with @c local_port = 0 (ephemeral),
 *     to read back the kernel-assigned port so it can be advertised
 *     to a peer.
 *   - For diagnostics — confirming the bound interface IP after
 *     DHCP / static configuration.
 *
 * @p addr is filled with the dotted-decimal station address (min
 * 16 bytes); @p out_port receives the bound local port.
 *
 * @return AXL_OK on success, AXL_ERR if the socket is unconfigured
 *     or the underlying GetModeData call fails.
 */
int
axl_udp_get_local_addr(
    AxlUdp *sock,      ///< open socket
    char         *addr,      ///< buffer for dotted-decimal address (min 16 bytes)
    size_t        size,      ///< size of addr buffer
    uint16_t     *out_port   ///< receives bound local port number
);

/**
 * AxlUdpSendCallback:
 *
 * Callback for axl_udp_send_async. Mirrors AxlTcpCallback's
 * shape — send is one-shot, so the return value is ignored
 * (convention: `return true`).
 *
 * @p status is an AxlStatus value:
 *   - AXL_OK on a successful Transmit
 *   - AXL_ERR on a UEFI-reported send error
 *   - AXL_CANCELLED if the cancellable was signalled before completion
 */
typedef bool (*AxlUdpSendCallback)(
    AxlUdp *sock,    ///< socket
    AxlStatus     status,  ///< AXL_OK / AXL_ERR / AXL_CANCELLED
    void         *data     ///< caller-provided context
);

/**
 * @brief Async send — initiates Transmit, callback on completion.
 *
 * Mirrors axl_tcp_send_async. The buffer at @p buf must stay
 * valid until the callback fires (the UEFI Transmit op references it
 * directly — no intermediate copy).
 *
 * **No preemption.** Calling this while a previous send on @p sock is
 * still in flight returns AXL_ERR. To interrupt an in-flight send,
 * pass an AxlCancellable to the original call and signal it; the
 * cancel callback fires cleanly with AXL_CANCELLED.
 *
 * @return AXL_OK if initiated, AXL_ERR on immediate failure or if a
 *     send is already in flight on this socket.
 */
int
axl_udp_send_async(
    AxlUdp          *sock,    ///< socket
    const AxlIPv4Address  *dest,    ///< destination IPv4 address
    uint16_t               port,    ///< destination port
    const void            *buf,     ///< send buffer (must stay valid until cb)
    size_t                 len,     ///< bytes to send
    AxlLoop               *loop,    ///< event loop
    AxlCancellable        *cancel,  ///< optional cancel token (NULL = uncancellable)
    AxlUdpSendCallback     cb,      ///< callback when send completes (return ignored)
    void                  *data     ///< caller context
);

/**
 * @brief Pin the socket to a single peer (Linux-style `connect()` on UDP).
 *
 * Re-configures the underlying UDP4 instance with `RemoteAddress` +
 * `RemotePort` set to @p peer / @p port. After this:
 *
 *   - The kernel filters incoming datagrams to ones from the peer
 *     (other senders' packets are dropped).
 *   - Subsequent `axl_udp_send` / `axl_udp_send_async` calls accept
 *     NULL @p dest — the configured peer is used. Passing a non-NULL
 *     @p dest still overrides the peer for that packet.
 *
 * Idempotent: calling again with a different peer rebinds. Use
 * axl_udp_disconnect to clear the lock without picking a new peer.
 *
 * @return AXL_OK on success, AXL_ERR on Configure failure.
 */
int
axl_udp_connect(
    AxlUdp                *sock,  ///< socket
    const AxlIPv4Address  *peer,  ///< peer address
    uint16_t               port   ///< peer port
);

/**
 * @brief Clear a peer lock previously installed by axl_udp_connect.
 *
 * After this, the socket accepts datagrams from any sender again
 * and `axl_udp_send` requires an explicit @p dest.
 *
 * @return AXL_OK on success, AXL_ERR on Configure failure.
 */
int
axl_udp_disconnect(
    AxlUdp *sock  ///< socket
);

/**
 * @brief Join an IPv4 multicast group on this socket.
 *
 * After joining, the socket receives datagrams sent to @p group
 * (in addition to its station unicast address). Useful for
 * service-discovery protocols (mDNS at 224.0.0.251, SSDP at
 * 239.255.255.250) and other multicast traffic.
 *
 * Idempotent only if the underlying UEFI driver is — UEFI 2.x
 * doesn't define behavior for joining the same group twice.
 *
 * @return AXL_OK on success, AXL_ERR if @p group is not a valid
 *     224.0.0.0/4 multicast address or the UEFI Groups() call fails.
 */
int
axl_udp_join_multicast(
    AxlUdp                *sock,    ///< socket
    const AxlIPv4Address  *group    ///< multicast group address (224.0.0.0/4)
);

/**
 * @brief Leave a previously-joined multicast group.
 *
 * Pass @p group = NULL to leave ALL groups this socket has joined
 * (matches UEFI 2.x UDP4.Groups() semantics for `JoinFlag=FALSE`
 * with `MulticastAddress = NULL`).
 *
 * @return AXL_OK on success, AXL_ERR if the UEFI Groups() call fails.
 */
int
axl_udp_leave_multicast(
    AxlUdp                *sock,    ///< socket
    const AxlIPv4Address  *group    ///< group to leave, or NULL for all
);

/**
 * @brief Enable or disable reception of broadcast datagrams.
 *
 * Re-Configures the underlying UDP4 instance with
 * `AcceptBroadcast = enable`. Default for a freshly-opened socket
 * is FALSE (broadcasts dropped). Sending broadcasts (e.g. to
 * 255.255.255.255 or a subnet broadcast) does not require this —
 * it gates the recv-side filter only.
 *
 * @return AXL_OK on success, AXL_ERR on Configure failure.
 */
int
axl_udp_set_broadcast(
    AxlUdp *sock,     ///< socket
    bool    enable    ///< true to accept inbound broadcasts
);

/**
 * @brief Send a UDP datagram. Blocking with 2-second timeout.
 *
 * Fire-and-forget at the protocol level — no response expected. The
 * call still blocks until the local Transmit completes (a few ms for a
 * datagram), so it is synchronous.
 *
 * Safe to call from an @c axl_loop_attach_driver pump callback (at
 * `TPL_CALLBACK`): the nested completion wait is raised-TPL-safe. It
 * busy-holds `TPL_CALLBACK` for the brief Transmit, so it adds a small
 * latency spike to co-located work on the same pump but does not wedge.
 * For a non-blocking send that returns immediately and reports
 * completion via a callback, use @c axl_udp_send_async (the caller
 * must keep @p data alive until the callback fires).
 *
 * @p dest may be NULL only if the socket has been pinned to a peer
 * via axl_udp_connect (the configured peer is used). Otherwise
 * @p dest is required.
 *
 * @return AXL_OK on success; AXL_CANCELLED if the 2-second send deadline
 *     elapses first (the synchronous wrapper arms an internal timeout that
 *     cancels the pending Transmit); AXL_ERR on a transmit or setup error.
 */
AxlStatus
axl_udp_send(
    AxlUdp              *sock,  ///< socket
    const AxlIPv4Address *dest, ///< destination IPv4 address (NULL = use connected peer)
    uint16_t                    port,  ///< destination port (ignored if dest is NULL)
    const void                 *data,  ///< payload
    size_t                      len    ///< payload length
);

/**
 * @brief Send a datagram and wait for a response.
 *
 * Sends @a tx_data, then polls for an incoming datagram up to
 * @a timeout_ms milliseconds. Useful for DNS queries, NTP, etc.
 *
 * @return AXL_OK on success, AXL_ERR on error or timeout.
 */
int
axl_udp_sendrecv(
    AxlUdp              *sock,      ///< socket
    const AxlIPv4Address *dest,     ///< destination IPv4 address
    uint16_t                    port,      ///< destination port
    const void                 *tx_data,   ///< request payload
    size_t                      tx_len,    ///< request length
    size_t                      timeout_ms,///< receive timeout in ms
    void                       *rx_buf,    ///< [out] response buffer
    size_t                      rx_size,   ///< response buffer capacity
    size_t                     *rx_len     ///< [out] bytes received
);

/**
 * AxlUdpCallback:
 *
 * Callback for async UDP receive. Mirrors AxlTcpCallback shape:
 * the consumer gets per-event @p status, can stop in-place by
 * returning false, and the op honors an optional AxlCancellable.
 *
 * @p status is an AxlStatus value:
 *   - AXL_OK on a delivered datagram (@p data / @p len / @p from
 *     describe the payload)
 *   - AXL_ERR on a UEFI-reported recv error (token Status non-zero
 *     or RxData NULL); @p data is NULL, @p len is 0
 *   - AXL_CANCELLED if the cancellable passed to axl_udp_recv_async
 *     was signalled before the next datagram arrived; @p data NULL,
 *     @p len 0
 *
 * Return value controls re-arming:
 *   - true (and @p status == AXL_OK): re-arm Receive for the next
 *     datagram. Sock must remain valid until the callback returns.
 *   - false: stop receiving. The library cancels the underlying
 *     UEFI Receive op and drops loop sources. The socket is NOT
 *     closed — caller may start a fresh recv or call axl_udp_close.
 *   - AXL_ERR / AXL_CANCELLED status: never re-arm regardless of
 *     return value (the UEFI op is already torn down on those paths).
 */
typedef bool (*AxlUdpCallback)(
    AxlUdp         *sock,       ///< socket
    AxlStatus             status,     ///< AXL_OK / AXL_ERR / AXL_CANCELLED
    const void           *data,       ///< received payload (NULL on err/cancel)
    size_t                len,        ///< payload length (0 on err/cancel)
    const AxlIPv4Address *from,       ///< sender address (NULL on err/cancel)
    uint16_t              from_port,  ///< sender port (0 on err/cancel)
    void                 *user_data   ///< caller context
);

/**
 * @brief Async receive — fires @p cb for each incoming datagram via
 *     the event loop, until the callback returns false, the
 *     cancellable is signalled, or the socket is closed.
 *
 * Mirrors axl_tcp_recv_async: takes an optional
 * AxlCancellable, the callback receives per-event status, and
 * the callback's bool return controls re-arming.
 *
 * Replaces the pre-parity-sweep `axl_udp_recv_start` /
 * `axl_udp_recv_stop` pair: returning false from the callback now
 * stops in-place (no separate stop call needed).
 *
 * @return AXL_OK on success, AXL_ERR on error.
 */
int
axl_udp_recv_async(
    AxlUdp   *sock,    ///< socket
    AxlLoop        *loop,    ///< event loop
    AxlCancellable *cancel,  ///< optional cancel token (NULL = uncancellable)
    AxlUdpCallback  cb,      ///< receive callback
    void           *data     ///< user data for callback
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_UDP_H */
