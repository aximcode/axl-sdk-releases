/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-request.c
    HTTP request parsing: headers, body accumulation, chunked encoding,
    early upload route detection.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Body buffer growth
// ---------------------------------------------------------------------------

void *
grow_buffer(
    void   *old_buf,
    size_t  old_size,
    size_t  new_size
    )
{
    void *new_buf = axl_malloc(new_size);
    if (new_buf == NULL) {
        axl_warning("body buffer grow failed: %zu bytes", new_size);
        return NULL;
    }

    if (old_buf != NULL && old_size > 0) {
        axl_memcpy(new_buf, old_buf, old_size);
        axl_free(old_buf);
    }

    return new_buf;
}

// ---------------------------------------------------------------------------
// Chunked transfer encoding
// ---------------------------------------------------------------------------

void
process_chunked_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    const char    *buf,
    size_t         buf_len
    )
{
    size_t pos = 0;

    while (pos < buf_len) {
        //
        // Find chunk size line (hex digits followed by \r\n)
        //
        size_t line_start = pos;
        size_t line_end = pos;
        bool   found_crlf = false;

        while (line_end + 1 < buf_len) {
            if (buf[line_end] == '\r' && buf[line_end + 1] == '\n') {
                found_crlf = true;
                break;
            }

            line_end++;
        }

        if (!found_crlf) {
            //
            // Incomplete chunk header — save the leftover bytes so they're
            // prepended to the next recv's data.
            //
            size_t left = buf_len - line_start;
            if (left > 0 && left < sizeof(conn->chunk_leftover)) {
                axl_memcpy(conn->chunk_leftover, buf + line_start, left);
                conn->chunk_leftover_len = left;
            }
            break;
        }

        //
        // Parse hex chunk size. axl_hex_parse_u64 stops at the first
        // non-hex byte, which covers both chunk-extensions (`;...`)
        // and an empty line — both are treated as end-of-size here.
        //
        uint64_t parsed_size = 0;
        size_t   chunk_size  = 0;
        if (axl_hex_parse_u64(buf + line_start,
                              line_end - line_start,
                              &parsed_size) >= 0) {
            chunk_size = (size_t)parsed_size;
        }

        pos = line_end + 2;  // skip \r\n after size line

        if (chunk_size == 0) {
            //
            // Terminal chunk — done
            //
            conn->chunked_done = true;
            conn->content_length = conn->body_bytes_read;
            if (conn->is_upload_stream) {
                stream_upload_data(s, conn, NULL, 0, true);
            }
            return;
        }

        //
        // Decoded chunk data available in buf starting at pos
        //
        size_t available = buf_len - pos;
        size_t copy_size = (available < chunk_size) ? available : chunk_size;

        if (conn->is_upload_stream) {
            //
            // Upload streaming — pass decoded chunk data directly
            //
            if (copy_size > 0) {
                stream_upload_data(s, conn, buf + pos, copy_size, false);
                if (!conn->active) {
                    return;  /* handler aborted */
                }
                conn->body_bytes_read += copy_size;
                pos += copy_size;
            }
        } else {
            //
            // Normal buffering path
            //
            if (conn->body_bytes_read + chunk_size > s->body_limit) {
                conn->chunked_done = true;
                conn->content_length = conn->body_bytes_read;
                return;
            }

            size_t needed = conn->body_bytes_read + chunk_size;
            if (needed > conn->body_alloc) {
                size_t new_alloc = conn->body_alloc;
                if (new_alloc == 0) {
                    new_alloc = 4096;
                }

                while (new_alloc < needed) {
                    new_alloc *= 2;
                }

                conn->body = grow_buffer(conn->body, conn->body_bytes_read, new_alloc);
                if (conn->body == NULL) {
                    conn->chunked_done = true;
                    return;
                }

                conn->body_alloc = new_alloc;
            }

            if (copy_size > 0) {
                axl_memcpy((uint8_t *)conn->body + conn->body_bytes_read, buf + pos, copy_size);
                conn->body_bytes_read += copy_size;
                pos += copy_size;
            }
        }

        //
        // If we didn't get the full chunk in this buffer, we can't read
        // more synchronously in the async model. Mark as not done and
        // let the next recv callback continue.
        //
        if (copy_size < chunk_size) {
            break;
        }

        //
        // Skip trailing \r\n after chunk data
        //
        if (pos + 1 < buf_len && buf[pos] == '\r' && buf[pos + 1] == '\n') {
            pos += 2;
        }
    }
}

// ---------------------------------------------------------------------------
// process_request_data — header parsing, body accumulation, upload detection
//
// Called from on_conn_data after TLS decryption and WS/upload dispatch.
// Handles the full lifecycle from header accumulation through body
// completion and dispatch.
// ---------------------------------------------------------------------------

void
process_request_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    size_t         bytes
    )
{
    if (!conn->headers_done) {
        //
        // Accumulate header data
        //
        conn->header_len += bytes;

        //
        // Look for \r\n\r\n to mark end of headers
        //
        size_t header_end = axl_http_find_header_end(conn->header_buf, conn->header_len);
        if (header_end == 0) {
            //
            // Need more header data
            //
            start_conn_recv(s, conn);
            return;
        }

        //
        // Parse request line
        //
        size_t first_line_end = 0;
        for (size_t j = 0; j + 1 < header_end; j++) {
            if (conn->header_buf[j] == '\r' && conn->header_buf[j + 1] == '\n') {
                first_line_end = j;
                break;
            }
        }

        int parse_status;
        parse_status = axl_http_parse_request_line(
                        conn->header_buf,
                        first_line_end,
                        &conn->method,
                        &conn->path,
                        &conn->query
                        );
        if (parse_status != 0) {
            send_error_response(conn, 400);
            reset_connection(conn);
            return;
        }

        //
        // Parse headers
        //
        size_t header_start = first_line_end + 2;
        parse_status = axl_http_parse_headers(
                        conn->header_buf + header_start,
                        header_end - header_start,
                        &conn->headers
                        );
        if (parse_status != 0) {
            send_error_response(conn, 400);
            reset_connection(conn);
            return;
        }

        conn->content_length = axl_http_get_content_length(conn->headers);
        conn->body_bytes_read = 0;
        conn->body = NULL;
        conn->body_alloc = 0;
        conn->chunked = false;
        conn->chunked_done = false;

        //
        // Check for chunked transfer encoding
        //
        const char *te_header = (const char *)axl_hash_table_lookup(conn->headers, "transfer-encoding");
        if (te_header != NULL && axl_strcasecmp(te_header, "chunked") == 0) {
            conn->chunked = true;
            conn->content_length = 0;
        }

        //
        // Check keep-alive
        //
        conn->keep_alive = (s->keep_alive_sec > 0);
        if (conn->keep_alive) {
            const char *conn_header = (const char *)axl_hash_table_lookup(conn->headers, "connection");
            if (conn_header != NULL && axl_strcasecmp(conn_header, "close") == 0) {
                conn->keep_alive = false;
            }
        }

        conn->headers_done = true;

        //
        // Early route lookup — detect upload routes before body buffering
        //
        size_t body_in_header = conn->header_len - header_end;
        HttpRoute *early_route = find_route(s, conn->method, conn->path);

        if (early_route != NULL && early_route->is_upload &&
            early_route->upload_handler != NULL) {
            //
            // Upload streaming — don't buffer the body, stream chunks
            // to the handler as they arrive.
            //
            conn->is_upload_stream = true;
            conn->upload_route     = early_route;
            axl_debug("upload stream started for %s %s",
                      conn->method, conn->path);

            conn->upload_buf = axl_malloc(s->upload_chunk_size);
            if (conn->upload_buf == NULL) {
                send_error_response(conn, 500);
                reset_connection(conn);
                return;
            }
            conn->upload_buf_len = 0;

            /* Build request and response structs that persist across chunks */
            axl_memset(&conn->upload_req, 0, sizeof(conn->upload_req));
            conn->upload_req.method    = conn->method;
            conn->upload_req.path      = conn->path;
            conn->upload_req.query     = conn->query;
            conn->upload_req.headers   = conn->headers;
            conn->upload_req.body      = NULL;
            conn->upload_req.body_size = 0;
            axl_memcpy(conn->upload_req.client_addr, conn->client_addr,
                       sizeof(conn->upload_req.client_addr));

            axl_memset(&conn->upload_resp, 0, sizeof(conn->upload_resp));
            conn->upload_resp.status_code = 200;

            /* Seed with any body data already in the header buffer */
            if (body_in_header > 0) {
                bool no_body = (conn->content_length == 0 ||
                                conn->content_length == SIZE_MAX) &&
                               !conn->chunked;
                bool final = no_body ||
                             (!conn->chunked &&
                              body_in_header >= conn->content_length);
                conn->body_bytes_read = body_in_header;
                stream_upload_data(s, conn,
                    conn->header_buf + header_end, body_in_header, final);
                if (final || !conn->active) {
                    return;
                }
            }

            /* No body expected at all? Send final immediately */
            if ((conn->content_length == 0 || conn->content_length == SIZE_MAX) &&
                !conn->chunked && body_in_header == 0) {
                stream_upload_data(s, conn, NULL, 0, true);
                return;
            }

            start_conn_recv(s, conn);
            return;
        }

        //
        // Normal (non-upload) body buffering
        //
        if (conn->chunked) {
            //
            // For chunked: seed body with leftover header data and process it
            //
            if (body_in_header > 0) {
                process_chunked_data(s, conn, conn->header_buf + header_end, body_in_header);
                if (conn->chunked_done) {
                    dispatch_and_respond(s, conn);
                    return;
                }
            }
        } else if (conn->content_length > 0 && conn->content_length != SIZE_MAX) {
            //
            // Content-Length body
            //
            if (conn->content_length > s->body_limit) {
                send_error_response(conn, 413);
                reset_connection(conn);
                return;
            }

            conn->body = axl_malloc(conn->content_length);
            conn->body_alloc = conn->content_length;
            if (conn->body == NULL) {
                send_error_response(conn, 500);
                reset_connection(conn);
                return;
            }

            if (body_in_header > 0) {
                if (body_in_header > conn->content_length) {
                    body_in_header = conn->content_length;
                }

                axl_memcpy(conn->body, conn->header_buf + header_end, body_in_header);
                conn->body_bytes_read = body_in_header;
            }
        }

        //
        // Continue to body read or dispatch
        //
        start_conn_recv(s, conn);
    } else if (conn->chunked && !conn->chunked_done) {
        //
        // Process chunked data that arrived in chunk_read_buf.
        // If there's leftover from a previous partial chunk header,
        // prepend it.
        //
        if (conn->chunk_leftover_len > 0) {
            char   tmp_buf[HTTP_CHUNK_READ_BUF_SIZE + 16];
            size_t tmp_len = conn->chunk_leftover_len + bytes;
            if (tmp_len <= sizeof(tmp_buf)) {
                axl_memcpy(tmp_buf, conn->chunk_leftover, conn->chunk_leftover_len);
                axl_memcpy(tmp_buf + conn->chunk_leftover_len, conn->chunk_read_buf, bytes);
                conn->chunk_leftover_len = 0;
                process_chunked_data(s, conn, tmp_buf, tmp_len);
            } else {
                conn->chunk_leftover_len = 0;
                process_chunked_data(s, conn, conn->chunk_read_buf, bytes);
            }
        } else {
            process_chunked_data(s, conn, conn->chunk_read_buf, bytes);
        }
        if (!conn->chunked_done) {
            start_conn_recv(s, conn);
        } else {
            dispatch_and_respond(s, conn);
        }
    } else {
        //
        // Content-Length body — data arrived directly into body buffer
        //
        conn->body_bytes_read += bytes;
        if (conn->body_bytes_read < conn->content_length) {
            start_conn_recv(s, conn);
        } else {
            dispatch_and_respond(s, conn);
        }
    }
}
