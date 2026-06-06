/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-response.c
    HTTP response building and public response helpers.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Response sending (internal)
// ---------------------------------------------------------------------------

static void
emit_header(
    const void *key,
    void       *value,
    void       *data
    )
{
    HeaderBuildCtx *ctx = (HeaderBuildCtx *)data;

    if (ctx->len < ctx->buf_size) {
        ctx->len += axl_snprintf(
                      ctx->buf + ctx->len,
                      ctx->buf_size - ctx->len,
                      "%s: %s\r\n",
                      (const char *)key,
                      (const char *)value
                      );
    }
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
// loop, and reset_connection's axl_tcp_close cancelled the send
// before the body got queued. End-to-end symptom on the wire was
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
on_response_sent(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    HttpConn *conn = (HttpConn *)data;
    (void)sock;

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
        if (conn->upload_resp.body != NULL) {
            axl_free(conn->upload_resp.body);
        }
        if (conn->upload_resp.headers != NULL) {
            axl_hash_table_free(conn->upload_resp.headers);
        }
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
            if (conn->upload_resp.body != NULL) {
                axl_free(conn->upload_resp.body);
            }
            if (conn->upload_resp.headers != NULL) {
                axl_hash_table_free(conn->upload_resp.headers);
            }
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
        axl_warning("stream: producer returned ERR mid-response — abrupt close");
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
                            "remaining of declared Content-Length — "
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
                        "Content-Length — truncating");
        }
    }

    int rc = (conn->tls_ctx != NULL)
           ? axl_tls_write_async(conn->tls_ctx, send_buf, send_len,
                                 conn->server->loop, on_response_sent, conn)
           : axl_tcp_send_async(conn->sock, send_buf, send_len,
                                conn->server->loop, NULL,
                                on_response_sent, conn);
    if (rc != AXL_OK) {
        axl_warning("stream: send_async submit failed — abrupt close");
        end_stream_state(conn);
        reset_connection(conn);
    }
    return AXL_SOURCE_REMOVE;
}

void
send_response(
    HttpConn        *conn,
    AxlHttpResponse *resp
    )
{
    char   header[2048];
    size_t header_len;
    bool   has_streamer = (resp->streamer != NULL);

    header_len = http_build_status_line(header, sizeof(header), resp->status_code);

    if (resp->content_type != NULL) {
        header_len += axl_snprintf(header + header_len, sizeof(header) - header_len,
                 "Content-Type: %s\r\n", resp->content_type);
    }

    if (has_streamer) {
        /* Streaming: Content-Length when total_size is known,
           Transfer-Encoding: chunked when not. */
        if (resp->streamer_total_size != (size_t)-1) {
            header_len += axl_snprintf(header + header_len,
                sizeof(header) - header_len,
                "Content-Length: %llu\r\n",
                (unsigned long long)resp->streamer_total_size);
        } else {
            header_len += axl_snprintf(header + header_len,
                sizeof(header) - header_len,
                "Transfer-Encoding: chunked\r\n");
        }
    } else if (resp->body != NULL && resp->body_size > 0) {
        header_len += axl_snprintf(header + header_len, sizeof(header) - header_len,
                 "Content-Length: %llu\r\n", (unsigned long long)resp->body_size);
    }

    header_len += axl_snprintf(header + header_len, sizeof(header) - header_len,
             "Connection: %s\r\n",
             conn->keep_alive ? "keep-alive" : "close");

    /* Emit any custom response headers */
    if (resp->headers != NULL) {
        HeaderBuildCtx ctx;
        ctx.buf      = header;
        ctx.buf_size = sizeof(header);
        ctx.len      = header_len;

        axl_hash_table_foreach(resp->headers, emit_header, &ctx);
        header_len = ctx.len;
    }

    /* End of headers */
    header_len += axl_snprintf(header + header_len, sizeof(header) - header_len, "\r\n");

    /* Defensive: if a previous response is still in flight on this
       conn, drop its tx_buf. Shouldn't happen — recv is single-armed
       so we don't dispatch a second handler before the first
       completes — but free-then-overwrite keeps the invariant
       trivially correct. */
    if (conn->tx_buf != NULL) {
        axl_warning("send_response: stale tx_buf on conn — freeing");
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
    size_t body_size = (resp->body != NULL) ? resp->body_size : 0;
    size_t total = header_len + body_size;

    void *combined = axl_malloc(total);
    if (combined == NULL) {
        axl_error("send_response: alloc(%zu) failed — abrupt close", total);
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
        axl_warning("send_response: submit failed — abrupt close");
        axl_free(conn->tx_buf);
        conn->tx_buf = NULL;
        reset_connection(conn);
    }
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
