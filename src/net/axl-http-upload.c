/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-upload.c
    Upload streaming — accumulate body data and flush to handler in chunks.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

void
stream_upload_fail(
    HttpConn *conn,
    size_t    status
    )
{
    HttpRoute *route = conn->upload_route;
    /* The status is set BEFORE the abort call and the response is sent
       after it: an abort the server makes itself is answered, and the
       handler may add a body saying what the abort left (a write in place
       left partial). */
    conn->upload_resp.status_code = status;
    if (!conn->upload_ended && route != NULL && route->upload_handler != NULL) {
        conn->upload_ended = true;
        int rc = route->upload_handler(&conn->upload_req, &conn->upload_resp,
                                       NULL, 0, route->data, true);
        if (rc != 0) {
            axl_debug("upload handler abort call returned %d", rc);
        }
    }
    axl_warning("upload %s %s: the request body could not be read (%zu); "
                "the upload was aborted", conn->method, conn->path, status);
    conn->upload_resp.status_code = status;   /* whatever the handler did to it */
    conn->keep_alive = false;
    send_response(conn, &conn->upload_resp);
}

void
stream_upload_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    const void    *data,
    size_t         len,
    bool           is_final
    )
{
    HttpRoute *route = conn->upload_route;

    /* The handler has had its terminating call (it failed a chunk, or took
       the clean EOF): the rest of the body is discarded. Its response is
       already queued with close-after-send, and recv keeps draining the
       peer's bytes until that close; without this they reached the handler
       as further chunk calls -- and at the end as a clean-EOF call that
       queued a second response behind the first. */
    if (conn->upload_ended) {
        return;
    }

    /* Append incoming data to the accumulation buffer */
    while (len > 0) {
        size_t space = s->upload_chunk_size - conn->upload_buf_len;
        size_t copy  = (len < space) ? len : space;

        axl_memcpy(conn->upload_buf + conn->upload_buf_len, data, copy);
        conn->upload_buf_len += copy;
        data = (const uint8_t *)data + copy;
        len -= copy;

        /* Flush when buffer is full */
        if (conn->upload_buf_len >= s->upload_chunk_size) {
            axl_debug("upload chunk flush: %zu bytes", conn->upload_buf_len);
            int rc = route->upload_handler(
                &conn->upload_req, &conn->upload_resp,
                conn->upload_buf, conn->upload_buf_len,
                route->data, false);
            conn->upload_buf_len = 0;
            if (rc != 0) {
                /* The handler ended the upload itself: that return is its
                   terminating call, so no abort call may follow it. */
                conn->upload_ended = true;
                axl_warning("upload handler aborted during chunk flush");
                /* Respect a status the handler set before aborting (e.g.
                   405 for a PUT to a read-only mount); default to 500
                   only if it left the response untouched. Matches the
                   clean-EOF path below. */
                if (conn->upload_resp.status_code == 200) {
                    conn->upload_resp.status_code = 500;
                }
                /* Force close-after-send. on_response_sent will run
                   reset_connection AFTER the status hits the wire. */
                conn->keep_alive = false;
                send_response(conn, &conn->upload_resp);
                return;
            }
        }
    }

    if (!is_final) {
        return;
    }

    /* Flush remaining data */
    if (conn->upload_buf_len > 0) {
        axl_debug("upload final flush: %zu bytes", conn->upload_buf_len);
        int rc = route->upload_handler(
            &conn->upload_req, &conn->upload_resp,
            conn->upload_buf, conn->upload_buf_len,
            route->data, false);
        conn->upload_buf_len = 0;
        if (rc != 0) {
            conn->upload_ended = true;   /* terminal, as in the chunk flush */
            axl_warning("upload handler aborted during final flush");
            if (conn->upload_resp.status_code == 200) {
                conn->upload_resp.status_code = 500;
            }
            conn->keep_alive = false;
            send_response(conn, &conn->upload_resp);
            return;
        }
    }

    /* Clean-EOF final call: chunk=NULL, chunk_size=0, aborted=false —
       handler sets response. */
    int rc = route->upload_handler(
        &conn->upload_req, &conn->upload_resp,
        NULL, 0, route->data, false);
    /* That was this upload's terminating call. AxlUploadHandler promises the
       clean-EOF and abort calls are mutually exclusive, so a teardown before
       the response's completion runs (do_reset_connection) must not add an
       abort call to it. */
    conn->upload_ended = true;
    if (rc != 0 && conn->upload_resp.status_code == 200) {
        axl_warning("upload handler failed on finalize");
        conn->upload_resp.status_code = 500;
    }

    /* on_response_sent runs the keep-alive vs reset_connection
       decision after the response actually transmits, AND knows
       to release upload-stream state (upload_buf, upload_resp.body
       /headers, is_upload_stream/upload_route). The synchronous
       teardown that lived here previously raced the new async
       send: axl_tcp_close inside reset_connection dropped the
       just-queued send before TCP had taken it (and reset the
       connection to say so). */
    (void)s;
    send_response(conn, &conn->upload_resp);
}
