/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-client.c
    AxlHttpClient — HTTP client with redirect following.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-runtime.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-fs.h>
#include <axl/axl-config.h>
#include "axl-net-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Internal client structure
// ---------------------------------------------------------------------------

#define HTTP_CLIENT_RECV_BUF  8192

struct AxlHttpClient {
    AxlConfig      *config;
    AxlTcp         *sock;
    AxlTlsContext  *tls_ctx;
    char           *connected_host;
    uint16_t        connected_port;
    AxlHashTable   *default_headers;
    bool            tls_enabled;
    bool            tls_verify;
    bool            retry_attempted;
    bool            keep_alive;
    size_t          timeout_ms;
    int             max_redirects;
};

static const AxlConfigDesc http_client_descs[] = {
    { "timeout.ms",    AXL_CFG_UINT, "10000", "Per-operation timeout in ms",
      offsetof(struct AxlHttpClient, timeout_ms), sizeof(size_t) },
    { "keep.alive",    AXL_CFG_BOOL, "true", "Reuse TCP connections",
      offsetof(struct AxlHttpClient, keep_alive), sizeof(bool) },
    { "max.redirects", AXL_CFG_INT,  "5", "HTTP redirect limit",
      offsetof(struct AxlHttpClient, max_redirects), sizeof(int) },
    { "tls.verify",    AXL_CFG_BOOL, "true", "TLS certificate verification",
      offsetof(struct AxlHttpClient, tls_verify), sizeof(bool) },
    { 0 }
};

/* Foreach callback context for emitting extra headers into request buffer */
typedef struct {
    char   *buf;
    size_t  buf_size;
    size_t  len;
} ReqHeaderCtx;

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
        axl_tls_free(c->tls_ctx);
    }

    if (c->sock != NULL) {
        axl_tcp_close(c->sock);
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
        axl_tls_free(c->tls_ctx);
        c->tls_ctx = NULL;
    }
    if (c->sock != NULL) {
        axl_tcp_close(c->sock);
        c->sock = NULL;
    }

    axl_free(c->connected_host);
    c->connected_host = NULL;

    /* Connect */
    if (axl_tcp_connect(host, port, &c->sock) != AXL_OK) {
        return -1;
    }

    /* TLS handshake if enabled */
    if (c->tls_enabled) {
        if (axl_tls_init() != AXL_OK) {
            axl_error("TLS init failed for %s:%u", host, port);
            axl_tcp_close(c->sock);
            c->sock = NULL;
            return -1;
        }

        c->tls_ctx = axl_tls_connect(c->sock, host);
        if (c->tls_ctx == NULL) {
            axl_error("TLS context creation failed for %s:%u", host, port);
            axl_tcp_close(c->sock);
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
                axl_tls_stage_data(c->tls_ctx, hs_buf, hs_len);
            }
            int rc = axl_tls_handshake(c->tls_ctx);
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
            axl_tls_free(c->tls_ctx);
            c->tls_ctx = NULL;
            axl_tcp_close(c->sock);
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
        return axl_tls_write(c->tls_ctx, data, len);
    }
    return axl_tcp_send(c->sock, data, len, c->timeout_ms);
}

static int
client_recv(AxlHttpClient *c, void *buf, size_t *len)
{
    if (c->tls_ctx != NULL) {
        /* Receive raw TLS record from TCP, decrypt */
        size_t raw_len = *len;
        if (axl_tcp_recv(c->sock, buf, &raw_len, c->timeout_ms) != AXL_OK) {
            return -1;
        }
        if (raw_len == 0) {
            *len = 0;
            return 0;
        }
        axl_tls_stage_data(c->tls_ctx, buf, raw_len);
        size_t out_len = 0;
        int rc = axl_tls_read(c->tls_ctx, buf, *len, &out_len);
        *len = out_len;
        if (rc < 0) {
            return -1;  /* TLS error or connection closed */
        }
        return 0;
    }
    return axl_tcp_recv(c->sock, buf, len, c->timeout_ms);
}

static void
emit_extra_header(
    const void *key,
    void       *value,
    void       *data)
{
    ReqHeaderCtx *ctx = (ReqHeaderCtx *)data;

    if (ctx->len < ctx->buf_size) {
        ctx->len += axl_snprintf(
            ctx->buf + ctx->len,
            ctx->buf_size - ctx->len,
            "%s: %s\r\n",
            (const char *)key,
            (const char *)value);
    }
}

static int
do_request(
    AxlHttpClient          *c,
    const char             *method,
    const char             *url,
    const void             *body,
    size_t                  body_size,
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **resp,
    size_t                  redirect_count)
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
        if (!axl_tls_available()) {
            axl_url_free(parsed);
            axl_error("HTTPS requires AXL_TLS=1 build");
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

    req_len = http_build_request_line(req_buf, sizeof(req_buf),
                                      method, full_path);
    req_len += axl_snprintf(req_buf + req_len, sizeof(req_buf) - req_len,
                            "Host: %s\r\n", parsed->host);

    if (body != NULL && body_size > 0) {
        req_len += axl_snprintf(req_buf + req_len,
                                sizeof(req_buf) - req_len,
                                "Content-Length: %llu\r\n",
                                (unsigned long long)body_size);
        if (content_type != NULL) {
            req_len += axl_snprintf(req_buf + req_len,
                                    sizeof(req_buf) - req_len,
                                    "Content-Type: %s\r\n", content_type);
        }
    }

    req_len += axl_snprintf(req_buf + req_len, sizeof(req_buf) - req_len,
                            "Connection: %s\r\n",
                            c->keep_alive ? "keep-alive" : "close");

    /* Emit default headers (from "header.*" config) */
    if (c->default_headers != NULL) {
        ReqHeaderCtx def_ctx;
        def_ctx.buf      = req_buf;
        def_ctx.buf_size = sizeof(req_buf);
        def_ctx.len      = req_len;

        axl_hash_table_foreach(c->default_headers,
                               emit_extra_header, &def_ctx);
        req_len = def_ctx.len;
    }

    /* Emit extra per-request headers (override defaults) */
    if (extra_headers != NULL) {
        ReqHeaderCtx hdr_ctx;
        hdr_ctx.buf      = req_buf;
        hdr_ctx.buf_size = sizeof(req_buf);
        hdr_ctx.len      = req_len;

        axl_hash_table_foreach(extra_headers,
                               emit_extra_header, &hdr_ctx);
        req_len = hdr_ctx.len;
    }

    /* End of headers */
    req_len += axl_snprintf(req_buf + req_len,
                            sizeof(req_buf) - req_len, "\r\n");

    /* Send request (with auto-reconnect on stale connection) */
    if (client_send(c, req_buf, req_len) != AXL_OK) {
        /*
         * Send failed — likely a stale connection (peer closed).
         * Close, reconnect, and retry once.
         */
        if (c->tls_ctx != NULL) {
            axl_tls_free(c->tls_ctx);
            c->tls_ctx = NULL;
        }
        axl_tcp_close(c->sock);
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

    /* Send body if present */
    if (body != NULL && body_size > 0) {
        if (client_send(c, body, body_size) != AXL_OK) {
            axl_url_free(parsed);
            return -1;
        }
    }

    /* Receive response headers */
    total_recv = 0;
    header_end = 0;

    while (total_recv < sizeof(recv_buf)) {
        recv_len = sizeof(recv_buf) - total_recv;
        if (client_recv(c, recv_buf + total_recv, &recv_len) != 0) {
            /*
             * If this is the first recv (no data yet), the connection
             * may have been reset between send and recv. Reconnect and
             * retry the entire request.
             */
            if (total_recv == 0 && !c->retry_attempted) {
                axl_tcp_close(c->sock);
                c->sock = NULL;
                axl_free(c->connected_host);
                c->connected_host = NULL;
                c->retry_attempted = true;
                axl_url_free(parsed);
                return do_request(c, method, url, body, body_size,
                                  content_type, extra_headers,
                                  resp, redirect_count);
            }
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

    /* Handle redirects */
    if ((status_code == 301 || status_code == 302 ||
         status_code == 307) &&
        redirect_count < (size_t)c->max_redirects)
    {
        const char *location = (const char *)axl_hash_table_lookup(
            resp_headers, "location");
        if (location != NULL) {
            char *redirect_url = axl_strdup(location);
            axl_debug("redirect %llu -> %s",
                      (unsigned long long)status_code, redirect_url);
            axl_hash_table_free(resp_headers);
            axl_url_free(parsed);

            /* Close connection for redirect (new host possible) */
            axl_tcp_close(c->sock);
            c->sock = NULL;
            axl_free(c->connected_host);
            c->connected_host = NULL;

            int rc = do_request(c, method, redirect_url, body,
                                body_size, content_type, extra_headers,
                                resp, redirect_count + 1);
            axl_free(redirect_url);
            return rc;
        }
    }

    /* Read body */
    resp_content_len = axl_http_get_content_length(resp_headers);
    resp_body = NULL;

    size_t resp_body_read = 0;

    if (resp_content_len > 0) {
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
               Yielding here dispatches the default loop and lets the
               auto-exit handler fire promptly on a large download. */
            axl_yield();
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
            axl_tcp_close(c->sock);
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

int
axl_http_get(AxlHttpClient *c, const char *url,
             AxlHttpClientResponse **out_resp)
{
    if (c == NULL || url == NULL) {
        return AXL_ERR;
    }

    c->retry_attempted = false;
    return do_request(c, "GET", url, NULL, 0, NULL, NULL, out_resp, 0);
}

int
axl_http_post(AxlHttpClient *c, const char *url, const void *body,
              size_t size, const char *content_type,
              AxlHttpClientResponse **out_resp)
{
    if (c == NULL || url == NULL) {
        return AXL_ERR;
    }

    c->retry_attempted = false;
    return do_request(c, "POST", url, body, (size_t)size, content_type,
                      NULL, out_resp, 0);
}

int
axl_http_put(AxlHttpClient *c, const char *url, const void *body,
             size_t size, const char *content_type,
             AxlHttpClientResponse **out_resp)
{
    if (c == NULL || url == NULL) {
        return AXL_ERR;
    }

    c->retry_attempted = false;
    return do_request(c, "PUT", url, body, (size_t)size, content_type,
                      NULL, out_resp, 0);
}

int
axl_http_delete(AxlHttpClient *c, const char *url,
                AxlHttpClientResponse **out_resp)
{
    if (c == NULL || url == NULL) {
        return AXL_ERR;
    }

    c->retry_attempted = false;
    return do_request(c, "DELETE", url, NULL, 0, NULL, NULL, out_resp, 0);
}

int
axl_http_request(AxlHttpClient *c, const char *method, const char *url,
                 const void *body, size_t body_size,
                 const char *content_type, AxlHashTable *extra_headers,
                 AxlHttpClientResponse **out_resp)
{
    if (c == NULL || method == NULL || url == NULL) {
        return AXL_ERR;
    }

    c->retry_attempted = false;
    return do_request(c, method, url, body, (size_t)body_size,
                      content_type, extra_headers, out_resp, 0);
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
