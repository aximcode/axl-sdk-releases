/** @file axl-test-net.c
    Test application for AxlNet — exercises URL parsing, HTTP core parsing,
    network utilities, TCP sockets, and HTTP server/client round-trip.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-time.h>
#include <axl/axl-net.h>
#include <axl/axl-http-core.h>

AXL_LOG_DOMAIN("test");

// ---------------------------------------------------------------------------
// URL Parsing Tests (no network needed)
// ---------------------------------------------------------------------------

static void
test_url_parse_basic(void)
{
    AxlUrl *u;
    int ret;

    ret = axl_url_parse("http://192.168.1.1:8080/api/version?v=1", &u);
    test_check(ret == 0, "URL parse basic");
    if (ret == 0) {
        test_check(axl_strcmp(u->scheme, "http") == 0, "URL scheme is http");
        test_check(axl_strcmp(u->host, "192.168.1.1") == 0, "URL host");
        test_check(u->port == 8080, "URL port 8080");
        test_check(axl_strcmp(u->path, "/api/version") == 0, "URL path");
        test_check(u->query != NULL && axl_strcmp(u->query, "v=1") == 0, "URL query");
        axl_url_free(u);
    }
}

static void
test_url_parse_https(void)
{
    AxlUrl *u;
    int ret;

    ret = axl_url_parse("https://example.com/path", &u);
    test_check(ret == 0, "URL parse https");
    if (ret == 0) {
        test_check(axl_strcmp(u->scheme, "https") == 0, "URL scheme is https");
        test_check(u->port == 443, "URL default https port 443");
        test_check(axl_strcmp(u->path, "/path") == 0, "URL https path");
        test_check(u->query == NULL, "URL https no query");
        axl_url_free(u);
    }
}

static void
test_url_parse_minimal(void)
{
    AxlUrl *u;
    int ret;

    ret = axl_url_parse("http://host/", &u);
    test_check(ret == 0, "URL parse minimal");
    if (ret == 0) {
        test_check(axl_strcmp(u->host, "host") == 0, "URL minimal host");
        test_check(u->port == 80, "URL default http port 80");
        test_check(axl_strcmp(u->path, "/") == 0, "URL minimal path /");
        axl_url_free(u);
    }
}

static void
test_url_parse_malformed(void)
{
    AxlUrl *u;
    int ret;

    ret = axl_url_parse("not-a-url", &u);
    test_check(ret != 0, "URL reject malformed");

    ret = axl_url_parse(NULL, &u);
    test_check(ret != 0, "URL reject NULL");
}

static void
test_url_build(void)
{
    char *url;

    url = axl_url_build("http", "example.com", 0, "/test");
    test_check(url != NULL, "URL build non-NULL");
    if (url != NULL) {
        test_check(axl_strcmp(url, "http://example.com/test") == 0, "URL build default port");
        axl_free(url);
    }

    url = axl_url_build("http", "host", 9090, "/api");
    test_check(url != NULL, "URL build custom port non-NULL");
    if (url != NULL) {
        test_check(axl_strcmp(url, "http://host:9090/api") == 0, "URL build custom port");
        axl_free(url);
    }
}

// ---------------------------------------------------------------------------
// HTTP Core Parsing Tests (no network needed)
// ---------------------------------------------------------------------------

static void
test_http_find_header_end(void)
{
    const char *hdr = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t end;

    end = axl_http_find_header_end(hdr, axl_strlen(hdr));
    test_check(end > 0, "HTTP find header end");
    test_check(end == axl_strlen(hdr), "HTTP header end at correct offset");
}

static void
test_http_parse_status(void)
{
    const char *line = "HTTP/1.1 200 OK";
    size_t code;
    int    status;

    status = axl_http_parse_status_line(line, axl_strlen(line), &code);
    test_check(status == 0, "HTTP parse status line");
    test_check(code == 200, "HTTP status code 200");
}

static void
test_http_parse_request(void)
{
    const char *line = "POST /api/data?key=val HTTP/1.1";
    char *method;
    char *path;
    char *query;
    int   status;

    status = axl_http_parse_request_line(line, axl_strlen(line), &method, &path, &query);
    test_check(status == 0, "HTTP parse request line");
    if (status == 0) {
        test_check(axl_strcmp(method, "POST") == 0, "HTTP method POST");
        test_check(axl_strcmp(path, "/api/data") == 0, "HTTP path /api/data");
        test_check(query != NULL && axl_strcmp(query, "key=val") == 0, "HTTP query key=val");
        axl_free(method);
        axl_free(path);
        if (query != NULL) {
            axl_free(query);
        }
    }
}

static void
test_http_parse_headers(void)
{
    const char *hdrs = "Content-Length: 42\r\nConnection: close\r\n\r\n";
    AxlHashTable *table;
    int           status;

    status = axl_http_parse_headers(hdrs, axl_strlen(hdrs), &table);
    test_check(status == 0, "HTTP parse headers");
    if (status == 0) {
        size_t clen = axl_http_get_content_length(table);
        test_check(clen == 42, "HTTP Content-Length 42");

        const char *conn_val = (const char *)axl_hash_table_lookup(table, "connection");
        test_check(conn_val != NULL && axl_strcmp(conn_val, "close") == 0,
            "HTTP Connection: close");
        axl_hash_table_free(table);
    }
}

// ---------------------------------------------------------------------------
// Network Tests (require networking in QEMU)
// ---------------------------------------------------------------------------

static void
test_net_available(void)
{
    bool available = axl_net_is_available();

    if (available) {
        test_pass("Network is available");

        AxlIPv4Address addr;
        int ret = axl_net_get_ip_address(&addr);
        test_check(ret == 0, "GetIpAddress succeeds");
        if (ret == 0) {
            axl_printf("  IP: %d.%d.%d.%d\n",
                addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3]);
            test_check(addr.addr[0] != 0 || addr.addr[1] != 0 ||
                addr.addr[2] != 0 || addr.addr[3] != 0,
                "IP address is non-zero");
        }
    } else {
        axl_printf("SKIP: Network not available\n");
    }
}

static void
test_tcp_echo(void)
{
    int ret;
    AxlTcp *listener;
    AxlTcp *client;
    AxlTcp *accepted;
    char send_buf[] = "Hello AxlNet";
    char recv_buf[64];
    size_t recv_size;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: TCP echo(no network)\n");
        return;
    }

    //
    // Listen
    //
    ret = axl_tcp_listen(9999, &listener);
    if (ret != 0) {
        axl_printf("SKIP: TCP listen failed\n");
        return;
    }

    //
    // Connect
    //
    AxlIPv4Address local_ip;
    char ip_str[16];
    axl_net_get_ip_address(&local_ip);
    axl_snprintf(ip_str, sizeof (ip_str), "%d.%d.%d.%d",
        local_ip.addr[0], local_ip.addr[1], local_ip.addr[2], local_ip.addr[3]);

    ret = axl_tcp_connect(ip_str, 9999, &client);
    if (ret != 0) {
        axl_printf("SKIP: TCP connect failed\n");
        axl_tcp_close(listener);
        return;
    }

    //
    // Accept
    //
    accepted = NULL;
    for (size_t i = 0; i < 100 && accepted == NULL; i++) {
        ret = axl_tcp_accept(listener, &accepted, 100);
        if (ret != 0) {
            axl_usleep(10000);
        }
    }

    if (accepted == NULL) {
        axl_printf("SKIP: TCP accept failed\n");
        axl_tcp_close(client);
        axl_tcp_close(listener);
        return;
    }

    //
    // Send from client, recv on server
    //
    ret = axl_tcp_send(client, send_buf, axl_strlen(send_buf), 0);
    test_check(ret == 0, "TCP send");

    recv_size = sizeof (recv_buf) - 1;
    ret = axl_tcp_recv(accepted, recv_buf, &recv_size, 0);
    test_check(ret == 0, "TCP recv");
    if (ret == 0) {
        recv_buf[recv_size] = '\0';
        test_check(axl_strcmp(recv_buf, "Hello AxlNet") == 0, "TCP echo match");
    }

    axl_tcp_close(accepted);
    axl_tcp_close(client);
    axl_tcp_close(listener);
}

// ---------------------------------------------------------------------------
// TCP async recv re-arm via bool return
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop *loop;
    int      fires;
    char     buf[64];
    size_t   total_bytes;
} TcpRearmCtx;

static bool
on_tcp_rearm_data(AxlTcp *sock, int status, void *data)
{
    TcpRearmCtx *c = (TcpRearmCtx *)data;
    size_t len = axl_tcp_recv_get_size(sock);

    c->fires++;
    if (status != 0 || len == 0) {
        axl_loop_quit(c->loop);
        return false;
    }

    c->total_bytes += len;
    /* Stop after 3 fires so the loop exits cleanly. */
    if (c->fires >= 3) {
        axl_loop_quit(c->loop);
        return false;
    }
    return true;  /* stay armed; library re-issues Receive on same buffer */
}

static bool
on_tcp_rearm_timeout(void *data)
{
    TcpRearmCtx *c = (TcpRearmCtx *)data;
    axl_loop_quit(c->loop);
    return AXL_SOURCE_REMOVE;
}

static void
test_tcp_recv_async_rearm(void)
{
    int ret;
    AxlTcp *listener = NULL;
    AxlTcp *client = NULL;
    AxlTcp *accepted = NULL;
    TcpRearmCtx ctx = { 0 };

    if (!axl_net_is_available()) {
        axl_printf("SKIP: TCP recv_async rearm (no network)\n");
        return;
    }

    if (axl_tcp_listen(9998, &listener) != 0) {
        axl_printf("SKIP: TCP recv_async rearm (listen failed)\n");
        return;
    }

    AxlIPv4Address local_ip;
    char ip_str[16];
    axl_net_get_ip_address(&local_ip);
    axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
        local_ip.addr[0], local_ip.addr[1],
        local_ip.addr[2], local_ip.addr[3]);

    if (axl_tcp_connect(ip_str, 9998, &client) != 0) {
        axl_printf("SKIP: TCP recv_async rearm (connect failed)\n");
        axl_tcp_close(listener);
        return;
    }

    for (size_t i = 0; i < 100 && accepted == NULL; i++) {
        if (axl_tcp_accept(listener, &accepted, 100) != 0) {
            axl_usleep(10000);
        }
    }
    if (accepted == NULL) {
        axl_printf("SKIP: TCP recv_async rearm (accept failed)\n");
        axl_tcp_close(client);
        axl_tcp_close(listener);
        return;
    }

    /* Arm async recv with bool-return re-arm */
    ctx.loop = axl_loop_new();
    ret = axl_tcp_recv_async(accepted, ctx.buf, sizeof(ctx.buf),
                             ctx.loop, NULL, on_tcp_rearm_data, &ctx);
    test_check(ret == 0, "TCP recv_async: initial arm");

    /* Send 3 chunks with small gaps so each arrives as a separate
       fire on the loop. Also register a safety timeout. */
    axl_loop_add_timeout(ctx.loop, 3000, on_tcp_rearm_timeout, &ctx);

    axl_tcp_send(client, "one", 3, 0);
    axl_usleep(50000);
    axl_tcp_send(client, "two", 3, 0);
    axl_usleep(50000);
    axl_tcp_send(client, "three", 5, 0);

    axl_loop_run(ctx.loop);
    axl_loop_free(ctx.loop);

    test_check(ctx.fires == 3, "TCP recv_async: callback fired 3 times");
    test_check(ctx.total_bytes == 11, "TCP recv_async: total bytes = 11");

    axl_tcp_close(accepted);
    axl_tcp_close(client);
    axl_tcp_close(listener);
}

// ---------------------------------------------------------------------------
// HTTP Server/Client Round-trip
// ---------------------------------------------------------------------------

static int
test_route_handler(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"ok\":true}");
    return 0;
}

static void
test_http_round_trip(void)
{
    int ret;
    AxlHttpServer *server;
    AxlHttpClient *client;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: HTTP round-trip(no network)\n");
        return;
    }

    //
    // Create server with a test route
    //
    server = axl_http_server_new(8888);
    if (server == NULL) {
        axl_printf("SKIP: HTTP server alloc failed\n");
        return;
    }

    axl_http_server_add_route(server, "GET", "/test", test_route_handler, NULL);

    //
    // Attach to a loop and manually drive it
    //
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_http_server_free(server);
        axl_printf("SKIP: Loop alloc failed\n");
        return;
    }

    ret = axl_http_server_attach(server, loop);
    if (ret != 0) {
        axl_http_server_free(server);
        axl_loop_free(loop);
        axl_printf("SKIP: HTTP server attach failed\n");
        return;
    }

    //
    // Create client and make a request
    //
    client = axl_http_client_new();
    if (client == NULL) {
        axl_http_server_free(server);
        axl_loop_free(loop);
        axl_printf("SKIP: HTTP client alloc failed\n");
        return;
    }

    //
    // Build URL using our IP
    //
    AxlIPv4Address ip;
    char url_buf[128];
    axl_net_get_ip_address(&ip);
    axl_snprintf(url_buf, sizeof (url_buf), "http://%d.%d.%d.%d:8888/test",
        ip.addr[0], ip.addr[1], ip.addr[2], ip.addr[3]);

    //
    // Note: In UEFI single-threaded environment, we need to interleave
    // client sends and server polls. For this test, we just verify the
    // server and client can be created. A full round-trip would require
    // manual TCP interleaving which is complex for a unit test.
    //
    axl_printf("  HTTP server on port 8888, client URL: %s\n", url_buf);
    test_pass("HTTP server/client creation");

    axl_http_client_free(client);
    axl_http_server_free(server);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// HTTP Server Mode -- "serve" argument starts a server for external testing
// ---------------------------------------------------------------------------

static int
on_get_version(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"version\":\"1.0\",\"module\":\"AxlNet\"}");
    return 0;
}

static int
on_get_health(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"status\":\"ok\"}");
    return 0;
}

static int
on_get_plain(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_text(resp, "Hello from AxlNet");
    return 0;
}

static int
on_echo(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)data;
    if (req->body != NULL && req->body_size > 0) {
        resp->body = axl_malloc(req->body_size);
        if (resp->body != NULL) {
            axl_memcpy(resp->body, req->body, req->body_size);
            resp->body_size = req->body_size;
        }

        resp->content_type = "application/octet-stream";
    }

    resp->status_code = 200;
    return 0;
}

// /client-test?url=http://... -- UEFI HTTP client fetches the given URL
// and returns the response body. Tests axl_http_get from inside UEFI.
static int
on_client_test(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    AxlHttpClient *c;
    AxlHttpClientResponse *client_resp;
    int ret;
    char url_buf[256];

    (void)data;

    if (req->query == NULL || req->query[0] == '\0') {
        axl_http_response_set_json(resp, "{\"error\":\"missing url query param\"}");
        axl_http_response_set_status(resp, 400);
        return 0;
    }

    //
    // Parse "url=..." from query string
    //
    const char *url_value = req->query;
    if (axl_strncmp(url_value, "url=", 4) == 0) {
        url_value += 4;
    }

    axl_strlcpy(url_buf, url_value, sizeof (url_buf));

    //
    // Make HTTP client request
    //
    c = axl_http_client_new();
    if (c == NULL) {
        axl_http_response_set_json(resp, "{\"error\":\"client alloc failed\"}");
        axl_http_response_set_status(resp, 500);
        return 0;
    }

    ret = axl_http_get(c, url_buf, &client_resp);
    if (ret != 0) {
        axl_http_response_set_json(resp, "{\"error\":\"axl_http_get failed\"}");
        axl_http_response_set_status(resp, 502);
        axl_http_client_free(c);
        return 0;
    }

    //
    // Build response with client result
    //
    char result_buf[512];
    axl_snprintf(result_buf, sizeof (result_buf),
        "{\"client_status\":%u,\"client_body_size\":%u,\"client_body\":\"%.*s\"}",
        (unsigned)client_resp->status_code,
        (unsigned)client_resp->body_size,
        (int)((client_resp->body_size < 200) ? client_resp->body_size : 200),
        (client_resp->body != NULL) ? (char *)client_resp->body : "");

    axl_http_response_set_json(resp, result_buf);
    axl_http_client_response_free(client_resp);
    axl_http_client_free(c);
    return 0;
}

// ---------------------------------------------------------------------------
// Route lookup test handlers — verify exact vs prefix routing
// ---------------------------------------------------------------------------

static int
on_route_test_exact(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req; (void)data;
    axl_http_response_set_text(resp, "exact");
    return 0;
}

static int
on_route_test_prefix(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req; (void)data;
    axl_http_response_set_text(resp, "prefix");
    return 0;
}

static int
on_route_test_nested_exact(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req; (void)data;
    axl_http_response_set_text(resp, "nested-exact");
    return 0;
}

static int
on_route_test_nested_prefix(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req; (void)data;
    axl_http_response_set_text(resp, "nested-prefix");
    return 0;
}

// ---------------------------------------------------------------------------
// Auth callback — accepts "Authorization: Bearer test-token"
// ---------------------------------------------------------------------------

static int
test_auth_callback(
    AxlHttpRequest *req,
    AxlAuthInfo *auth_out,
    void *data)
{
    (void)data;
    const char *auth = axl_hash_table_lookup(req->headers, "authorization");
    if (auth == NULL) {
        return -1;
    }
    if (axl_strcmp(auth, "Bearer test-token") == 0) {
        auth_out->username = "testuser";
        auth_out->role = AXL_ROUTE_AUTH;
        return 0;
    }
    return -1;
}

static int
on_secret(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"secret\":\"data\"}");
    return 0;
}

// ---------------------------------------------------------------------------
// Cached route — handler increments a counter each call
// ---------------------------------------------------------------------------

static size_t cache_hit_counter = 0;

static int
on_cached(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    cache_hit_counter++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"count\":%zu}", cache_hit_counter);
    axl_http_response_set_json(resp, buf);
    return 0;
}

// ---------------------------------------------------------------------------
// Upload streaming handler — tracks chunks and total bytes
// ---------------------------------------------------------------------------

static size_t upload_chunk_count = 0;
static size_t upload_total_bytes = 0;

static int
on_upload(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    const void      *chunk,
    size_t           chunk_size,
    void            *data)
{
    (void)req;
    (void)data;

    if (chunk == NULL && chunk_size == 0) {
        /* Final call — set response */
        char buf[128];
        axl_snprintf(buf, sizeof(buf),
            "{\"chunks\":%llu,\"total\":%llu}",
            (unsigned long long)upload_chunk_count,
            (unsigned long long)upload_total_bytes);
        axl_http_response_set_json(resp, buf);

        /* Reset for next upload */
        upload_chunk_count = 0;
        upload_total_bytes = 0;
        return 0;
    }

    upload_chunk_count++;
    upload_total_bytes += chunk_size;
    return 0;
}

// ---------------------------------------------------------------------------
// Cache policy test handlers — cache_max eviction, per-route TTL,
// prefix invalidation. Each handler increments its own static counter
// and returns it in JSON so the test script can verify whether the
// handler actually ran (cache miss) or not (cache hit).
// ---------------------------------------------------------------------------

static size_t cm_counters[4] = {0, 0, 0, 0};

static int
on_cm_route(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    size_t idx = (size_t)(uintptr_t)data;
    cm_counters[idx]++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"n\":%zu,\"count\":%zu}",
                 idx + 1, cm_counters[idx]);
    axl_http_response_set_json(resp, buf);
    return 0;
}

static size_t ttl_short_count = 0;
static size_t ttl_long_count  = 0;

static int
on_ttl_short(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    ttl_short_count++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"route\":\"short\",\"count\":%zu}", ttl_short_count);
    axl_http_response_set_json(resp, buf);
    return 0;
}

static int
on_ttl_long(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    ttl_long_count++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"route\":\"long\",\"count\":%zu}", ttl_long_count);
    axl_http_response_set_json(resp, buf);
    return 0;
}

static size_t users_1_count = 0;
static size_t users_2_count = 0;
static size_t posts_1_count = 0;

static int on_users_1(AxlHttpRequest *req, AxlHttpResponse *resp, void *data) {
    (void)req; (void)data;
    users_1_count++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"user\":1,\"count\":%zu}", users_1_count);
    axl_http_response_set_json(resp, buf);
    return 0;
}

static int on_users_2(AxlHttpRequest *req, AxlHttpResponse *resp, void *data) {
    (void)req; (void)data;
    users_2_count++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"user\":2,\"count\":%zu}", users_2_count);
    axl_http_response_set_json(resp, buf);
    return 0;
}

static int on_posts_1(AxlHttpRequest *req, AxlHttpResponse *resp, void *data) {
    (void)req; (void)data;
    posts_1_count++;
    char buf[64];
    axl_snprintf(buf, sizeof(buf), "{\"post\":1,\"count\":%zu}", posts_1_count);
    axl_http_response_set_json(resp, buf);
    return 0;
}

/* Calls cache_invalidate with a prefix. Does not set resp.body so
   the response itself is not cached (dispatch stores only when
   body != NULL and body_size > 0). */
static int
on_invalidate_users(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    AxlHttpServer *server = data;
    axl_http_server_cache_invalidate(server, "/api/users");
    resp->status_code = 204;
    return 0;
}

// ---------------------------------------------------------------------------
// WebSocket echo handler — echoes text frames back
// ---------------------------------------------------------------------------

static AxlHttpServer *ws_test_server = NULL;

static int
on_ws_echo(
    size_t event,
    const void *frame,
    size_t frame_size,
    void *data)
{
    (void)data;

    if (event == AXL_WS_TEXT && ws_test_server != NULL) {
        axl_http_server_ws_broadcast(ws_test_server, "/ws-echo",
                                     frame, frame_size);
    }

    return 0;
}

static int
run_serve_mode(void)
{
    AxlHttpServer *s;

    s = axl_http_server_new(8080);
    if (s == NULL) {
        axl_printf("ERROR: failed to create HTTP server\n");
        return -1;
    }

    axl_http_server_add_route(s, "GET",  "/api/version",  on_get_version,  NULL);
    axl_http_server_add_route(s, "GET",  "/api/health",   on_get_health,   NULL);
    axl_http_server_add_route(s, "GET",  "/plain",        on_get_plain,    NULL);
    axl_http_server_add_route(s, "POST", "/echo",         on_echo,         NULL);
    axl_http_server_add_route(s, "GET",  "/client-test",  on_client_test,  NULL);

    /*
     * Route lookup tests — verify exact and prefix routes coexist at the
     * same path. Before the split-tree fix, the second of these silently
     * overwrote the first because both produced the same radix key, and
     * /rt/ matched as longest prefix of /rt/anything.
     */
    axl_http_server_add_route(s, "GET", "/rt/",         on_route_test_exact,  NULL);
    axl_http_server_add_route(s, "GET", "/rt/*",        on_route_test_prefix, NULL);
    axl_http_server_add_route(s, "GET", "/rt/files",    on_route_test_nested_exact,  NULL);
    axl_http_server_add_route(s, "GET", "/rt/files/*",  on_route_test_nested_prefix, NULL);

    /* Auth-protected route */
    axl_http_server_use_auth(s, test_auth_callback, NULL);
    axl_http_server_add_route_auth(s, "GET", "/secret", on_secret, NULL,
                                   AXL_ROUTE_AUTH);

    /* Cached route — use a small cache_max (3) so the cache_max
       eviction test below can force FIFO eviction with a handful
       of distinct keys. Existing tests are immune to eviction
       because they check the response body, not the cache state. */
    axl_http_server_use_cache(s, 3);
    axl_http_server_add_route(s, "GET", "/cached", on_cached, NULL);

    /* Cache policy tests — see handlers above. */
    axl_http_server_add_route(s, "GET", "/cm/1", on_cm_route, (void *)(uintptr_t)0);
    axl_http_server_add_route(s, "GET", "/cm/2", on_cm_route, (void *)(uintptr_t)1);
    axl_http_server_add_route(s, "GET", "/cm/3", on_cm_route, (void *)(uintptr_t)2);
    axl_http_server_add_route(s, "GET", "/cm/4", on_cm_route, (void *)(uintptr_t)3);

    axl_http_server_add_route(s, "GET", "/ttl-short", on_ttl_short, NULL);
    axl_http_server_add_route(s, "GET", "/ttl-long",  on_ttl_long,  NULL);
    axl_http_server_set_route_ttl(s, "/ttl-short", 150);

    axl_http_server_add_route(s, "GET", "/api/users/1", on_users_1, NULL);
    axl_http_server_add_route(s, "GET", "/api/users/2", on_users_2, NULL);
    axl_http_server_add_route(s, "GET", "/api/posts/1", on_posts_1, NULL);
    axl_http_server_add_route(s, "GET", "/invalidate-users", on_invalidate_users, s);

    /* Upload streaming endpoint */
    axl_http_server_set(s, "upload.chunk.size", "1024");
    axl_http_server_add_upload_route(s, "POST", "/upload", on_upload, NULL);

    /* WebSocket echo endpoint */
    ws_test_server = s;
    axl_http_server_add_websocket(s, "/ws-echo", on_ws_echo, NULL);

    axl_printf("HTTP server listening on port 8080\n");
    axl_printf("Routes: /api/version, /api/health, /plain, /echo, /client-test, /secret, /cached, /ws-echo, /rt/*\n");
    axl_printf("READY\n");

    return axl_http_server_run(s);
}

// ---------------------------------------------------------------------------
// HTTPS Server Mode — "serve-tls" starts an HTTPS server
// ---------------------------------------------------------------------------

static int
run_serve_tls_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        return -1;
    }

    axl_net_auto_init(SIZE_MAX, 10);

    if (axl_tls_init() != 0) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    /* Generate self-signed cert */
    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;

    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len,
                                     &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    axl_printf("Generated self-signed cert (%zu bytes)\n", cert_len);

    AxlHttpServer *s = axl_http_server_new(8443);
    if (s == NULL) {
        axl_printf("ERROR: failed to create server\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }

    if (axl_http_server_use_tls(s, cert, cert_len, key, key_len) != 0) {
        axl_printf("ERROR: TLS setup failed\n");
        axl_http_server_free(s);
        axl_free(cert);
        axl_free(key);
        return -1;
    }

    axl_free(cert);
    axl_free(key);

    axl_http_server_add_route(s, "GET", "/api/version", on_get_version, NULL);
    axl_http_server_add_route(s, "GET", "/plain",       on_get_plain,   NULL);

    axl_printf("HTTPS server listening on port 8443\n");
    axl_printf("READY\n");

    return axl_http_server_run(s);
}

// ---------------------------------------------------------------------------
// URL encode / decode tests
// ---------------------------------------------------------------------------

static void
test_url_encode_decode(void)
{
    char buf[256];
    int  n;

    /* Simple path — no encoding needed */
    n = axl_url_encode("/path/to/file.txt", buf, sizeof(buf));
    test_check(n > 0, "url encode: simple returns > 0");
    test_check(axl_strcmp(buf, "/path/to/file.txt") == 0,
               "url encode: simple unchanged");

    /* Spaces */
    n = axl_url_encode("/my file.txt", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "/my%20file.txt") == 0,
               "url encode: space");

    /* Special chars */
    n = axl_url_encode("/dir?query#frag", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "/dir%3Fquery%23frag") == 0,
               "url encode: special chars");

    /* Slash preserved */
    n = axl_url_encode("/a/b/c", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "/a/b/c") == 0,
               "url encode: slashes preserved");

    /* Unreserved chars pass through */
    n = axl_url_encode("hello-world_v2.0~draft", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "hello-world_v2.0~draft") == 0,
               "url encode: unreserved passthrough");

    /* Decode basic */
    n = axl_url_decode("/my%20file.txt", buf, sizeof(buf));
    test_check(n > 0, "url decode: basic returns > 0");
    test_check(axl_strcmp(buf, "/my file.txt") == 0,
               "url decode: space");

    /* Decode special chars */
    n = axl_url_decode("/dir%3Fquery%23frag", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "/dir?query#frag") == 0,
               "url decode: special chars");

    /* Decode lowercase hex */
    n = axl_url_decode("%2f%2F", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "//") == 0,
               "url decode: lowercase hex");

    /* Roundtrip */
    const char *original = "/path/with spaces & stuff?yes";
    axl_url_encode(original, buf, sizeof(buf));
    char decoded[256];
    axl_url_decode(buf, decoded, sizeof(decoded));
    test_check(axl_strcmp(decoded, original) == 0,
               "url encode/decode: roundtrip");

    /* Buffer too small */
    test_check(axl_url_encode("/a b c d e", buf, 5) == -1,
               "url encode: buffer too small");

    /* NULL safety */
    test_check(axl_url_encode(NULL, buf, sizeof(buf)) == -1,
               "url encode: NULL src");
    test_check(axl_url_decode(NULL, buf, sizeof(buf)) == -1,
               "url decode: NULL src");

    /* Malformed percent — passes through */
    n = axl_url_decode("100%", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "100%") == 0,
               "url decode: trailing percent passthrough");
}

// ---------------------------------------------------------------------------
// HTTP Accept content negotiation tests
// ---------------------------------------------------------------------------

static void
test_http_accepts(void)
{
    /* Basic match */
    test_check(axl_http_accepts("application/json", "application/json"),
               "accepts: exact match");

    /* Multiple types */
    test_check(axl_http_accepts("text/html, application/json", "application/json"),
               "accepts: second in list");

    /* With quality parameter */
    test_check(axl_http_accepts("text/html;q=0.9, application/json;q=1.0", "application/json"),
               "accepts: with quality param");

    /* Wildcard */
    test_check(axl_http_accepts("*/*", "application/json"),
               "accepts: wildcard");

    /* Case-insensitive */
    test_check(axl_http_accepts("Application/JSON", "application/json"),
               "accepts: case-insensitive");

    /* No match */
    test_check(!axl_http_accepts("text/html", "application/json"),
               "accepts: no match");

    /* NULL safety */
    test_check(!axl_http_accepts(NULL, "application/json"),
               "accepts: NULL header");
    test_check(!axl_http_accepts("text/html", NULL),
               "accepts: NULL media type");

    /* Partial name should not match */
    test_check(!axl_http_accepts("application/jsonl", "application/json"),
               "accepts: partial no match");

    /* Whitespace handling */
    test_check(axl_http_accepts("text/html , application/json", "application/json"),
               "accepts: whitespace around comma");
}

// ---------------------------------------------------------------------------
// HTTP Range parsing tests
// ---------------------------------------------------------------------------

static void
test_http_parse_range(void)
{
    AxlHttpRange r;

    /* bytes=0-499 with 1000-byte file */
    test_check(axl_http_parse_range("bytes=0-499", 1000, &r), "range: bytes=0-499");
    test_check(r.start == 0 && r.end == 499, "range: 0-499 values");
    test_check(r.total == 1000, "range: total is file_size");

    /* bytes=500-999 */
    test_check(axl_http_parse_range("bytes=500-999", 1000, &r), "range: bytes=500-999");
    test_check(r.start == 500 && r.end == 999, "range: 500-999 values");

    /* Open-ended: bytes=500- */
    test_check(axl_http_parse_range("bytes=500-", 1000, &r), "range: bytes=500-");
    test_check(r.start == 500 && r.end == 999, "range: 500- fills to end");

    /* Suffix: bytes=-500 (last 500 bytes) */
    test_check(axl_http_parse_range("bytes=-500", 1000, &r), "range: bytes=-500");
    test_check(r.start == 500 && r.end == 999, "range: -500 suffix values");

    /* Clamp end to file size */
    test_check(axl_http_parse_range("bytes=0-1999", 1000, &r), "range: clamp end");
    test_check(r.end == 999, "range: clamped to 999");

    /* Start at/past end of file — invalid */
    test_check(!axl_http_parse_range("bytes=1000-", 1000, &r), "range: start=size invalid");

    /* NULL header */
    test_check(!axl_http_parse_range(NULL, 1000, &r), "range: NULL header");

    /* Invalid prefix */
    test_check(!axl_http_parse_range("notbytes=0-10", 1000, &r), "range: bad prefix");

    /* Empty string */
    test_check(!axl_http_parse_range("", 1000, &r), "range: empty string");

    /* Case-insensitive prefix */
    test_check(axl_http_parse_range("Bytes=0-99", 1000, &r), "range: case-insensitive");
    test_check(r.start == 0 && r.end == 99, "range: case-insensitive values");

    /* Zero file size */
    test_check(!axl_http_parse_range("bytes=0-0", 0, &r), "range: zero file size");
}

// ---------------------------------------------------------------------------
// IPv4 parse / format tests
// ---------------------------------------------------------------------------

static void
test_ipv4_parse_format(void)
{
    uint8_t octets[4];
    char    buf[32];

    /* Parse valid addresses */
    test_check(axl_ipv4_parse("192.168.1.1", octets) == 0, "ipv4 parse: 192.168.1.1");
    test_check(octets[0] == 192 && octets[1] == 168 &&
               octets[2] == 1 && octets[3] == 1, "ipv4 parse: octets correct");

    test_check(axl_ipv4_parse("0.0.0.0", octets) == 0, "ipv4 parse: 0.0.0.0");
    test_check(octets[0] == 0 && octets[3] == 0, "ipv4 parse: all zeros");

    test_check(axl_ipv4_parse("255.255.255.255", octets) == 0, "ipv4 parse: 255.255.255.255");
    test_check(octets[0] == 255 && octets[3] == 255, "ipv4 parse: all 255");

    test_check(axl_ipv4_parse("10.0.0.1", octets) == 0, "ipv4 parse: 10.0.0.1");

    /* Parse invalid addresses */
    test_check(axl_ipv4_parse("256.0.0.1", octets) == -1, "ipv4 parse: 256 octet");
    test_check(axl_ipv4_parse("1.2.3", octets) == -1, "ipv4 parse: 3 octets");
    test_check(axl_ipv4_parse("abc", octets) == -1, "ipv4 parse: letters");
    test_check(axl_ipv4_parse("", octets) == -1, "ipv4 parse: empty");
    test_check(axl_ipv4_parse(NULL, octets) == -1, "ipv4 parse: NULL str");
    test_check(axl_ipv4_parse("1.2.3.4", NULL) == -1, "ipv4 parse: NULL octets");
    test_check(axl_ipv4_parse("1.2.3.4.5", octets) == -1, "ipv4 parse: 5 octets");

    /* Format */
    uint8_t addr[] = { 10, 0, 0, 1 };
    test_check(axl_ipv4_format(addr, buf, sizeof(buf)) == 0, "ipv4 format: 10.0.0.1");
    test_check(axl_strcmp(buf, "10.0.0.1") == 0, "ipv4 format: string correct");

    /* Format roundtrip */
    axl_ipv4_parse("192.168.100.200", octets);
    axl_ipv4_format(octets, buf, sizeof(buf));
    test_check(axl_strcmp(buf, "192.168.100.200") == 0, "ipv4 format: roundtrip");

    /* Format small buffer */
    test_check(axl_ipv4_format(addr, buf, 4) == -1, "ipv4 format: small buffer");

    /* Format NULL safety */
    test_check(axl_ipv4_format(NULL, buf, sizeof(buf)) == -1, "ipv4 format: NULL octets");
    test_check(axl_ipv4_format(addr, NULL, 16) == -1, "ipv4 format: NULL buf");
}

// ---------------------------------------------------------------------------
// UDP Echo Test Mode — "udp-echo <host> <port>" sends a test datagram
// and prints the response. Used by test-udp.sh.
// ---------------------------------------------------------------------------

static int
run_udp_echo_mode(const char *host, const char *port_str)
{
    AxlIPv4Address dest;
    uint8_t octets[4];

    if (axl_ipv4_parse(host, octets) != 0) {
        axl_printf("UDP-ECHO: invalid host '%s'\n", host);
        return 1;
    }
    axl_memcpy(&dest, octets, 4);

    uint16_t port = (uint16_t)axl_strtou64(port_str);
    if (port == 0) {
        axl_printf("UDP-ECHO: invalid port '%s'\n", port_str);
        return 1;
    }

    /* Init networking */
    axl_net_auto_init(SIZE_MAX, 10);

    AxlUdpSocket *sock = NULL;
    if (axl_udp_open(&sock, 0) != 0) {
        axl_printf("UDP-ECHO: failed to open socket\n");
        return 1;
    }

    const char *msg = "hello from UEFI";
    char rx_buf[256];
    size_t rx_len = 0;

    axl_printf("UDP-ECHO: sending to %s:%u\n", host, (unsigned)port);

    int rc = axl_udp_sendrecv(sock, &dest, port,
                              msg, axl_strlen(msg),
                              rx_buf, sizeof(rx_buf) - 1, &rx_len,
                              5000);

    if (rc == 0 && rx_len > 0) {
        rx_buf[rx_len] = '\0';
        axl_printf("UDP-ECHO: received %zu bytes: %s\n", rx_len, rx_buf);
        axl_printf("PASS: udp-echo-response\n");
    } else {
        axl_printf("UDP-ECHO: no response (rc=%d)\n", rc);
        axl_printf("FAIL: udp-echo-response\n");
    }

    axl_udp_close(sock);
    return (rc == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// AxlInetAddress Tests (no network)
// ---------------------------------------------------------------------------

static void
test_inet_address_from_string(void)
{
    AxlInetAddress *addr = axl_inet_address_new_from_string("10.0.0.1");
    test_check(addr != NULL, "inet_address from_string: not NULL");
    if (addr != NULL) {
        const uint8_t *bytes = axl_inet_address_to_bytes(addr);
        test_check(bytes[0] == 10 && bytes[1] == 0 &&
                   bytes[2] == 0 && bytes[3] == 1,
                   "inet_address from_string: bytes correct");
        axl_inet_address_free(addr);
    }
}

static void
test_inet_address_from_bytes(void)
{
    uint8_t raw[] = { 192, 168, 1, 1 };
    AxlInetAddress *addr = axl_inet_address_new_from_bytes(raw);
    test_check(addr != NULL, "inet_address from_bytes: not NULL");
    if (addr != NULL) {
        const char *str = axl_inet_address_to_string(addr);
        test_check(str != NULL && axl_strcmp(str, "192.168.1.1") == 0,
                   "inet_address from_bytes: to_string correct");
        axl_inet_address_free(addr);
    }
}

static void
test_inet_address_any(void)
{
    AxlInetAddress *addr = axl_inet_address_new_any();
    test_check(addr != NULL, "inet_address any: not NULL");
    if (addr != NULL) {
        test_check(axl_inet_address_is_any(addr), "inet_address any: is_any");
        test_check(!axl_inet_address_is_loopback(addr), "inet_address any: not loopback");
        const uint8_t *bytes = axl_inet_address_to_bytes(addr);
        test_check(bytes[0] == 0 && bytes[1] == 0 &&
                   bytes[2] == 0 && bytes[3] == 0,
                   "inet_address any: bytes all zero");
        axl_inet_address_free(addr);
    }
}

static void
test_inet_address_loopback(void)
{
    AxlInetAddress *addr = axl_inet_address_new_loopback();
    test_check(addr != NULL, "inet_address loopback: not NULL");
    if (addr != NULL) {
        test_check(axl_inet_address_is_loopback(addr), "inet_address loopback: is_loopback");
        test_check(!axl_inet_address_is_any(addr), "inet_address loopback: not any");
        const uint8_t *bytes = axl_inet_address_to_bytes(addr);
        test_check(bytes[0] == 127 && bytes[1] == 0 &&
                   bytes[2] == 0 && bytes[3] == 1,
                   "inet_address loopback: bytes 127.0.0.1");
        axl_inet_address_free(addr);
    }
}

static void
test_inet_address_equal(void)
{
    AxlInetAddress *a = axl_inet_address_new_from_string("10.0.0.1");
    AxlInetAddress *b = axl_inet_address_new_from_string("10.0.0.1");
    AxlInetAddress *c = axl_inet_address_new_from_string("10.0.0.2");

    test_check(axl_inet_address_equal(a, b), "inet_address equal: same");
    test_check(!axl_inet_address_equal(a, c), "inet_address equal: different");
    test_check(!axl_inet_address_equal(a, NULL), "inet_address equal: NULL");

    axl_inet_address_free(a);
    axl_inet_address_free(b);
    axl_inet_address_free(c);
}

static void
test_inet_address_to_string_cache(void)
{
    uint8_t raw[] = { 172, 16, 0, 1 };
    AxlInetAddress *addr = axl_inet_address_new_from_bytes(raw);

    test_check(addr != NULL, "inet_address cache: not NULL");
    if (addr != NULL) {
        const char *s1 = axl_inet_address_to_string(addr);
        const char *s2 = axl_inet_address_to_string(addr);
        test_check(s1 == s2, "inet_address cache: same pointer");
        test_check(s1 != NULL && axl_strcmp(s1, "172.16.0.1") == 0,
                   "inet_address cache: correct value");
        axl_inet_address_free(addr);
    }
}

static void
test_inet_address_invalid(void)
{
    test_check(axl_inet_address_new_from_string("garbage") == NULL,
               "inet_address invalid: garbage");
    test_check(axl_inet_address_new_from_string("") == NULL,
               "inet_address invalid: empty");
    test_check(axl_inet_address_new_from_string(NULL) == NULL,
               "inet_address invalid: NULL");
    test_check(axl_inet_address_new_from_bytes(NULL) == NULL,
               "inet_address invalid: NULL bytes");
}

// ---------------------------------------------------------------------------
// AxlSocketAddress Tests (no network)
// ---------------------------------------------------------------------------

static void
test_socket_address_new(void)
{
    AxlInetAddress *addr = axl_inet_address_new_from_string("10.0.0.1");
    AxlSocketAddress *sa = axl_socket_address_new(addr, 8080);

    test_check(sa != NULL, "socket_address new: not NULL");
    if (sa != NULL) {
        test_check(axl_socket_address_get_port(sa) == 8080,
                   "socket_address new: port 8080");
        AxlInetAddress *got = axl_socket_address_get_address(sa);
        test_check(got != NULL, "socket_address new: address not NULL");
        if (got != NULL) {
            const char *str = axl_inet_address_to_string(got);
            test_check(str != NULL && axl_strcmp(str, "10.0.0.1") == 0,
                       "socket_address new: address correct");
        }
        axl_socket_address_free(sa);
    }
}

static void
test_socket_address_from_string(void)
{
    AxlSocketAddress *sa = axl_socket_address_new_from_string("10.0.0.1:8080", 0);

    test_check(sa != NULL, "socket_address from_string: not NULL");
    if (sa != NULL) {
        test_check(axl_socket_address_get_port(sa) == 8080,
                   "socket_address from_string: port 8080");
        AxlInetAddress *addr = axl_socket_address_get_address(sa);
        const char *str = axl_inet_address_to_string(addr);
        test_check(str != NULL && axl_strcmp(str, "10.0.0.1") == 0,
                   "socket_address from_string: address correct");
        axl_socket_address_free(sa);
    }
}

static void
test_socket_address_default_port(void)
{
    AxlSocketAddress *sa = axl_socket_address_new_from_string("10.0.0.1", 80);

    test_check(sa != NULL, "socket_address default_port: not NULL");
    if (sa != NULL) {
        test_check(axl_socket_address_get_port(sa) == 80,
                   "socket_address default_port: port 80");
        axl_socket_address_free(sa);
    }
}

static void
test_socket_address_to_ipv4(void)
{
    AxlSocketAddress *sa = axl_socket_address_new_from_string("192.168.1.100:514", 0);

    test_check(sa != NULL, "socket_address to_ipv4: not NULL");
    if (sa != NULL) {
        AxlIPv4Address ipv4;
        uint16_t port;
        axl_socket_address_to_ipv4(sa, &ipv4, &port);
        test_check(ipv4.addr[0] == 192 && ipv4.addr[1] == 168 &&
                   ipv4.addr[2] == 1 && ipv4.addr[3] == 100,
                   "socket_address to_ipv4: address correct");
        test_check(port == 514, "socket_address to_ipv4: port correct");
        axl_socket_address_free(sa);
    }
}

static void
test_socket_address_invalid(void)
{
    test_check(axl_socket_address_new_from_string("garbage:80", 0) == NULL,
               "socket_address invalid: bad host");
    test_check(axl_socket_address_new_from_string(":80", 0) == NULL,
               "socket_address invalid: empty host");
    test_check(axl_socket_address_new_from_string(NULL, 0) == NULL,
               "socket_address invalid: NULL");
    test_check(axl_socket_address_new_from_string("10.0.0.1:", 0) == NULL,
               "socket_address invalid: trailing colon");
    test_check(axl_socket_address_new(NULL, 80) == NULL,
               "socket_address invalid: NULL addr");
}

// ---------------------------------------------------------------------------
// AxlSocketClient Tests (network required)
// ---------------------------------------------------------------------------

static void
test_socket_client_connect(void)
{
    AxlSocket *listener;
    AxlSocket *sock;
    AxlSocketClient *client;
    int ret;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket_client connect (no network)\n");
        return;
    }

    /* Set up listener */
    listener = axl_socket_new(AXL_SOCKET_STREAM);
    ret = axl_socket_listen(listener, 9994);
    if (ret != 0) {
        axl_printf("SKIP: socket_client listen failed\n");
        axl_socket_free(listener);
        return;
    }

    /* Connect via client */
    AxlIPv4Address local_ip;
    char ip_str[16];
    axl_net_get_ip_address(&local_ip);
    axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
        local_ip.addr[0], local_ip.addr[1],
        local_ip.addr[2], local_ip.addr[3]);

    client = axl_socket_client_new();
    test_check(client != NULL, "socket_client: new");

    ret = axl_socket_client_connect_to_host(client, ip_str, 9994, &sock);
    if (ret != 0) {
        /* QEMU user-mode networking (SLIRP) does not route packets
           back to the guest, so self-connect fails.  Skip gracefully. */
        axl_printf("SKIP: socket_client connect_to_host (no loopback)\n");
    } else {
        test_check(ret == 0, "socket_client: connect_to_host");
        test_check(axl_socket_get_type(sock) == AXL_SOCKET_STREAM,
                   "socket_client: type is stream");
        axl_socket_free(sock);
    }

    axl_socket_client_free(client);
    axl_socket_free(listener);
}

static void
test_socket_client_invalid(void)
{
    AxlSocketClient *client = axl_socket_client_new();
    AxlSocket *sock = NULL;

    /* NULL host */
    test_check(axl_socket_client_connect_to_host(client, NULL, 80, &sock) == -1,
               "socket_client: NULL host");

    /* NULL out_sock */
    test_check(axl_socket_client_connect_to_host(client, "10.0.0.1", 80, NULL) == -1,
               "socket_client: NULL out_sock");

    axl_socket_client_free(client);
}

// ---------------------------------------------------------------------------
// AxlSocket Tests (network required)
// ---------------------------------------------------------------------------

static void
test_socket_stream_echo(void)
{
    AxlSocket *listener;
    AxlSocket *client;
    AxlSocket *accepted;
    int ret;
    char send_buf[] = "Hello AxlSocket";
    char recv_buf[64];
    size_t recv_size;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket stream echo (no network)\n");
        return;
    }

    /* Listen */
    listener = axl_socket_new(AXL_SOCKET_STREAM);
    test_check(listener != NULL, "socket stream: new listener");
    if (listener == NULL) {
        return;
    }

    ret = axl_socket_listen(listener, 9998);
    if (ret != 0) {
        axl_printf("SKIP: socket stream listen failed\n");
        axl_socket_free(listener);
        return;
    }

    /* Connect */
    AxlIPv4Address local_ip;
    char ip_str[16];
    axl_net_get_ip_address(&local_ip);
    axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
        local_ip.addr[0], local_ip.addr[1],
        local_ip.addr[2], local_ip.addr[3]);

    client = axl_socket_new(AXL_SOCKET_STREAM);
    AxlSocketAddress *remote = axl_socket_address_new(
        axl_inet_address_new_from_string(ip_str), 9998);

    ret = axl_socket_connect(client, remote);
    axl_socket_address_free(remote);

    if (ret != 0) {
        axl_printf("SKIP: socket stream connect failed\n");
        axl_socket_free(client);
        axl_socket_free(listener);
        return;
    }

    /* Accept */
    accepted = NULL;
    for (size_t i = 0; i < 100 && accepted == NULL; i++) {
        ret = axl_socket_accept(listener, &accepted, 100);
        if (ret != 0) {
            axl_usleep(10000);
        }
    }

    if (accepted == NULL) {
        axl_printf("SKIP: socket stream accept failed\n");
        axl_socket_free(client);
        axl_socket_free(listener);
        return;
    }

    /* Send from client, recv on server */
    ret = axl_socket_send(client, send_buf, axl_strlen(send_buf), 0);
    test_check(ret == 0, "socket stream: send");

    recv_size = sizeof(recv_buf) - 1;
    ret = axl_socket_receive(accepted, recv_buf, &recv_size, 0);
    test_check(ret == 0, "socket stream: receive");
    if (ret == 0) {
        recv_buf[recv_size] = '\0';
        test_check(axl_strcmp(recv_buf, "Hello AxlSocket") == 0,
                   "socket stream: echo match");
    }

    axl_socket_free(accepted);
    axl_socket_free(client);
    axl_socket_free(listener);
}

static void
test_socket_datagram_send(void)
{
    AxlSocket *sock;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket datagram (no network)\n");
        return;
    }

    sock = axl_socket_new(AXL_SOCKET_DATAGRAM);
    test_check(sock != NULL, "socket datagram: new");
    if (sock == NULL) {
        return;
    }

    test_check(axl_socket_get_type(sock) == AXL_SOCKET_DATAGRAM,
               "socket datagram: type correct");

    /* send_to should work (fire-and-forget, may fail if no route) */
    AxlSocketAddress *dest = axl_socket_address_new(
        axl_inet_address_new_loopback(), 9997);
    axl_socket_send_to(sock, "test", 4, dest);
    axl_socket_address_free(dest);

    /* send (stream-only) should fail on datagram */
    test_check(axl_socket_send(sock, "test", 4, 0) == -1,
               "socket datagram: send (stream-only) fails");

    axl_socket_free(sock);
}

static void
test_socket_get_addresses(void)
{
    AxlSocket *listener;
    AxlSocket *client;
    int ret;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket get_addresses (no network)\n");
        return;
    }

    listener = axl_socket_new(AXL_SOCKET_STREAM);
    ret = axl_socket_listen(listener, 9996);
    if (ret != 0) {
        axl_printf("SKIP: socket get_addresses listen failed\n");
        axl_socket_free(listener);
        return;
    }

    AxlIPv4Address local_ip;
    char ip_str[16];
    axl_net_get_ip_address(&local_ip);
    axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
        local_ip.addr[0], local_ip.addr[1],
        local_ip.addr[2], local_ip.addr[3]);

    client = axl_socket_new(AXL_SOCKET_STREAM);
    AxlSocketAddress *remote = axl_socket_address_new(
        axl_inet_address_new_from_string(ip_str), 9996);
    ret = axl_socket_connect(client, remote);
    axl_socket_address_free(remote);

    if (ret != 0) {
        axl_printf("SKIP: socket get_addresses connect failed\n");
        axl_socket_free(client);
        axl_socket_free(listener);
        return;
    }

    /* Check local address */
    AxlSocketAddress *local_sa = axl_socket_get_local_address(client);
    test_check(local_sa != NULL, "socket get_local_address: not NULL");
    if (local_sa != NULL) {
        test_check(axl_socket_address_get_port(local_sa) != 0,
                   "socket get_local_address: port non-zero");
        axl_socket_address_free(local_sa);
    }

    /* Check remote address */
    AxlSocketAddress *remote_sa = axl_socket_get_remote_address(client);
    test_check(remote_sa != NULL, "socket get_remote_address: not NULL");
    if (remote_sa != NULL) {
        test_check(axl_socket_address_get_port(remote_sa) == 9996,
                   "socket get_remote_address: port 9996");
        axl_socket_address_free(remote_sa);
    }

    axl_socket_free(client);
    axl_socket_free(listener);
}

static void
test_socket_type_errors(void)
{
    /* send_to on stream -> error */
    AxlSocket *stream = axl_socket_new(AXL_SOCKET_STREAM);
    test_check(stream != NULL, "socket type_errors: new stream");
    if (stream != NULL) {
        AxlSocketAddress *dest = axl_socket_address_new(
            axl_inet_address_new_loopback(), 80);
        test_check(axl_socket_send_to(stream, "x", 1, dest) == -1,
                   "socket type_errors: send_to on stream fails");
        axl_socket_address_free(dest);

        /* listen on datagram -> error */
        /* receive on unconnected stream -> error */
        size_t sz = 64;
        char buf[64];
        test_check(axl_socket_receive(stream, buf, &sz, 0) == -1,
                   "socket type_errors: receive on unconnected fails");

        axl_socket_free(stream);
    }

    /* listen on datagram -> error */
    AxlSocket *dgram = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (dgram != NULL) {
        test_check(axl_socket_listen(dgram, 9995) == -1,
                   "socket type_errors: listen on datagram fails");
        axl_socket_free(dgram);
    }
}

// ---------------------------------------------------------------------------
// AxlSocket UDP async receive (network required)
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop *loop;
    bool     received;
    size_t   recv_len;
} UdpRecvTestCtx;

static bool
on_udp_recv_test(AxlSocket *sock, int status, void *data)
{
    UdpRecvTestCtx *ctx = (UdpRecvTestCtx *)data;

    ctx->received = (status == 0);
    ctx->recv_len = axl_socket_receive_get_size(sock);
    axl_loop_quit(ctx->loop);
    return false;  /* single-fire test */
}

static bool
on_udp_recv_timeout(void *data)
{
    UdpRecvTestCtx *ctx = (UdpRecvTestCtx *)data;

    ctx->received = false;
    axl_loop_quit(ctx->loop);
    return AXL_SOURCE_REMOVE;
}

static void
test_socket_udp_async_recv(void)
{
    AxlSocket *receiver;
    AxlSocket *sender;
    char recv_buf[256];
    UdpRecvTestCtx ctx;
    const char *msg = "hello-udp-async";

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket UDP async recv (no network)\n");
        return;
    }

    /* Create receiver on a known port */
    receiver = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (receiver == NULL) {
        axl_printf("SKIP: socket UDP async recv (no UDP)\n");
        return;
    }

    if (axl_socket_bind(receiver, 9990) != 0) {
        axl_printf("SKIP: socket UDP async recv (bind 9990 failed)\n");
        axl_socket_free(receiver);
        return;
    }

    /* Set up async receive */
    AxlLoop *loop = axl_loop_new();
    ctx.loop = loop;
    ctx.received = false;
    ctx.recv_len = 0;

    int rc = axl_socket_receive_async(receiver, recv_buf,
                                      sizeof(recv_buf) - 1,
                                      loop, on_udp_recv_test, &ctx);
    test_check(rc == 0, "socket UDP async: receive_async");
    if (rc != 0) {
        axl_loop_free(loop);
        axl_socket_free(receiver);
        return;
    }

    /* Add a timeout so we don't hang forever */
    axl_loop_add_timeout(loop, 3000, on_udp_recv_timeout, &ctx);

    /* Send a datagram to the receiver */
    sender = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (sender != NULL) {
        AxlIPv4Address local_ip;
        char ip_str[16];
        axl_net_get_ip_address(&local_ip);
        axl_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
            local_ip.addr[0], local_ip.addr[1],
            local_ip.addr[2], local_ip.addr[3]);

        AxlSocketAddress *dest = axl_socket_address_new(
            axl_inet_address_new_from_string(ip_str), 9990);
        axl_socket_send_to(sender, msg, axl_strlen(msg), dest);
        axl_socket_address_free(dest);
        axl_socket_free(sender);
    }

    /* Run loop until receive callback or timeout */
    axl_loop_run(loop);

    /*
     * UDP self-send may not work in QEMU (SLIRP doesn't route
     * packets back to the guest's own IP). If we received data,
     * verify it; otherwise just note the skip.
     */
    if (ctx.received) {
        test_check(ctx.recv_len == axl_strlen(msg),
                   "socket UDP async: recv_len correct");
        recv_buf[ctx.recv_len] = '\0';
        test_check(axl_strcmp(recv_buf, msg) == 0,
                   "socket UDP async: data matches");
    } else {
        axl_printf("SKIP: socket UDP async recv (no loopback)\n");
    }

    /* Free the socket BEFORE the loop — axl_socket_free drives
       axl_udp_recv_stop, which calls axl_loop_remove_source on the
       loop it was registered with. Freeing the loop first leaves
       the socket holding a dangling loop pointer and a stale source
       id; the subsequent close path would then access freed memory
       and crash inside UDP4 Cancel's token-event access. */
    axl_socket_free(receiver);
    axl_loop_free(loop);
}

static void
test_socket_bind(void)
{
    AxlSocket *sock;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket bind (no network)\n");
        return;
    }

    sock = axl_socket_new(AXL_SOCKET_DATAGRAM);
    test_check(sock != NULL, "socket bind: new datagram");
    if (sock == NULL) {
        return;
    }

    /* Bind to a specific port */
    test_check(axl_socket_bind(sock, 9989) == 0,
               "socket bind: port 9989");

    /* Bind to a different port */
    test_check(axl_socket_bind(sock, 9988) == 0,
               "socket bind: rebind to 9988");

    /* Bind on stream should fail */
    AxlSocket *stream = axl_socket_new(AXL_SOCKET_STREAM);
    if (stream != NULL) {
        test_check(axl_socket_bind(stream, 9987) == -1,
                   "socket bind: stream fails");
        axl_socket_free(stream);
    }

    axl_socket_free(sock);
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_net_main(
    int argc,
    char **argv)
{
    axl_info("AxlTestNet starting");

    //
    // Check for "serve" argument -- start HTTP server mode
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve") == 0) {
        return run_serve_mode();
    }

    //
    // Check for "serve-tls" argument -- start HTTPS server mode
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-tls") == 0) {
        return run_serve_tls_mode();
    }

    //
    // Check for "udp-echo" argument -- send UDP datagram to host
    //
    if (argc >= 4 && axl_strcmp(argv[1], "udp-echo") == 0) {
        return run_udp_echo_mode(argv[2], argv[3]);
    }

    //
    // Normal unit test mode
    //
    test_print_header("AxlTestNet");

    //
    // URL parsing (no network)
    //
    axl_printf("--- URL Parsing ---\n");
    test_url_parse_basic();
    test_url_parse_https();
    test_url_parse_minimal();
    test_url_parse_malformed();
    test_url_build();
    test_url_encode_decode();

    //
    // HTTP core parsing (no network)
    //
    axl_printf("\n--- HTTP Core Parsing ---\n");
    test_http_find_header_end();
    test_http_parse_status();
    test_http_parse_request();
    test_http_parse_headers();
    test_http_parse_range();
    test_http_accepts();

    //
    // IPv4 parse / format (no network)
    //
    axl_printf("\n--- IPv4 Parse/Format ---\n");
    test_ipv4_parse_format();

    //
    // AxlInetAddress (no network)
    //
    axl_printf("\n--- InetAddress ---\n");
    test_inet_address_from_string();
    test_inet_address_from_bytes();
    test_inet_address_any();
    test_inet_address_loopback();
    test_inet_address_equal();
    test_inet_address_to_string_cache();
    test_inet_address_invalid();

    //
    // AxlSocketAddress (no network)
    //
    axl_printf("\n--- SocketAddress ---\n");
    test_socket_address_new();
    test_socket_address_from_string();
    test_socket_address_default_port();
    test_socket_address_to_ipv4();
    test_socket_address_invalid();

    //
    // Network tests
    //
    axl_printf("\n--- Network ---\n");
    test_net_available();
    test_tcp_echo();
    test_tcp_recv_async_rearm();
    test_http_round_trip();

    //
    // AxlSocket (network required)
    //
    axl_printf("\n--- Socket ---\n");
    test_socket_stream_echo();
    test_socket_datagram_send();
    test_socket_get_addresses();
    test_socket_type_errors();

    //
    // AxlSocket bind + UDP async (network required)
    //
    axl_printf("\n--- Socket Bind/UDP ---\n");
    test_socket_bind();
    test_socket_udp_async_recv();

    //
    // AxlSocketClient (network required)
    //
    axl_printf("\n--- SocketClient ---\n");
    test_socket_client_connect();
    test_socket_client_invalid();

    return test_print_results();
}

AXL_APP(test_net_main)
