/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-socket-client.h
 *
 * High-level socket client. Performs DNS resolution and TCP connection
 * in a single call. Reusable — one client can connect to multiple
 * hosts.
 *
 * @code
 * AxlSocketClient *c = axl_socket_client_new();
 * AxlSocket *sock;
 * if (axl_socket_client_connect_to_host(c, "192.168.1.1", 80, &sock) == 0) {
 *     axl_socket_send(sock, "GET / HTTP/1.0\r\n\r\n", 18, 0);
 *     axl_socket_free(sock);
 * }
 * axl_socket_client_free(c);
 * @endcode
 */

#ifndef AXL_SOCKET_CLIENT_H
#define AXL_SOCKET_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlSocket AxlSocket;
typedef struct AxlSocketAddress AxlSocketAddress;
typedef struct AxlSocketClient AxlSocketClient;

// ---------------------------------------------------------------------------
// AxlSocketClient
// ---------------------------------------------------------------------------

/**
 * @brief Create a new socket client.
 *
 * @return new client, or NULL on allocation failure.
 */
AxlSocketClient *
axl_socket_client_new(void);

/**
 * @brief Free a socket client. NULL-safe.
 */
void
axl_socket_client_free(
    AxlSocketClient *client  ///< client to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSocketClient, axl_socket_client_free)
#endif

/**
 * @brief Set the connect timeout.
 *
 * Reserved for future use. Currently the timeout is determined by
 * the underlying TCP stack (typically 10 seconds).
 */
void
axl_socket_client_set_timeout(
    AxlSocketClient *client,     ///< client
    size_t           timeout_ms  ///< timeout in ms (reserved, not yet applied)
);

/**
 * @brief Connect to a resolved address. Blocking.
 *
 * Creates a new AxlSocket, connects to @p addr, and returns it.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_socket_client_connect(
    AxlSocketClient  *client,    ///< client
    AxlSocketAddress *addr,      ///< resolved address (borrowed)
    AxlSocket       **out_sock   ///< [out] receives connected socket
);

/**
 * @brief Resolve a hostname and connect. Blocking.
 *
 * Performs DNS resolution (or parses a dotted-decimal IP), then
 * creates and connects a stream socket. The resolved address is
 * available via axl_socket_get_remote_address() on the returned
 * socket.
 *
 * @return AXL_OK on success, AXL_ERR on DNS or connect failure.
 */
int
axl_socket_client_connect_to_host(
    AxlSocketClient  *client,    ///< client
    const char       *host,      ///< hostname or dotted-decimal IP
    uint16_t          port,      ///< port number
    AxlSocket       **out_sock   ///< [out] receives connected socket
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SOCKET_CLIENT_H */
