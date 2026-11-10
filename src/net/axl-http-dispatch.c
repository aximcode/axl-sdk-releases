/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-dispatch.c
    Middleware, response caching, and request dispatch.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

int
axl_http_server_use(AxlHttpServer *s, AxlHttpMiddleware mw, void *data)
{
    if (s == NULL || mw == NULL) {
        return AXL_ERR;
    }

    if (s->mw_count >= s->mw_max) {
        axl_error("middleware table full");
        return AXL_ERR;
    }

    s->middleware[s->mw_count] = mw;
    s->mw_data[s->mw_count]    = data;
    s->mw_count++;
    return AXL_OK;
}

int
run_middleware(
    AxlHttpServer    *s,
    AxlHttpRequest   *req,
    AxlHttpResponse  *resp
    )
{
    for (size_t i = 0; i < s->mw_count; i++) {
        if (s->middleware[i](req, resp, s->mw_data[i]) != AXL_OK) {
            return AXL_ERR;
        }
    }

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Response caching
// ---------------------------------------------------------------------------

void
cached_response_free(void *data)
{
    CachedResponse *entry = (CachedResponse *)data;
    if (entry != NULL) {
        axl_free(entry->body);
        axl_free(entry);
    }
}

int
axl_http_server_use_cache(AxlHttpServer *s, size_t max_entries)
{
    if (s == NULL || max_entries == 0) {
        return AXL_ERR;
    }

    if (s->cache != NULL) {
        axl_hash_table_free(s->cache);
    }

    s->cache = axl_hash_table_new_full(
        axl_str_hash, axl_str_equal,
        axl_free_impl, cached_response_free);
    if (s->cache == NULL) {
        axl_error("cache alloc failed for %zu entries", max_entries);
        return AXL_ERR;
    }

    s->cache_max = max_entries;

    return AXL_OK;
}

/* FIFO eviction: find the entry with the lowest insert_seq (oldest
   insertion) and remove it. We can't use timestamp_ms for FIFO order
   because UEFI GetTime is 1-second granularity — many inserts in the
   same second tie, and tie-broken-by-hash-bucket-iteration is not
   FIFO. O(n) per call, amortized fine because evictions only happen
   when the cache is full. */
typedef struct {
    uint64_t    min_seq;
    const void *min_key;
    bool        seen_any;
} EvictCtx;

static void
find_oldest_cb(const void *key, void *value, void *data)
{
    CachedResponse *entry = value;
    EvictCtx       *ctx   = data;

    if (!ctx->seen_any || entry->insert_seq < ctx->min_seq) {
        ctx->min_seq  = entry->insert_seq;
        ctx->min_key  = key;
        ctx->seen_any = true;
    }
}

static void
evict_oldest_cache_entry(AxlHttpServer *s)
{
    EvictCtx ctx = { .min_seq = 0, .min_key = NULL, .seen_any = false };

    axl_hash_table_foreach(s->cache, find_oldest_cb, &ctx);
    if (ctx.min_key != NULL) {
        axl_hash_table_remove(s->cache, ctx.min_key);
    }
}

int
axl_http_server_set_route_ttl(AxlHttpServer *s, const char *path,
                              size_t ttl_ms)
{
    if (s == NULL || path == NULL) {
        return AXL_ERR;
    }

    if (s->route_ttls == NULL) {
        s->route_ttls = axl_hash_table_new_full(
            axl_str_hash, axl_str_equal,
            axl_free_impl, NULL);
        if (s->route_ttls == NULL) {
            axl_error("route_ttls alloc failed");
            return AXL_ERR;
        }
    }

    char *key = axl_strdup(path);
    if (key == NULL) {
        return AXL_ERR;
    }

    /* Value is the TTL encoded as a pointer — no allocation. */
    if (axl_hash_table_insert(s->route_ttls, key, (void *)(uintptr_t)ttl_ms) == AXL_HASH_TABLE_ERR) {
        axl_free(key);
        return AXL_ERR;
    }

    return AXL_OK;
}

/* Predicate for axl_hash_table_foreach_remove — removes cache
   entries whose path portion starts with the given prefix.
   Cache keys are "METHOD /path[?query]"; skip the method token. */
static bool
cache_prefix_match(const void *key, void *value, void *data)
{
    (void)value;
    const char *k      = key;
    const char *prefix = data;

    const char *space = axl_strchr(k, ' ');
    if (space == NULL) {
        return false;
    }

    size_t prefix_len = axl_strlen(prefix);
    return axl_strncmp(space + 1, prefix, prefix_len) == 0;
}

void
axl_http_server_cache_invalidate(AxlHttpServer *s, const char *prefix)
{
    if (s == NULL || s->cache == NULL) {
        return;
    }

    if (prefix == NULL || prefix[0] == '\0') {
        /* No prefix — clear the whole cache. */
        axl_hash_table_free(s->cache);
        s->cache = axl_hash_table_new_full(
            axl_str_hash, axl_str_equal,
            axl_free_impl, cached_response_free);
        return;
    }

    axl_hash_table_foreach_remove(s->cache, cache_prefix_match, (void *)prefix);
}

// ---------------------------------------------------------------------------
// Route authentication
// ---------------------------------------------------------------------------

/*
 * Apply a route's auth_flags against the server auth callback. Shared
 * by the normal dispatch path and the streaming-upload path (uploads
 * never reach dispatch_request, so they call this directly before the
 * first body byte). Returns 0 when authorized, else the HTTP error
 * status to send: 401 when authentication is required but missing or
 * fails, 403 when an admin route is presented a lesser role.
 */
size_t
http_check_route_auth(AxlHttpServer *s, uint32_t auth_flags,
                      AxlHttpRequest *req)
{
    if (auth_flags == AXL_ROUTE_NO_AUTH) {
        return 0;
    }
    if (s->auth_cb == NULL) {
        return 401;
    }
    AxlAuthInfo auth_info;
    axl_memset(&auth_info, 0, sizeof(auth_info));
    if (s->auth_cb(req, &auth_info, s->auth_data) != 0) {
        return 401;
    }
    if ((auth_flags & AXL_ROUTE_ADMIN) && auth_info.role < AXL_ROUTE_ADMIN) {
        return 403;
    }
    return 0;
}

/* The TTL a response to @p path is cached for: the route's override, else the
   server default. The override is stored AS the hash value, so a TTL of 0
   is a NULL value -- contains(), not lookup(), says whether one was set. */
static size_t
http_route_ttl(
    const AxlHttpServer *s,
    const char          *path
    )
{
    if (s->route_ttls != NULL && axl_hash_table_contains(s->route_ttls, path)) {
        return (size_t)(uintptr_t)axl_hash_table_lookup(s->route_ttls, path);
    }
    return s->cache_ttl_ms;
}

/* May this request's response be served from, or stored in, the cache? Only
   a GET of an open route. The key is the method, path and query -- no body,
   no caller identity -- so another method's repeat would skip its handler,
   and an authenticated route's answer would reach the next caller. */
static bool
http_cacheable(
    AxlHttpServer   *s,
    const HttpRoute *route,
    const char      *method_upper,
    const char      *path
    )
{
    return s->cache != NULL
        && route->auth_flags == AXL_ROUTE_NO_AUTH
        && axl_strcmp(method_upper, "GET") == 0
        && http_route_ttl(s, path) != 0;   /* a TTL of 0: never cached */
}

// ---------------------------------------------------------------------------
// Request dispatch
// ---------------------------------------------------------------------------

void
dispatch_request(
    AxlHttpServer *s,
    HttpConn      *conn
    )
{
    AxlHttpRequest   req;
    AxlHttpResponse  resp;
    HttpRoute       *route;
    char             method_upper[16];

    //
    // Build request struct
    //
    axl_memset(&req, 0, sizeof(req));
    req.method    = conn->method;
    req.path      = conn->path;
    req.query     = conn->query;
    req.headers   = conn->headers;
    req.body      = conn->body;
    req.body_size = conn->body_bytes_read;
    req.request_id = conn->request_id;
    axl_memcpy(req.client_addr, conn->client_addr, sizeof(req.client_addr));

    axl_memset(&resp, 0, sizeof(resp));
    resp.status_code = 200;

    //
    // WebSocket upgrade check
    //
    const char *upgrade_hdr = axl_hash_table_lookup(conn->headers, "upgrade");
    if (upgrade_hdr != NULL && axl_strcasecmp(upgrade_hdr, "websocket") == 0) {
        handle_websocket_upgrade(s, conn);
        return;
    }

    //
    // Run middleware pipeline
    //
    // EVERY exit from here on goes through `done`, which frees the response
    // state the middleware (and later the handler) set. Only the dispatch
    // tail used to: the 404, the 401/403 and the cache hit returned early
    // and leaked it -- a header table per refused request, remote and
    // unauthenticated (test-http-error-leak-qemu.sh).
    //
    if (run_middleware(s, &req, &resp) != 0) {
        axl_warning("middleware rejected %s %s (status %zu)",
                    conn->method, conn->path, resp.status_code);
        if (resp.status_code == 200) {
            resp.status_code = 500;
        }
        send_response(conn, &resp);
        goto done;
    }

    //
    // Match route
    //
    uppercase_method(method_upper, sizeof(method_upper), conn->method);
    route = find_route(s, conn->method, conn->path);

    if (route == NULL) {
        send_error_response_headers(conn, 404, &resp);
        goto done;
    }

    //
    // Authentication, BEFORE the cache: a cached response is the handler's
    // answer, and serving it ahead of the check handed an auth-required
    // route's body to anyone once an authorised request had filled the
    // cache.
    //
    size_t auth_status = http_check_route_auth(s, route->auth_flags, &req);
    if (auth_status != 0) {
        send_error_response_headers(conn, auth_status, &resp);
        goto done;
    }

    //
    // Cache lookup — serve cached response if available. Only for a GET of
    // an open route (http_cacheable): the key holds no request body and no
    // identity, so any other method would skip its handler on a repeat,
    // and an authenticated route would serve one caller's answer to the
    // next.
    //
    bool cacheable = http_cacheable(s, route, method_upper, conn->path);
    char cache_key[512];
    if (conn->query != NULL && conn->query[0] != '\0') {
        axl_snprintf(cache_key, sizeof(cache_key), "%s %s?%s",
                     method_upper, conn->path, conn->query);
    } else {
        axl_snprintf(cache_key, sizeof(cache_key), "%s %s",
                     method_upper, conn->path);
    }

    if (cacheable) {
        CachedResponse *cached = (CachedResponse *)axl_hash_table_lookup(
            s->cache, cache_key);
        if (cached != NULL) {
            uint64_t now = axl_time_get_ms();
            uint64_t age = now - cached->timestamp_ms;
            if (cached->ttl_ms == AXL_CACHE_FOREVER ||
                age < cached->ttl_ms) {
                /* Cache hit. The cache owns its body: a body the
                   middleware set is released first (pointing resp.body at
                   the cached one used to drop it), and the cached one is
                   marked static so `done` leaves it alone. The middleware's
                   headers go out with the cached body. */
                if (resp.body != NULL && !resp.body_static) {
                    axl_free(resp.body);
                }
                http_response_drop_streamer(&resp);   /* the cached body is the answer */
                resp.status_code  = cached->status_code;
                resp.content_type = cached->content_type;
                resp.body         = cached->body;
                resp.body_size    = cached->body_size;
                resp.body_static  = true;
                send_response(conn, &resp);
                goto done;
            }
            /* Expired — remove and re-fetch */
            axl_hash_table_remove(s->cache, cache_key);
        }
    }

    //
    // Call handler
    //
    if (route->handler != NULL) {
        if (route->handler(&req, &resp, route->data) != 0) {
            if (resp.status_code == 200) {
                resp.status_code = 500;
            }
        }
    } else {
        resp.status_code = 500;
    }

    send_response(conn, &resp);

    //
    // Store in cache if caching is enabled and response has a body
    //
    if (cacheable && resp.body != NULL && resp.body_size > 0 &&
        resp.status_code >= 200 && resp.status_code < 300) {
        CachedResponse *entry = axl_calloc(1, sizeof(*entry));
        if (entry == NULL) {
            axl_warning("cache entry alloc failed for %s", cache_key);
        } else {
            entry->body = axl_memdup(resp.body, resp.body_size);
            if (entry->body != NULL) {
                entry->body_size = resp.body_size;
                entry->status_code = resp.status_code;
                entry->timestamp_ms = axl_time_get_ms();
                entry->insert_seq = ++s->cache_seq;

                entry->ttl_ms = http_route_ttl(s, conn->path);

                if (resp.content_type != NULL) {
                    axl_strlcpy(entry->content_type, resp.content_type,
                                sizeof(entry->content_type));
                }
                char *dup_key = axl_strdup(cache_key);
                if (dup_key != NULL) {
                    /* Enforce cache_max via FIFO eviction. */
                    while (s->cache_max > 0
                           && axl_hash_table_size(s->cache) >= s->cache_max
                           && axl_hash_table_lookup(s->cache, dup_key) == NULL) {
                        evict_oldest_cache_entry(s);
                    }
                    axl_hash_table_insert(s->cache, dup_key, entry);
                } else {
                    axl_free(entry->body);
                    axl_free(entry);
                }
            } else {
                axl_free(entry);
            }
        }
    }

done:
    /* send_* has copied the response into the transmit buffer, so the
       originals go now -- on every path past the middleware. */
    http_response_release(&resp);
}
