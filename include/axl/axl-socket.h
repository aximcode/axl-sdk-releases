/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-socket.h:
 *
 * Unified socket abstraction. AxlSocket wraps AxlTcp (stream) and
 * AxlUdpSocket (datagram) behind a single API. Blocking and async
 * (event-loop integrated) operations are both supported.
 *
 * @code
 * AxlSocket *sock = axl_socket_new(AXL_SOCKET_STREAM);
 * AxlSocketAddress *remote = axl_socket_address_new(
 *     axl_inet_address_new_from_string("192.168.1.1"), 8080);
 * axl_socket_connect(sock, remote);
 * axl_socket_send(sock, "hello", 5, 0);
 * axl_socket_address_free(remote);
 * axl_socket_free(sock);
 * @endcode
 */

#ifndef AXL_SOCKET_H
#define AXL_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;
typedef struct AxlTcp AxlTcp;
typedef struct AxlSocket AxlSocket;
typedef struct AxlSocketAddress AxlSocketAddress;

/**
 * @brief Socket type: stream (TCP) or datagram (UDP).
 */
typedef enum {
    AXL_SOCKET_STREAM   = 1,   /**< TCP stream socket */
    AXL_SOCKET_DATAGRAM = 2    /**< UDP datagram socket */
} AxlSocketType;

/**
 * @brief Callback for async socket operations.
 *
 * @p sock is the socket for the completed operation (new socket for
 * accept, or the original socket for send/recv/connect).
 * @p status is 0 on success, -1 on error.
 * On error, @p sock may be NULL (accept) or half-initialized (connect).
 * Always check @p status before using @p sock.
 *
 * Return value controls re-arming for ops that support it:
 *   - `axl_socket_receive_async`: true = re-arm with same buffer, false = stop
 *   - `axl_socket_accept_async`: true = keep listening (default), false = stop accepting
 *   - `axl_socket_connect_async`, `axl_socket_send_async`: ignored (one-shot
 *     ops). Convention: `return true;`.
 *
 * ## Safety: closing the socket from inside the callback
 *
 * For recv: returning `true` requires the socket to remain valid
 * (library re-issues recv). Returning `false` permits closing the
 * socket inside the callback (library does not access it after false).
 *
 * For accept: @p sock is the accepted client, not the listener.
 * Closing the client is always safe. To stop accepting, return false;
 * to fully release the listener, call axl_socket_free on it afterwards.
 */
typedef bool (*AxlSocketCallback)(
    AxlSocket *sock,    ///< socket
    int        status,  ///< 0 = success, -1 = error
    void      *data     ///< caller-provided context
);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Create a new socket.
 *
 * For AXL_SOCKET_STREAM, the socket is unconnected — call
 * axl_socket_connect() or axl_socket_listen() next.
 * For AXL_SOCKET_DATAGRAM, a UDP socket is opened immediately
 * on an ephemeral port.
 *
 * @return new socket, or NULL on failure.
 */
AxlSocket *
axl_socket_new(
    AxlSocketType type  ///< AXL_SOCKET_STREAM or AXL_SOCKET_DATAGRAM
);

/**
 * @brief Wrap an existing AxlTcp as a stream socket.
 *
 * Takes ownership of @p tcp — the caller must not close it.
 * Useful for wrapping accepted connections from the raw TCP API.
 *
 * @return new socket, or NULL on failure (tcp is closed on failure).
 */
AxlSocket *
axl_socket_new_from_tcp(
    AxlTcp *tcp  ///< connected TCP socket (ownership transferred)
);

/**
 * @brief Close and free a socket. NULL-safe.
 */
void
axl_socket_free(
    AxlSocket *sock  ///< socket to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSocket, axl_socket_free)
#endif

// ---------------------------------------------------------------------------
// Blocking operations
//
// Sync wrappers (axl_socket_connect, axl_socket_accept,
// axl_socket_send, axl_socket_receive) delegate to the underlying
// AxlTcp/AxlUdpSocket blocking APIs, which each create their OWN
// temporary AxlLoop per call and run it to completion. They do NOT
// participate in the caller's event loop. If you are already inside
// a loop callback, calling a sync wrapper nests a second loop and
// pauses the outer one — use the _async variants below instead.
// ---------------------------------------------------------------------------

/**
 * @brief Bind a datagram socket to a specific local port.
 *
 * Closes the current ephemeral UDP socket and reopens on @p port.
 * Pass 0 to rebind to a new ephemeral port.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_socket_bind(
    AxlSocket *sock,  ///< datagram socket
    uint16_t   port   ///< local port (0 = ephemeral)
);

/**
 * @brief Connect a stream socket to a remote address. Blocking.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_socket_connect(
    AxlSocket        *sock,  ///< stream socket (unconnected)
    AxlSocketAddress *addr   ///< remote address (borrowed, not consumed)
);

/**
 * @brief Start listening on a stream socket.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_socket_listen(
    AxlSocket *sock,  ///< stream socket
    uint16_t   port   ///< port to listen on
);

/**
 * @brief Accept a connection on a listening stream socket. Blocking.
 *
 * Unlike axl_socket_send/_receive where timeout_ms=0 defaults to
 * 10s, a sync accept's semantic role is "wait for an incoming
 * client," so timeout_ms=0 means wait forever (no timeout source).
 * Pass a positive value to bound the wait; Ctrl-C ends the wait
 * either way via the loop's break observation.
 *
 * @return 0 on success, -1 on failure, timeout, or cancel.
 */
int
axl_socket_accept(
    AxlSocket  *sock,        ///< listening stream socket
    AxlSocket **out_client,  ///< [out] receives accepted client socket
    size_t      timeout_ms   ///< timeout in ms (0 = wait forever)
);

/**
 * @brief Send data on a connected stream socket. Blocking.
 *
 * @return 0 on success, -1 on failure or timeout.
 */
int
axl_socket_send(
    AxlSocket  *sock,       ///< connected stream socket
    const void *data,       ///< buffer to send
    size_t      size,       ///< number of bytes
    size_t      timeout_ms  ///< timeout in ms (0 = default 10s)
);

/**
 * @brief Send a datagram to a specific address. Datagram sockets only.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_socket_send_to(
    AxlSocket        *sock,  ///< datagram socket
    const void       *data,  ///< buffer to send
    size_t            size,  ///< number of bytes
    AxlSocketAddress *dest   ///< destination address (borrowed)
);

/**
 * @brief Receive data from a connected stream socket. Blocking.
 *
 * @p size is in/out: on entry the buffer capacity, on return the
 * number of bytes received.
 *
 * @return 0 on success, -1 on failure or timeout.
 */
int
axl_socket_receive(
    AxlSocket *sock,       ///< connected stream socket
    void      *buf,        ///< receive buffer
    size_t    *size,       ///< [in/out] buffer capacity / bytes received
    size_t     timeout_ms  ///< timeout in ms (0 = default 10s)
);

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

/**
 * @brief Get the local address of a connected or listening socket.
 *
 * @return new AxlSocketAddress (caller frees), or NULL.
 */
AxlSocketAddress *
axl_socket_get_local_address(
    AxlSocket *sock  ///< socket
);

/**
 * @brief Get the remote address of a connected stream socket.
 *
 * @return new AxlSocketAddress (caller frees), or NULL.
 */
AxlSocketAddress *
axl_socket_get_remote_address(
    AxlSocket *sock  ///< connected stream socket
);

/**
 * @brief Get the socket type.
 *
 * @return AXL_SOCKET_STREAM or AXL_SOCKET_DATAGRAM.
 */
AxlSocketType
axl_socket_get_type(
    AxlSocket *sock  ///< socket
);

// ---------------------------------------------------------------------------
// Async operations (event-loop integrated)
// ---------------------------------------------------------------------------

/**
 * @brief Async connect — returns immediately, callback fires from loop.
 *
 * @return 0 if initiated, -1 on immediate failure.
 */
int
axl_socket_connect_async(
    AxlSocket          *sock,  ///< stream socket (unconnected)
    AxlSocketAddress   *addr,  ///< remote address (borrowed)
    AxlLoop            *loop,  ///< event loop
    AxlSocketCallback   cb,    ///< callback on completion (return value ignored)
    void               *data   ///< opaque context
);

/**
 * @brief Async accept — callback fires for each accepted client.
 *
 * Re-arming is controlled by the callback's return value: `true`
 * keeps the listener armed, `false` stops accepting.
 *
 * @return 0 if initiated, -1 on immediate failure.
 */
int
axl_socket_accept_async(
    AxlSocket         *sock,  ///< listening stream socket
    AxlLoop           *loop,  ///< event loop
    AxlSocketCallback  cb,    ///< callback per accepted client (return bool controls re-arm)
    void              *data   ///< opaque context
);

/**
 * @brief Async send — callback fires when send completes.
 *
 * The buffer must stay valid until the callback fires.
 *
 * @return 0 if initiated, -1 on immediate failure.
 */
int
axl_socket_send_async(
    AxlSocket         *sock,  ///< connected stream socket
    const void        *buf,   ///< send buffer (must remain valid)
    size_t             size,  ///< bytes to send
    AxlLoop           *loop,  ///< event loop
    AxlSocketCallback  cb,    ///< callback on completion (return value ignored)
    void              *data   ///< opaque context
);

/**
 * @brief Async receive — callback fires when data arrives.
 *
 * Works for both stream and datagram sockets. For stream sockets,
 * data is received directly into @p buf. For datagram sockets, the
 * next datagram is copied into @p buf (truncated if larger than
 * @p size).
 *
 * The buffer must stay valid across re-arms. Re-arming is controlled
 * by the callback's return value: `true` re-arms with the same
 * buffer, `false` stops receiving.
 *
 * @return 0 if initiated, -1 on immediate failure.
 */
int
axl_socket_receive_async(
    AxlSocket         *sock,  ///< stream or datagram socket
    void              *buf,   ///< receive buffer (must remain valid across re-arms)
    size_t             size,  ///< buffer size
    AxlLoop           *loop,  ///< event loop
    AxlSocketCallback  cb,    ///< callback when data received (return bool controls re-arm)
    void              *data   ///< opaque context
);

/**
 * @brief Get bytes received by the last async receive.
 *
 * Call from inside the receive callback to get the actual byte count.
 * Works for both stream and datagram sockets.
 *
 * @return bytes received, or 0 if no receive has completed.
 */
size_t
axl_socket_receive_get_size(
    AxlSocket *sock  ///< socket from receive callback
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SOCKET_H */
