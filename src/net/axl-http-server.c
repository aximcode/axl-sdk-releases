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
static bool on_tls_handshake_data(AxlTcp *sock, AxlStatus status, void *data);

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
    { "listen.ip",      AXL_CFG_STRING, "", "Bind listener to interface with this station IP (dotted-quad, empty = auto)",
      offsetof(struct AxlHttpServer, listen_ip), sizeof(char *) },
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

static void
server_free_impl(AxlHttpServer *s, bool abortive)
{
    if (s == NULL) {
        return;
    }

    axl_debug("server free on port %u%s", (unsigned)s->port,
              abortive ? " (abortive)" : "");

    if (s->listener != NULL) {
        if (abortive) {
            axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
        } else {
            axl_tcp_close(s->listener, AXL_TEARDOWN_GRACEFUL);
        }
    }

    if (s->conns != NULL) {
        for (size_t i = 0; i < s->max_conns; i++) {
            if (s->conns[i].active) {
                if (abortive) {
                    reset_connection_abortive(&s->conns[i]);
                } else {
                    reset_connection(&s->conns[i]);
                }
            }
        }
        axl_free(s->conns);
    }

    axl_hash_table_free(s->exact_routes);
    axl_radix_tree_free(s->prefix_routes);

    axl_free(s->middleware);
    axl_free(s->mw_data);
    axl_free(s->auth_scheme);
    axl_free(s->auth_realm);

    if (s->cache != NULL) {
        axl_hash_table_free(s->cache);
    }

    if (s->route_ttls != NULL) {
        axl_hash_table_free(s->route_ttls);
    }

    for (size_t i = 0; i < s->ws_route_count; i++) {
        axl_free(s->ws_routes[i].path);
    }

    /* WebDAV mounts are heap-allocated AxlWebDavCtx (one per
       add_webdav call); free each. The route table itself is
       freed by the route_table_free path elsewhere. */
    for (size_t i = 0; i < s->webdav_ctx_count; i++) {
        webdav_ctx_free(s->webdav_ctxs[i]);
    }

    axl_config_free(s->config);
    axl_free(s);
}

void
axl_http_server_free(AxlHttpServer *s, AxlTeardown mode)
{
    server_free_impl(s, mode == AXL_TEARDOWN_RESET);
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
        axl_debug("cannot change max.connections after server started");
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

/* True if @p s contains any character in @p bad. */
static bool
str_has_any(const char *s, const char *bad)
{
    for (; *s != '\0'; s++) {
        if (axl_strchr(bad, *s) != NULL) {
            return true;
        }
    }
    return false;
}

int
axl_http_server_set_auth_challenge(AxlHttpServer *s, const char *scheme,
                                   const char *realm)
{
    if (s == NULL) {
        return AXL_ERR;
    }
    /* Reject characters that would break out of the header or its quoted
       realm (CR/LF injection, an unescaped quote; whitespace in scheme). */
    if (scheme != NULL && str_has_any(scheme, "\r\n \t\"")) {
        return AXL_ERR;
    }
    if (realm != NULL && str_has_any(realm, "\r\n\"")) {
        return AXL_ERR;
    }

    axl_free(s->auth_scheme);
    axl_free(s->auth_realm);
    s->auth_scheme = NULL;
    s->auth_realm  = NULL;

    if (scheme != NULL) {
        s->auth_scheme = axl_strdup(scheme);
        if (s->auth_scheme == NULL) {
            return AXL_ERR;
        }
        if (realm != NULL) {
            s->auth_realm = axl_strdup(realm);
            if (s->auth_realm == NULL) {
                axl_free(s->auth_scheme);
                s->auth_scheme = NULL;
                return AXL_ERR;
            }
        }
    }
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
        axl_debug("TLS not available: axl_tls_available() reported false");
        return AXL_ERR;
    }

    if (axl_tls_init() != AXL_OK) {
        return AXL_ERR;
    }

    if (axl_tls_server_set_cert(cert_der, cert_len, key_der, key_len) != AXL_OK) {
        return AXL_ERR;
    }

    s->tls_enabled = true;
    axl_debug("TLS enabled on port %u", (unsigned)s->port);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Server execution
// ---------------------------------------------------------------------------

int
axl_http_server_start(AxlHttpServer *s, AxlLoop *loop)
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
    // Start listener if not already. If `listen.ip` is set (non-empty,
    // non-zero, parseable), pin the listener to that interface; else
    // auto-pick (skip 0.0.0.0 interfaces, first valid handle).
    // Mirrors the source.ip flow on the http-client side; same parse-
    // failure-is-hard-error semantics.
    //
    if (s->listener == NULL) {
        AxlIPv4Address  src   = { 0 };
        AxlIPv4Address *src_p = NULL;
        if (s->listen_ip != NULL && s->listen_ip[0] != '\0') {
            if (axl_ipv4_parse(s->listen_ip, src.addr) != AXL_OK) {
                axl_error("listen.ip='%s' is not a valid IPv4 address",
                          s->listen_ip);
                return AXL_ERR;
            }
            bool nonzero = src.addr[0] || src.addr[1]
                        || src.addr[2] || src.addr[3];
            if (nonzero) {
                src_p = &src;
            }
        }
        if (axl_tcp_listen_via(s->port, src_p, &s->listener) != AXL_OK) {
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

    if (axl_http_server_start(s, loop) != 0) {
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
            axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
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
            s->conns[i].tls_draining   = false;
            s->conns[i].tls_rearm_wanted = false;

            s->conns[i].tls_ctx = NULL;

            axl_tcp_get_remote_addr(client, s->conns[i].client_addr,
                                    sizeof(s->conns[i].client_addr), &remote_port);
            axl_debug("accepted connection from %s:%u", s->conns[i].client_addr, remote_port);

            //
            // TLS handshake (if enabled) — run it ASYNCHRONOUSLY on the
            // server's own loop. A blocking handshake (recv + send) nests
            // ephemeral loops, which cannot make progress when accept is
            // dispatched by a resident AxlService loop; the async path works
            // under both a top-level axl_loop_run and a resident loop.
            //
            if (s->tls_enabled) {
                AxlTlsContext *tls = axl_tls_accept(client);
                if (tls == NULL) {
                    axl_warning("TLS context creation failed for %s",
                               s->conns[i].client_addr);
                    s->conns[i].active = false;
                    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
                    return true;  /* keep accepting more clients */
                }
                s->conns[i].tls_ctx        = tls;

                /* Receive the first handshake flight (the ClientHello);
                   on_tls_handshake_data drives the handshake to completion. */
                if (axl_tcp_recv_async(client, s->conns[i].tls_cipher_buf,
                                       sizeof(s->conns[i].tls_cipher_buf),
                                       s->loop, NULL, on_tls_handshake_data,
                                       &s->conns[i]) != AXL_OK) {
                    axl_warning("TLS handshake recv arm failed for %s",
                               s->conns[i].client_addr);
                    axl_tls_free(tls);
                    s->conns[i].tls_ctx = NULL;
                    s->conns[i].active = false;
                    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
                }
                return true;  /* keep accepting; handshake continues async */
            }

            //
            // Plain HTTP: start receiving header data asynchronously
            //
            start_conn_recv(s, &s->conns[i]);
            return true;  /* keep accepting */
        }
    }

    //
    // No free connection slots — reject the client
    //
    axl_debug("no free connection slots, rejecting client");
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
    return true;  /* keep accepting — next client may find a slot */
}

// ---------------------------------------------------------------------------
// on_tls_handshake_data — async TLS server handshake, pumped by s->loop.
//
// Each call: stage the received ciphertext, advance the handshake (which
// sends any ServerHello flight asynchronously on the same loop), and either
// finish (hand off to start_conn_recv), ask for more data (re-arm recv), or
// fail (close). This replaces the old blocking recv/send loop so the
// handshake completes whether the loop is a top-level axl_loop_run or a
// resident AxlService driver loop.
// ---------------------------------------------------------------------------

static bool
on_tls_handshake_data(
    AxlTcp    *sock,
    AxlStatus  status,
    void      *data
    )
{
    HttpConn      *conn = (HttpConn *)data;
    AxlHttpServer *s    = conn->server;

    if (status != AXL_OK || !s->running || conn->tls_ctx == NULL) {
        goto fail;
    }

    size_t bytes = axl_tcp_recv_get_size(sock);
    if (bytes > 0) {
        axl_tls_stage_data(conn->tls_ctx, conn->tls_cipher_buf, bytes);
    }

    AxlTlsStatus rc = axl_tls_handshake_async(conn->tls_ctx, s->loop);
    if (rc == AXL_TLS_OK) {
        /* Handshake complete — switch to normal request receive. */
        axl_debug("TLS handshake complete for %s", conn->client_addr);
        start_conn_recv(s, conn);
        return false;   /* start_conn_recv arms its own recv */
    }
    if (rc == AXL_TLS_WANT_MORE) {
        return true;    /* need more handshake data — re-arm this recv */
    }
    axl_debug("TLS handshake failed for %s", conn->client_addr);

fail:
    /* Same teardown as a request-phase failure: reset_connection frees
       tls_ctx, closes the socket, and zeroes the slot (and no-ops if the
       slot is already inactive). */
    reset_connection(conn);
    return false;
}
