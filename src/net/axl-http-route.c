/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-route.c
    Route registration, matching, and static file serving.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static char *
ascii_dup_str(const char *src)
{
    size_t len;
    char *dst;

    if (src == NULL) {
        return NULL;
    }

    len = axl_strlen(src);
    dst = axl_malloc(len + 1);
    if (dst != NULL) {
        axl_memcpy(dst, src, len + 1);
    }

    return dst;
}

void
uppercase_method(
    char       *buf,
    size_t      buf_size,
    const char *method
    )
{
    size_t i;
    for (i = 0; method[i] != '\0' && i + 1 < buf_size; i++) {
        buf[i] = (char)axl_toupper((unsigned char)method[i]);
    }
    buf[i] = '\0';
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

HttpRoute *
add_route_internal(
    AxlHttpServer  *s,
    const char     *method,
    const char     *path,
    AxlHttpHandler  handler,
    void           *data
    )
{
    HttpRoute  *r;
    char        key_buf[512];
    char        method_upper[16];
    const char *method_str;
    size_t      path_len;
    bool        is_prefix;

    if (s == NULL || path == NULL) {
        return NULL;
    }

    r = axl_calloc(1, sizeof(HttpRoute));
    if (r == NULL) {
        axl_error("route alloc failed for %s %s",
                  method ? method : "*", path);
        return NULL;
    }

    r->method  = (method != NULL) ? ascii_dup_str(method) : NULL;
    r->path    = ascii_dup_str(path);
    r->handler = handler;
    r->data    = data;

    /* Build radix tree key: "METHOD /path" or "* /path" */
    if (method != NULL) {
        uppercase_method(method_upper, sizeof(method_upper), method);
        method_str = method_upper;
    } else {
        method_str = "*";
    }

    /*
     * Exact routes go into the hash table (O(1) lookup, the common
     * case). Prefix routes go into the radix tree (longest-prefix
     * match). The split also ensures "/" exact and "/" + "*" prefix
     * don't collide on the same key, since they live in different
     * stores.
     */
    path_len = axl_strlen(path);
    is_prefix = (path_len >= 2
                 && path[path_len - 1] == '*'
                 && path[path_len - 2] == '/');

    bool ok;
    if (is_prefix) {
        /* Strip trailing '*' — we keep the '/' so the prefix lookup
           only matches paths inside the directory, not siblings. */
        axl_snprintf(key_buf, sizeof(key_buf), "%s %.*s",
                     method_str, (int)(path_len - 1), path);
        ok = (axl_radix_tree_insert(s->prefix_routes, key_buf, r) == 0);
    } else {
        axl_snprintf(key_buf, sizeof(key_buf), "%s %s",
                     method_str, path);
        /* Hash table owns its keys (key_destroy = axl_free_impl). */
        char *key_dup = axl_strdup(key_buf);
        if (key_dup == NULL) {
            axl_error("route key alloc failed for %s", key_buf);
            axl_free(r->method);
            axl_free(r->path);
            axl_free(r);
            return NULL;
        }
        /* insert returns 1=new, 0=replaced, -1=OOM */
        ok = (axl_hash_table_replace(s->exact_routes, key_dup, r) >= 0);
        if (!ok) {
            /* OOM: hash table did not take ownership of key_dup */
            axl_free(key_dup);
        }
    }

    if (!ok) {
        axl_error("route insert failed for %s", key_buf);
        axl_free(r->method);
        axl_free(r->path);
        axl_free(r);
        return NULL;
    }

    return r;
}

int
axl_http_server_add_route(AxlHttpServer *s, const char *method,
                          const char *path, AxlHttpHandler handler,
                          void *data)
{
    if (handler == NULL) {
        return -1;
    }
    return (add_route_internal(s, method, path, handler, data) != NULL)
           ? 0 : -1;
}

int
axl_http_server_add_route_auth(AxlHttpServer *s, const char *method,
                               const char *path, AxlHttpHandler handler,
                               void *data, uint32_t auth_flags)
{
    if (handler == NULL) {
        return -1;
    }
    HttpRoute *r = add_route_internal(s, method, path, handler, data);
    if (r == NULL) {
        return -1;
    }

    r->auth_flags = auth_flags;
    return 0;
}

// ---------------------------------------------------------------------------
// Static file serving
// ---------------------------------------------------------------------------

int
static_file_handler(
    AxlHttpRequest   *req,
    AxlHttpResponse  *resp,
    void             *data
    )
{
    axl_http_response_set_file(resp, (const char *)data);
    return 0;
}

int
axl_http_server_add_static(AxlHttpServer *s, const char *prefix,
                           const char *fs_path)
{
    char    path_buf[256];
    size_t  prefix_len;
    size_t  fs_len;
    char   *fs_copy;

    if (s == NULL || prefix == NULL || fs_path == NULL) {
        return -1;
    }

    /* Build wildcard path (e.g., "/css/" + "*") */
    prefix_len = axl_strlen(prefix);
    if (prefix_len + 2 >= sizeof(path_buf)) {
        axl_error("static prefix too long: %s", prefix);
        return -1;
    }

    axl_memcpy(path_buf, prefix, prefix_len);
    if (prefix_len > 0 && prefix[prefix_len - 1] != '/') {
        path_buf[prefix_len++] = '/';
    }

    path_buf[prefix_len]     = '*';
    path_buf[prefix_len + 1] = '\0';

    /* Copy fs_path for lifetime */
    fs_len = axl_strlen(fs_path);
    fs_copy = axl_malloc(fs_len + 1);
    if (fs_copy == NULL) {
        axl_error("static route alloc failed for %s", prefix);
        return -1;
    }

    axl_memcpy(fs_copy, fs_path, fs_len + 1);

    HttpRoute *r = add_route_internal(s, "GET", path_buf, static_file_handler,
                                      fs_copy);
    if (r != NULL) {
        r->is_static = true;
        r->fs_path   = fs_copy;
    } else {
        axl_free(fs_copy);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Upload route registration
// ---------------------------------------------------------------------------

int
axl_http_server_add_upload_route(AxlHttpServer *s, const char *method,
                                const char *path,
                                AxlUploadHandler handler, void *data)
{
    HttpRoute *r = add_route_internal(s, method, path, NULL, data);
    if (r == NULL) {
        axl_error("upload route registration failed for %s %s",
                  method ? method : "*", path);
        return -1;
    }

    r->is_upload = true;
    r->upload_handler = handler;
    r->handler = NULL;
    return 0;
}

// ---------------------------------------------------------------------------
// Route lookup
// ---------------------------------------------------------------------------

HttpRoute *
find_route(
    AxlHttpServer *s,
    const char    *method,
    const char    *path
    )
{
    HttpRoute *route;
    char       key_buf[512];
    char       method_upper[16];

    uppercase_method(method_upper, sizeof(method_upper), method);

    // 1. Exact match with method (O(1) hash lookup)
    axl_snprintf(key_buf, sizeof(key_buf), "%s %s", method_upper, path);
    route = (HttpRoute *)axl_hash_table_lookup(s->exact_routes, key_buf);

    // 2. Prefix match with method
    if (route == NULL) {
        route = (HttpRoute *)axl_radix_tree_lookup_prefix(
                    s->prefix_routes, key_buf, NULL);
    }

    // 3. Exact match with wildcard method
    if (route == NULL) {
        axl_snprintf(key_buf, sizeof(key_buf), "* %s", path);
        route = (HttpRoute *)axl_hash_table_lookup(s->exact_routes, key_buf);
    }

    // 4. Prefix match with wildcard method
    if (route == NULL) {
        route = (HttpRoute *)axl_radix_tree_lookup_prefix(
                    s->prefix_routes, key_buf, NULL);
    }

    return route;
}
