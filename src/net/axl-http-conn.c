/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-conn.c
    Connection I/O dispatcher, keep-alive management, cleanup.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// start_conn_recv — initiate an async receive on a connection
// ---------------------------------------------------------------------------

void
start_conn_recv(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    size_t avail;

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
// on_conn_data — async receive callback
//
// Handles TLS decryption and dispatches to the appropriate handler:
// WebSocket, upload streaming, or HTTP request parsing.
// ---------------------------------------------------------------------------

bool
on_conn_data(
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
    // TLS decryption: if this connection has TLS, decrypt the raw TCP
    // data and write the plaintext into the same buffer location.
    //
    if (conn->tls_ctx != NULL) {
        /* Figure out where the raw data was placed */
        void *raw_buf = NULL;
        size_t buf_avail = 0;

        if (!conn->headers_done) {
            raw_buf = conn->header_buf + conn->header_len;
            buf_avail = sizeof(conn->header_buf) - conn->header_len;
        } else if ((conn->chunked && !conn->chunked_done)
                   || conn->is_websocket
                   || conn->is_upload_stream) {
            /* Chunked bodies, WebSocket frames, and upload streams
               all decrypt into the shared chunk buffer. */
            raw_buf = conn->chunk_read_buf;
            buf_avail = sizeof(conn->chunk_read_buf);
        } else if (conn->body != NULL) {
            raw_buf = (char *)conn->body + conn->body_bytes_read;
            buf_avail = conn->content_length - conn->body_bytes_read;
        }

        if (raw_buf == NULL) {
            reset_connection(conn);
            return false;
        }

        /* Stage the TLS record data and decrypt */
        axl_tls_stage_data(conn->tls_ctx, raw_buf, bytes);

        uint8_t plain_buf[4096];
        size_t  plain_len = 0;
        int rc = axl_tls_read(conn->tls_ctx, plain_buf, sizeof(plain_buf), &plain_len);
        if (rc < 0 || plain_len == 0) {
            reset_connection(conn);
            return false;
        }

        /* Copy plaintext back, clamped to available buffer space */
        size_t copy_len = plain_len;
        if (copy_len > buf_avail) {
            copy_len = buf_avail;
        }
        axl_memcpy(raw_buf, plain_buf, copy_len);
        bytes = copy_len;
    }

    /* WebSocket mode — dispatch frames instead of HTTP parsing */
    if (conn->is_websocket) {
        process_websocket_data(s, conn,
            (const uint8_t *)conn->chunk_read_buf, bytes);
        return false;
    }

    /* Upload streaming — forward body data to handler */
    if (conn->is_upload_stream) {
        if (conn->chunked) {
            /* Chunked: decode framing via process_chunked_data, which
               calls stream_upload_data for decoded body bytes */
            process_chunked_data(s, conn, conn->chunk_read_buf, bytes);
            if (!conn->chunked_done && conn->active) {
                start_conn_recv(s, conn);
            }
        } else {
            /* Content-Length: raw data is body bytes */
            conn->body_bytes_read += bytes;
            bool final = (conn->content_length > 0 &&
                          conn->content_length != SIZE_MAX &&
                          conn->body_bytes_read >= conn->content_length);
            stream_upload_data(s, conn, conn->chunk_read_buf, bytes, final);
            if (!final && conn->active) {
                start_conn_recv(s, conn);
            }
        }
        return false;
    }

    /* HTTP request parsing — headers, body, dispatch */
    process_request_data(s, conn, bytes);
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

void
reset_connection(HttpConn *conn)
{
    if (!conn->active) {
        return;
    }

    /* Free any in-flight response buffer. axl_tcp_close below will
       cancel an outstanding send (and that cancel removes the
       send_source from the loop), so on_response_sent won't fire to
       free this for us. Safety-net here avoids the leak. */
    if (conn->tx_buf != NULL) {
        axl_free(conn->tx_buf);
        conn->tx_buf = NULL;
    }

    /* Streaming response state: same race as tx_buf — the producer
       owns ctx (file handle, generated-data state), and our cleanup
       hook is the only thing that can release it cleanly when the
       client disconnects mid-stream. end_stream_state is no-op when
       no stream is active, so always safe to call. */
    end_stream_state(conn);

    /* Fire WebSocket DISCONNECT while transport is still open */
    if (conn->is_websocket && conn->ws_handler != NULL) {
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
    if (conn->is_upload_stream && conn->upload_route != NULL &&
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

    axl_tcp_close(conn->sock);

    axl_free(conn->method);
    axl_free(conn->path);
    axl_free(conn->query);

    if (conn->headers != NULL) {
        axl_hash_table_free(conn->headers);
    }

    if (conn->body != NULL) {
        axl_free(conn->body);
    }

    /* Upload streaming cleanup */
    axl_free(conn->upload_buf);
    if (conn->upload_resp.body != NULL) {
        axl_free(conn->upload_resp.body);
    }
    if (conn->upload_resp.headers != NULL) {
        axl_hash_table_free(conn->upload_resp.headers);
    }

    /* WebSocket cleanup */
    axl_free(conn->ws_path);
    axl_free(conn->ws_partial_buf);

    //
    // Preserve the server back-pointer across reset
    //
    AxlHttpServer *saved_server = conn->server;
    axl_memset(conn, 0, sizeof(*conn));
    conn->server = saved_server;
}
