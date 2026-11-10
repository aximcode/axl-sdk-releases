/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-request.c
    HTTP request parsing: headers, body accumulation, chunked encoding,
    early upload route detection.
**/

#include "axl-http-server-internal.h"
#include "axl-http-core-internal.h"
#include <axl/axl-json.h>

AXL_LOG_DOMAIN("http");

/* The last AxlHttpRequest.request_id handed out. ONE sequence for the
   process, not one per server: an id names a request among every request
   any server has parsed, so state keyed on it can never meet another
   server's request under the same id. */
static uint64_t http_request_seq;

/* A fresh request_id: nonzero, never reused while the process runs. */
static uint64_t
http_next_request_id(void)
{
    return ++http_request_seq;
}

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
        axl_debug("body buffer grow failed: %zu bytes", new_size);
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

/* Decoder states. Each byte moves the decoder on by itself, so a size
   line, a chunk's CRLF or the final CRLF may be cut by a segment boundary
   anywhere and nothing is lost: a partial line waits in chunk_line. */
enum {
    HTTP_CHUNK_SIZE = 0,   /* reading a chunk-size line */
    HTTP_CHUNK_DATA,       /* chunk_remaining bytes of data owed */
    HTTP_CHUNK_DATA_END,   /* the CR (or a bare LF) after a chunk's data */
    HTTP_CHUNK_DATA_LF,    /* the LF after that CR */
    HTTP_CHUNK_TRAILER,    /* trailer lines after the last chunk, to an empty one */
    HTTP_CHUNK_DONE
};

void
chunked_reset(
    HttpConn *conn
    )
{
    conn->chunk_state       = HTTP_CHUNK_SIZE;
    conn->chunk_line_len    = 0;
    conn->chunk_trailer_len = 0;
    conn->chunk_remaining   = 0;
}

/* Parse a chunk-size line (CRLF stripped): hex digits, then optional
   whitespace, then nothing or ';' and extensions (ignored). @return true
   with @p out; false for no digits, an overflow, or any other byte. */
static bool
chunk_size_parse(
    const char *line,
    size_t      len,
    size_t     *out
    )
{
    size_t i      = 0;
    size_t size   = 0;
    while (i < len && axl_hex_nibble((unsigned char)line[i]) >= 0) {
        if (size > (SIZE_MAX >> 4)) {
            return false;   /* more than size_t holds */
        }
        size = (size << 4) | (size_t)axl_hex_nibble((unsigned char)line[i]);
        i++;
    }
    if (i == 0) {
        return false;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i < len && line[i] != ';') {
        return false;
    }
    *out = size;
    return true;
}

/* Hand @p n bytes of chunk data to the body: the upload handler, or the
   whole-body buffer. @return 0 or the status to fail with. */
static size_t
chunk_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    const char    *data,
    size_t         n
    )
{
    if (conn->is_upload_stream) {
        stream_upload_data(s, conn, data, n, false);
        conn->body_bytes_read += n;
        return 0;
    }
    if (conn->body_bytes_read + n > s->body_limit) {
        return 413;   /* never dispatch a cut body */
    }
    size_t needed = conn->body_bytes_read + n;
    if (needed > conn->body_alloc) {
        size_t new_alloc = conn->body_alloc ? conn->body_alloc : 4096;
        while (new_alloc < needed) {
            new_alloc *= 2;
        }
        void *grown = grow_buffer(conn->body, conn->body_bytes_read, new_alloc);
        if (grown == NULL) {
            return 500;
        }
        conn->body       = grown;
        conn->body_alloc = new_alloc;
    }
    axl_memcpy((uint8_t *)conn->body + conn->body_bytes_read, data, n);
    conn->body_bytes_read += n;
    return 0;
}

/* Take one byte of a size or trailer line. @return 0 while the line goes
   on, 1 when it is complete (in chunk_line, CR stripped), or 400 when it
   is longer than HTTP_CHUNK_LINE_MAX. A bare LF ends a line too (RFC 9112
   2.2 lets a recipient accept it). */
static size_t
chunk_line_byte(
    HttpConn *conn,
    char      c
    )
{
    if (c == '\n') {
        if (conn->chunk_line_len > 0 && conn->chunk_line[conn->chunk_line_len - 1] == '\r') {
            conn->chunk_line_len--;
        }
        return 1;
    }
    if (conn->chunk_line_len >= sizeof(conn->chunk_line)) {
        return 400;
    }
    conn->chunk_line[conn->chunk_line_len++] = c;
    return 0;
}

size_t
process_chunked_data(
    AxlHttpServer *s,
    HttpConn      *conn,
    const char    *buf,
    size_t         buf_len
    )
{
    size_t pos = 0;

    while (pos < buf_len && conn->chunk_state != HTTP_CHUNK_DONE) {
        if (conn->is_upload_stream && conn->upload_ended) {
            return 0;   /* the handler ended the upload and answered */
        }
        switch (conn->chunk_state) {
        case HTTP_CHUNK_DATA: {
            size_t avail = buf_len - pos;
            size_t take  = (avail < conn->chunk_remaining) ? avail : conn->chunk_remaining;
            size_t st    = chunk_data(s, conn, buf + pos, take);
            if (st != 0) {
                return st;
            }
            pos                   += take;
            conn->chunk_remaining -= take;
            if (conn->chunk_remaining == 0) {
                conn->chunk_state = HTTP_CHUNK_DATA_END;
            }
            break;
        }
        case HTTP_CHUNK_DATA_END:
            if (buf[pos] == '\r') {
                conn->chunk_state = HTTP_CHUNK_DATA_LF;
            } else if (buf[pos] == '\n') {
                conn->chunk_state = HTTP_CHUNK_SIZE;
            } else {
                return 400;   /* the data was longer than its size said */
            }
            pos++;
            break;
        case HTTP_CHUNK_DATA_LF:
            if (buf[pos] != '\n') {
                return 400;
            }
            conn->chunk_state = HTTP_CHUNK_SIZE;
            pos++;
            break;
        case HTTP_CHUNK_SIZE: {
            size_t got = chunk_line_byte(conn, buf[pos++]);
            if (got == 400) {
                return 400;
            }
            if (got == 0) {
                break;
            }
            size_t size = 0;
            if (!chunk_size_parse(conn->chunk_line, conn->chunk_line_len, &size)) {
                return 400;
            }
            conn->chunk_line_len = 0;
            if (size == 0) {
                conn->chunk_state = HTTP_CHUNK_TRAILER;
            } else {
                conn->chunk_remaining = size;
                conn->chunk_state     = HTTP_CHUNK_DATA;
            }
            break;
        }
        case HTTP_CHUNK_TRAILER: {
            conn->chunk_trailer_len++;
            if (conn->chunk_trailer_len > HTTP_CHUNK_TRAILER_MAX) {
                return 400;
            }
            size_t got = chunk_line_byte(conn, buf[pos++]);
            if (got == 400) {
                return 400;
            }
            if (got == 1) {
                bool empty = conn->chunk_line_len == 0;
                conn->chunk_line_len = 0;
                if (empty) {
                    /* The final CRLF: the body is whole. What follows it
                       (a pipelined request) is not read here. */
                    conn->chunk_state    = HTTP_CHUNK_DONE;
                    conn->chunked_done   = true;
                    conn->content_length = conn->body_bytes_read;
                    if (conn->is_upload_stream) {
                        stream_upload_data(s, conn, NULL, 0, true);
                    }
                }
            }
            break;
        }
        default:
            return 0;
        }
    }
    if (conn->chunk_state == HTTP_CHUNK_DONE && pos < buf_len) {
        /* The body ended inside this buffer: the rest is the next request. */
        conn_carry_keep(conn, buf + pos, buf_len - pos);
    }
    return 0;
}

/* A Transfer-Encoding value, read per RFC 9112 6.1/6.3: a comma list whose
   FINAL coding must be chunked, applied once. @return 0 for exactly
   chunked; 400 when the final coding is not chunked, chunked appears more
   than once, or the list is empty -- the body's length cannot be told;
   501 when a coding before it is one the server does not implement. */
static size_t
transfer_coding_check(
    const char *te
    )
{
    AxlHttpCodings cod;
    _axl_http_codings_scan(te, &cod);
    if (cod.empty || !cod.last_chunked || cod.chunked > 1) {
        return 400;
    }
    return (cod.other > 0) ? 501 : 0;
}

/* The request as the server has it before the body: what a gate that runs
   ahead of the body (an upload route's middleware and auth, the
   100-continue decision) judges. body is NULL by design. */
static void
prebody_request(
    const HttpConn  *conn,
    AxlHttpRequest  *req,
    AxlHttpResponse *resp
    )
{
    axl_memset(req, 0, sizeof(*req));
    req->method     = conn->method;
    req->path       = conn->path;
    req->query      = conn->query;
    req->headers    = conn->headers;
    req->request_id = conn->request_id;
    axl_memcpy(req->client_addr, conn->client_addr, sizeof(req->client_addr));

    axl_memset(resp, 0, sizeof(*resp));
    resp->status_code = 200;
}

/* May a request that asked for 100-continue have its body? Decided from
   what dispatch_request would refuse without it: no route (404), or a
   route's auth callback failing on the headers (401/403). A refusal is
   answered here, as dispatch_request would answer it -- the middleware
   first, its headers on the error -- and the connection closes, so the
   body the client then never sends is never waited for. A request that
   passes is judged again, with its body, by dispatch_request.
   @return true when the body may be read. */
static bool
continue_gate(
    AxlHttpServer *s,
    HttpConn      *conn,
    HttpRoute     *route
    )
{
    AxlHttpRequest  req;
    AxlHttpResponse resp;
    size_t          status = 404;

    prebody_request(conn, &req, &resp);
    if (route != NULL) {
        status = http_check_route_auth(s, route->auth_flags, &req);
        if (status == 0) {
            return true;
        }
    }
    if (run_middleware(s, &req, &resp) != AXL_OK) {
        axl_warning("middleware rejected %s %s (status %zu)",
                    conn->method, conn->path, resp.status_code);
        send_middleware_refusal(conn, &resp);
    } else {
        send_error_response_headers(conn, status, &resp);
        http_response_release(&resp);
    }
    return false;
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
        // Parse the request line and headers, and judge them: 400 for a
        // malformed line or block or a missing Host, 505 for HTTP/2+.
        //
        AxlHttpReqHead head;
        size_t         head_status = _axl_http_request_head_parse(conn->header_buf,
                                                                  header_end, &head);
        conn->method  = head.method;
        conn->path    = head.path;
        conn->query   = head.query;
        conn->headers = head.headers;
        bool http10   = head.http10;
        if (head_status != 0) {
            send_error_response(conn, head_status);
            /* reset_connection runs from on_response_sent now */
            return;
        }

        if (axl_http_parse_content_length(conn->headers, &conn->content_length) != AXL_OK) {
            /* Not a number, or past what the framing can represent: read
               leniently it was 0 (an empty body) or wrapped. */
            send_error_response(conn, 400);
            return;
        }
        conn->body_bytes_read = 0;
        conn->body = NULL;
        conn->body_alloc = 0;
        conn->chunked = false;
        conn->chunked_done = false;
        chunked_reset(conn);

        //
        // Check for chunked transfer encoding
        //
        const char *te_header = (const char *)axl_hash_table_lookup(conn->headers, "transfer-encoding");
        bool        close_after = false;
        if (te_header != NULL) {
            /* Exactly chunked, or refused: a body whose length cannot be
               told is never read as having none (its bytes would be taken
               for the next request). */
            size_t st = transfer_coding_check(te_header);
            if (st != 0) {
                send_error_response(conn, st);
                return;
            }
            conn->chunked = true;
            /* With Content-Length too, chunked frames the body and the
               connection closes after the response (RFC 9112 6.3): the
               sender's framing is in doubt. */
            close_after = axl_hash_table_lookup(conn->headers, "content-length") != NULL
                          || http10;   /* HTTP/1.0 has no chunked: faulty framing (6.1) */
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
        if (close_after) {
            conn->keep_alive = false;
        }

        conn->headers_done = true;
        conn->request_id   = http_next_request_id();

        //
        // Expect (RFC 9110 10.1.1): 100-continue is answered once the
        // server knows it will read the body; any other expectation is one
        // it cannot meet.
        //
        AxlHttpExpect expect = _axl_http_expect_check(
            (const char *)axl_hash_table_lookup(conn->headers, "expect"), http10);
        if (expect == AXL_HTTP_EXPECT_FAILED) {
            send_error_response(conn, 417);
            return;
        }

        //
        // Early route lookup — detect upload routes before body buffering
        //
        size_t body_in_header = conn->header_len - header_end;
        /* Bytes past this request's body are the next request, pipelined
           into the same segment: carry them. A chunked body's decoder
           finds its own end and carries what follows it. */
        if (!conn->chunked && body_in_header > conn->content_length) {
            conn_carry_keep(conn, conn->header_buf + header_end + conn->content_length,
                            body_in_header - conn->content_length);
            body_in_header = conn->content_length;
        }
        HttpRoute *early_route = find_route(s, conn->method, conn->path);
        /* A 100 is owed only while the client is still holding its body
           back: a body that has started arriving needs none. A WebSocket
           upgrade is left as it was: dispatch_request takes it before any
           route lookup, which the gate below would answer 404. */
        const char *upgrade = (const char *)axl_hash_table_lookup(conn->headers, "upgrade");
        bool want_continue = expect == AXL_HTTP_EXPECT_CONTINUE
                             && body_in_header == 0
                             && (conn->chunked
                                 || (conn->content_length > 0
                                     && conn->content_length != SIZE_MAX))
                             && !(upgrade != NULL && axl_strcasecmp(upgrade, "websocket") == 0);
        bool body_too_large = !conn->chunked && conn->content_length != SIZE_MAX
                              && conn->content_length > s->body_limit;

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
            prebody_request(conn, &mw_req, &mw_resp);

            if (run_middleware(s, &mw_req, &mw_resp) != AXL_OK) {
                axl_warning("middleware rejected upload %s %s (status %zu)",
                            conn->method, conn->path, mw_resp.status_code);
                /* Closes: the client almost certainly already pushed body
                   bytes (curl et al. send headers + body in one write),
                   which a kept-alive connection would parse as the next
                   request line. */
                send_middleware_refusal(conn, &mw_resp);
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
                /* With the middleware's headers, and freeing what it set:
                   this exit leaked it on every unauthenticated upload, as
                   dispatch_request's 401 did. */
                send_error_response_headers(conn, auth_status, &mw_resp);
                http_response_release(&mw_resp);
                return;
            }

            //
            // Upload streaming — don't buffer the body, stream chunks
            // to the handler as they arrive.
            //
            conn->is_upload_stream = true;
            conn->upload_ended     = false;
            conn->upload_route     = early_route;
            axl_debug("upload stream started for %s %s",
                      conn->method, conn->path);

            conn->upload_buf = axl_malloc(s->upload_chunk_size);
            if (conn->upload_buf == NULL) {
                send_error_response_headers(conn, 500, &mw_resp);
                http_response_release(&mw_resp);
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
            conn->upload_req.request_id = conn->request_id;
            axl_memcpy(conn->upload_req.client_addr, conn->client_addr,
                       sizeof(conn->upload_req.client_addr));

            axl_memset(&conn->upload_resp, 0, sizeof(conn->upload_resp));
            conn->upload_resp.status_code = 200;
            /* The middleware's headers go out with the upload's response,
               which owns and frees them when it is sent (or the connection
               resets) -- less any framing they carry, which that response
               writes itself. Anything else it set -- a body, a streamer --
               is released here. Both used to be dropped on the floor: a
               leak on every upload. */
            http_response_strip_framing(&mw_resp);
            conn->upload_resp.headers = mw_resp.headers;
            mw_resp.headers = NULL;
            http_response_drop_streamer(&mw_resp);
            http_response_release(&mw_resp);

            /* Seed with any body data already in the header buffer. A
               chunked body is DECODED first, as every later recv's is: fed
               raw, the chunk framing reached the handler as body bytes and
               a terminator that arrived with the headers was never seen, so
               the upload hung (and, with a later recv, stored the framing). */
            if (body_in_header > 0 && conn->chunked) {
                size_t st = process_chunked_data(s, conn, conn->header_buf + header_end,
                                                  body_in_header);
                if (st != 0) {
                    stream_upload_fail(conn, st);
                    return;
                }
                if (conn->chunked_done || !conn->active || conn->upload_ended) {
                    return;
                }
            } else if (body_in_header > 0) {
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

            /* Middleware and auth passed: the body is wanted. */
            if (want_continue && !send_continue(conn)) {
                return;
            }
            start_conn_recv(s, conn);
            return;
        }

        //
        // A client waiting on a 100 is answered before anything is spent on
        // its body: a refusal now, or (below, once the body can be held)
        // the 100. A too-large Content-Length is left to the 413 below.
        //
        if (want_continue && !body_too_large && !continue_gate(s, conn, early_route)) {
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
                size_t st = process_chunked_data(s, conn, conn->header_buf + header_end,
                                                 body_in_header);
                if (st != 0) {
                    send_error_response(conn, st);
                    return;
                }
                if (conn->chunked_done) {
                    dispatch_and_respond(s, conn);
                    return;
                }
            }
        } else if (conn->content_length > 0 && conn->content_length != SIZE_MAX) {
            //
            // Content-Length body
            //
            if (body_too_large) {
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
        // Continue to body read or dispatch -- after the 100 the gate above
        // allowed, now that the body has somewhere to go.
        //
        if (want_continue && !send_continue(conn)) {
            return;
        }
        start_conn_recv(s, conn);
    } else if (conn->chunked && !conn->chunked_done) {
        //
        // Process chunked data that arrived in chunk_read_buf; the decoder
        // keeps whatever a recv boundary cut.
        //
        size_t st = process_chunked_data(s, conn, conn->chunk_read_buf, bytes);
        if (st != 0) {
            send_error_response(conn, st);
        } else if (!conn->chunked_done) {
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
    /* STRICT, never AXL_JSON_RELAXED. A request body is the one JSON a server
     * never wrote: it arrives off the network from a client that may be
     * hostile, which is precisely the case axl-json.h names AXL_JSON_STRICT
     * for. The liberal dialect exists so the SDK can read sidecars and config
     * files it did not write -- a different situation with a different threat
     * model, and conflating them would make every AXL HTTP server quietly
     * accept JSON5 from anyone who could reach it. It was conflated by
     * DEFAULT until 2026-08-04, when the dialect became a parameter.
     *
     * A handler that genuinely wants a lenient body can still parse req->body
     * itself with whatever dialect it chooses. */
    return axl_json_parse((const char *)req->body, req->body_size,
                          AXL_JSON_STRICT, out);
}
