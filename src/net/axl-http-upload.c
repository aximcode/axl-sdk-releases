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
                route->data);
            conn->upload_buf_len = 0;
            if (rc != 0) {
                axl_warning("upload handler aborted during chunk flush");
                conn->upload_resp.status_code = 500;
                send_response(conn, &conn->upload_resp);
                reset_connection(conn);
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
            route->data);
        conn->upload_buf_len = 0;
        if (rc != 0) {
            axl_warning("upload handler aborted during final flush");
            conn->upload_resp.status_code = 500;
            send_response(conn, &conn->upload_resp);
            reset_connection(conn);
            return;
        }
    }

    /* Final call: chunk=NULL, chunk_size=0 — handler sets response */
    int rc = route->upload_handler(
        &conn->upload_req, &conn->upload_resp,
        NULL, 0, route->data);
    if (rc != 0 && conn->upload_resp.status_code == 200) {
        axl_warning("upload handler failed on finalize");
        conn->upload_resp.status_code = 500;
    }

    send_response(conn, &conn->upload_resp);

    if (conn->keep_alive) {
        /* Reset upload state */
        axl_free(conn->upload_buf);
        conn->upload_buf = NULL;
        conn->upload_buf_len = 0;
        conn->is_upload_stream = false;
        conn->upload_route = NULL;
        if (conn->upload_resp.body != NULL) {
            axl_free(conn->upload_resp.body);
        }
        if (conn->upload_resp.headers != NULL) {
            axl_hash_table_free(conn->upload_resp.headers);
        }
        axl_memset(&conn->upload_resp, 0, sizeof(conn->upload_resp));

        /* Reset request state for next request (same as dispatch_and_respond) */
        axl_free(conn->method);
        conn->method = NULL;
        axl_free(conn->path);
        conn->path = NULL;
        axl_free(conn->query);
        conn->query = NULL;
        if (conn->headers != NULL) {
            axl_hash_table_free(conn->headers);
            conn->headers = NULL;
        }
        conn->header_len      = 0;
        conn->headers_done    = false;
        conn->content_length  = 0;
        conn->body_bytes_read = 0;
        conn->body_alloc      = 0;
        conn->chunked         = false;
        conn->chunked_done    = false;

        start_conn_recv(s, conn);
    } else {
        reset_connection(conn);
    }
}
