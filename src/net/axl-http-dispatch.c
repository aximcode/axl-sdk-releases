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
    if (run_middleware(s, &req, &resp) != 0) {
        axl_warning("middleware rejected %s %s (status %zu)",
                    conn->method, conn->path, resp.status_code);
        if (resp.status_code == 200) {
            resp.status_code = 500;
        }

        send_response(conn, &resp);
        /* The dispatch tail's body/headers free runs only on the
           success path below. Mirror the cleanup here so middleware-
           allocated response state (set_text, set_json, etc.) doesn't
           leak on rejection. send_response already memcpy'd into
           tx_buf, so the originals are safe to free. */
        if (resp.body != NULL && !resp.body_static) {
            axl_free(resp.body);
        }
        if (resp.headers != NULL) {
            axl_hash_table_free(resp.headers);
        }
        return;
    }

    //
    // Match route
    //
    uppercase_method(method_upper, sizeof(method_upper), conn->method);
    route = find_route(s, conn->method, conn->path);

    if (route == NULL) {
        send_error_response(conn, 404);
        return;
    }

    //
    // Cache lookup — serve cached response if available
    //
    char cache_key[512];
    if (conn->query != NULL && conn->query[0] != '\0') {
        axl_snprintf(cache_key, sizeof(cache_key), "%s %s?%s",
                     method_upper, conn->path, conn->query);
    } else {
        axl_snprintf(cache_key, sizeof(cache_key), "%s %s",
                     method_upper, conn->path);
    }

    if (s->cache != NULL) {
        CachedResponse *cached = (CachedResponse *)axl_hash_table_lookup(
            s->cache, cache_key);
        if (cached != NULL) {
            uint64_t now = axl_time_get_ms();
            uint64_t age = now - cached->timestamp_ms;
            if (cached->ttl_ms == AXL_CACHE_FOREVER ||
                age < cached->ttl_ms) {
                /* Cache hit */
                resp.status_code = cached->status_code;
                resp.content_type = cached->content_type;
                resp.body = cached->body;
                resp.body_size = cached->body_size;
                send_response(conn, &resp);
                /* Cache owns the body. The early return below means
                   we don't reach the dispatch-tail axl_free, but
                   defense-in-depth: also flag body_static so a
                   future refactor that consolidates the cleanup
                   path can't accidentally double-free. */
                resp.body = NULL;
                resp.body_static = true;
                return;
            }
            /* Expired — remove and re-fetch */
            axl_hash_table_remove(s->cache, cache_key);
        }
    }

    //
    // Authentication check
    //
    size_t auth_status = http_check_route_auth(s, route->auth_flags, &req);
    if (auth_status != 0) {
        send_error_response(conn, auth_status);
        return;
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
    if (s->cache != NULL && resp.body != NULL && resp.body_size > 0 &&
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

                /* Per-route TTL override, else server-wide default. */
                entry->ttl_ms = s->cache_ttl_ms;
                if (s->route_ttls != NULL) {
                    void *v = axl_hash_table_lookup(s->route_ttls, conn->path);
                    if (v != NULL) {
                        entry->ttl_ms = (size_t)(uintptr_t)v;
                    }
                }

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

    if (resp.body != NULL && !resp.body_static) {
        axl_free(resp.body);
    }

    if (resp.headers != NULL) {
        axl_hash_table_free(resp.headers);
    }
}
