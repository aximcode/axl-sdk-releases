/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-server.c
    AxlHttpServer — core lifecycle, configuration, auth, TLS, accept loop.

    Uses async TCP API (axl_tcp_accept_async, axl_tcp_recv_async) for
    event-driven I/O instead of idle-based polling.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

static bool on_accept_ready(AxlTcp *client, AxlStatus status, void *data);

// ---------------------------------------------------------------------------
// Configuration descriptors
// ---------------------------------------------------------------------------

const AxlConfigDesc http_server_descs[] = {
    { "max.connections", AXL_CFG_UINT, "8", "Max simultaneous connections",
      offsetof(struct AxlHttpServer, max_conns), sizeof(size_t) },
    { "body.limit",      AXL_CFG_UINT, "4194304", "Max request body bytes",
      offsetof(struct AxlHttpServer, body_limit), sizeof(size_t) },
    { "keep.alive.sec",  AXL_CFG_UINT, "30", "Keep-alive timeout in seconds",
      offsetof(struct AxlHttpServer, keep_alive_sec), sizeof(size_t) },
    { "upload.chunk.size", AXL_CFG_UINT, "65536", "Upload streaming chunk size in bytes",
      offsetof(struct AxlHttpServer, upload_chunk_size), sizeof(size_t) },
    { "cache.ttl.ms",   AXL_CFG_UINT, "60000", "Default response cache TTL in milliseconds",
      offsetof(struct AxlHttpServer, cache_ttl_ms), sizeof(size_t) },
    { 0 }
};

// ---------------------------------------------------------------------------
// Route destructor (called by radix tree for each route on removal/free)
// ---------------------------------------------------------------------------

static void
route_free(void *data)
{
    HttpRoute *r = (HttpRoute *)data;
    if (r != NULL) {
        if (r->is_static) {
            axl_free(r->data);  /* fs_path copy */
        }
        axl_free(r->method);
        axl_free(r->path);
        axl_free(r);
    }
}

// ---------------------------------------------------------------------------
// Server creation / destruction
// ---------------------------------------------------------------------------

AxlHttpServer *
axl_http_server_new(uint16_t port)
{
    AXL_AUTOPTR(AxlHttpServer) s = axl_calloc(1, sizeof(AxlHttpServer));
    if (s == NULL) {
        axl_error("server alloc failed");
        return NULL;
    }

    s->port = port;
    axl_debug("server created on port %u", (unsigned)port);

    s->config = axl_config_new(http_server_descs, NULL, s);
    if (s->config == NULL) {
        return NULL;
    }

    /*
     * Exact routes: hash table for O(1) lookup of full "METHOD /path"
     * keys (the common case for HTTP routing). The table owns its keys
     * (strdup'd by the caller, freed via axl_free) and its values
     * (HttpRoute*, freed via route_free).
     *
     * Prefix routes: radix tree because hash tables can't do longest-
     * prefix matching.
     */
    s->exact_routes  = axl_hash_table_new_full(NULL, NULL,
                                               axl_free_impl, route_free);
    s->prefix_routes = axl_radix_tree_new_full(route_free);
    if (s->exact_routes == NULL || s->prefix_routes == NULL) {
        return NULL;
    }

    /* Middleware array */
    s->mw_max = HTTP_DEFAULT_MAX_MW;
    s->middleware = axl_calloc(s->mw_max, sizeof(AxlHttpMiddleware));
    s->mw_data   = axl_calloc(s->mw_max, sizeof(void *));
    if (s->middleware == NULL || s->mw_data == NULL) {
        return NULL;
    }

    return axl_steal_pointer(&s);
}

void
axl_http_server_free(AxlHttpServer *s)
{
    if (s == NULL) {
        return;
    }

    axl_debug("server free on port %u", (unsigned)s->port);

    if (s->listener != NULL) {
        axl_tcp_close(s->listener);
    }

    if (s->conns != NULL) {
        for (size_t i = 0; i < s->max_conns; i++) {
            if (s->conns[i].active) {
                reset_connection(&s->conns[i]);
            }
        }
        axl_free(s->conns);
    }

    axl_hash_table_free(s->exact_routes);
    axl_radix_tree_free(s->prefix_routes);

    axl_free(s->middleware);
    axl_free(s->mw_data);

    if (s->cache != NULL) {
        axl_hash_table_free(s->cache);
    }

    if (s->route_ttls != NULL) {
        axl_hash_table_free(s->route_ttls);
    }

    for (size_t i = 0; i < s->ws_route_count; i++) {
        axl_free(s->ws_routes[i].path);
    }

    axl_config_free(s->config);
    axl_free(s);
}

// ---------------------------------------------------------------------------
// Connection pool
// ---------------------------------------------------------------------------

static int
allocate_conns(AxlHttpServer *s)
{
    if (s->conns != NULL) {
        return AXL_OK;
    }

    s->conns = axl_calloc(s->max_conns, sizeof(HttpConn));
    if (s->conns == NULL) {
        axl_error("failed to allocate %zu connection slots", s->max_conns);
        return AXL_ERR;
    }

    for (size_t i = 0; i < s->max_conns; i++) {
        s->conns[i].server = s;
    }

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

int
axl_http_server_set(AxlHttpServer *s, const char *key, const char *value)
{
    if (s == NULL || key == NULL || value == NULL) {
        return AXL_ERR;
    }

    /* Guard: can't change max.connections after listener started */
    if (axl_streql(key, "max.connections") && s->conns != NULL) {
        axl_warning("cannot change max.connections after server started");
        return AXL_ERR;
    }

    return axl_config_set(s->config, key, value);
}

const char *
axl_http_server_get(AxlHttpServer *s, const char *key)
{
    if (s == NULL || key == NULL) {
        return NULL;
    }

    return axl_config_get(s->config, key);
}

int
axl_http_server_set_max_connections(AxlHttpServer *s, size_t max)
{
    char buf[32];
    axl_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)max);
    return axl_http_server_set(s, "max.connections", buf);
}

int
axl_http_server_set_body_limit(AxlHttpServer *s, size_t limit)
{
    char buf[32];
    axl_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)limit);
    return axl_http_server_set(s, "body.limit", buf);
}

int
axl_http_server_set_keep_alive(AxlHttpServer *s, size_t timeout_sec)
{
    char buf[32];
    axl_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)timeout_sec);
    return axl_http_server_set(s, "keep.alive.sec", buf);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

int
axl_http_server_use_auth(AxlHttpServer *s, AxlAuthCallback cb, void *data)
{
    if (s == NULL || cb == NULL) {
        return AXL_ERR;
    }

    s->auth_cb = cb;
    s->auth_data = data;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

int
axl_http_server_use_tls(
    AxlHttpServer *s,
    const void    *cert_der,
    size_t         cert_len,
    const void    *key_der,
    size_t         key_len
    )
{
    if (s == NULL || cert_der == NULL || key_der == NULL) {
        return AXL_ERR;
    }

    if (!axl_tls_available()) {
        axl_warning("TLS not available (build with AXL_TLS=1)");
        return AXL_ERR;
    }

    if (axl_tls_init() != AXL_OK) {
        return AXL_ERR;
    }

    if (axl_tls_server_set_cert(cert_der, cert_len, key_der, key_len) != AXL_OK) {
        return AXL_ERR;
    }

    s->tls_enabled = true;
    axl_info("TLS enabled on port %u", (unsigned)s->port);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Server execution
// ---------------------------------------------------------------------------

int
axl_http_server_attach(AxlHttpServer *s, AxlLoop *loop)
{
    if (s == NULL || loop == NULL) {
        return AXL_ERR;
    }

    //
    // Allocate connection pool (deferred so SetMaxConnections takes effect)
    //
    if (allocate_conns(s) != 0) {
        return AXL_ERR;
    }

    //
    // Start listener if not already
    //
    if (s->listener == NULL) {
        if (axl_tcp_listen(s->port, &s->listener) != AXL_OK) {
            return AXL_ERR;
        }
    }

    s->loop = loop;
    s->running = true;
    axl_debug("server attached to loop, listening on port %u",
              (unsigned)s->port);

    //
    // Register async accept — on_accept_ready fires for each incoming
    // connection.  The accept is automatically re-armed by the async
    // TCP layer, so we only call this once.
    //
    if (axl_tcp_accept_async(s->listener, loop, NULL,
                             on_accept_ready, s) != AXL_OK) {
        return AXL_ERR;
    }

    return AXL_OK;
}

int
axl_http_server_run(AxlHttpServer *s)
{
    AxlLoop *loop;

    if (s == NULL) {
        return AXL_ERR;
    }

    loop = axl_loop_new();
    if (loop == NULL) {
        return AXL_ERR;
    }

    if (axl_http_server_attach(s, loop) != 0) {
        axl_loop_free(loop);
        return AXL_ERR;
    }

    {
        int rc = axl_loop_run(loop);
        axl_loop_free(loop);
        s->loop = NULL;
        return rc;
    }
}

// ---------------------------------------------------------------------------
// on_accept_ready — async accept callback
// ---------------------------------------------------------------------------

static bool
on_accept_ready(
    AxlTcp    *client,
    AxlStatus  status,
    void      *data
    )
{
    AxlHttpServer *s = (AxlHttpServer *)data;
    uint16_t       remote_port;

    if (status != AXL_OK || client == NULL) {
        /* Per-accept error — keep listening. */
        return true;
    }
    if (!s->running) {
        /* Server shutting down — stop accepting. */
        if (client != NULL) {
            axl_tcp_close(client);
        }
        return false;
    }

    //
    // Find a free connection slot
    //
    for (size_t i = 0; i < s->max_conns; i++) {
        if (!s->conns[i].active) {
            s->conns[i].active         = true;
            s->conns[i].sock           = client;
            s->conns[i].header_len     = 0;
            s->conns[i].headers_done   = false;
            s->conns[i].method         = NULL;
            s->conns[i].path           = NULL;
            s->conns[i].query          = NULL;
            s->conns[i].headers        = NULL;
            s->conns[i].content_length = 0;
            s->conns[i].body_bytes_read = 0;
            s->conns[i].body           = NULL;
            s->conns[i].body_alloc     = 0;
            s->conns[i].chunked        = false;
            s->conns[i].chunked_done   = false;
            s->conns[i].keep_alive     = false;

            s->conns[i].tls_ctx = NULL;

            axl_tcp_get_remote_addr(client, s->conns[i].client_addr,
                                    sizeof(s->conns[i].client_addr), &remote_port);
            axl_debug("accepted connection from %s:%u", s->conns[i].client_addr, remote_port);

            //
            // TLS handshake (if enabled)
            //
            if (s->tls_enabled) {
                AxlTlsContext *tls = axl_tls_accept(client);
                if (tls == NULL) {
                    axl_warning("TLS context creation failed for %s",
                               s->conns[i].client_addr);
                    s->conns[i].active = false;
                    axl_tcp_close(client);
                    return true;  /* keep accepting more clients */
                }

                /* Blocking handshake — recv data, feed to TLS, repeat */
                int hs_result = -1;
                for (int attempt = 0; attempt < 50; attempt++) {
                    uint8_t hs_buf[4096];
                    size_t  hs_len = sizeof(hs_buf);
                    if (axl_tcp_recv(client, hs_buf,
                                     &hs_len, 200) == AXL_OK && hs_len > 0) {
                        axl_tls_stage_data(tls, hs_buf, hs_len);
                    }
                    int rc = axl_tls_handshake(tls);
                    if (rc == 0) {
                        hs_result = 0;
                        break;
                    }
                    if (rc < 0) {
                        break;
                    }
                    /* rc == 1: need more data, loop */
                }

                if (hs_result != 0) {
                    axl_warning("TLS handshake failed for %s",
                               s->conns[i].client_addr);
                    axl_tls_free(tls);
                    s->conns[i].active = false;
                    axl_tcp_close(client);
                    return true;  /* keep accepting more clients */
                }

                s->conns[i].tls_ctx = tls;
                axl_debug("TLS handshake complete for %s", s->conns[i].client_addr);
            }

            //
            // Start receiving header data asynchronously
            //
            start_conn_recv(s, &s->conns[i]);
            return true;  /* keep accepting */
        }
    }

    //
    // No free connection slots — reject the client
    //
    axl_debug("no free connection slots, rejecting client");
    axl_tcp_close(client);
    return true;  /* keep accepting — next client may find a slot */
}
