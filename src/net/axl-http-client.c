/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-client.c
    AxlHttpClient — HTTP client with redirect following.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-str.h>
#include <axl/axl-format.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-config.h>
#include "axl-net-internal.h"
#include "axl-http-client-tls.h"
#include "axl-http-client-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// TLS indirection — see axl-http-client-tls.h. The client never references
// axl_tls_* directly; axl_tls_init() registers these ops, so a plain-HTTP
// consumer (no TLS reference) lets the linker strip mbedTLS entirely.
// ---------------------------------------------------------------------------

static const AxlHttpClientTlsOps *g_http_tls_ops;

void
axl_http_client_set_tls_ops(const AxlHttpClientTlsOps *ops)
{
    g_http_tls_ops = ops;
}

const AxlHttpClientTlsOps *
_axl_http_client_tls_ops(void)
{
    return g_http_tls_ops;
}

/* NULL-safe TLS free via the registered ops. A non-NULL ctx implies the ops
   were registered (the ctx was created by ops->connect), so the guard is
   belt-and-suspenders. */
static void
client_tls_free(AxlTlsContext *ctx)
{
    if (ctx != NULL && g_http_tls_ops != NULL) {
        g_http_tls_ops->free(ctx);
    }
}

// ---------------------------------------------------------------------------
// Internal client structure — struct AxlHttpClient + HTTP_CLIENT_RECV_BUF live
// in axl-http-client-internal.h (shared with the async state machine).
// ---------------------------------------------------------------------------

static const AxlConfigDesc http_client_descs[] = {
    { "timeout.ms",    AXL_CFG_UINT, "10000",
      "Idle/per-phase timeout in ms - bounds each phase (connect, handshake, "
      "send, each recv), re-armed on progress (not a whole-operation ceiling)",
      offsetof(struct AxlHttpClient, timeout_ms), sizeof(size_t) },
    { "connect.timeout.ms", AXL_CFG_UINT, "0",
      "Connect-phase timeout in ms (0 = inherit timeout.ms)",
      offsetof(struct AxlHttpClient, connect_timeout_ms), sizeof(size_t) },
    { "keep.alive",    AXL_CFG_BOOL, "true", "Reuse TCP connections",
      offsetof(struct AxlHttpClient, keep_alive), sizeof(bool) },
    { "max.redirects", AXL_CFG_INT,  "5", "HTTP redirect limit",
      offsetof(struct AxlHttpClient, max_redirects), sizeof(int) },
    { "tls.verify",    AXL_CFG_BOOL, "true", "TLS certificate verification",
      offsetof(struct AxlHttpClient, tls_verify), sizeof(bool) },
    { "source.ip",     AXL_CFG_STRING, "", "Pin connect to interface with this station IP (dotted-quad, empty = auto)",
      offsetof(struct AxlHttpClient, source_ip), sizeof(char *) },
    { 0 }
};

static int
http_client_apply(void *target, const char *key, const char *value)
{
    AxlHttpClient *c = (AxlHttpClient *)target;

    /* "header.*" — dynamic keys, store in hash table */
    if (axl_strlen(key) > 7 && axl_strncmp(key, "header.", 7) == 0) {
        axl_hash_table_replace(c->default_headers,
                           axl_strdup(key + 7), axl_strdup(value));
        return 1;  /* handled */
    }

    return 0;  /* not handled — proceed with descriptor lookup */
}

// ---------------------------------------------------------------------------
// Shared request-header builder (overflow-safe). axl_snprintf returns the
// would-have-written count (C99), so the `len += axl_snprintf(buf + len,
// cap - len, ...)` idiom underflows `cap - len` and writes OOB once `len`
// passes `cap`. This bounded writer never writes past `cap` and records
// truncation in `overflow` instead. Shared by the sync streaming path
// (do_request) and the async core so there is ONE builder.
// ---------------------------------------------------------------------------

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;        /* bytes actually written, always <= cap */
    bool    overflow;   /* a write was truncated */
} ReqBuf;

static void
reqbuf_write(const char *data, size_t n, void *arg)
{
    ReqBuf *b = (ReqBuf *)arg;
    for (size_t i = 0; i < n; i++) {
        if (b->len < b->cap) {
            b->buf[b->len++] = data[i];
        } else {
            b->overflow = true;
        }
    }
}

static void
reqbuf_append(ReqBuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void
reqbuf_append(ReqBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    axl_vformat(reqbuf_write, b, fmt, ap);
    va_end(ap);
}

static void
reqbuf_emit_header(const void *key, void *value, void *data)
{
    reqbuf_append((ReqBuf *)data, "%s: %s\r\n",
                  (const char *)key, (const char *)value);
}

int
_axl_http_build_request(
    char         *buf,
    size_t        cap,
    size_t       *out_len,
    const char   *method,
    const char   *full_path,
    const char   *host,
    size_t        content_length,
    bool          chunked,
    const char   *content_type,
    bool          keep_alive,
    AxlHashTable *default_headers,
    AxlHashTable *extra_headers)
{
    ReqBuf b = { buf, cap, 0, false };

    reqbuf_append(&b, "%s %s HTTP/1.1\r\n", method, full_path);
    reqbuf_append(&b, "Host: %s\r\n", host);

    bool has_body = chunked || content_length > 0;
    if (chunked) {
        reqbuf_append(&b, "Transfer-Encoding: chunked\r\n");
    } else if (content_length > 0) {
        reqbuf_append(&b, "Content-Length: %llu\r\n",
                      (unsigned long long)content_length);
    }
    if (has_body && content_type != NULL) {
        reqbuf_append(&b, "Content-Type: %s\r\n", content_type);
    }

    reqbuf_append(&b, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close");

    if (default_headers != NULL) {
        axl_hash_table_foreach(default_headers, reqbuf_emit_header, &b);
    }
    if (extra_headers != NULL) {
        axl_hash_table_foreach(extra_headers, reqbuf_emit_header, &b);
    }

    reqbuf_append(&b, "\r\n");

    if (b.overflow) {
        axl_error("request headers exceed %zu bytes", cap);
        return AXL_ERR;
    }
    *out_len = b.len;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_http_client_new / axl_http_client_free
// ---------------------------------------------------------------------------

AxlHttpClient *
axl_http_client_new(void)
{
    AXL_AUTOPTR(AxlHttpClient) c = axl_calloc(1, sizeof(AxlHttpClient));
    if (c == NULL) {
        return NULL;
    }

    c->default_headers = axl_hash_table_new_full(
        NULL, NULL, axl_free_impl, axl_free_impl);
    if (c->default_headers == NULL) {
        return NULL;
    }

    c->config = axl_config_new(http_client_descs, http_client_apply, c);
    if (c->config == NULL) {
        return NULL;
    }

    /* Defaults auto-applied: timeout_ms=10000, keep_alive=true, etc. */
    return axl_steal_pointer(&c);
}

void
axl_http_client_free(AxlHttpClient *c)
{
    if (c == NULL) {
        return;
    }

    if (c->tls_ctx != NULL) {
        client_tls_free(c->tls_ctx);
    }

    if (c->sock != NULL) {
        axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
    }

    axl_free(c->connected_host);
    axl_config_free(c->config);

    if (c->default_headers != NULL) {
        axl_hash_table_free(c->default_headers);
    }

    axl_free(c);
}

// ---------------------------------------------------------------------------
// Configuration: set / get (thin wrappers over AxlConfig)
// ---------------------------------------------------------------------------

int
axl_http_client_set(AxlHttpClient *c, const char *key, const char *value)
{
    if (c == NULL || key == NULL || value == NULL) {
        return AXL_ERR;
    }

    return axl_config_set(c->config, key, value);
}

const char *
axl_http_client_get(AxlHttpClient *c, const char *key)
{
    if (c == NULL || key == NULL) {
        return NULL;
    }

    /* "header.*" stored in hash table, not in config values */
    if (axl_strlen(key) > 7 && axl_strncmp(key, "header.", 7) == 0) {
        return (const char *)axl_hash_table_lookup(c->default_headers, key + 7);
    }

    return axl_config_get(c->config, key);
}

// ---------------------------------------------------------------------------
// Internal: connect to host:port if not already connected
// ---------------------------------------------------------------------------

static int
ensure_connected(
    AxlHttpClient  *c,
    const char     *host,
    uint16_t        port)
{
    /* Reuse existing connection if same host:port */
    if (c->sock != NULL && c->connected_host != NULL &&
        axl_strcmp(c->connected_host, host) == 0 &&
        c->connected_port == port)
    {
        return 0;
    }

    /* Close existing connection */
    if (c->tls_ctx != NULL) {
        client_tls_free(c->tls_ctx);
        c->tls_ctx = NULL;
    }
    if (c->sock != NULL) {
        axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
        c->sock = NULL;
    }

    axl_free(c->connected_host);
    c->connected_host = NULL;

    /* Connect. If `source.ip` is set (non-empty, non-zero, parseable),
       pin to that interface — fail fast on a malformed value rather
       than silently falling through to auto-pick: an explicit pin that
       silently isn't honored defeats the whole point of the flag. */
    AxlIPv4Address  src   = { 0 };
    AxlIPv4Address *src_p = NULL;
    if (c->source_ip != NULL && c->source_ip[0] != '\0') {
        if (axl_ipv4_parse(c->source_ip, src.addr) != AXL_OK) {
            axl_error("source.ip='%s' is not a valid IPv4 address",
                      c->source_ip);
            return -1;
        }
        bool nonzero = src.addr[0] || src.addr[1]
                    || src.addr[2] || src.addr[3];
        if (nonzero) {
            src_p = &src;
        }
        /* "0.0.0.0" string falls through to auto-pick — a documented
           way for callers to spell "no preference" using the same
           string slot, mirroring the C-level (NULL || zero) contract
           on axl_tcp_connect_via. */
    }
    /* Bound the connect phase too — not just send/recv. connect.timeout.ms
       overrides; 0 inherits timeout.ms, so a caller that sets only
       timeout.ms=2000 gets a 2 s connect bound instead of the old hardcoded
       10 s (the surprise this knob fixes). */
    size_t connect_ms = (c->connect_timeout_ms > 0)
                            ? c->connect_timeout_ms : c->timeout_ms;
    if (axl_tcp_connect_timeout(host, port, src_p, connect_ms, &c->sock) != AXL_OK) {
        return -1;
    }

    /* TLS handshake if enabled */
    if (c->tls_enabled) {
        if (g_http_tls_ops == NULL) {
            axl_error("https requires TLS: call axl_tls_init() once at startup "
                      "(and build with AXL_TLS=1) before issuing https:// "
                      "requests");
            axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
            c->sock = NULL;
            return -1;
        }

        c->tls_ctx = g_http_tls_ops->connect(c->sock, host);
        if (c->tls_ctx == NULL) {
            axl_error("TLS context creation failed for %s:%u", host, port);
            axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
            c->sock = NULL;
            return -1;
        }

        /* Blocking handshake */
        bool hs_done = false;
        for (int i = 0; i < 100; i++) {
            uint8_t hs_buf[4096];
            size_t  hs_len = sizeof(hs_buf);
            if (axl_tcp_recv(c->sock, hs_buf, &hs_len,
                             200) == AXL_OK && hs_len > 0) {
                g_http_tls_ops->stage_data(c->tls_ctx, hs_buf, hs_len);
            }
            int rc = g_http_tls_ops->handshake(c->tls_ctx);
            if (rc == 0) {
                hs_done = true;
                break;
            }
            if (rc < 0) {
                break;
            }
        }
        if (!hs_done) {
            axl_warning("TLS handshake failed for %s:%u", host, port);
            client_tls_free(c->tls_ctx);
            c->tls_ctx = NULL;
            axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
            c->sock = NULL;
            return -1;
        }
    }

    c->connected_host = axl_strdup(host);
    c->connected_port = port;
    return 0;
}

// ---------------------------------------------------------------------------
// Internal: send an HTTP request and receive response
// ---------------------------------------------------------------------------

/* TLS-aware send/recv helpers */
static int
client_send(AxlHttpClient *c, const void *data, size_t len)
{
    if (c->tls_ctx != NULL) {
        return g_http_tls_ops->write(c->tls_ctx, data, len);
    }
    return axl_tcp_send(c->sock, data, len, c->timeout_ms);
}

/// Drain plaintext from mbedtls into `dst[0..want]` until either the
/// destination fills, mbedtls signals WANT_READ (staging exhausted),
/// or the connection closes. Returns plaintext bytes written.
/// `*closed` is set true if peer sent close-notify or mbedtls error.
static size_t
tls_drain(
    AxlTlsContext *ctx,
    uint8_t       *dst,
    size_t         want,
    bool          *closed)
{
    size_t got = 0;
    while (got < want) {
        size_t out = 0;
        int    rc  = g_http_tls_ops->read(ctx, dst + got, want - got, &out);
        if (rc == 0 && out > 0) {
            got += out;
            continue;
        }
        if (rc == 1) {
            break;  /* WANT_READ — staging exhausted */
        }
        /* rc < 0 (mbedtls error / close-notify) or rc == 0 with
           out == 0 (shouldn't happen per axl_tls_read contract). */
        *closed = true;
        break;
    }
    return got;
}

static int
client_recv(AxlHttpClient *c, void *buf, size_t *len)
{
    if (c->tls_ctx != NULL) {
        /* TLS pattern (mirrors softbmc HttpServer.c TlsShimRead loop):
           1. Drain any plaintext from already-staged ciphertext into
              the caller's buf. The BIO's stage_buf points to our
              persistent c->tls_rx_buf, which remains valid across
              client_recv calls (and is never aliased with the
              caller's plaintext destination, so mbedtls's
              record-decrypt can't overwrite still-staged ciphertext).
           2. If we got nothing, fetch a fresh TCP burst into
              c->tls_rx_buf, restage, and drain again. A single TCP
              burst can carry multiple TLS records; the drain loop
              extracts them all in one call so the next stage_data
              doesn't replace unconsumed ciphertext. */
        size_t want   = *len;
        bool   closed = false;

        size_t got = tls_drain(c->tls_ctx, (uint8_t *)buf, want, &closed);
        if (got > 0) {
            *len = got;
            return 0;
        }
        if (closed) {
            return -1;
        }

        size_t raw_len = sizeof(c->tls_rx_buf);
        if (axl_tcp_recv(c->sock, c->tls_rx_buf, &raw_len,
                         c->timeout_ms) != AXL_OK) {
            return -1;
        }
        if (raw_len == 0) {
            *len = 0;
            return 0;
        }
        g_http_tls_ops->stage_data(c->tls_ctx, c->tls_rx_buf, raw_len);

        got = tls_drain(c->tls_ctx, (uint8_t *)buf, want, &closed);
        *len = got;
        if (got == 0 && closed) {
            return -1;
        }
        return 0;
    }
    return axl_tcp_recv(c->sock, buf, len, c->timeout_ms);
}

/// Read a Transfer-Encoding: chunked response body.
///
/// `initial` / `initial_len` are the bytes already received past the
/// blank-line that ends the headers. The function consumes those plus
/// reads more from the socket, decoding chunk framing until the
/// terminating zero-sized chunk + (possibly empty) trailers.
///
/// On success: `*out_body` is malloc'd (axl_free) and `*out_size` is
/// the decoded byte count. Returns 0 / -1.
static int
read_chunked_body(
    AxlHttpClient *c,
    const char    *initial,
    size_t         initial_len,
    void         **out_body,
    size_t        *out_size)
{
    enum { ST_SIZE, ST_DATA, ST_TRAIL, ST_TRAILERS, ST_DONE };
    int    state = ST_SIZE;
    size_t chunk_remaining = 0;

    /* work_buf carries undecoded bytes pending parsing. Sized to match
       the recv buffer so we can absorb everything that came in with the
       headers without reallocation. */
    char   work_buf[HTTP_CLIENT_RECV_BUF];
    size_t work_len = 0;
    if (initial_len > sizeof(work_buf)) {
        return -1;
    }
    if (initial_len > 0) {
        axl_memcpy(work_buf, initial, initial_len);
        work_len = initial_len;
    }

    /* Output body — grows by doubling as chunks arrive. */
    void  *body      = NULL;
    size_t body_cap  = 0;
    size_t body_size = 0;

    while (state != ST_DONE) {
        bool progress = false;

        if (state == ST_SIZE) {
            char *crlf = (char *)axl_memmem(work_buf, work_len, "\r\n", 2);
            if (crlf != NULL) {
                size_t   line_len = (size_t)(crlf - work_buf);
                uint64_t sz       = 0;
                /* axl_hex_parse_u64 stops at first non-hex byte (chunk
                   extension `;` or end-of-line) and returns -1 if no
                   digit was present, which means a malformed line. */
                if (axl_hex_parse_u64(work_buf, line_len, &sz) < 0) {
                    axl_free(body);
                    return -1;
                }
                size_t consumed = line_len + 2;
                axl_memmove(work_buf, work_buf + consumed,
                            work_len - consumed);
                work_len -= consumed;
                chunk_remaining = (size_t)sz;
                state = (sz == 0) ? ST_TRAILERS : ST_DATA;
                progress = true;
            }
        } else if (state == ST_DATA) {
            size_t take = chunk_remaining < work_len
                          ? chunk_remaining : work_len;
            if (take > 0) {
                if (body_size + take > body_cap) {
                    size_t new_cap = body_cap == 0 ? 256 : body_cap * 2;
                    while (new_cap < body_size + take) {
                        new_cap *= 2;
                    }
                    void *nb = axl_realloc(body, new_cap);
                    if (nb == NULL) {
                        axl_free(body);
                        return -1;
                    }
                    body = nb;
                    body_cap = new_cap;
                }
                axl_memcpy((uint8_t *)body + body_size, work_buf, take);
                body_size += take;
                chunk_remaining -= take;
                axl_memmove(work_buf, work_buf + take, work_len - take);
                work_len -= take;
                progress = true;
            }
            if (chunk_remaining == 0) {
                state = ST_TRAIL;
            }
        } else if (state == ST_TRAIL) {
            if (work_len >= 2) {
                if (work_buf[0] != '\r' || work_buf[1] != '\n') {
                    axl_free(body);
                    return -1;
                }
                axl_memmove(work_buf, work_buf + 2, work_len - 2);
                work_len -= 2;
                state = ST_SIZE;
                progress = true;
            }
        } else if (state == ST_TRAILERS) {
            /* Drain optional trailer-fields. Each is CRLF-terminated;
               an empty line (CRLF at buffer start) ends the body. */
            char *crlf = (char *)axl_memmem(work_buf, work_len, "\r\n", 2);
            if (crlf != NULL) {
                size_t line_len = (size_t)(crlf - work_buf);
                size_t consumed = line_len + 2;
                axl_memmove(work_buf, work_buf + consumed,
                            work_len - consumed);
                work_len -= consumed;
                if (line_len == 0) {
                    state = ST_DONE;
                }
                progress = true;
            }
        }

        if (progress) {
            continue;
        }

        /* Need more data. */
        if (work_len >= sizeof(work_buf)) {
            axl_free(body);
            return -1;
        }
        size_t want = sizeof(work_buf) - work_len;
        if (client_recv(c, work_buf + work_len, &want) != 0) {
            axl_free(body);
            return -1;
        }
        /* `want == 0` with success is the TLS `MBEDTLS_ERR_SSL_WANT_READ`
           case: bytes arrived on the wire but didn't complete a TLS
           record yet. Retry — `client_recv` returns -1 on real EOF /
           close-notify / TCP timeout, so this can't loop forever. */
        work_len += want;
        _axl_poll_break();
    }

    *out_body = body;
    *out_size = body_size;
    return 0;
}

/* Streaming-request I/O. The contiguous-body sync entry points
   (axl_http_get/post/put/delete/request) now wrap the async core, so this path
   serves ONLY axl_http_request_streaming — a producer-callback body framed as
   Transfer-Encoding: chunked when @p stream_total_size == (size_t)-1, else
   Content-Length: stream_total_size with byte-count verification at EOF.
   Streaming bodies are a one-shot producer, so they are NOT replayed on a stale
   connection and NOT redirect-followed (a 3xx is returned as-is for the caller
   to re-issue with a fresh streamer). */
static int
do_streaming_request(
    AxlHttpClient          *c,
    const char             *method,
    const char             *url,
    AxlRequestBodyStreamer  streamer,
    void                   *stream_ctx,
    size_t                  stream_total_size,
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **resp)
{
    AxlUrl                 *parsed;
    char                    req_buf[2048];
    size_t                  req_len;
    char                    recv_buf[HTTP_CLIENT_RECV_BUF];
    size_t                  recv_len;
    size_t                  total_recv;
    size_t                  header_end;
    size_t                  status_code;
    AxlHashTable           *resp_headers;
    size_t                  resp_content_len;
    void                   *resp_body;
    AxlHttpClientResponse  *r;

    if (resp == NULL) {
        return -1;
    }

    *resp = NULL;

    /* Parse URL */
    if (axl_url_parse(url, &parsed) != AXL_OK) {
        return -1;
    }

    /* Enable TLS for HTTPS URLs */
    if (axl_strcmp(parsed->scheme, "https") == 0) {
        if (g_http_tls_ops == NULL) {
            axl_url_free(parsed);
            axl_error("https requires TLS: build with AXL_TLS=1 and call "
                      "axl_tls_init() once at startup");
            return -1;
        }
        c->tls_enabled = true;
    } else {
        c->tls_enabled = false;
    }

    /* Connect */
    if (ensure_connected(c, parsed->host, parsed->port) != 0) {
        axl_url_free(parsed);
        return -1;
    }

    /* Build request */
    const char *req_path = parsed->path;
    if (req_path == NULL || req_path[0] == '\0') {
        req_path = "/";
    }

    /* Build path with query string if present */
    char full_path[512];
    if (parsed->query != NULL && parsed->query[0] != '\0') {
        axl_snprintf(full_path, sizeof(full_path), "%s?%s",
                     req_path, parsed->query);
    } else {
        axl_snprintf(full_path, sizeof(full_path), "%s", req_path);
    }

    /* Body framing: chunked when the total size is unknown ((size_t)-1), else
       Content-Length: stream_total_size. */
    bool   stream_chunked  = (stream_total_size == (size_t)-1);
    size_t req_content_len = stream_chunked ? 0 : stream_total_size;

    if (_axl_http_build_request(req_buf, sizeof(req_buf), &req_len,
                                method, full_path, parsed->host,
                                req_content_len, stream_chunked, content_type,
                                c->keep_alive, c->default_headers,
                                extra_headers) != AXL_OK) {
        axl_url_free(parsed);
        return -1;
    }

    /* Send request (with auto-reconnect on stale connection).
       The retry path replays the header send — safe because the
       header buffer is still valid. For streaming bodies, retry
       is permitted only on a header-send failure BEFORE any body
       bytes have been pulled from the streamer (the streamer is a
       one-shot pipe). The producer's first pull happens below, so
       retrying the header send is still safe here. */
    if (client_send(c, req_buf, req_len) != AXL_OK) {
        if (c->tls_ctx != NULL) {
            client_tls_free(c->tls_ctx);
            c->tls_ctx = NULL;
        }
        axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
        c->sock = NULL;
        axl_free(c->connected_host);
        c->connected_host = NULL;

        if (ensure_connected(c, parsed->host, parsed->port) != 0 ||
            client_send(c, req_buf, req_len) != AXL_OK)
        {
            axl_url_free(parsed);
            return -1;
        }
    }

    /* Send the streaming body — Content-Length or chunked transfer.
       8 KiB pull buffer (same size class the upload route uses), stack-resident. */
    {
        unsigned char chunk[8192];
        uint64_t bytes_sent = 0;
        for (;;) {
            size_t got = 0;
            if (streamer(stream_ctx, chunk, sizeof(chunk), &got) != AXL_OK) {
                axl_warning("streaming request body: producer returned error");
                axl_url_free(parsed);
                return -1;
            }
            if (got == 0) {
                /* End of body. */
                if (stream_chunked) {
                    static const char eof_chunk[] = "0\r\n\r\n";
                    if (client_send(c, eof_chunk,
                                    sizeof(eof_chunk) - 1) != AXL_OK)
                    {
                        axl_url_free(parsed);
                        return -1;
                    }
                } else if (bytes_sent != stream_total_size) {
                    axl_error("streaming request body: producer EOF after "
                              "%llu bytes, declared Content-Length %llu",
                              (unsigned long long)bytes_sent,
                              (unsigned long long)stream_total_size);
                    axl_url_free(parsed);
                    return -1;
                }
                break;
            }
            if (stream_chunked) {
                char hex_hdr[32];
                int hlen = axl_snprintf(hex_hdr, sizeof(hex_hdr),
                                        "%zx\r\n", got);
                if (hlen < 0 || (size_t)hlen >= sizeof(hex_hdr)) {
                    axl_url_free(parsed);
                    return -1;
                }
                if (client_send(c, hex_hdr, (size_t)hlen) != AXL_OK ||
                    client_send(c, chunk, got) != AXL_OK ||
                    client_send(c, "\r\n", 2) != AXL_OK)
                {
                    axl_url_free(parsed);
                    return -1;
                }
            } else {
                /* Content-Length transfer: defend against a producer
                   overshooting the declared length. */
                if (bytes_sent + got > stream_total_size) {
                    axl_error("streaming request body: producer over-ran "
                              "declared Content-Length %llu",
                              (unsigned long long)stream_total_size);
                    axl_url_free(parsed);
                    return -1;
                }
                if (client_send(c, chunk, got) != AXL_OK) {
                    axl_url_free(parsed);
                    return -1;
                }
            }
            bytes_sent += got;
        }
    }

    /* Receive response headers */
    total_recv = 0;
    header_end = 0;

    while (total_recv < sizeof(recv_buf)) {
        recv_len = sizeof(recv_buf) - total_recv;
        if (client_recv(c, recv_buf + total_recv, &recv_len) != 0) {
            /* A streaming body has already drained its one-shot producer, so we
               can't replay the request on a stale connection — fail. */
            axl_url_free(parsed);
            return -1;
        }

        total_recv += recv_len;

        header_end = axl_http_find_header_end(recv_buf, total_recv);
        if (header_end > 0) {
            break;
        }
    }

    if (header_end == 0) {
        axl_url_free(parsed);
        axl_error("response headers too large or incomplete");
        return -1;
    }

    /* Parse status line */
    size_t first_line_end = 0;
    for (size_t j = 0; j + 1 < header_end; j++) {
        if (recv_buf[j] == '\r' && recv_buf[j + 1] == '\n') {
            first_line_end = j;
            break;
        }
    }

    int status = axl_http_parse_status_line(recv_buf, first_line_end,
                                            &status_code);
    if (status != AXL_OK) {
        axl_url_free(parsed);
        return -1;
    }

    /* Parse response headers */
    size_t hdr_start = first_line_end + 2;
    status = axl_http_parse_headers(recv_buf + hdr_start,
                                    header_end - hdr_start,
                                    &resp_headers);
    if (status != AXL_OK) {
        if (resp_headers != NULL) {
            axl_hash_table_free(resp_headers);
        }
        axl_url_free(parsed);
        return -1;
    }

    /* Streaming requests are NOT redirect-followed (the one-shot producer can't
       be replayed): a 3xx is returned as-is for the caller to re-issue with a
       fresh streamer. */

    /* Read body. Two transports: Content-Length (size known up front)
       or Transfer-Encoding: chunked (size discovered chunk-by-chunk).
       Per RFC 7230 §3.3.3, if both are present chunked wins. */
    resp_content_len = axl_http_get_content_length(resp_headers);
    resp_body = NULL;

    size_t resp_body_read = 0;

    bool chunked = false;
    {
        const char *te = (const char *)axl_hash_table_lookup(
            resp_headers, "transfer-encoding");
        if (te != NULL && axl_strcasecmp(te, "chunked") == 0) {
            chunked = true;
        }
    }

    if (chunked) {
        if (read_chunked_body(c,
                              recv_buf + header_end,
                              total_recv - header_end,
                              &resp_body,
                              &resp_body_read) != 0) {
            axl_hash_table_free(resp_headers);
            axl_url_free(parsed);
            return -1;
        }
    } else if (resp_content_len > 0) {
        resp_body = axl_malloc(resp_content_len);
        if (resp_body == NULL) {
            axl_hash_table_free(resp_headers);
            axl_url_free(parsed);
            return -1;
        }

        /* Copy any body data already received with headers */
        size_t body_in_buf = total_recv - header_end;
        if (body_in_buf > resp_content_len) {
            body_in_buf = resp_content_len;
        }

        if (body_in_buf > 0) {
            axl_memcpy(resp_body, recv_buf + header_end, body_in_buf);
            resp_body_read = body_in_buf;
        }

        /* Read remaining body */
        while (resp_body_read < resp_content_len) {
            recv_len = resp_content_len - resp_body_read;
            if (client_recv(c, (uint8_t *)resp_body + resp_body_read,
                            &recv_len) != 0) {
                /* Partial body — network error during download */
                axl_free(resp_body);
                axl_hash_table_free(resp_headers);
                axl_url_free(parsed);
                return -1;
            }

            resp_body_read += recv_len;
            /* client_recv already goes through an ephemeral loop that
               observes Ctrl-C, but a rapid burst of small recvs could
               complete many iterations between break-event dispatches.
               Poll the break flag here so the auto-exit handler fires
               promptly on a large download — without re-dispatching the
               consumer's loop (this runs inside their request call). */
            _axl_poll_break();
        }
    }

    /* Build response object */
    r = axl_calloc(1, sizeof(AxlHttpClientResponse));
    if (r == NULL) {
        if (resp_body != NULL) {
            axl_free(resp_body);
        }

        axl_hash_table_free(resp_headers);
        axl_url_free(parsed);
        return -1;
    }

    r->status_code = status_code;
    r->headers     = resp_headers;
    r->body        = resp_body;
    r->body_size   = resp_body_read;

    *resp = r;
    axl_url_free(parsed);

    /*
     * Close connection if keep-alive is disabled or server sent
     * Connection: close
     */
    {
        bool close_conn = !c->keep_alive;
        if (!close_conn) {
            const char *conn_hdr = (const char *)axl_hash_table_lookup(
                resp_headers, "connection");
            if (conn_hdr != NULL &&
                axl_strcasecmp(conn_hdr, "close") == 0) {
                close_conn = true;
            }
        }
        if (close_conn) {
            axl_tcp_close(c->sock, AXL_TEARDOWN_GRACEFUL);
            c->sock = NULL;
            axl_free(c->connected_host);
            c->connected_host = NULL;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// The contiguous-body sync entry points are thin ephemeral-loop wrappers over
// the async core (Option A — one I/O implementation). _axl_http_request_sync
// (axl-http-client-async.c) spins a private loop, runs the async request, and
// harvests the response. Only the streaming path below still uses do_request.

AxlStatus
axl_http_get(AxlHttpClient *c, const char *url,
             AxlHttpClientResponse **out_resp)
{
    return _axl_http_request_sync(c, "GET", url, NULL, 0, NULL, NULL, out_resp);
}

AxlStatus
axl_http_post(AxlHttpClient *c, const char *url, const void *body,
              size_t size, const char *content_type,
              AxlHttpClientResponse **out_resp)
{
    return _axl_http_request_sync(c, "POST", url, body, size, content_type,
                                  NULL, out_resp);
}

AxlStatus
axl_http_put(AxlHttpClient *c, const char *url, const void *body,
             size_t size, const char *content_type,
             AxlHttpClientResponse **out_resp)
{
    return _axl_http_request_sync(c, "PUT", url, body, size, content_type,
                                  NULL, out_resp);
}

AxlStatus
axl_http_delete(AxlHttpClient *c, const char *url,
                AxlHttpClientResponse **out_resp)
{
    return _axl_http_request_sync(c, "DELETE", url, NULL, 0, NULL, NULL,
                                  out_resp);
}

AxlStatus
axl_http_request(AxlHttpClient *c, const char *method, const char *url,
                 const void *body, size_t body_size,
                 const char *content_type, AxlHashTable *extra_headers,
                 AxlHttpClientResponse **out_resp)
{
    if (method == NULL) {
        return AXL_ERR;
    }
    return _axl_http_request_sync(c, method, url, body, body_size, content_type,
                                  extra_headers, out_resp);
}

/* Adapter that pulls the producer's bytes out of an AxlStream via
   axl_read. Lets axl_http_request_stream_file (and any other
   "stream-backed body" wrapper consumers add) share the streaming
   transport with the raw-callback path. */
static int
stream_producer_adapter(void *ctx, void *out_buf, size_t out_buf_size,
                        size_t *out_size)
{
    AxlStream  *s = ctx;
    axl_ssize_t n = axl_read(s, out_buf, out_buf_size);
    if (n < 0) {
        return AXL_ERR;
    }
    *out_size = (size_t)n;
    return AXL_OK;
}

static void
stream_producer_cleanup(void *ctx)
{
    axl_fclose((AxlStream *)ctx);
}

int
axl_http_request_stream_file(AxlHttpClient *c, const char *method,
                             const char *url, const char *path,
                             const char *content_type,
                             AxlHashTable *extra_headers,
                             AxlHttpClientResponse **out_resp)
{
    AxlFsEntry info;
    if (c == NULL || method == NULL || url == NULL || path == NULL) {
        return AXL_ERR;
    }
    if (axl_file_info(path, &info) != AXL_OK) {
        axl_warning("stream_file: cannot stat '%s'", path);
        return AXL_ERR;
    }
    if (axl_fs_entry_is_dir(&info)) {
        axl_warning("stream_file: '%s' is a directory", path);
        return AXL_ERR;
    }
    AxlStream *src = axl_fopen(path, "r");
    if (src == NULL) {
        axl_warning("stream_file: cannot open '%s' for read", path);
        return AXL_ERR;
    }
    /* Hand the stream off as the producer ctx — cleanup callback
       closes it on completion (success OR error). */
    return axl_http_request_streaming(
        c, method, url,
        stream_producer_adapter, src, stream_producer_cleanup,
        (size_t)info.size, content_type, extra_headers, out_resp);
}

int
axl_http_request_streaming(AxlHttpClient *c, const char *method,
                           const char *url,
                           AxlRequestBodyStreamer streamer, void *ctx,
                           void (*cleanup_fn)(void *ctx),
                           size_t total_size,
                           const char *content_type,
                           AxlHashTable *extra_headers,
                           AxlHttpClientResponse **out_resp)
{
    if (c == NULL || method == NULL || url == NULL || streamer == NULL) {
        if (cleanup_fn != NULL) {
            cleanup_fn(ctx);
        }
        return AXL_ERR;
    }
    int rc = do_streaming_request(c, method, url, streamer, ctx, total_size,
                                  content_type, extra_headers, out_resp);
    /* Cleanup fires regardless of success — consumer doesn't need
       to thread free-on-error through every callsite. */
    if (cleanup_fn != NULL) {
        cleanup_fn(ctx);
    }
    return rc;
}

void
axl_http_client_response_free(AxlHttpClientResponse *resp)
{
    if (resp == NULL) {
        return;
    }

    if (resp->body != NULL) {
        axl_free(resp->body);
    }

    if (resp->headers != NULL) {
        axl_hash_table_free(resp->headers);
    }

    axl_free(resp);
}

AxlBytes *
axl_http_client_response_get_bytes(const AxlHttpClientResponse *resp)
{
    if (resp == NULL || resp->body == NULL || resp->body_size == 0) {
        return NULL;
    }
    return axl_bytes_new(resp->body, resp->body_size);
}

int
axl_http_download(AxlHttpClient *c, const char *url,
                  const char *local_path)
{
    AxlHttpClientResponse *resp;
    int                    result = 0;

    if (c == NULL || url == NULL || local_path == NULL) {
        return AXL_ERR;
    }

    if (axl_http_get(c, url, &resp) != AXL_OK) {
        return AXL_ERR;
    }

    if (resp->status_code != 200) {
        axl_http_client_response_free(resp);
        return AXL_ERR;
    }

    if (resp->body != NULL && resp->body_size > 0) {
        result = axl_file_set_contents(local_path, resp->body,
                                       (size_t)resp->body_size);
    }

    axl_http_client_response_free(resp);
    return result;
}
