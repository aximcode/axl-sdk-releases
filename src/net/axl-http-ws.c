/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-ws.c
    WebSocket route registration, upgrade handshake, frame dispatch, broadcast.
**/

#include "axl-http-server-internal.h"
#include <axl/axl-net.h>   /* axl_ipv4_parse for axl_ws_conn_peer */

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// WebSocket route registration
// ---------------------------------------------------------------------------

int
axl_http_server_add_websocket(AxlHttpServer *s, const char *path,
                              AxlWsHandler handler, void *data)
{
    if (s == NULL || path == NULL || handler == NULL) {
        return AXL_ERR;
    }

    if (s->ws_route_count >= 8) {
        axl_warning("too many WebSocket routes (max 8)");
        return AXL_ERR;
    }

    size_t idx = s->ws_route_count;
    s->ws_routes[idx].path = axl_strdup(path);
    s->ws_routes[idx].handler = handler;
    s->ws_routes[idx].conn_handler = NULL;
    s->ws_routes[idx].data = data;
    s->ws_routes[idx].auth_flags = AXL_ROUTE_NO_AUTH;
    s->ws_route_count++;

    return AXL_OK;
}

int
axl_http_server_add_websocket_ex(AxlHttpServer *s, const char *path,
                                 AxlWsConnHandler handler, void *data,
                                 uint32_t auth_flags)
{
    if (s == NULL || path == NULL || handler == NULL) {
        return AXL_ERR;
    }
    if (s->ws_route_count >= 8) {
        axl_warning("too many WebSocket routes (max 8)");
        return AXL_ERR;
    }

    size_t idx = s->ws_route_count;
    s->ws_routes[idx].path = axl_strdup(path);
    s->ws_routes[idx].handler = NULL;
    s->ws_routes[idx].conn_handler = handler;
    s->ws_routes[idx].data = data;
    s->ws_routes[idx].auth_flags = auth_flags;
    s->ws_route_count++;

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// WebSocket broadcast
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-connection outbound frame queue — the ws-broadcast-over-TLS desync fix.
//
// axl_tcp_send_async is strictly one-send-in-flight, and axl_tls_write_async
// advances the TLS write sequence number at ENCRYPT time. So two outbound
// frames racing a single async completion used to encrypt-then-drop the second
// — emitting a record the seqno had already consumed but which never reached
// the wire — desyncing the TLS stream and wedging the server loop. (The
// console-mirror echoes one keystroke as >=3 back-to-back broadcasts, hitting
// this on the first key.)
//
// Every outbound WS frame — broadcast, axl_ws_send, pong — now goes through
// this per-connection FIFO. Frames are enqueued PRE-encryption; only the head
// is sent at a time (encrypted at flush, for TLS), and the next is pumped from
// the send-completion callback, so frames SERIALIZE instead of racing. Lossy
// back-pressure (drop-oldest on overflow) drops a RAW frame before
// mbedtls_ssl_write ever runs, so an overflow can never desync the stream.
// ---------------------------------------------------------------------------

#define WS_OUT_MAX_FRAMES  1024u
#define WS_OUT_MAX_BYTES   (512u * 1024u)

static void ws_outq_pump(HttpConn *conn);

/* Remove + free the head node (the frame whose send just completed). */
static void
ws_outq_pop_free_head(HttpConn *conn)
{
    WsOutNode *n = conn->ws_out_head;
    if (n == NULL) {
        return;
    }
    conn->ws_out_head = n->next;
    if (conn->ws_out_head == NULL) {
        conn->ws_out_tail = NULL;
    }
    conn->ws_out_count--;
    conn->ws_out_bytes -= n->len;
    axl_free(n->data);
    axl_free(n);
}

/* Drop the oldest DROPPABLE frame to stay within budget. The in-flight head
   (its buffer is referenced by a pending send) is never dropped — drop the
   oldest queued-but-not-yet-sent frame instead. */
static void
ws_outq_drop_one(HttpConn *conn)
{
    WsOutNode *prev   = NULL;
    WsOutNode *victim = conn->ws_out_head;
    if (conn->ws_out_inflight) {
        prev   = conn->ws_out_head;          /* keep the in-flight head */
        victim = (prev != NULL) ? prev->next : NULL;
    }
    if (victim == NULL) {
        return;                               /* nothing droppable */
    }
    if (prev == NULL) {
        conn->ws_out_head = victim->next;
    } else {
        prev->next = victim->next;
    }
    if (conn->ws_out_tail == victim) {
        conn->ws_out_tail = prev;             /* prev is the head, or NULL */
    }
    conn->ws_out_count--;
    conn->ws_out_bytes -= victim->len;
    axl_free(victim->data);
    axl_free(victim);
}

/* Send completion: advance the queue. On error, reap the connection
   (reset_connection frees the rest of the queue). */
static bool
ws_out_on_sent(AxlTcp *sock, AxlStatus status, void *data)
{
    (void)sock;
    HttpConn *conn = (HttpConn *)data;
    if (conn == NULL || !conn->active) {
        return false;
    }
    conn->ws_out_inflight = false;
    if (status != AXL_OK) {
        /* Broken transport — reap the connection. reset_connection clears the
           whole queue (including the head we were sending), so do NOT pop. */
        reset_connection(conn);
        return false;
    }
    ws_outq_pop_free_head(conn);
    ws_outq_pump(conn);
    return false;   /* one-shot per frame */
}

/* If idle and a frame is queued, send the head (encrypted, for TLS). */
static void
ws_outq_pump(HttpConn *conn)
{
    if (conn->ws_out_inflight || !conn->active || conn->ws_out_head == NULL) {
        return;
    }
    WsOutNode *head = conn->ws_out_head;
    conn->ws_out_inflight = true;

    int rc;
    if (conn->tls_ctx != NULL) {
        /* tls_write_async encrypts head->data synchronously into its own
           buffer; the head node stays queued until ws_out_on_sent. */
        rc = axl_tls_write_async(conn->tls_ctx, head->data, head->len,
                                 conn->server->loop, ws_out_on_sent, conn);
    } else {
        /* The async send references head->data until completion — the head
           stays queued (and alive) until ws_out_on_sent pops it. */
        rc = axl_tcp_send_async(conn->sock, head->data, head->len,
                                conn->server->loop, NULL, ws_out_on_sent, conn);
    }
    if (rc == AXL_OK) {
        return;
    }
    conn->ws_out_inflight = false;
    if (rc == AXL_BUSY) {
        /* A non-queue send is somehow in flight on this sock. The SSL context
           is untouched (the axl_tls_write_async floor), so the head is intact
           and stays queued for a later pump. The queue owns every send after
           the upgrade, so this should not occur — degrade safely rather than
           desync or drop. */
        return;
    }
    /* Real submission failure (encrypt error / Transmit refused). Tear down. */
    reset_connection(conn);
}

int
ws_outq_enqueue(HttpConn *conn, const void *frame, size_t len)
{
    if (conn == NULL || frame == NULL || len == 0) {
        return AXL_ERR;
    }

    /* Reject a single frame larger than the whole outbound budget instead of
       admitting it. The old escape hatch ("always allow a single frame larger
       than the byte budget") handed a multi-MB frame to the one-Transmit-in-
       flight transport as one giant send; a client that could not drain it
       (slow read / mid-send close) then wedged the entire single-threaded
       server. A frame this large is a caller bug — chunk the payload. */
    if (len > WS_OUT_MAX_BYTES) {
        axl_warning("ws: frame %zu B exceeds outbound budget %u B; dropping "
                    "(chunk larger payloads)", len, WS_OUT_MAX_BYTES);
        return AXL_ERR;
    }

    /* Lossy back-pressure: drop the oldest droppable frame(s) until the new one
       fits within budget. Keep at least the in-flight head. */
    while ((conn->ws_out_count >= WS_OUT_MAX_FRAMES
            || conn->ws_out_bytes + len > WS_OUT_MAX_BYTES)
           && conn->ws_out_count > (conn->ws_out_inflight ? 1u : 0u)) {
        size_t before = conn->ws_out_count;
        ws_outq_drop_one(conn);
        if (conn->ws_out_count == before) {
            break;   /* nothing more is droppable */
        }
    }

    WsOutNode *n = axl_new(WsOutNode);
    if (n == NULL) {
        return AXL_ERR;
    }
    n->data = axl_memdup(frame, len);
    if (n->data == NULL) {
        axl_free(n);
        return AXL_ERR;
    }
    n->len  = len;
    n->next = NULL;

    if (conn->ws_out_tail != NULL) {
        conn->ws_out_tail->next = n;
    } else {
        conn->ws_out_head = n;
    }
    conn->ws_out_tail = n;
    conn->ws_out_count++;
    conn->ws_out_bytes += len;

    ws_outq_pump(conn);
    return AXL_OK;
}

void
ws_outq_clear(HttpConn *conn)
{
    if (conn == NULL) {
        return;
    }
    /* Frees every queued frame including the in-flight head: its async send was
       just cancelled by axl_tcp_close (reset_connection runs close first), so
       head->data is no longer referenced. NOTE: a TLS in-flight frame's
       CIPHERTEXT lives in a TlsWriteAsyncCtx that axl_tcp_close cancels without
       firing tls_write_async_done — so that wctx + enc buffer leak. This is the
       same bounded "one frame per teardown-mid-send" leak axl_tls_free has long
       accepted (the airtight fix — axl_tcp_close flushing on_send(CANCELLED) —
       re-enters reset_connection via on_response_sent). Not a wedge or UAF. */
    WsOutNode *n = conn->ws_out_head;
    while (n != NULL) {
        WsOutNode *next = n->next;
        axl_free(n->data);
        axl_free(n);
        n = next;
    }
    conn->ws_out_head     = NULL;
    conn->ws_out_tail     = NULL;
    conn->ws_out_count    = 0;
    conn->ws_out_bytes    = 0;
    conn->ws_out_inflight = false;
}

int
axl_http_server_ws_broadcast(AxlHttpServer *s, const char *path,
                             const void *bcast_data, size_t size)
{
    uint8_t *frame;
    size_t   frame_len;
    size_t   frame_buf_size;

    if (s == NULL || path == NULL || bcast_data == NULL || size == 0) {
        return AXL_ERR;
    }

    if (s->conns == NULL) {
        return AXL_ERR;
    }

    /* Build the frame once, send to all matching clients */
    frame_buf_size = size + 14;
    frame = axl_malloc(frame_buf_size);
    if (frame == NULL) {
        axl_error("ws broadcast frame alloc failed: %zu bytes", frame_buf_size);
        return AXL_ERR;
    }

    frame_len = ws_build_frame(WS_OP_TEXT, bcast_data, size,
                               frame, frame_buf_size);
    if (frame_len == 0) {
        axl_free(frame);
        return AXL_ERR;
    }

    for (size_t i = 0; i < s->max_conns; i++) {
        HttpConn *conn = &s->conns[i];
        if (!conn->active || !conn->is_websocket) {
            continue;
        }
        if (conn->ws_path == NULL || axl_strcmp(conn->ws_path, path) != 0) {
            continue;
        }

        /* Serialize through the connection's outbound queue (it copies the
           frame), so a burst of broadcasts can't race the one-send-in-flight
           transport and desync TLS. */
        if (ws_outq_enqueue(conn, frame, frame_len) != AXL_OK) {
            axl_warning("ws broadcast enqueue failed for conn %zu", i);
        }
    }

    axl_free(frame);
    return AXL_OK;
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
        /* reset_connection runs from on_response_sent now */
        return;
    }

    if (axl_strcmp(version, "13") != 0) {
        send_error_response(conn, 400);
        /* reset_connection runs from on_response_sent now */
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
        /* reset_connection runs from on_response_sent now */
        return;
    }

    /* Authenticate the upgrade for a per-connection (_ex) endpoint that
       requested it — exactly like an HTTP route, but capturing the resolved
       identity so axl_ws_conn_auth can surface it. Reject (no handshake)
       before computing the accept key. */
    AxlAuthInfo ws_auth;
    axl_memset(&ws_auth, 0, sizeof(ws_auth));
    bool authed = false;
    uint32_t auth_flags = s->ws_routes[i].auth_flags;
    if (auth_flags != AXL_ROUTE_NO_AUTH) {
        if (s->auth_cb == NULL) {
            send_error_response(conn, 401);
            return;
        }
        AxlHttpRequest req;
        axl_memset(&req, 0, sizeof(req));
        req.method  = conn->method;
        req.path    = conn->path;
        req.query   = conn->query;
        req.headers = conn->headers;
        axl_memcpy(req.client_addr, conn->client_addr, sizeof(req.client_addr));
        if (s->auth_cb(&req, &ws_auth, s->auth_data) != 0) {
            send_error_response(conn, 401);
            return;
        }
        if ((auth_flags & AXL_ROUTE_ADMIN) && ws_auth.role < AXL_ROUTE_ADMIN) {
            send_error_response(conn, 403);
            return;
        }
        authed = true;
    }

    /* Build the 101 Switching Protocols response and send it
       through the regular async path. send_response builds the
       status line + Connection header itself; we add the WS
       custom headers (Upgrade, Sec-WebSocket-Accept) via the
       AxlHttpResponse.headers hash table. send_response
       special-cases status 101 to emit "Connection: Upgrade"
       (RFC 6455 §4.2.2) instead of the keep-alive/close token, so
       we deliberately do NOT add a Connection header here (a
       second one would violate the handshake).

       Pre-stamp the WS state and fire CONNECT. on_response_sent
       sees keep_alive=true with is_websocket=true and runs the
       keep-alive branch — start_conn_recv re-arms the recv that
       on_recv_complete routes to process_websocket_data when
       is_websocket is set. */
    (void)response;
    (void)resp_len;

    /* Sec-WebSocket-Accept (computed once). */
    accept_key = ws_compute_accept_key(key);
    if (accept_key == NULL) {
        send_error_response(conn, 500);
        return;
    }

    AxlHttpResponse upgrade_resp = {0};
    upgrade_resp.status_code = 101;
    upgrade_resp.headers = axl_hash_table_new_str();
    if (upgrade_resp.headers == NULL) {
        axl_free(accept_key);
        send_error_response(conn, 500);
        return;
    }
    axl_hash_table_insert(upgrade_resp.headers,
                          axl_strdup("Upgrade"), axl_strdup("websocket"));
    axl_hash_table_insert(upgrade_resp.headers,
                          axl_strdup("Sec-WebSocket-Accept"), accept_key);

    conn->is_websocket    = true;
    conn->ws_handler      = s->ws_routes[i].handler;
    conn->ws_conn_handler = s->ws_routes[i].conn_handler;
    conn->ws_data         = s->ws_routes[i].data;
    conn->ws_user_data    = NULL;
    conn->ws_auth         = ws_auth;
    conn->ws_authed       = authed;
    conn->ws_path         = axl_strdup(s->ws_routes[i].path);
    conn->keep_alive      = true;

    /* Defer AXL_WS_CONNECT until the 101 has actually been sent (fired from
       on_response_sent's keep-alive branch). Firing it here — before
       send_response queues the 101 — would let a greeting axl_ws_send write a
       WS frame ahead of the status line and corrupt the handshake, and the
       handler's return couldn't gate the connection. */
    conn->ws_connect_pending = true;

    send_response(conn, &upgrade_resp);
    /* send_response memcpy'd headers via axl_hash_table_foreach;
       free our table here (caller owns it — dispatch_request would
       have freed via its tail, but we're not in dispatch_request). */
    axl_hash_table_free(upgrade_resp.headers);
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
            /* Respond with pong via the outbound queue — the connection stays
               open, and the queue serializes it behind any in-flight frame (a
               synchronous send would wedge a resident driver-tick loop, and a
               direct encrypt-then-send could desync TLS; see ws_outq_enqueue). */
            uint8_t pong_buf[256];
            size_t pong_len = ws_build_frame(WS_OP_PONG, payload,
                hdr.payload_len, pong_buf, sizeof(pong_buf));
            if (pong_len > 0) {
                ws_outq_enqueue(conn, pong_buf, pong_len);
            }
        } else if (hdr.opcode == WS_OP_CLOSE) {
            /* Clean close: tear down. Do NOT synchronously echo a close
               frame — a blocking send here spins a nested ephemeral
               axl_loop_run that cannot progress at the raised TPL of a
               resident driver-tick loop (the adbf5461 / axl_tls_free
               hazard), wedging the loop. The transport teardown from
               axl_tcp_close conveys the close (mirrors axl_tls_free
               dropping the close_notify alert): a FIN in the foreground,
               and — because a graceful Close() would itself wedge at a
               raised TPL when un-drained TX is still buffered — an RST
               under the driver pump (axl_tcp_close promotes a raised-TPL
               connection close to abortive; see tcp_close_impl). */
            if (buf != data) {
                axl_free(buf);
            }
            reset_connection(conn);
            return;
        } else if (hdr.opcode == WS_OP_TEXT || hdr.opcode == WS_OP_BINARY) {
            /* Dispatch to the per-connection handler (_ex) if registered,
               else the broadcast-style handler. */
            AxlWsEvent event = (hdr.opcode == WS_OP_TEXT)
                ? AXL_WS_TEXT : AXL_WS_BINARY;
            if (conn->ws_conn_handler != NULL) {
                conn->ws_conn_handler((AxlWsConn *)conn, event, payload,
                                      hdr.payload_len, conn->ws_data);
            } else if (conn->ws_handler != NULL) {
                conn->ws_handler(event, payload, hdr.payload_len,
                                 conn->ws_data);
            }
            /* A handler may have torn the connection down (axl_ws_conn_close):
               stop parsing further frames into a reset conn. */
            if (!conn->active) {
                if (buf != data) {
                    axl_free(buf);
                }
                return;
            }
        }

        offset += hdr.header_len + hdr.payload_len;
    }

    if (buf != data) {
        axl_free(buf);
    }

    /* Continue receiving (skip if a handler closed the connection). */
    if (conn->active) {
        start_conn_recv(s, conn);
    }
}

// ---------------------------------------------------------------------------
// Per-connection WebSocket API (P1)
// ---------------------------------------------------------------------------

int
axl_ws_send(AxlWsConn *ws_conn, size_t opcode, const void *data, size_t size)
{
    HttpConn *conn = (HttpConn *)ws_conn;
    if (conn == NULL || data == NULL || size == 0) {
        return AXL_ERR;
    }
    if (!conn->active || !conn->is_websocket) {
        return AXL_ERR;
    }

    uint8_t wsop;
    if (opcode == AXL_WS_TEXT) {
        wsop = WS_OP_TEXT;
    } else if (opcode == AXL_WS_BINARY) {
        wsop = WS_OP_BINARY;
    } else {
        return AXL_ERR;
    }

    size_t   frame_buf_size = size + 14;
    uint8_t *frame = axl_malloc(frame_buf_size);
    if (frame == NULL) {
        return AXL_ERR;
    }
    size_t frame_len = ws_build_frame(wsop, data, size, frame, frame_buf_size);
    if (frame_len == 0) {
        axl_free(frame);
        return AXL_ERR;
    }

    /* Serialize through the connection's outbound queue (it copies the frame),
       so back-to-back sends can't race the one-send-in-flight transport and
       desync TLS. */
    int rc = ws_outq_enqueue(conn, frame, frame_len);
    axl_free(frame);
    return rc;
}

int
axl_ws_conn_auth(AxlWsConn *ws_conn, AxlAuthInfo *out)
{
    HttpConn *conn = (HttpConn *)ws_conn;
    if (conn == NULL || out == NULL || !conn->ws_authed) {
        return AXL_ERR;
    }
    *out = conn->ws_auth;
    return AXL_OK;
}

int
axl_ws_conn_peer(AxlWsConn *ws_conn, uint8_t out[4])
{
    HttpConn *conn = (HttpConn *)ws_conn;
    if (conn == NULL || out == NULL) {
        return AXL_ERR;
    }
    uint8_t tmp[4];
    if (axl_ipv4_parse(conn->client_addr, tmp) != AXL_OK) {
        return AXL_ERR;
    }
    axl_memcpy(out, tmp, 4);
    return AXL_OK;
}

void
axl_ws_conn_set_user_data(AxlWsConn *ws_conn, void *user)
{
    HttpConn *conn = (HttpConn *)ws_conn;
    if (conn != NULL) {
        conn->ws_user_data = user;
    }
}

void *
axl_ws_conn_user_data(AxlWsConn *ws_conn)
{
    HttpConn *conn = (HttpConn *)ws_conn;
    return (conn != NULL) ? conn->ws_user_data : NULL;
}

int
axl_ws_conn_close(AxlWsConn *ws_conn)
{
    HttpConn *conn = (HttpConn *)ws_conn;
    if (conn == NULL || !conn->active || !conn->is_websocket) {
        return AXL_ERR;
    }
    /* Tear down (reset_connection fires the handler's AXL_WS_DISCONNECT).
       Do NOT synchronously send a close frame: a handler may call this from
       inside the loop dispatch, and a blocking send wedges a resident
       driver-tick loop (the nested-ephemeral-loop / raised-TPL hazard — see
       process_websocket_data's WS_OP_CLOSE branch and axl_tls_free). The TCP
       FIN from axl_tcp_close conveys the close. */
    reset_connection(conn);
    return AXL_OK;
}
