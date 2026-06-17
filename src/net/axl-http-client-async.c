/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-client-async.c
    axl_http_get_async / axl_http_post_async — the loop-integrated async HTTP
    client. The whole request (DNS resolve, TCP connect, optional TLS
    handshake, send, receive, redirects) runs as events on the caller's AxlLoop
    with NO nested ephemeral loop, so it is safe to issue from inside a loop
    callback or a resident driver-pump tick at raised TPL — where the sync
    axl_http_get/post nests a loop and warns.

    Shape (the Samba tevent_req discipline, NOT the framework): one heap
    HttpAsyncReq per request; each I/O step launches the next _async op and its
    completion callback advances the machine; exactly ONE completion path
    (req_finish) frees the state and invokes the user callback; cancel + a
    whole-op deadline are checked at each transition. Modeled on the proven
    single-completion shape in axl-net-resolve.c.

    TLS is reached ONLY through the registered ops vtable
    (_axl_http_client_tls_ops) — never a static axl_tls_* reference — so a
    plain-HTTP consumer still lets the linker strip mbedTLS.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-net.h>
#include <axl/axl-loop.h>
#include <axl/axl-http-core.h>
#include <axl/axl-http-client.h>

#include "axl-net-internal.h"
#include "axl-tcp-internal.h"          /* axl_tcp_connect_addr_async, AxlTcp */
#include "axl-http-client-internal.h"  /* struct AxlHttpClient, TLS ops accessor */

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Request state
// ---------------------------------------------------------------------------

/* Header-accumulation cap — a response whose status line + headers don't end
   (no CRLFCRLF) within this is rejected rather than grown without bound. Headers
   are tiny in practice; this only fires on a malformed/hostile response. */
#define HTTP_ASYNC_HDR_MAX  (1u << 20)   /* 1 MiB of headers = malformed */

/* Whole-body sanity ceiling. A KNOWN Content-Length body is allowed to grow to
   its declared size (the response buffer is pre-reserved to exactly that, so an
   absurd declaration fails the one allocation cleanly — like the old sync
   do_request's malloc(content_len)); this ceiling rejects an absurd declaration
   up front and bounds the unknown-size (chunked) case. 256 MiB is far above any
   realistic firmware image / API response the client fetches, so it does not
   bite real use — it is purely an OOM-by-declaration guard. (The earlier 1 MiB
   cap here silently truncated a ~1 MB gBS->LoadImage GET to an error.) */
#define HTTP_ASYNC_BODY_MAX  (256u * 1024u * 1024u)

typedef struct {
    AxlHttpClient        *client;
    AxlLoop              *loop;
    AxlCancellable       *cancel;
    AxlHttpClientDoneFn   cb;
    void                 *user;

    /* request (owned copies; body is BORROWED) */
    char                 *method;
    char                 *url;          /* current hop URL (changes on redirect) */
    const void           *body;
    size_t                body_size;
    char                 *content_type;
    AxlHashTable         *extra_headers;  /* borrowed until cb (NULL for none) */
    size_t                redirect_count;

    /* current hop */
    AxlUrl               *parsed;
    bool                  https;
    char                 *req_buf;      /* built request bytes (header block) */
    size_t                req_len;
    bool                  headers_sent;
    bool                  body_sent;
    bool                  reused;         /* this hop started on a kept-alive conn */
    bool                  retried;        /* stale-conn replay already attempted */

    /* receive accumulation */
    uint8_t              *rx;
    size_t                rx_len;
    size_t                rx_cap;
    uint8_t               recv_chunk[HTTP_CLIENT_RECV_BUF];

    /* parsed response */
    size_t                header_end;
    size_t                status_code;
    AxlHashTable         *resp_headers;
    bool                  have_framing;
    bool                  chunked;
    size_t                content_len;
    void                 *resp_body;
    size_t                resp_body_size;

    AxlSourceId           kick_source;    /* deferred first-hop start */
    AxlSourceId           timeout_source;  /* IDLE deadline (re-armed on progress) */
    uint32_t              idle_ms;         /* the per-phase idle bound */
    bool                  finished;
    bool                  sync_close;      /* sync wrapper: complete connection
                                              drops inline (loop is ephemeral) */
} HttpAsyncReq;

/* req_process return codes. */
enum { REQ_COMPLETE = 0, REQ_NEED_MORE = 1, REQ_ERROR = -1, REQ_REDIRECT = 2 };

// forward declarations (the machine is a graph of callbacks)
static void req_begin_hop(HttpAsyncReq *req);
static void req_start_send(HttpAsyncReq *req);
static int  req_arm_recv(HttpAsyncReq *req);
static bool on_app_recv(AxlTcp *sock, AxlStatus status, void *data);
static void req_touch_deadline(HttpAsyncReq *req);

// ---------------------------------------------------------------------------
// TLS ops shorthand
// ---------------------------------------------------------------------------

static const AxlHttpClientTlsOps *
tls_ops(void)
{
    return _axl_http_client_tls_ops();
}

// ---------------------------------------------------------------------------
// Teardown + single completion
// ---------------------------------------------------------------------------

/* Drop the client's connection (cancels any armed recv/send via axl_tcp_close,
   frees the TLS context). Used on error / redirect / Connection: close. */
static void
req_drop_connection(HttpAsyncReq *req)
{
    AxlHttpClient *c = req->client;
    if (c->tls_ctx != NULL && tls_ops() != NULL) {
        tls_ops()->free(c->tls_ctx);
        c->tls_ctx = NULL;
    }
    if (c->sock != NULL) {
        /* A sync request drops the connection (Connection: close / error /
           redirect) from INSIDE its ephemeral loop's dispatch. The async ops
           stamped sock->async_loop with that loop, so axl_tcp_close would
           register a close_event on it and return — but the loop is freed the
           moment axl_loop_run unwinds (and at a raised TPL the firmware notify
           is starved, so the event never fires first), orphaning the source and
           leaking the socket + close-ctx. Clear async_loop so axl_tcp_close
           takes its sync fallback, which (since we are in a loop callback)
           completes the close loop-free — no nested loop, no orphaned source.
           Genuine async requests keep the non-blocking async close on the
           consumer's persistent loop (it drains there between pump ticks). */
        if (req->sync_close) {
            c->sock->async_loop = NULL;
        }
        axl_tcp_close(c->sock);
        c->sock = NULL;
    }
    axl_free(c->connected_host);
    c->connected_host = NULL;
}

/* Free everything the hop owns EXCEPT the connection (the caller decides
   whether to keep it for keep-alive). Does not touch the user callback. */
static void
req_free(HttpAsyncReq *req)
{
    axl_loop_remove_source(req->loop, req->kick_source);
    axl_loop_remove_source(req->loop, req->timeout_source);
    if (req->parsed != NULL) {
        axl_url_free(req->parsed);
    }
    if (req->resp_headers != NULL) {
        axl_hash_table_free(req->resp_headers);
    }
    axl_free(req->resp_body);
    axl_free(req->req_buf);
    axl_free(req->rx);
    axl_free(req->method);
    axl_free(req->url);
    axl_free(req->content_type);
    axl_free(req);
}

/* The one completion path: clears the in-flight flag, tears down, then invokes
   the user callback exactly once. On a clean keep-alive success the connection
   is left open for reuse; every other outcome drops it. */
static void
req_finish(HttpAsyncReq *req, bool keep_conn, AxlHttpClientResponse *resp,
           AxlStatus st)
{
    if (req->finished) {
        if (resp != NULL) {
            axl_http_client_response_free(resp);
        }
        return;
    }
    req->finished = true;

    if (!keep_conn) {
        req_drop_connection(req);
    }

    AxlHttpClientDoneFn cb   = req->cb;
    void               *user = req->user;
    req->client->async_busy  = false;

    req_free(req);

    if (cb != NULL) {
        cb(resp, st, user);
    } else if (resp != NULL) {
        /* Fire-and-forget: discard the response. */
        axl_http_client_response_free(resp);
    }
}

/* Error/timeout/cancel finish — always drops the connection, resp is NULL. */
static void
req_fail(HttpAsyncReq *req, AxlStatus st)
{
    req_finish(req, false, NULL, st);
}

// ---------------------------------------------------------------------------
// Response assembly
// ---------------------------------------------------------------------------

static void
req_complete_ok(HttpAsyncReq *req)
{
    AxlHttpClientResponse *r = axl_calloc(1, sizeof(AxlHttpClientResponse));
    if (r == NULL) {
        req_fail(req, AXL_ERR);
        return;
    }
    r->status_code = req->status_code;
    r->headers     = req->resp_headers;
    r->body        = req->resp_body;
    r->body_size   = req->resp_body_size;
    /* Ownership transferred to the response — clear so req_free doesn't
       double-free. */
    req->resp_headers   = NULL;
    req->resp_body      = NULL;
    req->resp_body_size = 0;

    /* Keep-alive unless disabled or the server asked to close. */
    bool keep = req->client->keep_alive;
    if (keep) {
        const char *conn_hdr =
            (const char *)axl_hash_table_lookup(r->headers, "connection");
        if (conn_hdr != NULL && axl_strcasecmp(conn_hdr, "close") == 0) {
            keep = false;
        }
    }

    req_finish(req, keep, r, AXL_OK);
}

// ---------------------------------------------------------------------------
// Chunked transfer decode (pure buffer; re-run over the whole accumulated body
// on each recv). Returns REQ_COMPLETE/REQ_NEED_MORE/REQ_ERROR.
// ---------------------------------------------------------------------------

static int
chunked_try_decode(const uint8_t *in, size_t in_len,
                   void **out_body, size_t *out_size)
{
    enum { ST_SIZE, ST_DATA, ST_TRAIL, ST_TRAILERS, ST_DONE };
    int    state           = ST_SIZE;
    size_t pos             = 0;
    size_t chunk_remaining = 0;
    void  *body            = NULL;
    size_t body_cap        = 0;
    size_t body_size       = 0;

    while (state != ST_DONE) {
        if (state == ST_SIZE) {
            const uint8_t *crlf =
                axl_memmem(in + pos, in_len - pos, "\r\n", 2);
            if (crlf == NULL) {
                axl_free(body);
                return REQ_NEED_MORE;
            }
            size_t   line_len = (size_t)(crlf - (in + pos));
            uint64_t sz       = 0;
            if (axl_hex_parse_u64((const char *)(in + pos), line_len, &sz) < 0) {
                axl_free(body);
                return REQ_ERROR;
            }
            pos += line_len + 2;
            chunk_remaining = (size_t)sz;
            state = (sz == 0) ? ST_TRAILERS : ST_DATA;
        } else if (state == ST_DATA) {
            size_t avail = in_len - pos;
            size_t take  = chunk_remaining < avail ? chunk_remaining : avail;
            if (take == 0) {
                axl_free(body);
                return REQ_NEED_MORE;
            }
            if (body_size + take > body_cap) {
                size_t new_cap = body_cap == 0 ? 256 : body_cap * 2;
                while (new_cap < body_size + take) {
                    new_cap *= 2;
                }
                void *nb = axl_realloc(body, new_cap);
                if (nb == NULL) {
                    axl_free(body);
                    return REQ_ERROR;
                }
                body = nb;
                body_cap = new_cap;
            }
            axl_memcpy((uint8_t *)body + body_size, in + pos, take);
            body_size += take;
            pos += take;
            chunk_remaining -= take;
            if (chunk_remaining == 0) {
                state = ST_TRAIL;
            }
        } else if (state == ST_TRAIL) {
            if (in_len - pos < 2) {
                axl_free(body);
                return REQ_NEED_MORE;
            }
            if (in[pos] != '\r' || in[pos + 1] != '\n') {
                axl_free(body);
                return REQ_ERROR;
            }
            pos += 2;
            state = ST_SIZE;
        } else { /* ST_TRAILERS — drain optional trailer fields */
            const uint8_t *crlf =
                axl_memmem(in + pos, in_len - pos, "\r\n", 2);
            if (crlf == NULL) {
                axl_free(body);
                return REQ_NEED_MORE;
            }
            size_t line_len = (size_t)(crlf - (in + pos));
            pos += line_len + 2;
            if (line_len == 0) {
                state = ST_DONE;
            }
        }
    }

    *out_body = body;
    *out_size = body_size;
    return REQ_COMPLETE;
}

// ---------------------------------------------------------------------------
// Incremental response processing — drive after each recv appends to req->rx.
// ---------------------------------------------------------------------------

static int
req_process(HttpAsyncReq *req)
{
    AxlHttpClient *c = req->client;

    /* 1. Parse status line + headers once the blank line arrives. */
    if (req->header_end == 0) {
        size_t he = axl_http_find_header_end((char *)req->rx, req->rx_len);
        if (he == 0) {
            return (req->rx_len >= HTTP_ASYNC_HDR_MAX) ? REQ_ERROR : REQ_NEED_MORE;
        }
        req->header_end = he;

        size_t first_line_end = 0;
        for (size_t j = 0; j + 1 < he; j++) {
            if (req->rx[j] == '\r' && req->rx[j + 1] == '\n') {
                first_line_end = j;
                break;
            }
        }
        if (axl_http_parse_status_line((char *)req->rx, first_line_end,
                                       &req->status_code) != AXL_OK) {
            return REQ_ERROR;
        }
        size_t hdr_start = first_line_end + 2;
        if (axl_http_parse_headers((char *)req->rx + hdr_start,
                                   he - hdr_start, &req->resp_headers) != AXL_OK) {
            return REQ_ERROR;
        }

        /* Redirect? Reported before framing — a 3xx body is irrelevant. */
        if ((req->status_code == 301 || req->status_code == 302 ||
             req->status_code == 307) &&
            req->redirect_count < (size_t)c->max_redirects) {
            const char *location =
                (const char *)axl_hash_table_lookup(req->resp_headers, "location");
            if (location != NULL) {
                char *redir = axl_strdup(location);
                if (redir == NULL) {
                    return REQ_ERROR;
                }
                axl_free(req->url);
                req->url = redir;
                return REQ_REDIRECT;
            }
        }

        /* Framing: chunked wins over Content-Length (RFC 7230 §3.3.3). */
        const char *te = (const char *)axl_hash_table_lookup(
            req->resp_headers, "transfer-encoding");
        req->chunked     = (te != NULL && axl_strcasecmp(te, "chunked") == 0);
        req->content_len = axl_http_get_content_length(req->resp_headers);
        req->have_framing = true;

        /* For a known Content-Length: reject an absurd declaration up front, then
           pre-reserve the response buffer to exactly header_end + content_len in
           ONE allocation (so a huge-but-bogus length fails cleanly here rather
           than growing by doubling, and a legitimate multi-MB body — e.g. a
           gBS->LoadImage whole-image read — is NOT capped). */
        if (!req->chunked && req->content_len > 0) {
            if (req->content_len > HTTP_ASYNC_BODY_MAX) {
                axl_error("response Content-Length %llu exceeds the %u-byte cap",
                          (unsigned long long)req->content_len,
                          (unsigned)HTTP_ASYNC_BODY_MAX);
                return REQ_ERROR;
            }
            size_t want = req->header_end + req->content_len;
            if (want > req->rx_cap) {
                uint8_t *nb = axl_realloc(req->rx, want);
                if (nb == NULL) {
                    return REQ_ERROR;
                }
                req->rx     = nb;
                req->rx_cap = want;
            }
        }
    }

    /* 2. Body completeness. */
    size_t body_avail = req->rx_len - req->header_end;

    if (req->chunked) {
        void  *decoded = NULL;
        size_t dsize   = 0;
        int rc = chunked_try_decode(req->rx + req->header_end, body_avail,
                                    &decoded, &dsize);
        if (rc == REQ_COMPLETE) {
            req->resp_body      = decoded;
            req->resp_body_size = dsize;
        } else if (rc == REQ_NEED_MORE && req->rx_len >= HTTP_ASYNC_BODY_MAX) {
            /* Chunked has no declared size — bound the accumulation. */
            return REQ_ERROR;
        }
        return rc;
    }

    if (req->content_len > 0) {
        /* The body grows to the declared Content-Length (the buffer was
           pre-reserved + ceiling-checked at framing) — no separate cap. */
        if (body_avail < req->content_len) {
            return REQ_NEED_MORE;
        }
        req->resp_body = axl_memdup(req->rx + req->header_end, req->content_len);
        if (req->resp_body == NULL) {
            return REQ_ERROR;
        }
        req->resp_body_size = req->content_len;
        return REQ_COMPLETE;
    }

    /* No Content-Length and not chunked → no body (matches the sync client). */
    req->resp_body      = NULL;
    req->resp_body_size = 0;
    return REQ_COMPLETE;
}

/* Reset all per-hop state so the machine can re-enter from URL parse. Shared
   by the redirect and stale-connection-replay paths. Leaves the request inputs
   (method, url, body) and cross-hop counters (redirect_count, retried) intact;
   the connection is dropped separately by the caller. */
static void
req_reset_hop(HttpAsyncReq *req)
{
    if (req->parsed != NULL) {
        axl_url_free(req->parsed);
        req->parsed = NULL;
    }
    if (req->resp_headers != NULL) {
        axl_hash_table_free(req->resp_headers);
        req->resp_headers = NULL;
    }
    axl_free(req->req_buf);
    req->req_buf = NULL;
    axl_free(req->rx);
    req->rx = NULL;
    req->rx_len = 0;
    req->rx_cap = 0;
    axl_free(req->resp_body);
    req->resp_body = NULL;
    req->resp_body_size = 0;
    req->status_code = 0;
    req->header_end = 0;
    req->have_framing = false;
    req->chunked = false;
    req->content_len = 0;
    req->headers_sent = false;
    req->body_sent = false;
}

/* Restart the machine for a redirect hop: drop the connection (new host
   possible) and reset all per-hop state, then re-enter from URL parse. The
   request method + body are preserved — mirroring the sync client, which
   re-issues the same method/body on 301/302/307. */
static void
req_redirect(HttpAsyncReq *req)
{
    req_drop_connection(req);
    req_reset_hop(req);
    req->redirect_count++;
    axl_debug("redirect %llu -> %s",
              (unsigned long long)req->status_code, req->url);
    req_begin_hop(req);
}

/* Replay the request on a fresh connection after a reused keep-alive socket
   turned out to be stale (the server idle-closed it). Mirrors the sync
   client's single stale-connection retry. */
static void
req_retry(HttpAsyncReq *req)
{
    req->retried = true;
    req_drop_connection(req);
    req_reset_hop(req);
    axl_debug("reused connection stale - reconnecting and replaying");
    req_begin_hop(req);
}

/* A transport failure: replay once if it was a stale reused connection that
   produced no response bytes (the sync client's `total_recv == 0` retry),
   otherwise complete the request with the error. */
static void
req_transport_fail(HttpAsyncReq *req)
{
    if (req->reused && !req->retried && req->rx_len == 0) {
        req_retry(req);
        return;
    }
    req_fail(req, AXL_ERR);
}

// ---------------------------------------------------------------------------
// Receive (application data)
// ---------------------------------------------------------------------------

static int
rx_append(HttpAsyncReq *req, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (req->rx_len + len > req->rx_cap) {
        size_t new_cap = req->rx_cap == 0 ? HTTP_CLIENT_RECV_BUF : req->rx_cap * 2;
        while (new_cap < req->rx_len + len) {
            new_cap *= 2;
        }
        uint8_t *nb = axl_realloc(req->rx, new_cap);
        if (nb == NULL) {
            return -1;
        }
        req->rx = nb;
        req->rx_cap = new_cap;
    }
    axl_memcpy(req->rx + req->rx_len, data, len);
    req->rx_len += len;
    return 0;
}

/* React to a processing result after rx grew. Returns true to keep the recv
   armed, false once the machine has moved on (completed, failed, or
   redirected — each of which tears down or re-arms on its own). */
static bool
req_advance(HttpAsyncReq *req)
{
    int rc = req_process(req);
    switch (rc) {
    case REQ_COMPLETE:
        req_complete_ok(req);
        return false;
    case REQ_REDIRECT:
        req_redirect(req);
        return false;
    case REQ_ERROR:
        req_fail(req, AXL_ERR);
        return false;
    default: /* REQ_NEED_MORE */
        return true;
    }
}

/* Peer EOF / transport error during the receive phase. A response framed by
   connection close (no Content-Length, not chunked) is complete at EOF;
   anything else is truncated — replay if it was a stale reused connection,
   else fail. */
static void
on_recv_eof(HttpAsyncReq *req)
{
    if (req->header_end != 0 && !req->chunked && req->content_len == 0) {
        req_complete_ok(req);
    } else {
        req_transport_fail(req);
    }
}

static bool
on_app_recv(AxlTcp *sock, AxlStatus status, void *data)
{
    HttpAsyncReq  *req = (HttpAsyncReq *)data;
    AxlHttpClient *c   = req->client;

    if (status == AXL_CANCELLED) {
        req_fail(req, AXL_CANCELLED);
        return false;
    }
    if (status != AXL_OK) {
        on_recv_eof(req);
        return false;
    }

    size_t bytes = axl_tcp_recv_get_size(sock);
    if (bytes == 0) {
        on_recv_eof(req);
        return false;
    }
    req_touch_deadline(req);   /* bytes arrived — progress */

    if (c->tls_ctx != NULL) {
        /* Ciphertext landed in recv_chunk — stage it and drain every plaintext
           record it yields into rx (a single TCP read can carry several). */
        tls_ops()->stage_data(c->tls_ctx, req->recv_chunk, bytes);
        for (;;) {
            uint8_t plain[HTTP_CLIENT_RECV_BUF];
            size_t  out = 0;
            int rc = tls_ops()->read(c->tls_ctx, plain, sizeof(plain), &out);
            if (rc == 0 && out > 0) {
                if (rx_append(req, plain, out) != 0) {
                    req_fail(req, AXL_ERR);
                    return false;
                }
                continue;
            }
            if (rc == 1) {
                break;          /* WANT_READ — staging drained */
            }
            /* rc < 0: close-notify / mbedtls error. We've drained all the
               plaintext the staged ciphertext yields, and the body may already
               be complete — a server can coalesce the final TLS record and the
               close-notify alert into ONE TCP segment, so this read sees the
               close right after the last data we just appended. Check
               completion FIRST (req_advance returns false once it completes /
               fails / redirects); only a body that is still short here is a
               genuine truncation (on_recv_eof). The recv must not re-arm either
               way. */
            if (req_advance(req)) {
                on_recv_eof(req);
            }
            return false;
        }
    } else {
        if (rx_append(req, req->recv_chunk, bytes) != 0) {
            req_fail(req, AXL_ERR);
            return false;
        }
    }

    return req_advance(req);
}

static int
req_arm_recv(HttpAsyncReq *req)
{
    return axl_tcp_recv_async(req->client->sock, req->recv_chunk,
                              sizeof(req->recv_chunk), req->loop, req->cancel,
                              on_app_recv, req);
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

static int
req_send(HttpAsyncReq *req, const void *buf, size_t len, AxlTcpCallback cb)
{
    AxlHttpClient *c = req->client;
    if (c->tls_ctx != NULL) {
        /* The async TLS write op carries no cancellable, so a cancel requested
           mid-TLS-send is observed only at the next recv (or the whole-op
           deadline) — never a hang, just a small cancellation-latency gap on
           https. The plain path threads req->cancel through directly. */
        return tls_ops()->write_async(c->tls_ctx, buf, len, req->loop, cb, req);
    }
    return axl_tcp_send_async(c->sock, buf, len, req->loop, req->cancel,
                              cb, req);
}

static bool
on_send_done(AxlTcp *sock, AxlStatus status, void *data)
{
    (void)sock;
    HttpAsyncReq *req = (HttpAsyncReq *)data;

    if (status == AXL_CANCELLED) {
        req_fail(req, AXL_CANCELLED);
        return false;
    }
    if (status != AXL_OK) {
        req_transport_fail(req);
        return false;
    }
    req_touch_deadline(req);   /* a send completed — progress */

    if (!req->headers_sent) {
        req->headers_sent = true;
        if (req->body != NULL && req->body_size > 0) {
            if (req_send(req, req->body, req->body_size, on_send_done) != AXL_OK) {
                req_fail(req, AXL_ERR);
            }
            return false;
        }
    } else {
        req->body_sent = true;
    }

    /* Headers (and body, if any) are on the wire — start receiving. */
    if (req_arm_recv(req) != AXL_OK) {
        req_fail(req, AXL_ERR);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Request build
// ---------------------------------------------------------------------------

#define HTTP_ASYNC_REQ_BUF  2048

static int
req_build(HttpAsyncReq *req)
{
    AxlHttpClient *c      = req->client;
    AxlUrl        *parsed = req->parsed;

    char *buf = axl_malloc(HTTP_ASYNC_REQ_BUF);
    if (buf == NULL) {
        return AXL_ERR;
    }

    const char *req_path = parsed->path;
    if (req_path == NULL || req_path[0] == '\0') {
        req_path = "/";
    }
    char full_path[512];
    if (parsed->query != NULL && parsed->query[0] != '\0') {
        axl_snprintf(full_path, sizeof(full_path), "%s?%s", req_path, parsed->query);
    } else {
        axl_snprintf(full_path, sizeof(full_path), "%s", req_path);
    }

    size_t content_len = (req->body != NULL && req->body_size > 0)
                         ? req->body_size : 0;
    if (_axl_http_build_request(buf, HTTP_ASYNC_REQ_BUF, &req->req_len,
                                req->method, full_path, parsed->host,
                                content_len, /*chunked=*/false,
                                req->content_type, c->keep_alive,
                                c->default_headers, req->extra_headers) != AXL_OK) {
        axl_free(buf);
        return AXL_ERR;
    }
    req->req_buf = buf;
    return AXL_OK;
}

static void
req_start_send(HttpAsyncReq *req)
{
    if (req_build(req) != AXL_OK) {
        req_fail(req, AXL_ERR);
        return;
    }
    if (req_send(req, req->req_buf, req->req_len, on_send_done) != AXL_OK) {
        req_fail(req, AXL_ERR);
    }
}

// ---------------------------------------------------------------------------
// TLS handshake (client side)
// ---------------------------------------------------------------------------

static bool
on_tls_handshake_recv(AxlTcp *sock, AxlStatus status, void *data)
{
    HttpAsyncReq  *req = (HttpAsyncReq *)data;
    AxlHttpClient *c   = req->client;

    if (status == AXL_CANCELLED) {
        req_fail(req, AXL_CANCELLED);
        return false;
    }
    if (status != AXL_OK || c->tls_ctx == NULL) {
        req_fail(req, AXL_ERR);
        return false;
    }

    size_t bytes = axl_tcp_recv_get_size(sock);
    if (bytes > 0) {
        tls_ops()->stage_data(c->tls_ctx, req->recv_chunk, bytes);
        req_touch_deadline(req);   /* handshake bytes arrived — progress */
    }

    int rc = tls_ops()->handshake_async(c->tls_ctx, req->loop);
    if (rc == 0) {
        /* Handshake complete — send the request (arms its own recv). */
        req_start_send(req);
        return false;
    }
    if (rc > 0) {
        /* Need more handshake data — re-arm this recv. (A server pipelining
           bytes past the current flight into one segment would leave them
           staged in recv_chunk, lost on re-arm; an HTTP server doesn't send
           ahead of our request during the handshake, and this mirrors the
           proven sync handshake loop in ensure_connected.) */
        return true;
    }
    req_fail(req, AXL_ERR);
    return false;
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

static bool
on_connected(AxlTcp *sock, AxlStatus status, void *data)
{
    HttpAsyncReq  *req = (HttpAsyncReq *)data;
    AxlHttpClient *c   = req->client;

    if (status == AXL_CANCELLED) {
        /* on_connect_cancel already closed the partial socket. */
        c->sock = NULL;
        req_fail(req, AXL_CANCELLED);
        return false;
    }
    if (status != AXL_OK || sock == NULL) {
        /* Connect failure tore the socket down internally. */
        c->sock = NULL;
        req_fail(req, AXL_ERR);
        return false;
    }

    c->sock = sock;
    c->connected_host = axl_strdup(req->parsed->host);
    c->connected_port = req->parsed->port;
    req_touch_deadline(req);   /* connect done — reset the idle bound */

    if (req->https) {
        const AxlHttpClientTlsOps *ops = tls_ops();
        if (ops == NULL) {
            axl_error("https requires axl_tls_init() at startup");
            req_fail(req, AXL_ERR);
            return false;
        }
        c->tls_ctx = ops->connect(sock, req->parsed->host);
        if (c->tls_ctx == NULL) {
            axl_error("TLS context creation failed for %s", req->parsed->host);
            req_fail(req, AXL_ERR);
            return false;
        }
        /* Kick the handshake (sends ClientHello via async flush), then drive
           it from received ServerHello flights. */
        int rc = ops->handshake_async(c->tls_ctx, req->loop);
        if (rc < 0) {
            req_fail(req, AXL_ERR);
            return false;
        }
        if (rc == 0) {
            req_start_send(req);
            return false;
        }
        if (axl_tcp_recv_async(sock, req->recv_chunk, sizeof(req->recv_chunk),
                               req->loop, req->cancel, on_tls_handshake_recv,
                               req) != AXL_OK) {
            req_fail(req, AXL_ERR);
        }
        return false;
    }

    req_start_send(req);
    return false;
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

static void
on_resolved(const AxlIPv4Address *addr, AxlStatus st, void *user)
{
    HttpAsyncReq  *req = (HttpAsyncReq *)user;
    AxlHttpClient *c   = req->client;

    if (st != AXL_OK || addr == NULL) {
        axl_error("async: cannot resolve '%s'", req->parsed->host);
        req_fail(req, st == AXL_CANCELLED ? AXL_CANCELLED : AXL_ERR);
        return;
    }

    AxlIPv4Address  src;
    AxlIPv4Address *src_p = NULL;
    if (c->source_ip != NULL && c->source_ip[0] != '\0') {
        if (axl_ipv4_parse(c->source_ip, src.addr) != AXL_OK) {
            axl_error("source.ip='%s' is not a valid IPv4 address", c->source_ip);
            req_fail(req, AXL_ERR);
            return;
        }
        bool nonzero = src.addr[0] || src.addr[1] || src.addr[2] || src.addr[3];
        if (nonzero) {
            src_p = &src;
        }
    }

    /* Hand the in-progress socket straight into c->sock (out_pending) — set
       only on the AXL_OK return. The sync wrapper's Poll tick reads c->sock to
       drive the TCP4 state machine while it blocks at a raised TPL, and the
       connect handshake is exactly the window before on_connected runs; without
       this the connect would never advance there. on_connected re-stamps the
       same pointer; a connect failure tears the socket down internally and
       on_connected then NULLs c->sock. */
    if (axl_tcp_connect_addr_async(addr, req->parsed->port, src_p, req->loop,
                                   req->cancel, on_connected, req,
                                   &c->sock) != AXL_OK) {
        req_fail(req, AXL_ERR);
    }
}

// ---------------------------------------------------------------------------
// Hop entry — parse URL, reuse or (resolve -> connect)
// ---------------------------------------------------------------------------

static void
req_begin_hop(HttpAsyncReq *req)
{
    AxlHttpClient *c = req->client;

    if (axl_url_parse(req->url, &req->parsed) != AXL_OK) {
        req_fail(req, AXL_ERR);
        return;
    }
    req->https = (axl_strcmp(req->parsed->scheme, "https") == 0);
    c->tls_enabled = req->https;

    /* Reuse an open keep-alive connection to the same host:port:scheme. */
    bool reuse = (c->sock != NULL && c->connected_host != NULL &&
                  axl_strcmp(c->connected_host, req->parsed->host) == 0 &&
                  c->connected_port == req->parsed->port &&
                  (req->https == (c->tls_ctx != NULL)));
    req->reused = reuse;
    if (reuse) {
        req_start_send(req);
        return;
    }

    /* Different / no connection — drop any stale one and dial fresh. */
    req_drop_connection(req);

    if (req->https && tls_ops() == NULL) {
        axl_error("https requires axl_tls_init() at startup (build AXL_TLS=1)");
        req_fail(req, AXL_ERR);
        return;
    }

    if (axl_net_resolve_async(req->parsed->host, req->loop, req->cancel,
                              on_resolved, req) != AXL_OK) {
        req_fail(req, AXL_ERR);
    }
}

// ---------------------------------------------------------------------------
// Whole-op deadline
// ---------------------------------------------------------------------------

static bool
on_req_timeout(void *data)
{
    req_fail((HttpAsyncReq *)data, AXL_TIMEOUT);
    return AXL_SOURCE_REMOVE;
}

/* Re-arm the deadline after observable progress (connect done, send completed,
   bytes received). This makes `timeout.ms` an IDLE bound — "no single phase
   stalls longer than timeout.ms" — preserving the old sync client's per-op
   timeout semantics (it reset a fresh budget on every send/recv), so a slow but
   steadily-progressing transfer no longer trips a whole-op ceiling. */
static void
req_touch_deadline(HttpAsyncReq *req)
{
    if (req->finished) {
        return;
    }
    axl_loop_remove_source(req->loop, req->timeout_source);
    req->timeout_source = axl_loop_add_timeout(req->loop, req->idle_ms,
                                               on_req_timeout, req);
}

/* Deferred first-hop start. Scheduling the first transition on the loop (rather
   than running it inline) guarantees the contract: a call that returns AXL_OK
   NEVER fires the user callback re-entrantly — it always fires later, from a
   loop dispatch, even when the very first step (URL parse, resolve, connect)
   fails synchronously. */
static bool
on_req_kick(void *data)
{
    HttpAsyncReq *req = (HttpAsyncReq *)data;
    req->kick_source = 0;   /* self-removing; clear so req_free won't re-remove */
    req_begin_hop(req);
    return AXL_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// Public API + internal entries
// ---------------------------------------------------------------------------

int
_axl_http_request_async(AxlHttpClient *c, AxlLoop *loop, const char *method,
                        const char *url, const void *body, size_t size,
                        const char *content_type, AxlHashTable *extra_headers,
                        AxlCancellable *cancel, AxlHttpClientDoneFn cb,
                        void *user, bool sync_close)
{
    if (c == NULL || loop == NULL || url == NULL || method == NULL) {
        return AXL_ERR;
    }
    if (c->async_busy) {
        return AXL_BUSY;
    }

    HttpAsyncReq *req = axl_calloc(1, sizeof(*req));
    if (req == NULL) {
        return AXL_ERR;
    }
    req->client        = c;
    req->loop          = loop;
    req->sync_close    = sync_close;
    req->cancel        = cancel;
    req->cb            = cb;
    req->user          = user;
    req->method        = axl_strdup(method);
    req->url           = axl_strdup(url);
    req->body          = body;
    req->body_size     = size;
    req->extra_headers = extra_headers;   /* borrowed until cb */
    req->content_type  = (content_type != NULL) ? axl_strdup(content_type) : NULL;

    if (req->method == NULL || req->url == NULL ||
        (content_type != NULL && req->content_type == NULL)) {
        axl_free(req->method);
        axl_free(req->url);
        axl_free(req->content_type);
        axl_free(req);
        return AXL_ERR;
    }

    /* IDLE deadline: timeout.ms bounds each phase (connect, handshake, send,
       each recv), re-armed on progress (req_touch_deadline) — not the whole
       operation. The FIRST arming covers the connect phase, honoring
       connect.timeout.ms when set (0 inherits timeout.ms). timeout_ms is
       always set on a real client (default 10000); the 30000 floor only guards
       a misconfigured 0. */
    req->idle_ms = c->timeout_ms > 0 ? (uint32_t)c->timeout_ms : 30000;
    uint32_t connect_ms = c->connect_timeout_ms > 0
                          ? (uint32_t)c->connect_timeout_ms : req->idle_ms;
    req->timeout_source = axl_loop_add_timeout(loop, connect_ms,
                                               on_req_timeout, req);
    if (req->timeout_source == 0) {
        axl_free(req->method);
        axl_free(req->url);
        axl_free(req->content_type);
        axl_free(req);
        return AXL_ERR;
    }

    /* Defer the first hop to the next tick so the callback is NEVER re-entrant
       (see on_req_kick). axl_loop_add_timeout rejects a 0 delay; 1 ms means
       "next tick". If even scheduling fails, report the error WITHOUT firing
       cb (it never started). */
    req->kick_source = axl_loop_add_timeout(loop, 1, on_req_kick, req);
    if (req->kick_source == 0) {
        axl_loop_remove_source(loop, req->timeout_source);
        axl_free(req->method);
        axl_free(req->url);
        axl_free(req->content_type);
        axl_free(req);
        return AXL_ERR;
    }

    c->async_busy = true;
    return AXL_OK;
}

int
axl_http_get_async(AxlHttpClient *c, AxlLoop *loop, const char *url,
                   AxlCancellable *cancel, AxlHttpClientDoneFn cb, void *user)
{
    return _axl_http_request_async(c, loop, "GET", url, NULL, 0, NULL, NULL,
                                   cancel, cb, user, false);
}

int
axl_http_post_async(AxlHttpClient *c, AxlLoop *loop, const char *url,
                    const void *body, size_t size, const char *content_type,
                    AxlCancellable *cancel, AxlHttpClientDoneFn cb, void *user)
{
    return _axl_http_request_async(c, loop, "POST", url, body, size,
                                   content_type, NULL, cancel, cb, user, false);
}

// ---------------------------------------------------------------------------
// Synchronous wrapper — the single ephemeral-loop driver all sync entry points
// (axl_http_get/post/put/delete/request) share. The sync API is allowed to spin
// a private loop (that is the difference from the async API); a caller already
// inside a loop callback that does this trips the warn-only re-entrancy guard,
// exactly as the old sync client did.
// ---------------------------------------------------------------------------

typedef struct {
    AxlHttpClientResponse *resp;
    AxlStatus              st;
    AxlLoop               *loop;
} HttpSyncResult;

static void
on_http_sync_done(AxlHttpClientResponse *resp, AxlStatus st, void *user)
{
    HttpSyncResult *r = (HttpSyncResult *)user;
    r->resp = resp;     /* on failure resp is NULL; on success we own it */
    r->st   = st;
    axl_loop_quit(r->loop);
}

/* 10 ms — matches the sync TCP wrappers' Poll cadence. */
#define HTTP_SYNC_POLL_TICK_MS  10u

/* Drive the client's CURRENT socket's TCP4 state machine while the ephemeral
   loop blocks. At TPL_APPLICATION the firmware notify advances the async ops
   for free and this tick is never armed; but when a sync HTTP call runs at a
   raised TPL (from inside a driver-pump notify), the blocking loop holds that
   TPL and starves the TCP4 notify, so the connect/handshake/send/recv would
   stall to the deadline. The socket is created mid-run (during connect) and
   replaced on redirect, so the tick re-reads c->sock each fire rather than
   capturing a fixed handle. (DNS is polled by axl_net_resolve_async itself.) */
static bool
http_sync_poll_tick(void *data)
{
    AxlHttpClient *c = (AxlHttpClient *)data;
    if (c->sock != NULL && c->sock->tcp4 != NULL) {
        axl_efi_call(c->sock->tcp4->Poll, 1, c->sock->tcp4);
    }
    return AXL_SOURCE_CONTINUE;
}

int
_axl_http_request_sync(AxlHttpClient *c, const char *method, const char *url,
                       const void *body, size_t size, const char *content_type,
                       AxlHashTable *extra_headers,
                       AxlHttpClientResponse **out_resp)
{
    if (c == NULL || url == NULL || out_resp == NULL) {
        return AXL_ERR;
    }
    *out_resp = NULL;

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    HttpSyncResult r = { .resp = NULL, .st = AXL_ERR, .loop = loop };
    if (_axl_http_request_async(c, loop, method, url, body, size, content_type,
                                extra_headers, NULL, on_http_sync_done,
                                &r, true) != AXL_OK) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    /* Only needed at a raised TPL (see http_sync_poll_tick); at TPL_APPLICATION
       a 10 ms timer would just burn CPU on a long transfer. */
    AxlSourceId poll_src = axl_backend_at_raised_tpl()
        ? axl_loop_add_timer(loop, HTTP_SYNC_POLL_TICK_MS, http_sync_poll_tick, c)
        : 0;

    axl_loop_run(loop);

    if (poll_src != 0) {
        axl_loop_remove_source(loop, poll_src);
    }

    /* The async ops stamped c->sock->async_loop with this ephemeral loop. Clear
       it before the loop is freed so a later axl_tcp_close on the kept-alive
       socket (or the next sync call) does not dereference a freed loop while
       choosing its sync-vs-async finalize path (mirrors axl_tcp_connect_timeout).
       Also zero the recv source ids: on a keep-alive completion on_recv_complete
       removed the recv sources from this (now-freed) loop but left the ids set
       (it must not touch a sock the callback may have freed); the next recv_async
       on a reused socket would otherwise issue a needless tcp4->Cancel on the
       already-completed token (same reasoning as axl-tcp-sync.c's recv wrapper). */
    if (c->sock != NULL) {
        c->sock->async_loop        = NULL;
        c->sock->recv_source       = 0;
        c->sock->recv_cancel_source = 0;
    }
    axl_loop_free(loop);

    if (r.st == AXL_OK) {
        *out_resp = r.resp;
        return AXL_OK;
    }
    return AXL_ERR;   /* r.resp is NULL on every failure path */
}
