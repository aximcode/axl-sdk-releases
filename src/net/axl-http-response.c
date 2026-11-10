/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-response.c
    HTTP response building and public response helpers.
**/

#include "axl-http-server-internal.h"

#include <stdarg.h>

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Response sending (internal)
// ---------------------------------------------------------------------------

/* Append one formatted line to the header block, or mark it overflowed.
   axl_snprintf returns the length it WOULD have written, so adding that to
   the running length unchecked ran past the buffer on one long header (a
   long CSP from a middleware) -- the next append's `size - len` wrapped, and
   the final copy read past the stack buffer. A line that does not fit is
   not written at all, and send_response turns an overflowed block into a
   500 rather than send a truncated one. */
static void
hb_appendf(
    HeaderBuildCtx *hb,
    const char     *fmt,
    ...
    )
{
    va_list ap;
    size_t  room;
    int     n;

    if (hb->overflow) {
        return;
    }
    room = hb->buf_size - hb->len;
    va_start(ap, fmt);
    n = axl_vsnprintf(hb->buf + hb->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        hb->buf[hb->len] = '\0';
        hb->overflow = true;
        return;
    }
    hb->len += (size_t)n;
}

static void
emit_header(
    const void *key,
    void       *value,
    void       *data
    )
{
    hb_appendf((HeaderBuildCtx *)data, "%s: %s\r\n",
               (const char *)key, (const char *)value);
}

// ---------------------------------------------------------------------------
// Response transmit — fully async
//
// send_response builds the headers + body into a single heap-owned
// buffer (conn->tx_buf), submits one axl_tcp_send_async (or
// axl_tls_write_async for TLS), and returns. The completion
// callback (on_response_sent) frees tx_buf and decides whether to
// keep the connection alive (re-arm recv for the next request) or
// close it.
//
// Why fully async, why not sync wait: AxlLoop driver-mode dispatch
// (axl_loop_attach_driver) fires source callbacks at TPL_CALLBACK,
// where gBS->WaitForEvent returns EFI_UNSUPPORTED. The previous
// sync send_response (axl_tcp_send + ephemeral axl_loop_run) lost
// completions silently in driver mode — headers landed at TCP4
// (Transmit was issued) but the body send was rejected because
// sock->send_source was still set against the freed ephemeral
// loop, and reset_connection's axl_tcp_close dropped the send
// before the body got queued (a close drops a send still queued --
// see axl_tcp_close; it never cancels a Transmit the firmware holds). End-to-end symptom on the wire was
// "headers arrive, body never does, server stuck in CLOSE-WAIT."
// Fully async send_response avoids the WaitForEvent dependency
// entirely; works identically in foreground and driver mode.
// ---------------------------------------------------------------------------

// Forward decls for the streaming-response state machine
static bool stream_pump_next(AxlTcp *sock, AxlStatus status, void *data);

/* Single source of truth for "the streaming response is ending" —
   fires the cleanup hook (if any) and clears the streaming state.
   Safe to call when stream_active is false (no-op). Used by:
     - stream_pump_next on EOF or streamer error
     - reset_connection (defined elsewhere) on connection teardown
       before the stream finished — invoke from there to avoid
       leaking ctx-owned resources on client disconnect.
   The cleanup_fn may be NULL; the streamer self-cleans in that
   case (its EOF or error transition is its own teardown signal). */
void
end_stream_state(HttpConn *conn)
{
    if (!conn->stream_active) {
        return;
    }
    conn->stream_active = false;
    if (conn->stream_cleanup != NULL) {
        conn->stream_cleanup(conn->stream_ctx);
    }
    conn->stream_fn       = NULL;
    conn->stream_ctx      = NULL;
    conn->stream_cleanup  = NULL;
    if (conn->stream_chunk_buf != NULL) {
        axl_free(conn->stream_chunk_buf);
        conn->stream_chunk_buf = NULL;
        conn->stream_chunk_cap = 0;
    }
    conn->stream_remaining       = 0;
    conn->stream_chunked         = false;
    conn->stream_terminator_sent = false;
}

static bool
on_response_sent_body(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    );

static bool
on_response_sent(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    /* @p data is always a live pool connection here. */
    return http_dispatch(((HttpConn *)data)->server, on_response_sent_body, sock,
                         status, data);
}

static bool
on_response_sent_body(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    HttpConn *conn = (HttpConn *)data;
    (void)sock;

    /* Delivered from inside this connection's own teardown: axl_tcp_close
       runs every callback it still owes before it returns, and a send that
       had already FINISHED arrives with its real status -- AXL_OK here, not
       AXL_CANCELLED. Acting on it would complete a 101 upgrade (firing the
       app's WS CONNECT after do_reset_connection decided on DISCONNECT, for
       a slot about to be zeroed and reused) or re-arm recv on the closing
       socket. do_reset_connection owns tx_buf, the upload state and the
       stream state from the moment it starts, so there is nothing to
       release here. This one check also covers a streamed response: every
       chunk and terminator send completes through this callback, which is
       the only way into stream_pump_next (and teardown has already cleared
       stream_active, so an owed chunk would otherwise land in the
       keep-alive branch below and re-arm recv). */
    if (conn->tearing_down) {
        return AXL_SOURCE_REMOVE;
    }

    /* Free the combined headers+body buffer first — reset_connection
       below also tries to free it as a safety net, so NULL-out to
       avoid double-free. */
    if (conn->tx_buf != NULL) {
        axl_free(conn->tx_buf);
        conn->tx_buf = NULL;
    }

    /* Upload-stream cleanup. The upload handler builds its response
       in conn->upload_resp; we have to release that state AFTER the
       send completes (the body bytes pointed at upload_resp.body
       were memcpy'd into tx_buf, but the AxlHttpResponse fields
       themselves are owned by the upload-stream path and get freed
       here, mirroring the synchronous teardown the old
       stream_upload_data did inline before the async refactor).
       Runs regardless of send status — clearing is_upload_stream is
       what tells reset_connection's abort guard "the handler already
       ran its clean-EOF call, do NOT also fire an abort." Without
       this clear ahead of the error-return below, a TCP send failure
       AFTER the handler's clean-EOF call would double-fire the
       handler with aborted=true. */
    if (conn->is_upload_stream) {
        axl_free(conn->upload_buf);
        conn->upload_buf     = NULL;
        conn->upload_buf_len = 0;
        http_response_release(&conn->upload_resp);   /* a static body is the handler's */
        axl_memset(&conn->upload_resp, 0, sizeof(conn->upload_resp));
        conn->is_upload_stream = false;
        conn->upload_route     = NULL;
    }

    /* Cancel/error: tear down the connection. axl_tcp_send_async's
       contract leaves the sock valid on cancel/error, so
       reset_connection's axl_tcp_close still works. */
    if (status != AXL_OK) {
        end_stream_state(conn);
        reset_connection(conn);
        return AXL_SOURCE_REMOVE;
    }

    /* Streaming response: the headers send (or a prior chunk send)
       just completed. Hand off to stream_pump_next, which produces
       the next chunk by invoking the consumer's streamer. EOF / error
       transitions live inside pump_next; if it returns successfully
       the connection enters the next chunk's in-flight state — we
       do NOT fall through to keep-alive / reset here. */
    if (conn->stream_active) {
        return stream_pump_next(sock, status, data);
    }

    /* Post-send: keep-alive or close, the work that used to live
       in dispatch_and_respond's tail. */
    if (conn->keep_alive) {
        axl_free(conn->method);  conn->method = NULL;
        axl_free(conn->path);    conn->path   = NULL;
        axl_free(conn->query);   conn->query  = NULL;

        if (conn->headers != NULL) {
            axl_hash_table_free(conn->headers);
            conn->headers = NULL;
        }
        if (conn->body != NULL) {
            axl_free(conn->body);
            conn->body = NULL;
        }

        conn->header_len      = 0;
        conn->headers_done    = false;
        conn->content_length  = 0;
        conn->body_bytes_read = 0;
        conn->body_alloc      = 0;
        conn->chunked         = false;
        conn->chunked_done    = false;

        /* Fire the deferred WebSocket CONNECT now that the 101 is on the
           wire (P1): a greeting axl_ws_send from the handler is valid here.
           THE PAIRING RULE: a CONNECT the app receives and does not reject
           gets exactly one DISCONNECT; a handler returning AXL_ERR rejects
           the connection, and a rejected one gets none. So ws_connected
           (which gates DISCONNECT) is set as CONNECT is delivered and cleared
           again on a reject -- setting it only after an accept left a
           handler that closed its own connection from inside CONNECT with no
           DISCONNECT at all. A teardown asked for while the handler runs is
           deferred (do_reset_connection) and run here, after it returns. */
        if (conn->is_websocket && conn->ws_connect_pending) {
            conn->ws_connect_pending = false;
            conn->ws_connected       = true;
            conn->ws_in_connect      = true;
            int crc = AXL_OK;
            if (conn->ws_conn_handler != NULL) {
                crc = conn->ws_conn_handler((AxlWsConn *)conn, AXL_WS_CONNECT,
                                            NULL, 0, conn->ws_data);
            } else if (conn->ws_handler != NULL) {
                crc = conn->ws_handler(AXL_WS_CONNECT, NULL, 0, conn->ws_data);
            }
            conn->ws_in_connect = false;
            if (crc != AXL_OK) {
                conn->ws_connected = false;   /* rejected: no DISCONNECT */
            }
            if (crc != AXL_OK || conn->ws_close_deferred) {
                /* A rejected connection is closed silently; an accepted one
                   whose close was asked for during CONNECT gets its
                   DISCONNECT here. Either way honour an abortive request. */
                if (conn->ws_close_abortive) {
                    reset_connection_abortive(conn);
                } else {
                    reset_connection(conn);
                }
                return AXL_SOURCE_REMOVE;
            }
        }

        start_conn_recv(conn->server, conn);
    } else {
        reset_connection(conn);
    }

    return AXL_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Streaming response — per-chunk send loop
// ---------------------------------------------------------------------------

/* Chunk-buffer layout for streaming responses:
 *
 *   [ prefix reserve ][ payload .................... ][ \r\n ]
 *   ^---STREAM_CHUNK_PREFIX---^                       ^trailing CRLF (2 bytes)
 *
 * Content-Length path: payload starts at offset 0, no framing, no trailing
 * CRLF. We pass (buf, produced) directly to send_async.
 *
 * Chunked path: payload starts at STREAM_CHUNK_PREFIX. The hex-size +
 * "\r\n" prefix (3..10 bytes) is written backward-fit so it ends at
 * offset STREAM_CHUNK_PREFIX, immediately before the payload. The
 * send buffer pointer is `buf + (STREAM_CHUNK_PREFIX - prefix_len)`,
 * length = prefix_len + produced + 2 (trailing CRLF). 16 bytes is
 * comfortably more than the 7-char "10000\r\n" worst case for a
 * 64KiB chunk; we also write the trailing "\r\n" at offset
 * STREAM_CHUNK_PREFIX + produced. */

#define STREAM_CHUNK_PAYLOAD_MAX  (16 * 1024)
#define STREAM_CHUNK_PREFIX       16
#define STREAM_CHUNK_TRAILER      2
#define STREAM_CHUNK_BUF_SIZE     \
    (STREAM_CHUNK_PREFIX + STREAM_CHUNK_PAYLOAD_MAX + STREAM_CHUNK_TRAILER)

static bool
stream_pump_next(AxlTcp *sock, AxlStatus status, void *data)
{
    HttpConn *conn = (HttpConn *)data;
    (void)sock;

    if (status != AXL_OK) {
        end_stream_state(conn);
        reset_connection(conn);
        return AXL_SOURCE_REMOVE;
    }

    /* Stream complete? Two terminating conditions:
       - Chunked: the final "0\r\n\r\n" marker just got acknowledged.
       - Content-Length: stream_remaining hit 0 (the last chunk was
         the closing one; nothing more to produce).
       Either way: invoke cleanup, run upload-stream teardown if the
       handler that built this response was an upload handler that
       also installed a streamer (mutually-exclusive by convention,
       but enforce here so a future caller doing both doesn't leak
       upload_buf / upload_resp.body / upload_resp.headers), then
       keep-alive vs reset. */
    if (conn->stream_terminator_sent ||
        (!conn->stream_chunked && conn->stream_remaining == 0))
    {
        end_stream_state(conn);
        if (conn->is_upload_stream) {
            axl_free(conn->upload_buf);
            conn->upload_buf     = NULL;
            conn->upload_buf_len = 0;
            http_response_release(&conn->upload_resp);
            axl_memset(&conn->upload_resp, 0, sizeof(conn->upload_resp));
            conn->is_upload_stream = false;
            conn->upload_route     = NULL;
        }
        if (conn->keep_alive) {
            axl_free(conn->method);  conn->method = NULL;
            axl_free(conn->path);    conn->path   = NULL;
            axl_free(conn->query);   conn->query  = NULL;
            if (conn->headers != NULL) {
                axl_hash_table_free(conn->headers);
                conn->headers = NULL;
            }
            if (conn->body != NULL) {
                axl_free(conn->body);
                conn->body = NULL;
            }
            conn->header_len      = 0;
            conn->headers_done    = false;
            conn->content_length  = 0;
            conn->body_bytes_read = 0;
            conn->body_alloc      = 0;
            conn->chunked         = false;
            conn->chunked_done    = false;
            start_conn_recv(conn->server, conn);
        } else {
            reset_connection(conn);
        }
        return AXL_SOURCE_REMOVE;
    }

    /* Pull the next chunk. The streamer fills payload bytes; we'll
       wrap them with chunked framing if needed. */
    uint8_t *payload  = (uint8_t *)conn->stream_chunk_buf + STREAM_CHUNK_PREFIX;
    size_t   pay_cap  = STREAM_CHUNK_PAYLOAD_MAX;
    if (!conn->stream_chunked && conn->stream_remaining < pay_cap) {
        pay_cap = conn->stream_remaining;
    }

    size_t produced = 0;
    int r = conn->stream_fn(conn->stream_ctx, payload, pay_cap, &produced);
    if (r != AXL_OK) {
        axl_warning("stream: producer returned ERR mid-response - abrupt close");
        end_stream_state(conn);
        reset_connection(conn);
        return AXL_SOURCE_REMOVE;
    }

    /* EOF (produced == 0). Chunked needs the terminator marker on
       the wire; Content-Length is just done — re-enter pump on the
       next tick to take the completion branch above. */
    if (produced == 0) {
        if (!conn->stream_chunked) {
            /* Content-Length stream that the producer ended early.
               Treat as success but flag — the client will likely see
               a truncated response. (We can't backfill bytes; the
               Content-Length header was already sent.) */
            if (conn->stream_remaining > 0) {
                axl_warning("stream: producer EOF with %zu bytes "
                            "remaining of declared Content-Length - "
                            "client will see a truncated body",
                            conn->stream_remaining);
            }
            conn->stream_remaining = 0;
            return stream_pump_next(sock, AXL_OK, data);
        }
        /* Chunked: emit "0\r\n\r\n" terminator and let the next
           completion fall into the "stream complete" branch above. */
        const char term[] = "0\r\n\r\n";
        axl_memcpy(conn->stream_chunk_buf, term, sizeof(term) - 1);
        conn->stream_terminator_sent = true;

        int rc = (conn->tls_ctx != NULL)
               ? axl_tls_write_async(conn->tls_ctx, conn->stream_chunk_buf,
                                     sizeof(term) - 1,
                                     conn->server->loop, on_response_sent, conn)
               : axl_tcp_send_async(conn->sock, conn->stream_chunk_buf,
                                    sizeof(term) - 1,
                                    conn->server->loop, NULL,
                                    on_response_sent, conn);
        if (rc != AXL_OK) {
            end_stream_state(conn);
            reset_connection(conn);
        }
        return AXL_SOURCE_REMOVE;
    }

    /* produced > 0. Build the send buffer with framing if chunked,
       or just point at the raw payload otherwise. */
    void  *send_buf;
    size_t send_len;
    if (conn->stream_chunked) {
        char   prefix[STREAM_CHUNK_PREFIX];
        size_t prefix_len = axl_snprintf(prefix, sizeof(prefix),
                                         "%zx\r\n", produced);
        /* Backward-fit the prefix so it ends right at the payload
           start. Same byte image; the send pointer is shifted to
           include the prefix in the contiguous buffer. */
        uint8_t *prefix_dst = (uint8_t *)conn->stream_chunk_buf
                            + STREAM_CHUNK_PREFIX - prefix_len;
        axl_memcpy(prefix_dst, prefix, prefix_len);
        /* Trailing "\r\n" after the payload bytes. */
        uint8_t *trail_dst = payload + produced;
        trail_dst[0] = '\r';
        trail_dst[1] = '\n';

        send_buf = prefix_dst;
        send_len = prefix_len + produced + 2;
    } else {
        send_buf = payload;
        send_len = produced;
        if (conn->stream_remaining >= produced) {
            conn->stream_remaining -= produced;
        } else {
            /* Producer overshot Content-Length. Truncate the send
               so we don't desync the client. */
            send_len = conn->stream_remaining;
            conn->stream_remaining = 0;
            axl_warning("stream: producer overshot declared "
                        "Content-Length - truncating");
        }
    }

    int rc = (conn->tls_ctx != NULL)
           ? axl_tls_write_async(conn->tls_ctx, send_buf, send_len,
                                 conn->server->loop, on_response_sent, conn)
           : axl_tcp_send_async(conn->sock, send_buf, send_len,
                                conn->server->loop, NULL,
                                on_response_sent, conn);
    if (rc != AXL_OK) {
        axl_warning("stream: send_async submit failed - abrupt close");
        end_stream_state(conn);
        reset_connection(conn);
    }
    return AXL_SOURCE_REMOVE;
}

/* RFC 9110 §6.4.1: a 1xx, 204, or 304 response carries no body and must
   not carry Content-Length; every other status may have a body, so an
   empty-body response on a keep-alive connection still needs an explicit
   Content-Length: 0 to delimit it (without one the client blocks waiting
   for a body that never arrives). */
static bool
status_allows_body(
    size_t status_code
    )
{
    return !(status_code / 100 == 1
             || status_code == 204
             || status_code == 304);
}

/* Case-insensitive scan of a response's custom header table for an
   already-set Content-Length, so the auto-emitted zero-length delimiter
   never duplicates one a handler set itself (e.g. a HEAD response that
   reports the entity length with an empty body). */
static void
note_content_length(
    const void *key,
    void       *value,
    void       *data
    )
{
    (void)value;
    if (axl_strcasecmp((const char *)key, "Content-Length") == 0) {
        *(bool *)data = true;
    }
}

static bool
response_has_content_length(
    const AxlHttpResponse *resp
    )
{
    if (resp->headers == NULL) {
        return false;
    }
    bool found = false;
    axl_hash_table_foreach(resp->headers, note_content_length, &found);
    return found;
}

void
send_response(
    HttpConn        *conn,
    AxlHttpResponse *resp
    )
{
    char           header[2048];
    size_t         header_len;
    bool           has_streamer = (resp->streamer != NULL);
    bool           drop_body    = false;
    HeaderBuildCtx hb = { header, sizeof(header), 0, false };

    hb.len = http_build_status_line(header, sizeof(header), resp->status_code);

    if (resp->content_type != NULL) {
        hb_appendf(&hb, "Content-Type: %s\r\n", resp->content_type);
    }

    if (has_streamer) {
        /* Streaming: Content-Length when total_size is known,
           Transfer-Encoding: chunked when not. */
        if (resp->streamer_total_size != (size_t)-1) {
            hb_appendf(&hb, "Content-Length: %llu\r\n",
                (unsigned long long)resp->streamer_total_size);
        } else {
            hb_appendf(&hb, "Transfer-Encoding: chunked\r\n");
        }
    } else if (resp->body != NULL && resp->body_size > 0) {
        hb_appendf(&hb, "Content-Length: %llu\r\n", (unsigned long long)resp->body_size);
    } else if (status_allows_body(resp->status_code)
               && !response_has_content_length(resp)) {
        /* Empty body, no streamer, but the status permits one (201 from
           WebDAV PUT/MKCOL/MOVE/COPY, an empty 200 from OPTIONS, …): a
           keep-alive client needs an explicit zero-length delimiter or
           it blocks waiting for a body. Skipped when a handler already
           set its own Content-Length (HEAD reports the entity length
           with an empty body) so the two never collide. */
        hb_appendf(&hb, "Content-Length: 0\r\n");
    }

    /* A 101 Switching Protocols response is an upgrade, not a keep-alive
       reuse: RFC 6455 §4.2.2 (and RFC 7230 for any Upgrade) requires
       "Connection: Upgrade". Emitting "keep-alive" here makes every
       standard WebSocket client (websocket-client, browsers) reject the
       handshake with "Invalid WebSocket Header". The upgrade handler keeps
       conn->keep_alive = true so the socket is *held open* for framing —
       that's the recv-rearm signal, distinct from the wire Connection
       token, so it must not leak into this header. */
    if (resp->status_code == 101) {
        hb_appendf(&hb, "Connection: Upgrade\r\n");
    } else {
        hb_appendf(&hb, "Connection: %s\r\n",
                 conn->keep_alive ? "keep-alive" : "close");
    }

    /* Challenge interactive clients on a 401 when the server configured
       one (axl_http_server_set_auth_challenge). Without it a 401 carries
       no WWW-Authenticate and browsers/Finder never prompt for creds. */
    if (resp->status_code == 401 && conn->server->auth_scheme != NULL) {
        if (conn->server->auth_realm != NULL) {
            hb_appendf(&hb, "WWW-Authenticate: %s realm=\"%s\"\r\n",
                conn->server->auth_scheme, conn->server->auth_realm);
        } else {
            hb_appendf(&hb, "WWW-Authenticate: %s\r\n", conn->server->auth_scheme);
        }
    }

    /* Emit any custom response headers */
    if (resp->headers != NULL) {
        axl_hash_table_foreach(resp->headers, emit_header, &hb);
    }

    /* End of headers */
    hb_appendf(&hb, "\r\n");

    /* Headers that do not fit are not sent truncated: that is a malformed
       response a client may misread. Answer 500, empty, and close -- and run
       a streamer's cleanup, since it will never be pumped. */
    if (hb.overflow) {
        axl_error("send_response: headers for %zu exceed %zu bytes; sending 500",
                  resp->status_code, sizeof(header));
        if (has_streamer && resp->streamer_cleanup != NULL) {
            resp->streamer_cleanup(resp->streamer_ctx);
        }
        has_streamer     = false;
        drop_body        = true;
        conn->keep_alive = false;
        hb.overflow      = false;
        hb.len = http_build_status_line(header, sizeof(header), 500);
        hb_appendf(&hb, "Content-Length: 0\r\nConnection: close\r\n\r\n");
    }
    header_len = hb.len;

    /* Defensive: if a previous response is still in flight on this
       conn, drop its tx_buf. Shouldn't happen — recv is single-armed
       so we don't dispatch a second handler before the first
       completes — but free-then-overwrite keeps the invariant
       trivially correct. */
    if (conn->tx_buf != NULL) {
        axl_warning("send_response: stale tx_buf on conn - freeing");
        axl_free(conn->tx_buf);
        conn->tx_buf = NULL;
    }

    /* Streaming responses send headers FIRST as their own buffer,
       then chain chunk sends from on_response_sent → stream_pump_next.
       Initialize the stream state here so the first completion knows
       to enter the streaming branch. */
    if (has_streamer) {
        conn->stream_chunk_buf = axl_malloc(STREAM_CHUNK_BUF_SIZE);
        if (conn->stream_chunk_buf == NULL) {
            axl_error("send_response: stream chunk-buf alloc(%d) failed",
                      STREAM_CHUNK_BUF_SIZE);
            if (resp->streamer_cleanup != NULL) {
                resp->streamer_cleanup(resp->streamer_ctx);
            }
            reset_connection(conn);
            return;
        }
        conn->stream_chunk_cap = STREAM_CHUNK_BUF_SIZE;
        conn->stream_active    = true;
        conn->stream_fn        = resp->streamer;
        conn->stream_ctx       = resp->streamer_ctx;
        conn->stream_cleanup   = resp->streamer_cleanup;
        conn->stream_terminator_sent = false;

        if (resp->streamer_total_size != (size_t)-1) {
            conn->stream_chunked   = false;
            conn->stream_remaining = resp->streamer_total_size;
        } else {
            conn->stream_chunked   = true;
            conn->stream_remaining = 0;  /* unused on chunked path */
        }

        void *hdr_buf = axl_malloc(header_len);
        if (hdr_buf == NULL) {
            axl_error("send_response: header-buf alloc(%zu) failed", header_len);
            end_stream_state(conn);
            reset_connection(conn);
            return;
        }
        axl_memcpy(hdr_buf, header, header_len);
        conn->tx_buf = hdr_buf;

        int rc;
        if (conn->tls_ctx != NULL) {
            rc = axl_tls_write_async(conn->tls_ctx, hdr_buf, header_len,
                                     conn->server->loop,
                                     on_response_sent, conn);
        } else {
            rc = axl_tcp_send_async(conn->sock, hdr_buf, header_len,
                                    conn->server->loop, NULL,
                                    on_response_sent, conn);
        }
        if (rc != AXL_OK) {
            axl_warning("send_response: stream-headers submit failed");
            axl_free(conn->tx_buf);
            conn->tx_buf = NULL;
            end_stream_state(conn);
            reset_connection(conn);
        }
        return;
    }

    /* Non-streaming: build the combined headers + body buffer. We
       memcpy the body (rather than chaining two sends) so the
       response struct can be freed by the dispatcher as soon as
       send_response returns — the in-flight tx_buf is the only thing
       the completion callback needs to reach. */
    size_t body_size = (!drop_body && resp->body != NULL) ? resp->body_size : 0;
    size_t total = header_len + body_size;

    void *combined = axl_malloc(total);
    if (combined == NULL) {
        axl_error("send_response: alloc(%zu) failed - abrupt close", total);
        reset_connection(conn);
        return;
    }
    axl_memcpy(combined, header, header_len);
    if (body_size > 0) {
        axl_memcpy((char *)combined + header_len, resp->body, body_size);
    }
    conn->tx_buf = combined;

    /* Submit. Both async APIs deliver the completion at the loop's
       normal dispatch level — TPL_APPLICATION in foreground mode
       (inside axl_loop_run), TPL_CALLBACK in driver mode (the
       firmware's notify-timer queue). Either way, on_response_sent
       runs without our caller having to WaitForEvent. */
    int rc;
    if (conn->tls_ctx != NULL) {
        rc = axl_tls_write_async(conn->tls_ctx, combined, total,
                                 conn->server->loop,
                                 on_response_sent, conn);
    } else {
        rc = axl_tcp_send_async(conn->sock, combined, total,
                                conn->server->loop, NULL,
                                on_response_sent, conn);
    }

    if (rc != AXL_OK) {
        axl_warning("send_response: submit failed - abrupt close");
        axl_free(conn->tx_buf);
        conn->tx_buf = NULL;
        reset_connection(conn);
    }
}

/* Static: a send's buffer must outlive its callback, and this one is never
   freed. TLS encrypts it synchronously; plain TCP copies each piece before
   the firmware sees it. */
static const char http_continue_line[] = "HTTP/1.1 100 Continue\r\n\r\n";

/* Nothing to release, and nothing to do on failure: a connection whose send
   failed is torn down by the receive armed after it, as a peer that went
   away is. */
static bool
on_continue_sent(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    (void)sock;
    (void)status;
    (void)data;
    return true;
}

bool
send_continue(
    HttpConn *conn
    )
{
    size_t len = sizeof(http_continue_line) - 1;
    int    rc  = (conn->tls_ctx != NULL)
                 ? axl_tls_write_async(conn->tls_ctx, http_continue_line, len,
                                       conn->server->loop, on_continue_sent, NULL)
                 : axl_tcp_send_async(conn->sock, http_continue_line, len,
                                      conn->server->loop, NULL, on_continue_sent, NULL);
    if (rc != AXL_OK) {
        /* As on_response_sent does for a failed send: a TLS record can fail
           after it took its sequence number, and every later one would then
           fail the peer's MAC check. */
        axl_debug("100 Continue could not be queued - closing");
        reset_connection(conn);
        return false;
    }
    return true;
}

void
send_middleware_refusal(
    HttpConn        *conn,
    AxlHttpResponse *resp
    )
{
    if (resp->status_code == 200) {
        resp->status_code = 500;
    }
    conn->keep_alive = false;
    send_response(conn, resp);
    /* send_response copied it into the transmit buffer. */
    http_response_release(resp);
}

void
send_error_response(
    HttpConn *conn,
    size_t    status_code
    )
{
    AxlHttpResponse resp;

    axl_memset(&resp, 0, sizeof(resp));
    resp.status_code = status_code;

    /* Error responses always close the connection. The previous
       synchronous-send pattern was
       `send_error_response(); reset_connection();` at every caller —
       that explicit close raced the new async send (axl_tcp_close
       cancels the just-queued Transmit before TCP4 puts it on the
       wire, so the client sees nothing). Force keep_alive=false
       here and let on_response_sent's post-send branch run
       reset_connection AFTER the response actually transmits. */
    conn->keep_alive = false;

    send_response(conn, &resp);
}

/* Collects the keys of the message-framing headers in a response's table:
   send_response frames the response itself, and a copy the middleware set
   would contradict it. */
#define AXL_HTTP_FRAMING_MAX 8
typedef struct {
    const char *keys[AXL_HTTP_FRAMING_MAX];
    size_t      n;
} FramingKeys;

static void
note_framing_header(
    const void *key,
    void       *value,
    void       *data
    )
{
    FramingKeys *fk = (FramingKeys *)data;
    const char  *k  = (const char *)key;

    (void)value;
    if (fk->n < AXL_HTTP_FRAMING_MAX
        && (axl_strcasecmp(k, "Connection") == 0
            || axl_strcasecmp(k, "Content-Length") == 0
            || axl_strcasecmp(k, "Transfer-Encoding") == 0)) {
        fk->keys[fk->n++] = k;
    }
}

/* A streamer the response carries but will not be sent: run its cleanup and
   clear it, as send_response would on taking it over. */
void
http_response_drop_streamer(
    AxlHttpResponse *resp
    )
{
    if (resp->streamer != NULL && resp->streamer_cleanup != NULL) {
        resp->streamer_cleanup(resp->streamer_ctx);
    }
    resp->streamer         = NULL;
    resp->streamer_ctx     = NULL;
    resp->streamer_cleanup = NULL;
}

void
http_response_strip_framing(
    AxlHttpResponse *resp
    )
{
    if (resp->headers != NULL) {
        FramingKeys fk = { { NULL }, 0 };
        axl_hash_table_foreach(resp->headers, note_framing_header, &fk);
        for (size_t i = 0; i < fk.n; i++) {
            axl_hash_table_remove(resp->headers, fk.keys[i]);
        }
    }
}

void
http_response_release(
    AxlHttpResponse *resp
    )
{
    if (resp->body != NULL && !resp->body_static) {
        axl_free(resp->body);
    }
    resp->body = NULL;
    if (resp->headers != NULL) {
        axl_hash_table_free(resp->headers);
        resp->headers = NULL;
    }
}

void
send_error_response_headers(
    HttpConn        *conn,
    size_t           status_code,
    AxlHttpResponse *resp
    )
{
    /* The dispatcher's refusals (404, 401, 403) go out with the headers the
       middleware set -- a consumer's security headers belong on an error as
       much as on a success -- but not its body: the status is the answer.
       The caller still owns and frees resp. */
    AxlHttpResponse out;

    /* Never sent here: a streamer the middleware set is cleaned up, and a
       Connection / Content-Length / Transfer-Encoding it set is dropped --
       on a bodyless error it would contradict the response's own framing
       (a Content-Length: N with no body leaves the client waiting). */
    http_response_drop_streamer(resp);
    http_response_strip_framing(resp);
    axl_memset(&out, 0, sizeof(out));
    out.status_code = status_code;
    out.headers     = resp->headers;
    conn->keep_alive = false;   /* as send_error_response, for the same reason */
    send_response(conn, &out);
}

// ---------------------------------------------------------------------------
// Public response helpers
// ---------------------------------------------------------------------------

static void
set_body_copy(
    AxlHttpResponse *r,
    const void      *data,
    size_t           size
    )
{
    /* Only free a previously-installed body if WE owned it. A body
       marked static was lent to us by the caller via set_static —
       drop the pointer without freeing. */
    if (r->body != NULL && !r->body_static) {
        axl_free(r->body);
    }
    r->body_static = false;

    r->body = axl_malloc(size);
    if (r->body != NULL) {
        axl_memcpy(r->body, data, size);
        r->body_size = size;
    } else {
        /* The set_json/set_text/set_range public wrappers all return
         * void, so a failed alloc would otherwise be invisible — the
         * client would see a 200 response with an empty body and no
         * idea why. Surface it via the operator log; the response goes
         * out empty (current behaviour preserved for ABI). */
        axl_warning("response body alloc failed: %zu bytes", size);
        r->body_size = 0;
    }
}

void
axl_http_response_set_json(AxlHttpResponse *r, const char *json)
{
    if (r == NULL || json == NULL) {
        return;
    }

    set_body_copy(r, json, axl_strlen(json));
    r->content_type = "application/json";
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_text(AxlHttpResponse *r, const char *text)
{
    if (r == NULL || text == NULL) {
        return;
    }

    set_body_copy(r, text, axl_strlen(text));
    r->content_type = "text/plain";
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_bytes(
    AxlHttpResponse *r,
    const AxlBytes  *body,
    const char      *content_type
    )
{
    if (r == NULL || body == NULL) {
        return;
    }

    size_t      n = 0;
    const void *p = axl_bytes_get_data(body, &n);

    /* p is NULL for an empty AxlBytes; pass a valid (zero-length)
       pointer so set_body_copy's memcpy never sees NULL. */
    set_body_copy(r, (p != NULL) ? p : "", n);   /* copies; frees any prior owned body */
    if (content_type != NULL) {
        r->content_type = content_type;
    }
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_status(AxlHttpResponse *r, size_t code)
{
    if (r != NULL) {
        r->status_code = code;
    }
}

void
axl_http_response_set_file(AxlHttpResponse *r, const char *path)
{
    if (r == NULL || path == NULL) {
        return;
    }

    {
        void    *tmp  = NULL;
        size_t   len  = 0;
        int      rc;

        rc = axl_file_get_contents(path, &tmp, &len);
        if (rc != AXL_OK) {
            axl_debug("static file not found: %s", path);
            r->status_code = 404;
            return;
        }
        void *buffer = axl_malloc(len);
        if (buffer == NULL) {
            axl_error("response body alloc failed: %zu bytes for %s",
                      len, path);
            axl_free(tmp);
            r->status_code = 500;
            return;
        }
        axl_memcpy(buffer, tmp, len);
        size_t size = (size_t)len;
        axl_free(tmp);

        if (r->body != NULL && !r->body_static) {
            axl_free(r->body);
        }
        r->body_static = false;

        r->body      = buffer;
        r->body_size = size;
    }

    //
    // Infer content type from extension
    //
    const char *ext = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            ext = p;
        }
    }

    r->content_type = "application/octet-stream";
    if (ext != NULL) {
        if (axl_strcmp(ext, ".html") == 0 || axl_strcmp(ext, ".htm") == 0) {
            r->content_type = "text/html";
        } else if (axl_strcmp(ext, ".css") == 0) {
            r->content_type = "text/css";
        } else if (axl_strcmp(ext, ".js") == 0) {
            r->content_type = "application/javascript";
        } else if (axl_strcmp(ext, ".json") == 0) {
            r->content_type = "application/json";
        } else if (axl_strcmp(ext, ".txt") == 0) {
            r->content_type = "text/plain";
        } else if (axl_strcmp(ext, ".png") == 0) {
            r->content_type = "image/png";
        } else if (axl_strcmp(ext, ".jpg") == 0 || axl_strcmp(ext, ".jpeg") == 0) {
            r->content_type = "image/jpeg";
        } else if (axl_strcmp(ext, ".svg") == 0) {
            r->content_type = "image/svg+xml";
        } else if (axl_strcmp(ext, ".ico") == 0) {
            r->content_type = "image/x-icon";
        }
    }

    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_static(
    AxlHttpResponse *r,
    const void      *body,
    size_t           size,
    const char      *content_type
    )
{
    if (r == NULL || body == NULL) {
        return;
    }

    /* Free any previously-installed *owned* body. A body already
       marked static was lent to us by the caller — we don't free it,
       we just overwrite the pointer with the new one. */
    if (r->body != NULL && !r->body_static) {
        axl_free(r->body);
    }

    /* Cast away const: the struct stores a single `void *` for both
       owned and borrowed bodies, with body_static disambiguating
       lifetime. The dispatch loop will not write to it; the only
       read is the send path, which treats body as `const`. */
    r->body        = (void *)body;
    r->body_size   = size;
    r->body_static = true;

    if (content_type != NULL) {
        r->content_type = content_type;
    }
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

/* Internal helper — formats a Content-Range header per RFC 9110
   §15.3.7 (`bytes <start>-<end>/<total>`, end inclusive) and inserts
   it into r->headers, lazy-allocating the table. Used by both
   axl_http_response_set_range (the all-in-one buffered path) and
   axl_http_response_set_content_range (the streaming-response
   path where the consumer manages status separately).

   Lazy table allocation uses new_full(axl_free_impl, axl_free_impl)
   — both keys AND values are heap-owned and freed on table free.
   Matches the request-side pattern in axl_http_parse_headers.
   axl_hash_table_new_str would leak values (str table only owns
   keys; values are borrowed). */
static void
set_content_range_header(AxlHttpResponse *r, uint64_t start,
                         uint64_t end, uint64_t total)
{
    if (r->headers == NULL) {
        r->headers = axl_hash_table_new_full(
            axl_str_hash, axl_str_equal,
            axl_free_impl, axl_free_impl);
        if (r->headers == NULL) {
            return;
        }
    } else if (!axl_hash_table_owns_entries(r->headers)) {
        /* Caller pre-allocated r->headers under the wrong destroy-
           func contract — most likely with axl_hash_table_new_str()
           which has copy_keys=true (would double-strdup our key
           and leak the outer copy) AND value_destroy=NULL (would
           never free our strdup'd value). Refuse rather than
           silently leak. The header IS the API surface we're
           trying to deliver, so callers will notice the missing
           Content-Range; the warning here points them at the
           docstring contract. */
        axl_warning("set_content_range_header: r->headers was "
                    "pre-allocated with the wrong destroy-func "
                    "contract; header NOT inserted (would leak "
                    "key+value strdups). Allocate via "
                    "axl_hash_table_new_full(axl_str_hash, "
                    "axl_str_equal, axl_free_impl, axl_free_impl) "
                    "or leave NULL for lazy allocation.");
        return;
    }
    char buf[64];
    axl_snprintf(buf, sizeof(buf),
                 "bytes %llu-%llu/%llu",
                 (unsigned long long)start,
                 (unsigned long long)end,
                 (unsigned long long)total);
    char *key = axl_strdup("Content-Range");
    char *val = axl_strdup(buf);
    if (key == NULL || val == NULL) {
        axl_free(key);
        axl_free(val);
        return;
    }
    /* axl_hash_table_replace: if Content-Range was already set
       (consumer called set_range twice on the same response, say),
       the OLD key + value get freed via the destroy hooks above and
       the NEW pair is kept. axl_hash_table_insert would keep the
       OLD key, which would leak our new key strdup. */
    axl_hash_table_replace(r->headers, key, val);
}

void
axl_http_response_set_range(AxlHttpResponse *r, const void *data,
                            size_t offset, size_t length,
                            size_t total_size)
{
    if (r == NULL || data == NULL || length == 0) {
        return;
    }

    set_body_copy(r, (const uint8_t *)data + offset, (size_t)length);
    r->status_code  = 206;
    r->content_type = "application/octet-stream";
    /* RFC 9110 §15.3.7: every 206 MUST carry Content-Range. The
       docstring claimed this for years; the body of the function
       never did it. Tolerant clients (curl, browsers) accepted the
       header-less 206 because Content-Length matched the requested
       slice; strict clients would reject it. */
    set_content_range_header(r, offset, offset + length - 1, total_size);
}

void
axl_http_response_set_content_range(AxlHttpResponse *r, uint64_t start,
                                    uint64_t end, uint64_t total)
{
    if (r == NULL) {
        return;
    }
    set_content_range_header(r, start, end, total);
}

void
axl_http_response_set_streamer(
    AxlHttpResponse     *r,
    AxlResponseStreamer  streamer,
    void                *ctx,
    AxlResponseCleanup   cleanup,
    size_t               total_size,
    const char          *content_type
    )
{
    if (r == NULL || streamer == NULL) {
        return;
    }
    /* Replace any prior contiguous body — the streaming and copy
       paths are mutually exclusive. Owned bodies are freed; static
       bodies are dropped without freeing (lifetime is the caller's). */
    if (r->body != NULL && !r->body_static) {
        axl_free(r->body);
    }
    r->body        = NULL;
    r->body_size   = 0;
    r->body_static = false;

    r->streamer            = streamer;
    r->streamer_ctx        = ctx;
    r->streamer_cleanup    = cleanup;
    r->streamer_total_size = total_size;

    if (content_type != NULL) {
        r->content_type = content_type;
    }
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}
