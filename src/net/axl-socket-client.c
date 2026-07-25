/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-socket-client.c
    High-level socket client — DNS + connect in one call.
**/

#include <axl/axl-socket-client.h>
#include <axl/axl-socket.h>
#include <axl/axl-inet-address.h>
#include <axl/axl-net.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("socket");

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

struct AxlSocketClient {
    size_t timeout_ms;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlSocketClient *
axl_socket_client_new(void)
{
    return axl_calloc(1, sizeof(AxlSocketClient));
}

void
axl_socket_client_free(AxlSocketClient *client)
{
    axl_free(client);
}

void
axl_socket_client_set_timeout(AxlSocketClient *client, size_t timeout_ms)
{
    if (client != NULL) {
        client->timeout_ms = timeout_ms;
    }
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

int
axl_socket_client_connect(AxlSocketClient *client, AxlSocketAddress *addr,
                          AxlSocket **out_sock)
{
    AxlSocket *sock;
    int rc;

    (void)client;

    if (addr == NULL || out_sock == NULL) {
        return AXL_ERR;
    }

    sock = axl_socket_new(AXL_SOCKET_STREAM);
    if (sock == NULL) {
        return AXL_ERR;
    }

    rc = axl_socket_connect(sock, addr);
    if (rc != 0) {
        axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
        return AXL_ERR;
    }

    *out_sock = sock;
    return AXL_OK;
}

int
axl_socket_client_connect_to_host(AxlSocketClient *client, const char *host,
                                  uint16_t port, AxlSocket **out_sock)
{
    AxlIPv4Address ipv4;
    AxlInetAddress *inet;
    AxlSocketAddress *sa;

    (void)client;

    if (host == NULL || out_sock == NULL) {
        return AXL_ERR;
    }

    /* Resolve hostname (or parse dotted-decimal) */
    if (axl_net_resolve(host, &ipv4) != AXL_OK) {
        axl_warning("socket_client: DNS resolve failed for '%s'", host);
        return AXL_ERR;
    }

    inet = axl_inet_address_new_from_bytes(ipv4.addr);
    if (inet == NULL) {
        return AXL_ERR;
    }

    sa = axl_socket_address_new(inet, port);
    if (sa == NULL) {
        return AXL_ERR;
    }

    int rc = axl_socket_client_connect(client, sa, out_sock);
    axl_socket_address_free(sa);

    return rc;
}
