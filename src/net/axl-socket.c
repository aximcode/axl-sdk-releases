/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-socket.c
    Unified socket abstraction — delegates to AxlTcp / AxlUdpSocket.
**/

#include <axl/axl-socket.h>
#include <axl/axl-inet-address.h>
#include <axl/axl-net.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("socket");

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

/**
 * Adapter for bridging AxlTcpCallback -> AxlSocketCallback.
 * Heap-allocated per async op, freed in the callback (except accept
 * which re-arms — that ctx is owned by AxlSocket and freed on close).
 */
typedef struct {
    AxlSocket         *sock;
    AxlSocketCallback  cb;
    void              *data;
} SocketAsyncCtx;

struct AxlSocket {
    AxlSocketType type;
    union {
        AxlTcp       *tcp;
        AxlUdpSocket *udp;
    };
    SocketAsyncCtx *accept_ctx;  /* owned, freed on socket_free */

    /* UDP async receive state (for bridging AxlUdpRecvCallback) */
    void              *udp_recv_buf;
    size_t             udp_recv_buf_size;
    size_t             udp_recv_len;       /* bytes received */
    AxlSocketCallback  udp_recv_cb;
    void              *udp_recv_data;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

AxlSocket *
axl_socket_new(AxlSocketType type)
{
    AxlSocket *sock;

    if (type != AXL_SOCKET_STREAM && type != AXL_SOCKET_DATAGRAM) {
        return NULL;
    }

    sock = axl_calloc(1, sizeof(*sock));
    if (sock == NULL) {
        return NULL;
    }

    sock->type = type;

    if (type == AXL_SOCKET_DATAGRAM) {
        if (axl_udp_open(&sock->udp, 0) != 0) {
            axl_warning("socket: failed to open UDP socket");
            axl_free(sock);
            return NULL;
        }
    }

    return sock;
}

AxlSocket *
axl_socket_new_from_tcp(AxlTcp *tcp)
{
    AxlSocket *sock;

    if (tcp == NULL) {
        return NULL;
    }

    sock = axl_calloc(1, sizeof(*sock));
    if (sock == NULL) {
        axl_tcp_close(tcp);
        return NULL;
    }

    sock->type = AXL_SOCKET_STREAM;
    sock->tcp = tcp;

    return sock;
}

void
axl_socket_free(AxlSocket *sock)
{
    if (sock == NULL) {
        return;
    }

    if (sock->type == AXL_SOCKET_STREAM) {
        axl_tcp_close(sock->tcp);
    } else {
        axl_udp_close(sock->udp);
    }

    axl_free(sock->accept_ctx);
    axl_free(sock);
}

// ---------------------------------------------------------------------------
// Blocking operations
// ---------------------------------------------------------------------------

int
axl_socket_bind(AxlSocket *sock, uint16_t port)
{
    if (sock == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_DATAGRAM) {
        axl_warning("socket: bind on non-datagram socket");
        return -1;
    }

    /* Close existing UDP socket and reopen on the requested port */
    axl_udp_close(sock->udp);
    sock->udp = NULL;

    if (axl_udp_open(&sock->udp, port) != 0) {
        axl_warning("socket: bind to port %u failed", (unsigned)port);
        return -1;
    }

    return 0;
}

int
axl_socket_connect(AxlSocket *sock, AxlSocketAddress *addr)
{
    AxlInetAddress *inet;
    const char *host;
    uint16_t port;

    if (sock == NULL || addr == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM) {
        axl_warning("socket: connect on non-stream socket");
        return -1;
    }
    if (sock->tcp != NULL) {
        axl_warning("socket: already connected");
        return -1;
    }

    inet = axl_socket_address_get_address(addr);
    host = axl_inet_address_to_string(inet);
    port = axl_socket_address_get_port(addr);

    if (host == NULL) {
        return -1;
    }

    return axl_tcp_connect(host, port, &sock->tcp);
}

int
axl_socket_listen(AxlSocket *sock, uint16_t port)
{
    if (sock == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM) {
        axl_warning("socket: listen on non-stream socket");
        return -1;
    }
    if (sock->tcp != NULL) {
        axl_warning("socket: already connected/listening");
        return -1;
    }

    return axl_tcp_listen(port, &sock->tcp);
}

int
axl_socket_accept(AxlSocket *sock, AxlSocket **out_client, size_t timeout_ms)
{
    AxlTcp *client_tcp;
    int rc;

    if (sock == NULL || out_client == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        return -1;
    }

    rc = axl_tcp_accept(sock->tcp, &client_tcp, timeout_ms);
    if (rc != 0) {
        return -1;
    }

    *out_client = axl_socket_new_from_tcp(client_tcp);
    if (*out_client == NULL) {
        return -1;
    }

    return 0;
}

int
axl_socket_send(AxlSocket *sock, const void *data, size_t size,
                size_t timeout_ms)
{
    if (sock == NULL || data == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        axl_warning("socket: send on non-stream or unconnected socket");
        return -1;
    }

    return axl_tcp_send(sock->tcp, data, size, timeout_ms);
}

int
axl_socket_send_to(AxlSocket *sock, const void *data, size_t size,
                   AxlSocketAddress *dest)
{
    AxlIPv4Address ipv4;
    uint16_t port;

    if (sock == NULL || data == NULL || dest == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_DATAGRAM || sock->udp == NULL) {
        axl_warning("socket: send_to on non-datagram socket");
        return -1;
    }

    axl_socket_address_to_ipv4(dest, &ipv4, &port);

    return axl_udp_send(sock->udp, &ipv4, port, data, size);
}

int
axl_socket_receive(AxlSocket *sock, void *buf, size_t *size,
                   size_t timeout_ms)
{
    if (sock == NULL || buf == NULL || size == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        axl_warning("socket: receive on non-stream or unconnected socket");
        return -1;
    }

    return axl_tcp_recv(sock->tcp, buf, size, timeout_ms);
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

AxlSocketAddress *
axl_socket_get_local_address(AxlSocket *sock)
{
    char addr_buf[16];
    uint16_t port;
    AxlInetAddress *inet;

    if (sock == NULL) {
        return NULL;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        return NULL;
    }

    if (axl_tcp_get_local_addr(sock->tcp, addr_buf, sizeof(addr_buf),
                               &port) != 0) {
        return NULL;
    }

    inet = axl_inet_address_new_from_string(addr_buf);
    if (inet == NULL) {
        return NULL;
    }

    return axl_socket_address_new(inet, port);
}

AxlSocketAddress *
axl_socket_get_remote_address(AxlSocket *sock)
{
    char addr_buf[16];
    uint16_t port;
    AxlInetAddress *inet;

    if (sock == NULL) {
        return NULL;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        return NULL;
    }

    if (axl_tcp_get_remote_addr(sock->tcp, addr_buf, sizeof(addr_buf),
                                &port) != 0) {
        return NULL;
    }

    inet = axl_inet_address_new_from_string(addr_buf);
    if (inet == NULL) {
        return NULL;
    }

    return axl_socket_address_new(inet, port);
}

AxlSocketType
axl_socket_get_type(AxlSocket *sock)
{
    if (sock == NULL) {
        return AXL_SOCKET_STREAM;
    }

    return sock->type;
}

// ---------------------------------------------------------------------------
// Async operations
// ---------------------------------------------------------------------------

static bool
tcp_connect_bridge(AxlTcp *tcp, int status, void *ctx_data)
{
    SocketAsyncCtx *ctx = ctx_data;

    if (status == 0 && tcp != NULL) {
        ctx->sock->tcp = tcp;
    }

    if (ctx->cb != NULL) {
        (void)ctx->cb(ctx->sock, status, ctx->data);  /* connect is one-shot */
    }

    axl_free(ctx);
    return false;  /* connect never re-arms */
}

int
axl_socket_connect_async(AxlSocket *sock, AxlSocketAddress *addr,
                         AxlLoop *loop, AxlSocketCallback cb, void *data)
{
    SocketAsyncCtx *ctx;
    AxlInetAddress *inet;
    const char *host;
    uint16_t port;

    if (sock == NULL || addr == NULL || loop == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM) {
        return -1;
    }

    ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }

    ctx->sock = sock;
    ctx->cb = cb;
    ctx->data = data;

    inet = axl_socket_address_get_address(addr);
    host = axl_inet_address_to_string(inet);
    port = axl_socket_address_get_port(addr);

    if (host == NULL) {
        axl_free(ctx);
        return -1;
    }

    int rc = axl_tcp_connect_async(host, port, loop, NULL,
                                   tcp_connect_bridge, ctx);
    if (rc != 0) {
        axl_free(ctx);
    }
    return rc;
}

static bool
tcp_accept_bridge(AxlTcp *tcp, int status, void *ctx_data)
{
    SocketAsyncCtx *ctx = ctx_data;
    AxlSocket *client = NULL;
    bool keep = true;  /* default: keep listening if no user cb */

    if (status == 0 && tcp != NULL) {
        client = axl_socket_new_from_tcp(tcp);
        if (client == NULL) {
            status = -1;
        }
    }

    if (ctx->cb != NULL) {
        keep = ctx->cb(client, status, ctx->data);
    }

    /* Don't free ctx — it's owned by sock->accept_ctx and freed on
       socket close, regardless of whether we keep listening. */
    return keep;
}

int
axl_socket_accept_async(AxlSocket *sock, AxlLoop *loop,
                        AxlSocketCallback cb, void *data)
{
    SocketAsyncCtx *ctx;
    int rc;

    if (sock == NULL || loop == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        return -1;
    }

    /* Free any previous accept ctx (re-arm case) */
    axl_free(sock->accept_ctx);

    ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        sock->accept_ctx = NULL;
        return -1;
    }

    ctx->sock = sock;
    ctx->cb = cb;
    ctx->data = data;

    /* Store on socket so it's freed when the socket is closed */
    sock->accept_ctx = ctx;

    rc = axl_tcp_accept_async(sock->tcp, loop, NULL, tcp_accept_bridge, ctx);
    if (rc != 0) {
        axl_free(ctx);
        sock->accept_ctx = NULL;
    }
    return rc;
}

static bool
tcp_send_bridge(AxlTcp *tcp, int status, void *ctx_data)
{
    SocketAsyncCtx *ctx = ctx_data;
    (void)tcp;

    if (ctx->cb != NULL) {
        (void)ctx->cb(ctx->sock, status, ctx->data);  /* send is one-shot */
    }

    axl_free(ctx);
    return false;  /* send never re-arms */
}

int
axl_socket_send_async(AxlSocket *sock, const void *buf, size_t size,
                      AxlLoop *loop, AxlSocketCallback cb, void *data)
{
    SocketAsyncCtx *ctx;

    if (sock == NULL || buf == NULL || loop == NULL) {
        return -1;
    }
    if (sock->type != AXL_SOCKET_STREAM || sock->tcp == NULL) {
        return -1;
    }

    ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }

    ctx->sock = sock;
    ctx->cb = cb;
    ctx->data = data;

    int rc = axl_tcp_send_async(sock->tcp, buf, size, loop, NULL,
                                tcp_send_bridge, ctx);
    if (rc != 0) {
        axl_free(ctx);
    }
    return rc;
}

static bool
tcp_recv_bridge(AxlTcp *tcp, int status, void *ctx_data)
{
    SocketAsyncCtx *ctx = ctx_data;
    bool keep = false;
    (void)tcp;

    if (ctx->cb != NULL) {
        keep = ctx->cb(ctx->sock, status, ctx->data);
    }

    /* One ctx per recv op. If cb returns true, TCP re-arms into the
       same buffer — ctx must stay alive for the next fire. If false,
       TCP tears down — free ctx now. */
    if (!keep) {
        axl_free(ctx);
    }
    return keep;
}

static void
udp_recv_bridge(AxlUdpSocket *udp, const void *payload, size_t len,
                const AxlIPv4Address *from, uint16_t from_port, void *udata)
{
    AxlSocket *sock = (AxlSocket *)udata;
    bool keep = false;

    (void)udp;
    (void)from;
    (void)from_port;

    /* Copy received data into the user's buffer */
    size_t copy_len = len;
    if (copy_len > sock->udp_recv_buf_size) {
        copy_len = sock->udp_recv_buf_size;
    }
    if (copy_len > 0 && sock->udp_recv_buf != NULL) {
        axl_memcpy(sock->udp_recv_buf, payload, copy_len);
    }
    sock->udp_recv_len = copy_len;

    /* Fire user callback — return value controls re-arm like TCP. */
    if (sock->udp_recv_cb != NULL) {
        keep = sock->udp_recv_cb(sock, 0, sock->udp_recv_data);
    }

    /* If not keeping, stop the underlying UDP recv. axl_udp_recv_start
       stays armed until _stop is called; not calling stop here means
       the next datagram will fire this bridge again. */
    if (!keep) {
        axl_udp_recv_stop(sock->udp);
    }
}

int
axl_socket_receive_async(AxlSocket *sock, void *buf, size_t size,
                         AxlLoop *loop, AxlSocketCallback cb, void *data)
{
    if (sock == NULL || buf == NULL || loop == NULL) {
        return -1;
    }

    if (sock->type == AXL_SOCKET_DATAGRAM) {
        if (sock->udp == NULL) {
            return -1;
        }

        /* Store receive state for the UDP bridge */
        sock->udp_recv_buf = buf;
        sock->udp_recv_buf_size = size;
        sock->udp_recv_len = 0;
        sock->udp_recv_cb = cb;
        sock->udp_recv_data = data;

        return axl_udp_recv_start(sock->udp, loop, udp_recv_bridge, sock);
    }

    /* Stream (TCP) path */
    if (sock->tcp == NULL) {
        return -1;
    }

    SocketAsyncCtx *ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }

    ctx->sock = sock;
    ctx->cb = cb;
    ctx->data = data;

    int rc = axl_tcp_recv_async(sock->tcp, buf, size, loop, NULL,
                                tcp_recv_bridge, ctx);
    if (rc != 0) {
        axl_free(ctx);
    }
    return rc;
}

size_t
axl_socket_receive_get_size(AxlSocket *sock)
{
    if (sock == NULL) {
        return 0;
    }

    if (sock->type == AXL_SOCKET_DATAGRAM) {
        return sock->udp_recv_len;
    }

    if (sock->tcp == NULL) {
        return 0;
    }

    return axl_tcp_recv_get_size(sock->tcp);
}
