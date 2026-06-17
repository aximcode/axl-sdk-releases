/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-upload.c
    Upload streaming — accumulate body data and flush to handler in chunks.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

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
    if (rc != 0 && conn->upload_resp.status_code == 200) {
        axl_warning("upload handler failed on finalize");
        conn->upload_resp.status_code = 500;
    }

    /* on_response_sent runs the keep-alive vs reset_connection
       decision after the response actually transmits, AND knows
       to release upload-stream state (upload_buf, upload_resp.body
       /headers, is_upload_stream/upload_route). The synchronous
       teardown that lived here previously raced the new async
       send: axl_tcp_close inside reset_connection cancelled the
       just-queued Transmit before TCP4 put it on the wire. */
    (void)s;
    send_response(conn, &conn->upload_resp);
}
