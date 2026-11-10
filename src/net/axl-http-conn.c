/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-conn.c
    Connection I/O dispatcher, keep-alive management, cleanup.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

/* Per-record decrypt scratch size. The drain decrypts into a buffer of
   this size then copies the plaintext back into the phase buffer; the
   decrypt-in-place safety argument (plaintext never overruns un-consumed
   ciphertext) and the no-lossy-clamp guarantee both require it not to
   exceed the phase buffers, so a future bump of one must keep this in step. */
#define TLS_DRAIN_BATCH  4096
_Static_assert(TLS_DRAIN_BATCH <= HTTP_CHUNK_READ_BUF_SIZE
               && TLS_DRAIN_BATCH <= HTTP_HEADER_BUF_SIZE,
               "TLS_DRAIN_BATCH must fit the chunk/header phase buffers");

static void arm_transport_recv(AxlHttpServer *s, HttpConn *conn);
static void dispatch_decrypted(AxlHttpServer *s, HttpConn *conn, size_t bytes);
static void drive_tls_drain(AxlHttpServer *s, HttpConn *conn);

// ---------------------------------------------------------------------------
// Buffer selection — the conn buffer the current read phase targets. Used
// both to stage received ciphertext and to land decrypted plaintext, so the
// two always agree on the destination.
// ---------------------------------------------------------------------------

static void
select_decrypt_buf(
    HttpConn  *conn,
    void     **raw_buf,
    size_t    *buf_avail
    )
{
    if (conn->is_websocket
        || conn->is_upload_stream
        || (conn->chunked && !conn->chunked_done)) {
        /* WebSocket frames, upload streams, and chunked bodies all use the
           shared chunk buffer. This MUST be checked before !headers_done:
           a 101 upgrade clears headers_done (on_response_sent resets the
           request-parse state), so an inbound WS frame would otherwise land
           in header_buf while dispatch_decrypted reads chunk_read_buf — the
           frame silently dropped. is_websocket / is_upload_stream are only
           set after the headers are parsed, so the initial header read still
           falls through to header_buf below. Matches the phase order in
           arm_transport_recv + dispatch_decrypted. */
        *raw_buf   = conn->chunk_read_buf;
        *buf_avail = sizeof(conn->chunk_read_buf);
    } else if (!conn->headers_done) {
        *raw_buf   = conn->header_buf + conn->header_len;
        *buf_avail = sizeof(conn->header_buf) - conn->header_len;
    } else if (conn->body != NULL) {
        *raw_buf   = (char *)conn->body + conn->body_bytes_read;
        *buf_avail = conn->content_length - conn->body_bytes_read;
    } else {
        *raw_buf   = NULL;
        *buf_avail = 0;
    }
}

// ---------------------------------------------------------------------------
// Carried bytes -- what a client pipelined behind a finished request in the
// same segment. Every read phase takes them before it reads the transport,
// so they are parsed as the next request, in order.
// ---------------------------------------------------------------------------

/* PREPEND: every caller keeps bytes out of the buffer the current phase
   was filled from, and those always come BEFORE what is already carried --
   the header buffer past a request's end, the upload read past its length,
   the chunk decoder past its terminator are all filled from the transport
   or from the front of the carry, so anything still carried follows them in
   the stream. The one caller that keeps a TLS batch's tail does so only
   with the carry empty (the drain feeds the carry before decrypting more),
   where prepending and appending agree. Appending put a request's tail
   behind the bytes that follow it (a TLS header split, then a pipelined
   request past the header room). */
void
conn_carry_keep(
    HttpConn   *conn,
    const char *data,
    size_t      n
    )
{
    if (n == 0) {
        return;
    }
    char *grown = axl_realloc(conn->carry, conn->carry_len + n);
    if (grown == NULL) {
        axl_warning("%zu pipelined bytes could not be kept; closing the "
                    "connection after this response", n);
        conn->keep_alive = false;
        return;
    }
    axl_memmove(grown + n, grown, conn->carry_len);
    axl_memcpy(grown, data, n);
    conn->carry      = grown;
    conn->carry_len += n;
}

/* Move up to @p avail carried bytes into @p buf, in order. @return the
   count moved. */
static size_t
carry_take(
    HttpConn *conn,
    void     *buf,
    size_t    avail
    )
{
    size_t n = (conn->carry_len < avail) ? conn->carry_len : avail;
    axl_memcpy(buf, conn->carry, n);
    conn->carry_len -= n;
    if (conn->carry_len == 0) {
        axl_free(conn->carry);
        conn->carry = NULL;
    } else {
        axl_memmove(conn->carry, conn->carry + n, conn->carry_len);
    }
    return n;
}


/* Feed carried bytes to the current read phase as if they had just been
   received. @return false when the phase takes no bytes (its body is
   complete), leaving them for the next request. */
static bool
carry_feed(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    void  *buf   = NULL;
    size_t avail = 0;
    select_decrypt_buf(conn, &buf, &avail);
    if (buf == NULL || avail == 0) {
        return false;
    }
    dispatch_decrypted(s, conn, carry_take(conn, buf, avail));
    return true;
}

// ---------------------------------------------------------------------------
// arm_transport_recv — issue the actual async TCP receive for the current
// read phase. (start_conn_recv wraps this with TLS-drain handling.)
// ---------------------------------------------------------------------------

static void
arm_transport_recv(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    size_t avail;

    /* Bytes a client pipelined come before anything still on the wire. */
    if (conn->carry_len > 0 && carry_feed(s, conn)) {
        return;
    }

    /* TLS: every phase receives ciphertext into the dedicated cipher
       buffer; drive_tls_drain decrypts and routes the plaintext to the
       phase buffer. The one exception is "body already complete / no
       body" — there is nothing to receive, so dispatch now. */
    if (conn->tls_ctx != NULL) {
        bool need_more =
            conn->is_websocket || conn->is_upload_stream
            || !conn->headers_done
            || (conn->chunked && !conn->chunked_done)
            || (conn->content_length > 0 && conn->content_length != SIZE_MAX
                && conn->body_bytes_read < conn->content_length);
        if (!need_more) {
            dispatch_and_respond(s, conn);
            return;
        }
        if (!conn->headers_done
            && HTTP_HEADER_BUF_SIZE - conn->header_len == 0) {
            axl_warning("header too large, dropping connection");
            reset_connection(conn);
            return;
        }
        if (axl_tcp_recv_async(conn->sock,
                               conn->tls_cipher_buf, sizeof(conn->tls_cipher_buf),
                               s->loop, NULL, on_conn_data, conn) != AXL_OK)
        {
            reset_connection(conn);
        }
        return;
    }

    if (conn->is_websocket || conn->is_upload_stream) {
        /* WebSocket / upload stream — recv into chunk_read_buf */
        if (axl_tcp_recv_async(conn->sock,
                               conn->chunk_read_buf, sizeof(conn->chunk_read_buf),
                               s->loop, NULL, on_conn_data, conn) != AXL_OK)
        {
            reset_connection(conn);
        }
        return;
    }

    if (!conn->headers_done) {
        //
        // Still reading headers — recv into header_buf
        //
        avail = HTTP_HEADER_BUF_SIZE - conn->header_len;
        if (avail == 0) {
            axl_warning("header too large, dropping connection");
            reset_connection(conn);
            return;
        }

        if (axl_tcp_recv_async(conn->sock,
                               conn->header_buf + conn->header_len,
                               avail, s->loop, NULL, on_conn_data, conn) != AXL_OK)
        {
            axl_error("recv_start failed for header phase");
            reset_connection(conn);
        }
    } else if (conn->chunked && !conn->chunked_done) {
        //
        // Chunked body — recv into the chunk read buffer
        //
        if (axl_tcp_recv_async(conn->sock,
                               conn->chunk_read_buf, sizeof(conn->chunk_read_buf),
                               s->loop, NULL, on_conn_data, conn) != AXL_OK)
        {
            axl_error("recv_start failed for chunked body");
            reset_connection(conn);
        }
    } else if (conn->content_length > 0 && conn->content_length != SIZE_MAX &&
               conn->body_bytes_read < conn->content_length)
    {
        //
        // Content-Length body — recv directly into the body buffer
        //
        size_t remaining = conn->content_length - conn->body_bytes_read;
        if (axl_tcp_recv_async(conn->sock,
                               (char *)conn->body + conn->body_bytes_read,
                               remaining, s->loop, NULL, on_conn_data, conn) != AXL_OK)
        {
            axl_error("recv_start failed for body");
            reset_connection(conn);
        }
    } else {
        //
        // No body expected or body already complete — dispatch now
        //
        dispatch_and_respond(s, conn);
    }
}

// ---------------------------------------------------------------------------
// start_conn_recv — continue receiving on a connection.
//
// For a TLS connection, a single TCP segment can carry several TLS records.
// If decrypted/decryptable application data is still buffered, draining it
// must take precedence over a transport recv — re-arming the socket then
// would block forever on bytes that have already arrived (the chunked-body
// + Expect: 100-continue upload deadlock). While the drain loop is running,
// this records the re-arm intent instead of issuing it, so the loop owns the
// single transport re-arm once the TLS buffer empties.
// ---------------------------------------------------------------------------

void
start_conn_recv(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    if (conn->tls_draining) {
        conn->tls_rearm_wanted = true;
        return;
    }
    /* Carried bytes first: they arrived before anything TLS still holds. */
    if (conn->carry_len > 0 && carry_feed(s, conn)) {
        return;
    }
    if (conn->tls_ctx != NULL && axl_tls_pending(conn->tls_ctx)) {
        drive_tls_drain(s, conn);
        return;
    }
    arm_transport_recv(s, conn);
}

// ---------------------------------------------------------------------------
// dispatch_decrypted — route one batch of plaintext to the right handler:
// WebSocket frames, upload streaming, or HTTP request parsing.
// ---------------------------------------------------------------------------

static void
dispatch_decrypted(
    AxlHttpServer *s,
    HttpConn      *conn,
    size_t         bytes
    )
{
    /* WebSocket mode — dispatch frames instead of HTTP parsing */
    if (conn->is_websocket) {
        process_websocket_data(s, conn,
            (const uint8_t *)conn->chunk_read_buf, bytes);
        return;
    }

    /* Upload streaming — forward body data to handler */
    if (conn->is_upload_stream) {
        if (conn->chunked) {
            /* Chunked: decode framing via process_chunked_data, which
               calls stream_upload_data for decoded body bytes */
            size_t st = process_chunked_data(s, conn, conn->chunk_read_buf, bytes);
            if (st != 0) {
                stream_upload_fail(conn, st);
            } else if (!conn->chunked_done && conn->active && !conn->upload_ended) {
                start_conn_recv(s, conn);
            }
        } else {
            /* Content-Length: raw data is body bytes -- up to the
               length; what follows it is the next request. */
            size_t want = (conn->content_length > conn->body_bytes_read)
                          ? conn->content_length - conn->body_bytes_read : 0;
            if (bytes > want) {
                conn_carry_keep(conn, conn->chunk_read_buf + want, bytes - want);
                bytes = want;
            }
            conn->body_bytes_read += bytes;
            bool final = (conn->content_length > 0 &&
                          conn->content_length != SIZE_MAX &&
                          conn->body_bytes_read >= conn->content_length);
            stream_upload_data(s, conn, conn->chunk_read_buf, bytes, final);
            if (!final && conn->active) {
                start_conn_recv(s, conn);
            }
        }
        return;
    }

    /* HTTP request parsing — headers, body, dispatch */
    process_request_data(s, conn, bytes);
}

// ---------------------------------------------------------------------------
// drive_tls_drain — decrypt and dispatch every TLS record buffered from the
// current TCP segment, one at a time, then re-arm the transport once.
//
// The dispatch re-arms via start_conn_recv, which — while tls_draining is set
// — only records the intent (tls_rearm_wanted) rather than issuing a recv. So
// this loop, not the dispatch, owns the single transport re-arm, and only
// after the TLS buffer is drained.
// ---------------------------------------------------------------------------

static void
drive_tls_drain(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    conn->tls_draining = true;

    for (;;) {
        void   *raw_buf   = NULL;
        size_t  buf_avail = 0;
        select_decrypt_buf(conn, &raw_buf, &buf_avail);
        if (raw_buf == NULL) {
            /* The request in hand is whole and takes no more bytes: what
               is carried or still buffered is the NEXT request. Leave the
               drain; arm_transport_recv dispatches this one, and the next
               is read after its response (this once reset the connection,
               dropping a pipelined request). */
            break;
        }

        if (conn->carry_len > 0) {
            /* Plaintext an earlier record held past the request it ended:
               it goes before anything decrypted next. */
            conn->tls_rearm_wanted = false;
            dispatch_decrypted(s, conn, carry_take(conn, raw_buf, buf_avail));
            if (!conn->active) {
                return;
            }
            if (!conn->tls_rearm_wanted) {
                conn->tls_draining = false;
                return;
            }
            continue;
        }

        uint8_t plain_buf[TLS_DRAIN_BATCH];
        size_t  plain_len = 0;
        AxlTlsStatus rc = axl_tls_read(conn->tls_ctx, plain_buf, sizeof(plain_buf),
                              &plain_len);
        if (rc == AXL_TLS_WANT_MORE) {
            /* WANT_READ: the staged ciphertext is an incomplete record;
               we need more from the transport. mbedTLS has copied what it
               consumed into its own buffer, so re-arming (which overwrites
               the staging buffer) is safe — it reassembles across reads. */
            break;
        }
        if (rc == AXL_TLS_ERR || plain_len == 0) {
            conn->tls_draining = false;
            reset_connection(conn);
            return;
        }

        /* Land plaintext in the phase buffer, clamped to its space; what
           does not fit (the next pipelined request, or the rest of a
           record longer than the phase wants) is carried, not dropped. */
        size_t copy_len = plain_len > buf_avail ? buf_avail : plain_len;
        axl_memcpy(raw_buf, plain_buf, copy_len);
        conn_carry_keep(conn, (const char *)plain_buf + copy_len, plain_len - copy_len);

        conn->tls_rearm_wanted = false;
        dispatch_decrypted(s, conn, copy_len);

        if (!conn->active) {
            /* Connection completed or reset inside the dispatch. */
            return;
        }
        if (!conn->tls_rearm_wanted) {
            /* The dispatch finished the request (response sent) without
               asking for more body — stop this drain. on_response_sent
               owns the next read after the keep-alive reset (and re-enters
               the drain then if anything is still buffered). */
            conn->tls_draining = false;
            return;
        }
        if (!axl_tls_pending(conn->tls_ctx) && conn->carry_len == 0) {
            /* Nothing more buffered — re-arm the transport for the rest. */
            break;
        }
    }

    conn->tls_draining = false;
    arm_transport_recv(s, conn);
}

// ---------------------------------------------------------------------------
// on_conn_data — async receive callback
//
// Decrypts (for TLS) and dispatches to the appropriate handler: WebSocket,
// upload streaming, or HTTP request parsing.
// ---------------------------------------------------------------------------

static bool
on_conn_data_body(
    AxlTcp    *sock,
    AxlStatus  status,
    void      *data
    );

bool
on_conn_data(
    AxlTcp    *sock,
    AxlStatus  status,
    void      *data
    )
{
    /* @p data is always a live pool connection here. */
    return http_dispatch(((HttpConn *)data)->server, on_conn_data_body, sock,
                         status, data);
}

static bool
on_conn_data_body(
    AxlTcp    *sock,
    AxlStatus  status,
    void      *data
    )
{
    HttpConn       *conn = (HttpConn *)data;
    AxlHttpServer  *s    = conn->server;
    size_t          bytes;

    if (status != AXL_OK) {
        reset_connection(conn);
        return false;
    }

    bytes = axl_tcp_recv_get_size(sock);
    if (bytes == 0) {
        //
        // Peer closed the connection
        //
        reset_connection(conn);
        return false;
    }

    //
    // TLS: ciphertext was received into the dedicated cipher buffer (kept
    // separate from every plaintext buffer, so decrypting one record's
    // plaintext can't clobber a following record's not-yet-consumed
    // ciphertext that shared the TCP read). Stage it and drain every
    // record it yields — a single TCP read can carry several records, and
    // a body spans arbitrarily many records across many reads.
    //
    if (conn->tls_ctx != NULL) {
        axl_tls_stage_data(conn->tls_ctx, conn->tls_cipher_buf, bytes);
        drive_tls_drain(s, conn);
        return false;
    }

    dispatch_decrypted(s, conn, bytes);
    return false;
}

// ---------------------------------------------------------------------------
// dispatch_and_respond — dispatch request and handle keep-alive
// ---------------------------------------------------------------------------

void
dispatch_and_respond(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    dispatch_request(s, conn);

    /* Post-send work — keep-alive vs close — runs from
       on_response_sent (axl-http-response.c) when the async send
       completes. The websocket and upload-stream branches sequence
       their own follow-up sends and don't go through send_response,
       so dispatch_request already returned them in their final
       state. Nothing left for this function to do. */
    (void)s;
    (void)conn;
}

// ---------------------------------------------------------------------------
// Connection cleanup
// ---------------------------------------------------------------------------

static void
do_reset_connection(HttpConn *conn, bool abortive)
{
    /* Re-entry guard: a DISCONNECT / upload-abort handler fired below may
       itself reach reset_connection (e.g. axl_ws_conn_close). active stays
       true until the closing memset, so guard on a dedicated teardown flag
       to make this idempotent — the outer call owns the single teardown. */
    if (!conn->active || conn->tearing_down) {
        return;
    }
    /* NOT WHILE THE APP'S CONNECT HANDLER IS ON THE STACK. It may close its
       own connection (axl_ws_conn_close), or a send it makes may fail; either
       reaches here. Tearing down now would fire DISCONNECT nested inside the
       CONNECT that is still running, and free per-connection state its
       caller -- on_response_sent -- still reads. Record it instead; the
       CONNECT delivery runs it once the handler has returned. */
    if (conn->ws_in_connect) {
        conn->ws_close_deferred  = true;
        conn->ws_close_abortive |= abortive;
        return;
    }
    conn->tearing_down = true;

    /* Take the in-flight response buffers. From here on this function owns
       every piece of per-response state. axl_tcp_close below delivers each
       send callback it still owes before it returns -- AXL_CANCELLED for a
       send still pending, the REAL status (usually AXL_OK) for one that had
       already finished -- and on_response_sent (which every response and
       stream-chunk send completes through) and ws_out_on_sent return at once
       when they see tearing_down. They must: an AXL_OK 101 completion
       acted on here would fire the app's AXL_WS_CONNECT after the
       DISCONNECT decision below was taken.

       Both buffers may still be LENT to axl_tcp_send_async, whose contract is
       "valid until the callback" -- and those callbacks run inside the close
       below. So they are detached now and freed only after it, with the
       app's DISCONNECT / upload-abort handlers running in between. */
    void *lent_tx_buf    = conn->tx_buf;
    void *lent_chunk_buf = conn->stream_chunk_buf;
    conn->tx_buf           = NULL;
    conn->stream_chunk_buf = NULL;
    conn->stream_chunk_cap = 0;

    /* Streaming response state: same race as tx_buf — the producer
       owns ctx (file handle, generated-data state), and our cleanup
       hook is the only thing that can release it cleanly when the
       client disconnects mid-stream. end_stream_state is no-op when
       no stream is active, so always safe to call. */
    end_stream_state(conn);

    /* Fire WebSocket DISCONNECT while transport is still open — via the
       per-connection handler (_ex) if registered, else the broadcast one.
       Gated on ws_connected (CONNECT delivered AND accepted), NOT is_websocket:
       a connection torn down before CONNECT fired (a 101 send that failed) or
       one whose CONNECT was rejected must NOT get an unpaired DISCONNECT. */
    if (conn->ws_connected && conn->ws_conn_handler != NULL) {
        conn->ws_conn_handler((AxlWsConn *)conn, AXL_WS_DISCONNECT, NULL, 0,
                              conn->ws_data);
    } else if (conn->ws_connected && conn->ws_handler != NULL) {
        conn->ws_handler(AXL_WS_DISCONNECT, NULL, 0, conn->ws_data);
    }

    /* Fire upload-handler abort signal while transport is still open
       and the handler's req/data are still valid. The handler
       accumulated per-request state (open file handles, partial
       writes) across earlier chunk calls — without this notification
       that state hangs into the next request and corrupts it. Mirror
       of the WebSocket DISCONNECT just above. Clear is_upload_stream
       immediately so on_response_sent's upload-stream cleanup path
       (which would normally fire after a clean response) doesn't also
       run — we own the teardown from here. */
    if (conn->is_upload_stream && !conn->upload_ended &&
        conn->upload_route != NULL &&
        conn->upload_route->upload_handler != NULL) {
        conn->upload_route->upload_handler(
            &conn->upload_req, &conn->upload_resp,
            NULL, 0, conn->upload_route->data, true);
        conn->is_upload_stream = false;
        conn->upload_route     = NULL;
    }

    if (conn->tls_ctx != NULL) {
        axl_tls_free(conn->tls_ctx);
        conn->tls_ctx = NULL;
    }

    if (abortive) {
        axl_tcp_close(conn->sock, AXL_TEARDOWN_RESET);
    } else {
        axl_tcp_close(conn->sock, AXL_TEARDOWN_GRACEFUL);
    }
    /* Every send that borrowed them has had its callback. */
    axl_free(lent_tx_buf);
    axl_free(lent_chunk_buf);

    axl_free(conn->method);
    axl_free(conn->path);
    axl_free(conn->query);

    if (conn->headers != NULL) {
        axl_hash_table_free(conn->headers);
    }

    if (conn->body != NULL) {
        axl_free(conn->body);
    }
    axl_free(conn->carry);   /* a closed connection has no next request */

    /* Upload streaming cleanup */
    axl_free(conn->upload_buf);
    http_response_release(&conn->upload_resp);   /* a static body is the handler's */

    /* WebSocket cleanup. ws_outq_clear frees any queued outbound frames; the
       in-flight head's send was retired by axl_tcp_close above (its callback
       delivered and ignored). The firmware only ever held AxlTcp's own copy
       of its bytes, never the frame buffer, so once that callback has run
       nothing references it and it is safe to free here. */
    axl_free(conn->ws_path);
    axl_free(conn->ws_partial_buf);
    ws_outq_clear(conn);

    //
    // Preserve the server back-pointer across reset
    //
    AxlHttpServer *saved_server = conn->server;
    axl_memset(conn, 0, sizeof(*conn));
    conn->server = saved_server;
}

void
reset_connection(HttpConn *conn)
{
    do_reset_connection(conn, false);
}

void
reset_connection_abortive(HttpConn *conn)
{
    do_reset_connection(conn, true);
}
