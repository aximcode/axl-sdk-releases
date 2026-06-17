/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-request.c
    HTTP request parsing: headers, body accumulation, chunked encoding,
    early upload route detection.
**/

#include "axl-http-server-internal.h"
#include <axl/axl-json.h>

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
        // Skip the trailing CRLF that follows a chunk's data. It can span
        // recvs (the \r at the end of one buffer, the \n at the start of
        // the next), so it is tracked as a 0..2 byte countdown.
        //
        while (conn->chunk_crlf_pending > 0 && pos < buf_len) {
            pos++;
            conn->chunk_crlf_pending--;
        }
        if (conn->chunk_crlf_pending > 0) {
            return;  /* need the rest of the CRLF from the next recv */
        }

        //
        // Consume the in-progress chunk's data. A single chunk can be far
        // larger than one recv buffer, so chunk_remaining carries the count
        // still owed across recvs — without it, a continuation buffer would
        // be misparsed as a new chunk size-line.
        //
        if (conn->chunk_remaining > 0) {
            size_t avail = buf_len - pos;
            size_t take  = (avail < conn->chunk_remaining)
                           ? avail : conn->chunk_remaining;

            if (conn->is_upload_stream) {
                stream_upload_data(s, conn, buf + pos, take, false);
                if (!conn->active) {
                    return;  /* handler aborted */
                }
                conn->body_bytes_read += take;
            } else {
                if (conn->body_bytes_read + take > s->body_limit) {
                    conn->chunked_done = true;
                    conn->content_length = conn->body_bytes_read;
                    return;
                }
                size_t needed = conn->body_bytes_read + take;
                if (needed > conn->body_alloc) {
                    size_t new_alloc = conn->body_alloc ? conn->body_alloc : 4096;
                    while (new_alloc < needed) {
                        new_alloc *= 2;
                    }
                    conn->body = grow_buffer(conn->body, conn->body_bytes_read,
                                             new_alloc);
                    if (conn->body == NULL) {
                        conn->chunked_done = true;
                        return;
                    }
                    conn->body_alloc = new_alloc;
                }
                axl_memcpy((uint8_t *)conn->body + conn->body_bytes_read,
                           buf + pos, take);
                conn->body_bytes_read += take;
            }

            pos += take;
            conn->chunk_remaining -= take;
            if (conn->chunk_remaining > 0) {
                return;  /* more data owed; resume on the next recv */
            }
            conn->chunk_crlf_pending = 2;  /* skip the trailing \r\n */
            continue;
        }

        //
        // Parse the next chunk size-line (hex digits up to \r\n).
        //
        size_t line_end   = pos;
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
            // Incomplete size-line — save it so it's prepended to the next
            // recv's data (see process_request_data's chunk_leftover path).
            //
            size_t left = buf_len - pos;
            if (left > 0 && left < sizeof(conn->chunk_leftover)) {
                axl_memcpy(conn->chunk_leftover, buf + pos, left);
                conn->chunk_leftover_len = left;
            }
            break;
        }

        //
        // axl_hex_parse_u64 stops at the first non-hex byte, covering
        // chunk-extensions (`;...`) and an empty line.
        //
        uint64_t parsed_size = 0;
        size_t   chunk_size  = 0;
        if (axl_hex_parse_u64(buf + pos, line_end - pos, &parsed_size) >= 0) {
            chunk_size = (size_t)parsed_size;
        }
        pos = line_end + 2;  /* skip \r\n after the size-line */

        if (chunk_size == 0) {
            //
            // Terminal chunk — done.
            //
            conn->chunked_done = true;
            conn->content_length = conn->body_bytes_read;
            if (conn->is_upload_stream) {
                stream_upload_data(s, conn, NULL, 0, true);
            }
            return;
        }

        //
        // Arm the data-consumption state and loop back to consume it (the
        // chunk_remaining branch above handles however much fits here, and
        // carries the rest to the next recv).
        //
        conn->chunk_remaining = chunk_size;
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
            /* reset_connection runs from on_response_sent now */
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
            /* reset_connection runs from on_response_sent now */
            return;
        }

        conn->content_length = axl_http_get_content_length(conn->headers);
        conn->body_bytes_read = 0;
        conn->body = NULL;
        conn->body_alloc = 0;
        conn->chunked = false;
        conn->chunked_done = false;
        conn->chunk_remaining = 0;
        conn->chunk_crlf_pending = 0;
        conn->chunk_leftover_len = 0;

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
            // Middleware first, before a single body byte reaches the
            // upload handler. The regular dispatch path runs middleware
            // inside dispatch_request, but uploads never go through
            // dispatch_request — without this short-circuit any
            // cross-cutting middleware (auth, rate limit, read-only
            // gating) silently bypasses upload routes.
            //
            // Body is NULL here by design: streaming uploads don't
            // materialize the body, so middleware that needs the body
            // can't be applied to upload routes — header-based gating
            // is the contract.
            //
            AxlHttpRequest  mw_req;
            AxlHttpResponse mw_resp;
            axl_memset(&mw_req, 0, sizeof(mw_req));
            mw_req.method  = conn->method;
            mw_req.path    = conn->path;
            mw_req.query   = conn->query;
            mw_req.headers = conn->headers;
            axl_memcpy(mw_req.client_addr, conn->client_addr,
                       sizeof(mw_req.client_addr));

            axl_memset(&mw_resp, 0, sizeof(mw_resp));
            mw_resp.status_code = 200;

            if (run_middleware(s, &mw_req, &mw_resp) != AXL_OK) {
                axl_warning("middleware rejected upload %s %s (status %zu)",
                            conn->method, conn->path, mw_resp.status_code);
                if (mw_resp.status_code == 200) {
                    mw_resp.status_code = 500;
                }
                /* Force-close: the client almost certainly already
                   pushed body bytes into the kernel TCP buffer (curl
                   et al. send headers + body in one write). If we
                   stayed in keep-alive, on_response_sent's rearm would
                   read those leftover body bytes into header_buf and
                   parse them as the next request line. Same reason
                   send_error_response forces close. */
                conn->keep_alive = false;
                send_response(conn, &mw_resp);
                /* send_response memcpy'd headers + body into tx_buf;
                   the originals (set by middleware via
                   axl_http_response_set_text / _json / etc.) are
                   ours to free. */
                if (mw_resp.body != NULL && !mw_resp.body_static) {
                    axl_free(mw_resp.body);
                }
                if (mw_resp.headers != NULL) {
                    axl_hash_table_free(mw_resp.headers);
                }
                return;
            }

            //
            // Auth gate. Uploads never reach dispatch_request, so its
            // auth check never runs for them — enforce the route's
            // auth_flags here, after middleware and before a single
            // body byte is accepted. mw_req.body is NULL (uploads
            // don't materialize the body), so only header-based auth
            // (cookie / Authorization / client address) can gate an
            // upload route. send_error_response force-closes, so the
            // body bytes the client already pushed are discarded.
            //
            size_t auth_status =
                http_check_route_auth(s, early_route->auth_flags, &mw_req);
            if (auth_status != 0) {
                axl_warning("upload %s %s rejected: auth (%zu)",
                            conn->method, conn->path, auth_status);
                send_error_response(conn, auth_status);
                return;
            }

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
                /* reset_connection runs from on_response_sent now */
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
                /* reset_connection runs from on_response_sent now */
                return;
            }

            conn->body = axl_malloc(conn->content_length);
            conn->body_alloc = conn->content_length;
            if (conn->body == NULL) {
                send_error_response(conn, 500);
                /* reset_connection runs from on_response_sent now */
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

// ---------------------------------------------------------------------------
// Public request helpers — content negotiation + JSON body parse
//
// These are not on the parsing path; they're consumer-side conveniences for
// REST-shaped handlers. Lifted out of axl-webfs's serve module so any HTTP
// consumer (REST API, one-shot fetch tool, future services) can reuse them.
// ---------------------------------------------------------------------------

bool
axl_http_request_accepts(const AxlHttpRequest *req, const char *mime)
{
    if (req == NULL || req->headers == NULL) {
        return false;
    }
    const char *accept = (const char *)axl_hash_table_lookup(
        req->headers, "accept");
    /* axl_http_accepts already handles NULL accept-header, wildcards,
       case-insensitive matching, q-values, and whitespace. This wrapper
       just routes from AxlHttpRequest to that primitive — same semantics. */
    return axl_http_accepts(accept, mime);
}

bool
axl_http_request_wants_json(const AxlHttpRequest *req)
{
    return axl_http_request_accepts(req, "application/json");
}

bool
axl_http_request_get_json(const AxlHttpRequest *req, AxlJsonReader *out)
{
    if (req == NULL || out == NULL || req->body == NULL || req->body_size == 0) {
        return false;
    }
    return axl_json_parse((const char *)req->body, req->body_size, out);
}
