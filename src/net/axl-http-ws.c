/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-ws.c
    WebSocket route registration, upgrade handshake, frame dispatch, broadcast.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// WebSocket route registration
// ---------------------------------------------------------------------------

int
axl_http_server_add_websocket(AxlHttpServer *s, const char *path,
                              AxlWsHandler handler, void *data)
{
    if (s == NULL || path == NULL || handler == NULL) {
        return -1;
    }

    if (s->ws_route_count >= 8) {
        axl_warning("too many WebSocket routes (max 8)");
        return -1;
    }

    size_t idx = s->ws_route_count;
    s->ws_routes[idx].path = axl_strdup(path);
    s->ws_routes[idx].handler = handler;
    s->ws_routes[idx].data = data;
    s->ws_route_count++;

    return 0;
}

// ---------------------------------------------------------------------------
// WebSocket broadcast
// ---------------------------------------------------------------------------

static bool
on_ws_send_done(AxlTcp *sock, int status, void *data)
{
    (void)sock;
    (void)status;
    axl_free(data);
    return false;  /* send is one-shot */
}

int
axl_http_server_ws_broadcast(AxlHttpServer *s, const char *path,
                             const void *bcast_data, size_t size)
{
    uint8_t *frame;
    size_t   frame_len;
    size_t   frame_buf_size;

    if (s == NULL || path == NULL || bcast_data == NULL || size == 0) {
        return -1;
    }

    if (s->conns == NULL) {
        return -1;
    }

    /* Build the frame once, send to all matching clients */
    frame_buf_size = size + 14;
    frame = axl_malloc(frame_buf_size);
    if (frame == NULL) {
        axl_error("ws broadcast frame alloc failed: %zu bytes", frame_buf_size);
        return -1;
    }

    frame_len = ws_build_frame(WS_OP_TEXT, bcast_data, size,
                               frame, frame_buf_size);
    if (frame_len == 0) {
        axl_free(frame);
        return -1;
    }

    for (size_t i = 0; i < s->max_conns; i++) {
        HttpConn *conn = &s->conns[i];
        if (!conn->active || !conn->is_websocket) {
            continue;
        }
        if (conn->ws_path == NULL || axl_strcmp(conn->ws_path, path) != 0) {
            continue;
        }

        if (conn->tls_ctx != NULL) {
            /* Encrypt synchronously, send ciphertext async */
            axl_tls_write_async(conn->tls_ctx, frame, frame_len,
                                s->loop, NULL, NULL);
        } else {
            /* Async send — each connection needs its own buffer copy */
            void *copy = axl_memdup(frame, frame_len);
            if (copy == NULL) {
                axl_warning("ws broadcast copy alloc failed for conn %zu", i);
                continue;
            }
            if (axl_tcp_send_async(conn->sock, copy, frame_len,
                                   s->loop, NULL,
                                   on_ws_send_done, copy) != 0) {
                axl_free(copy);
            }
        }
    }

    axl_free(frame);
    return 0;
}

// ---------------------------------------------------------------------------
// WebSocket upgrade
// ---------------------------------------------------------------------------

void
handle_websocket_upgrade(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    const char *key;
    const char *version;
    char *accept_key;
    char response[512];
    size_t resp_len;
    size_t i;

    /* Validate required headers */
    key = axl_hash_table_lookup(conn->headers, "sec-websocket-key");
    version = axl_hash_table_lookup(conn->headers, "sec-websocket-version");

    if (key == NULL || version == NULL) {
        send_error_response(conn, 400);
        reset_connection(conn);
        return;
    }

    if (axl_strcmp(version, "13") != 0) {
        send_error_response(conn, 400);
        reset_connection(conn);
        return;
    }

    /* Find matching WebSocket route */
    for (i = 0; i < s->ws_route_count; i++) {
        if (axl_strcmp(s->ws_routes[i].path, conn->path) == 0) {
            break;
        }
    }

    if (i >= s->ws_route_count) {
        send_error_response(conn, 404);
        reset_connection(conn);
        return;
    }

    /* Compute Sec-WebSocket-Accept */
    accept_key = ws_compute_accept_key(key);
    if (accept_key == NULL) {
        send_error_response(conn, 500);
        reset_connection(conn);
        return;
    }

    /* Send 101 Switching Protocols */
    resp_len = axl_snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_key);
    axl_free(accept_key);

    if (conn->tls_ctx != NULL) {
        axl_tls_write(conn->tls_ctx, response, resp_len);
    } else {
        axl_tcp_send(conn->sock, response, resp_len, 0);
    }

    /* Transition to WebSocket mode */
    conn->is_websocket = true;
    conn->ws_handler = s->ws_routes[i].handler;
    conn->ws_data = s->ws_routes[i].data;
    conn->ws_path = axl_strdup(s->ws_routes[i].path);
    conn->keep_alive = false;

    /* Notify handler of connection */
    if (conn->ws_handler != NULL) {
        conn->ws_handler(AXL_WS_CONNECT, NULL, 0, conn->ws_data);
    }

    /* Start receiving WebSocket frames */
    start_conn_recv(s, conn);
}

// ---------------------------------------------------------------------------
// WebSocket frame dispatch
// ---------------------------------------------------------------------------

void
process_websocket_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    const uint8_t *data,
    size_t         data_len
    )
{
    uint8_t *buf;
    size_t   buf_len;

    /* Combine partial data from previous receive */
    if (conn->ws_partial_buf != NULL && conn->ws_partial_len > 0) {
        buf_len = conn->ws_partial_len + data_len;
        buf = axl_malloc(buf_len);
        if (buf == NULL) {
            reset_connection(conn);
            return;
        }
        axl_memcpy(buf, conn->ws_partial_buf, conn->ws_partial_len);
        axl_memcpy(buf + conn->ws_partial_len, data, data_len);
        axl_free(conn->ws_partial_buf);
        conn->ws_partial_buf = NULL;
        conn->ws_partial_len = 0;
    } else {
        buf = (uint8_t *)data;
        buf_len = data_len;
    }

    /* Process all complete frames in the buffer */
    size_t offset = 0;
    while (offset < buf_len) {
        WsFrameHeader hdr;
        size_t hdr_len = ws_parse_header(buf + offset, buf_len - offset, &hdr);

        if (hdr_len == 0) {
            /* Incomplete frame — save for next receive */
            size_t remaining = buf_len - offset;
            conn->ws_partial_buf = axl_malloc(remaining);
            if (conn->ws_partial_buf != NULL) {
                axl_memcpy(conn->ws_partial_buf, buf + offset, remaining);
                conn->ws_partial_len = remaining;
            }
            break;
        }

        uint8_t *payload = buf + offset + hdr.header_len;

        /* Unmask client data */
        if (hdr.masked) {
            ws_unmask(payload, hdr.payload_len, hdr.mask);
        }

        /* Handle control frames */
        if (hdr.opcode == WS_OP_PING) {
            /* Respond with pong */
            uint8_t pong_buf[256];
            size_t pong_len = ws_build_frame(WS_OP_PONG, payload,
                hdr.payload_len, pong_buf, sizeof(pong_buf));
            if (pong_len > 0) {
                if (conn->tls_ctx != NULL) {
                    axl_tls_write(conn->tls_ctx, pong_buf, pong_len);
                } else {
                    axl_tcp_send(conn->sock, pong_buf, pong_len, 0);
                }
            }
        } else if (hdr.opcode == WS_OP_CLOSE) {
            /* Echo close frame and disconnect */
            uint8_t close_buf[16];
            size_t close_len = ws_build_frame(WS_OP_CLOSE, NULL, 0,
                close_buf, sizeof(close_buf));
            if (close_len > 0) {
                if (conn->tls_ctx != NULL) {
                    axl_tls_write(conn->tls_ctx, close_buf, close_len);
                } else {
                    axl_tcp_send(conn->sock, close_buf, close_len, 0);
                }
            }
            if (buf != data) {
                axl_free(buf);
            }
            reset_connection(conn);
            return;
        } else if (hdr.opcode == WS_OP_TEXT || hdr.opcode == WS_OP_BINARY) {
            /* Dispatch to handler */
            if (conn->ws_handler != NULL) {
                size_t event = (hdr.opcode == WS_OP_TEXT)
                    ? AXL_WS_TEXT : AXL_WS_BINARY;
                conn->ws_handler(event, payload, hdr.payload_len,
                                 conn->ws_data);
            }
        }

        offset += hdr.header_len + hdr.payload_len;
    }

    if (buf != data) {
        axl_free(buf);
    }

    /* Continue receiving */
    start_conn_recv(s, conn);
}
