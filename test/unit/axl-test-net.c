/** @file axl-test-net.c
    Test application for AxlNet — exercises URL parsing, HTTP core parsing,
    network utilities, TCP sockets, and HTTP server/client round-trip.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-time.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>
#include <axl/axl-http-core.h>
#include <axl/axl-shell.h>
#include <axl/axl-console-mirror.h>
#include <axl/axl-file-view.h>   /* read-back of the edited file (rung 3) */
#include <uefi/axl-uefi.h>   /* gST + console protocols for mirror-selftest */

AXL_LOG_DOMAIN("test");

/* Slirp echo target for TCP. The unit-test runner wires `guestfwd`
   rules that intercept connections to this address (any port the
   runner was told about) and forward the byte stream to a host-side
   stream-echo server. Everything sent to it comes straight back.
   Slirp does not intercept connections to the guest's own DHCP IP,
   so client-side TCP tests use this fixed IP instead. */
#define AXL_TEST_ECHO_HOST  "10.0.2.100"

/* UDP echo target. Slirp's `guestfwd` is TCP-only, but UDP
   datagrams sent to the slirp gateway (10.0.2.2) are delivered to
   the host's loopback natively. The unit-test runner pins a fixed
   port so the guest-side test code can target it directly. */
#define AXL_TEST_UDP_ECHO_HOST  "10.0.2.2"
#define AXL_TEST_UDP_ECHO_PORT  35555

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
test_url_parse_userinfo_full(void)
{
    /* http://user:pass@host:port/path?q#frag — every URI component
       populated, including userinfo and fragment. */
    AxlUrl *u;
    int ret = axl_url_parse(
        "http://alice:s3cret@host.example.com:8080/api?v=1#section",
        &u);
    test_check(ret == 0, "URL parse: full userinfo+fragment");
    if (ret == 0) {
        test_check(u->user != NULL &&
                   axl_strcmp(u->user, "alice") == 0,
                   "URL user is 'alice'");
        test_check(u->password != NULL &&
                   axl_strcmp(u->password, "s3cret") == 0,
                   "URL password is 's3cret'");
        test_check(axl_strcmp(u->host, "host.example.com") == 0,
                   "URL host after @ is host.example.com");
        test_check(u->port == 8080, "URL port 8080");
        test_check(axl_strcmp(u->path, "/api") == 0, "URL path /api");
        test_check(u->query != NULL && axl_strcmp(u->query, "v=1") == 0,
                   "URL query v=1");
        test_check(u->fragment != NULL &&
                   axl_strcmp(u->fragment, "section") == 0,
                   "URL fragment 'section'");
        axl_url_free(u);
    }
}

static void
test_url_parse_userinfo_user_only(void)
{
    /* http://user@host — no password (no `:` in userinfo). */
    AxlUrl *u;
    int ret = axl_url_parse("http://anon@example.com/", &u);
    test_check(ret == 0, "URL parse: user-only userinfo");
    if (ret == 0) {
        test_check(u->user != NULL && axl_strcmp(u->user, "anon") == 0,
                   "URL user 'anon'");
        test_check(u->password == NULL, "URL no password");
        test_check(axl_strcmp(u->host, "example.com") == 0,
                   "URL host after user@");
        axl_url_free(u);
    }
}

static void
test_url_parse_userinfo_empty_password(void)
{
    /* http://user:@host — explicit empty password (the `:` is
       present but the password slot is empty). */
    AxlUrl *u;
    int ret = axl_url_parse("http://bob:@example.com/", &u);
    test_check(ret == 0, "URL parse: empty password");
    if (ret == 0) {
        test_check(u->user != NULL && axl_strcmp(u->user, "bob") == 0,
                   "URL user 'bob'");
        test_check(u->password != NULL && u->password[0] == '\0',
                   "URL password present but empty");
        axl_url_free(u);
    }
}

static void
test_url_parse_no_userinfo(void)
{
    /* No '@' in authority — user/password must be NULL. */
    AxlUrl *u;
    int ret = axl_url_parse("https://example.com/path", &u);
    test_check(ret == 0, "URL parse: no userinfo");
    if (ret == 0) {
        test_check(u->user == NULL, "URL no user");
        test_check(u->password == NULL, "URL no password");
        axl_url_free(u);
    }
}

static void
test_url_parse_fragment_only(void)
{
    /* http://host/path#frag — fragment without a query. */
    AxlUrl *u;
    int ret = axl_url_parse("http://h/p#anchor", &u);
    test_check(ret == 0, "URL parse: fragment without query");
    if (ret == 0) {
        test_check(u->query == NULL, "URL no query");
        test_check(u->fragment != NULL &&
                   axl_strcmp(u->fragment, "anchor") == 0,
                   "URL fragment 'anchor'");
        axl_url_free(u);
    }
}

static void
test_url_parse_no_fragment(void)
{
    /* No '#' in URL — fragment must be NULL. */
    AxlUrl *u;
    int ret = axl_url_parse("http://h/p?q=1", &u);
    test_check(ret == 0, "URL parse: no fragment");
    if (ret == 0) {
        test_check(u->fragment == NULL, "URL no fragment");
        axl_url_free(u);
    }
}

static void
test_url_parse_at_in_path_not_userinfo(void)
{
    /* '@' in the path/query is NOT userinfo — only an '@' BEFORE
       the next '/' counts. Common case: /search?q=foo@bar. */
    AxlUrl *u;
    int ret = axl_url_parse("http://h/p?q=foo@bar", &u);
    test_check(ret == 0, "URL parse: @ in query not treated as userinfo");
    if (ret == 0) {
        test_check(u->user == NULL, "URL no spurious user");
        test_check(u->password == NULL, "URL no spurious password");
        test_check(axl_strcmp(u->host, "h") == 0, "URL host correct");
        test_check(u->query != NULL && axl_strcmp(u->query, "q=foo@bar") == 0,
                   "URL query preserves @");
        axl_url_free(u);
    }
}

static void
test_url_parse_userinfo_colon_in_password(void)
{
    /* `:` inside the password (after the first `:`) is part of the
       password. user:p:a:s:s@host → user="user", password="p:a:s:s". */
    AxlUrl *u;
    int ret = axl_url_parse("http://user:p:a:s:s@host/", &u);
    test_check(ret == 0, "URL parse: colon-in-password");
    if (ret == 0) {
        test_check(u->user != NULL && axl_strcmp(u->user, "user") == 0,
                   "URL user 'user'");
        test_check(u->password != NULL &&
                   axl_strcmp(u->password, "p:a:s:s") == 0,
                   "URL password 'p:a:s:s'");
        axl_url_free(u);
    }
}

static void
test_url_parse_fragment_after_host(void)
{
    /* http://host#frag — fragment directly after host (no port,
       no path, no query). Exercises the new `#` terminator on the
       host-end scan; default path becomes "/". */
    AxlUrl *u;
    int ret = axl_url_parse("http://host#frag", &u);
    test_check(ret == 0, "URL parse: fragment directly after host");
    if (ret == 0) {
        test_check(axl_strcmp(u->host, "host") == 0, "URL host stops at #");
        test_check(axl_strcmp(u->path, "/") == 0, "URL default path /");
        test_check(u->query == NULL, "URL no query");
        test_check(u->fragment != NULL &&
                   axl_strcmp(u->fragment, "frag") == 0,
                   "URL fragment 'frag'");
        axl_url_free(u);
    }
}

static void
test_url_parse_port_nondigit(void)
{
    /* Non-digit bytes after ':' must be rejected, not silently
       eaten with port=0 leaving the path/query parser staring at
       garbage. */
    AxlUrl *u = NULL;
    int ret = axl_url_parse("http://host:abc/path", &u);
    test_check(ret != 0, "URL reject: port with non-digit bytes");
    if (ret == 0) {
        axl_url_free(u);
    }
}

static void
test_url_parse_port_overflow(void)
{
    /* Port values > 65535 don't fit uint16_t — reject rather than
       silently truncate. The old impl cast each intermediate
       result back to uint16_t, producing wrong port numbers. */
    AxlUrl *u = NULL;
    int ret = axl_url_parse("http://host:99999/", &u);
    test_check(ret != 0, "URL reject: port > 65535");
    if (ret == 0) {
        axl_url_free(u);
    }
}

static void
test_url_parse_port_trailing_garbage(void)
{
    /* Digits followed by non-terminator (e.g. "80abc") is
       malformed — the digit run isn't a complete port. */
    AxlUrl *u = NULL;
    int ret = axl_url_parse("http://host:80abc/", &u);
    test_check(ret != 0, "URL reject: port with trailing non-digit garbage");
    if (ret == 0) {
        axl_url_free(u);
    }
}

static void
test_url_parse_port_empty(void)
{
    /* `host:` (RFC 3986 §3.2.3 allows an empty port — client uses
       scheme default) — accept with port = default. */
    AxlUrl *u = NULL;
    int ret = axl_url_parse("http://host:/path", &u);
    test_check(ret == 0, "URL parse: empty port falls back to scheme default");
    if (ret == 0) {
        test_check(u->port == 80, "URL empty-port uses http default 80");
        test_check(axl_strcmp(u->path, "/path") == 0,
                   "URL empty-port: path still parsed");
        axl_url_free(u);
    }
}

static void
test_url_parse_port_max(void)
{
    /* 65535 is the maximum valid port — must succeed. */
    AxlUrl *u = NULL;
    int ret = axl_url_parse("http://host:65535/", &u);
    test_check(ret == 0, "URL parse: port 65535 (max)");
    if (ret == 0) {
        test_check(u->port == 65535, "URL port equals 65535");
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
        test_check(ret == AXL_OK, "GetIpAddress succeeds");
        if (ret == AXL_OK) {
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
    /* Slirp does not route the guest's own IP back to it, so a true
       intra-guest listen+accept cannot happen here. The unit-test
       runner installs a slirp `guestfwd` rule that pipes connections
       to a host-side stream-echo helper — every byte sent comes back.
       That gives us a working remote peer for the client-side path
       (connect → send → recv → close). The listen+accept path is
       covered by test-tcp-echo.sh as a separate integration test. */
    int    ret;
    AxlTcp *client;
    char   send_buf[] = "Hello AxlNet";
    char   recv_buf[64];
    size_t recv_size;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: TCP echo (no network)\n");
        return;
    }

    ret = axl_tcp_connect(AXL_TEST_ECHO_HOST, 9999, &client);
    test_check(ret == 0, "TCP connect");
    if (ret != 0) {
        return;
    }

    ret = axl_tcp_send(client, send_buf, axl_strlen(send_buf), 0);
    test_check(ret == 0, "TCP send");

    /* Pull the echo back. Echo is byte-for-byte; one recv typically
       returns the full buffer. */
    recv_size = sizeof(recv_buf) - 1;
    ret = axl_tcp_recv(client, recv_buf, &recv_size, 0);
    test_check(ret == 0, "TCP recv");
    if (ret == 0) {
        recv_buf[recv_size] = '\0';
        test_check(axl_strcmp(recv_buf, "Hello AxlNet") == 0,
                   "TCP echo match");
    }

    axl_tcp_close(client);
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
on_tcp_rearm_data(AxlTcp *sock, AxlStatus status, void *data)
{
    TcpRearmCtx *c = (TcpRearmCtx *)data;
    size_t len = axl_tcp_recv_get_size(sock);

    c->fires++;
    if (status != AXL_OK || len == 0) {
        axl_loop_quit(c->loop);
        return false;
    }

    c->total_bytes += len;
    /* Stop once we've collected the full echoed payload (11 bytes:
       "one" + "two" + "three"). The callback may fire 1–3 times
       depending on how the host stack chunks the echo back to us. */
    if (c->total_bytes >= 11) {
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
    /* Client-only against the runner-provided echo backend.
       Original test exercised server-side async recv re-arm by
       sending three chunks from a client to an accepted socket
       and expecting three callback fires. With the echo backend
       the chunks come back via slirp+host-echo, and TCP doesn't
       preserve message boundaries on a single connection — host
       buffering may coalesce or split echoes. We've narrowed the
       check to "the async-recv path fires at least once with the
       full echoed payload," which still verifies the re-arm /
       wakeup wiring without depending on packet-boundary luck. */
    int ret;
    AxlTcp *client = NULL;
    TcpRearmCtx ctx = { 0 };

    if (!axl_net_is_available()) {
        axl_printf("SKIP: TCP recv_async rearm (no network)\n");
        return;
    }

    if (axl_tcp_connect(AXL_TEST_ECHO_HOST, 9998, &client) != AXL_OK) {
        axl_printf("SKIP: TCP recv_async rearm (connect failed)\n");
        return;
    }

    /* Arm async recv on the client socket. Echo backend sends every
       byte we transmit straight back. */
    ctx.loop = axl_loop_new();
    ret = axl_tcp_recv_async(client, ctx.buf, sizeof(ctx.buf),
                             ctx.loop, NULL, on_tcp_rearm_data, &ctx);
    test_check(ret == 0, "TCP recv_async: initial arm");

    /* Send 3 chunks with gaps. The exact number of receive callbacks
       depends on coalescing across the slirp+host-echo round-trip,
       so we only assert at least one fire and the full byte count. */
    axl_loop_add_timeout(ctx.loop, 3000, on_tcp_rearm_timeout, &ctx);

    axl_tcp_send(client, "one", 3, 0);
    axl_usleep(50000);
    axl_tcp_send(client, "two", 3, 0);
    axl_usleep(50000);
    axl_tcp_send(client, "three", 5, 0);

    axl_loop_run(ctx.loop);

    test_check(ctx.fires >= 1, "TCP recv_async: callback fired");
    test_check(ctx.total_bytes == 11, "TCP recv_async: total bytes = 11");

    /* Close the socket BEFORE freeing the loop. axl_tcp_recv_async
       stamps sock->async_loop with this loop, and axl_tcp_close calls
       axl_loop_remove_source on it during teardown. Freeing the loop
       first would dangle the pointer. */
    axl_tcp_close(client);
    axl_loop_free(ctx.loop);
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

    ret = axl_http_server_start(server, loop);
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
// AxlHttpResponse — set_static contract
// ---------------------------------------------------------------------------

static const char STATIC_ASSET_BODY[] =
    "console.log('embedded asset');\n";

static void
test_http_response_set_static(void)
{
    AxlHttpResponse r;
    axl_memset(&r, 0, sizeof(r));

    axl_http_response_set_static(&r,
                                 STATIC_ASSET_BODY,
                                 sizeof(STATIC_ASSET_BODY) - 1,
                                 "application/javascript");

    test_check(r.body == (void *)STATIC_ASSET_BODY,
               "set_static: body points at the caller's buffer (no copy)");
    test_check(r.body_size == sizeof(STATIC_ASSET_BODY) - 1,
               "set_static: body_size matches the size argument");
    test_check(r.body_static,
               "set_static: body_static flag is set so the dispatch "
               "loop will not call axl_free on the static buffer");
    test_check(r.content_type != NULL
               && axl_strcmp(r.content_type, "application/javascript") == 0,
               "set_static: content_type captured");
    test_check(r.status_code == 200,
               "set_static: defaults status to 200 when unset");

    /* NULL content_type leaves the existing one in place. */
    AxlHttpResponse r2;
    axl_memset(&r2, 0, sizeof(r2));
    r2.content_type = "text/plain";
    axl_http_response_set_static(&r2, "abc", 3, NULL);
    test_check(r2.content_type != NULL
               && axl_strcmp(r2.content_type, "text/plain") == 0,
               "set_static: NULL content_type preserves prior value");

    /* Switching from a copy-based helper to set_static frees the
       previous body without leaking. The leak detector / fence
       checker would catch a regression here. */
    AxlHttpResponse r3;
    axl_memset(&r3, 0, sizeof(r3));
    axl_http_response_set_text(&r3, "previous body");
    test_check(r3.body != NULL && !r3.body_static,
               "set_static: pre-state — set_text installed an owned body");
    axl_http_response_set_static(&r3, STATIC_ASSET_BODY,
                                 sizeof(STATIC_ASSET_BODY) - 1, NULL);
    test_check(r3.body == (void *)STATIC_ASSET_BODY && r3.body_static,
               "set_static: replaces a previously-owned body cleanly");
}

// ---------------------------------------------------------------------------
// AxlHttpResponse — set_range / set_content_range emit RFC 9110 §15.3.7
// Content-Range header
// ---------------------------------------------------------------------------

static const uint8_t RANGE_TEST_BUF[1024] = { 0 };

static void
test_http_response_set_range(void)
{
    AxlHttpResponse r;
    axl_memset(&r, 0, sizeof(r));

    /* offset=100, length=24, total=1024 → bytes 100-123/1024 */
    axl_http_response_set_range(&r, RANGE_TEST_BUF, 100, 24,
                                sizeof(RANGE_TEST_BUF));

    test_check(r.status_code == 206,
               "set_range: status_code is 206");
    test_check(r.body != NULL && r.body_size == 24,
               "set_range: body slice copied (24 bytes)");
    test_check(r.headers != NULL,
               "set_range: headers table allocated");
    if (r.headers != NULL) {
        const char *cr = (const char *)axl_hash_table_lookup(
            r.headers, "Content-Range");
        test_check(cr != NULL,
                   "set_range: Content-Range header present");
        if (cr != NULL) {
            /* Exact wire format per RFC 9110: end is inclusive. */
            test_check(axl_strcmp(cr, "bytes 100-123/1024") == 0,
                       "set_range: Content-Range value matches "
                       "RFC 9110 (bytes 100-123/1024)");
        }
        axl_hash_table_free(r.headers);
    }
    if (r.body != NULL && !r.body_static) {
        axl_free(r.body);
    }
}

static void
test_http_response_set_content_range(void)
{
    AxlHttpResponse r;
    axl_memset(&r, 0, sizeof(r));

    /* Streaming-response path: caller manages status separately, just
       needs the header formatted + inserted. */
    axl_http_response_set_content_range(&r, 1024, 2047, 1048576);

    test_check(r.status_code == 0,
               "set_content_range: does NOT touch status_code "
               "(caller's responsibility)");
    test_check(r.headers != NULL,
               "set_content_range: headers table allocated");
    if (r.headers != NULL) {
        const char *cr = (const char *)axl_hash_table_lookup(
            r.headers, "Content-Range");
        test_check(cr != NULL,
                   "set_content_range: Content-Range header present");
        if (cr != NULL) {
            test_check(axl_strcmp(cr, "bytes 1024-2047/1048576") == 0,
                       "set_content_range: value matches RFC 9110 "
                       "format (bytes 1024-2047/1048576)");
        }
        axl_hash_table_free(r.headers);
    }

    /* Pre-allocated headers table with the CORRECT destroy-func
       contract (full ownership of both keys and values) — the
       helper must compose with it: existing entries preserved, the
       new Content-Range entry inserted. */
    AxlHttpResponse r2;
    axl_memset(&r2, 0, sizeof(r2));
    r2.headers = axl_hash_table_new_full(
        axl_str_hash, axl_str_equal,
        axl_free_impl, axl_free_impl);
    axl_hash_table_insert(r2.headers,
                          axl_strdup("X-Existing"), axl_strdup("value"));
    axl_http_response_set_content_range(&r2, 0, 99, 100);
    test_check(axl_hash_table_lookup(r2.headers, "X-Existing") != NULL,
               "set_content_range: preserves existing headers "
               "(full-ownership table)");
    test_check(axl_hash_table_lookup(r2.headers, "Content-Range") != NULL,
               "set_content_range: inserts into existing headers "
               "table when destroy-func contract is satisfied");
    axl_hash_table_free(r2.headers);

    /* Pre-allocated headers table with the WRONG contract (str-table
       has copy_keys=true + no value_destroy). The helper must REFUSE
       to insert rather than silently leak the strdup'd key+value.
       The existing entry stays in place; Content-Range is NOT
       inserted; consumer notices via the missing header. */
    AxlHttpResponse r3;
    axl_memset(&r3, 0, sizeof(r3));
    r3.headers = axl_hash_table_new_str();
    axl_hash_table_insert(r3.headers, "X-Existing", (void *)"value");
    axl_http_response_set_content_range(&r3, 0, 99, 100);
    test_check(axl_hash_table_lookup(r3.headers, "X-Existing") != NULL,
               "set_content_range: existing entries unaffected on "
               "contract mismatch");
    test_check(axl_hash_table_lookup(r3.headers, "Content-Range") == NULL,
               "set_content_range: refuses insert on wrong "
               "destroy-func contract (would leak strdups)");
    axl_hash_table_free(r3.headers);
}

// ---------------------------------------------------------------------------
// AxlHttpResponse — set_streamer contract
// ---------------------------------------------------------------------------

static int
streamer_test_dummy(void *ctx, void *out, size_t cap, size_t *out_len)
{
    (void)ctx; (void)out; (void)cap;
    *out_len = 0;
    return 0;
}

static int
streamer_test_dummy2(void *ctx, void *out, size_t cap, size_t *out_len)
{
    (void)ctx; (void)out; (void)cap;
    *out_len = 0;
    return 0;
}

static int  m_test_cleanup_calls = 0;
static void streamer_test_cleanup(void *ctx) { (void)ctx; m_test_cleanup_calls++; }

static void
test_http_response_set_streamer(void)
{
    /* set_streamer populates streamer + ctx + cleanup + total_size,
       defaults status to 200, captures content_type. */
    AxlHttpResponse r;
    axl_memset(&r, 0, sizeof(r));

    int marker = 0;
    axl_http_response_set_streamer(&r, streamer_test_dummy, &marker, NULL,
                                   1024, "application/octet-stream");

    test_check(r.streamer == streamer_test_dummy,
               "set_streamer: streamer fn captured");
    test_check(r.streamer_ctx == &marker,
               "set_streamer: ctx pointer captured");
    test_check(r.streamer_cleanup == NULL,
               "set_streamer: NULL cleanup retained");
    test_check(r.streamer_total_size == 1024,
               "set_streamer: total_size captured");
    test_check(r.content_type != NULL
               && axl_strcmp(r.content_type, "application/octet-stream") == 0,
               "set_streamer: content_type captured");
    test_check(r.status_code == 200,
               "set_streamer: defaults status to 200 when unset");
    test_check(r.body == NULL && r.body_size == 0 && !r.body_static,
               "set_streamer: clears contiguous-body fields");

    /* (size_t)-1 signals the chunked-encoding path. The setter just
       stores it; the dispatcher uses the sentinel to pick framing. */
    AxlHttpResponse r2;
    axl_memset(&r2, 0, sizeof(r2));
    axl_http_response_set_streamer(&r2, streamer_test_dummy, NULL, NULL,
                                   (size_t)-1, "text/plain");
    test_check(r2.streamer_total_size == (size_t)-1,
               "set_streamer: (size_t)-1 sentinel captured "
               "(chunked-encoding signal)");

    /* Switching from a copy-based body to a streamer frees the prior
       body cleanly (leak detector / fence check would catch a
       regression). */
    AxlHttpResponse r3;
    axl_memset(&r3, 0, sizeof(r3));
    axl_http_response_set_text(&r3, "stale body");
    test_check(r3.body != NULL && !r3.body_static,
               "set_streamer: pre-state — copy body installed");
    axl_http_response_set_streamer(&r3, streamer_test_dummy2, NULL, NULL,
                                   42, NULL);
    test_check(r3.body == NULL && r3.body_size == 0,
               "set_streamer: replaces a prior owned body without leaking");
    test_check(r3.streamer == streamer_test_dummy2,
               "set_streamer: new streamer installed");

    /* NULL streamer / NULL response are no-ops. (The doc contract is
       "if streamer is NULL the call returns without touching the
       response struct" so prior fields stay intact.) */
    AxlHttpResponse r4;
    axl_memset(&r4, 0, sizeof(r4));
    r4.status_code = 503;
    axl_http_response_set_streamer(&r4, NULL, NULL, NULL, 0, NULL);
    test_check(r4.streamer == NULL && r4.status_code == 503,
               "set_streamer: NULL streamer is a no-op");

    /* Cleanup-fn pointer is captured (the dispatcher invokes it on
       end-of-stream / error / reset; here we just verify the field
       is stored — end-to-end invocation is exercised by the
       integration test). */
    AxlHttpResponse r5;
    axl_memset(&r5, 0, sizeof(r5));
    m_test_cleanup_calls = 0;
    axl_http_response_set_streamer(&r5, streamer_test_dummy, &marker,
                                   streamer_test_cleanup, 100, NULL);
    test_check(r5.streamer_cleanup == streamer_test_cleanup,
               "set_streamer: cleanup fn captured");
    test_check(m_test_cleanup_calls == 0,
               "set_streamer: cleanup NOT called from setter "
               "(dispatcher fires it at end-of-stream / reset)");
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

/* Static-asset route — exercises axl_http_response_set_static end-to-end.
   Returns the same .rodata buffer twice in a row; the integration test
   curls /static-asset twice and verifies both bodies match exactly. If
   the dispatch loop ever stops honoring body_static and frees the
   .rodata pointer, the second request hangs / returns truncated data /
   blows up the heap (the original axl-webfs bug shape). */
static int
on_get_static_asset(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_static(resp,
                                 STATIC_ASSET_BODY,
                                 sizeof(STATIC_ASSET_BODY) - 1,
                                 "application/javascript");
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
// /streaming-put-test — exercises axl_http_request_streaming (raw
// producer-callback path) and axl_http_request_stream_file (the
// AxlStream-backed convenience wrapper). Query params:
//   url=<host upload url>  required, target for the streaming PUT
//   size=<N>              required, bytes to produce
//   mode=cb|chunked|err|file
//
// Producer pattern: byte at offset i = (i & 0xFF). Host's
// /upload endpoint records {len, head_hex, tail_hex}; the
// integration test then GETs /last-upload to verify byte-perfect
// reception.
// ---------------------------------------------------------------------------

typedef struct {
    size_t  size;       ///< total bytes the producer will emit
    size_t  sent;       ///< bytes emitted so far
    bool    err_on_2nd; ///< true → return AXL_ERR after first non-empty pull
    bool    overshoot;  ///< true → emit size + extra bytes (overrun guard test)
    int     pull_count; ///< number of times the producer has been pulled
} StreamingPutCtx;

static int
streaming_put_producer(void *vctx, void *out_buf, size_t cap, size_t *got)
{
    StreamingPutCtx *s = vctx;
    s->pull_count++;
    if (s->err_on_2nd && s->pull_count > 1) {
        return AXL_ERR;
    }
    size_t want      = s->overshoot ? (s->size + 64) : s->size;
    size_t remaining = (s->sent < want) ? (want - s->sent) : 0;
    size_t take      = (remaining < cap) ? remaining : cap;
    uint8_t *out = out_buf;
    for (size_t i = 0; i < take; i++) {
        out[i] = (uint8_t)((s->sent + i) & 0xFF);
    }
    s->sent += take;
    *got = take;
    return AXL_OK;
}

static int
on_streaming_put_test(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)data;
    if (req->query == NULL) {
        axl_http_response_set_status(resp, 400);
        return 0;
    }

    /* Parse query: url=...&size=N&mode=... */
    char url_buf[256] = { 0 };
    size_t size_param = 0;
    char mode[16] = "cb";
    const char *p = req->query;
    while (*p != '\0') {
        const char *eq = NULL;
        const char *amp = p;
        while (*amp != '\0' && *amp != '&') {
            if (*amp == '=' && eq == NULL) eq = amp;
            amp++;
        }
        if (eq != NULL) {
            size_t klen = (size_t)(eq - p);
            const char *v = eq + 1;
            size_t vlen = (size_t)(amp - v);
            if (klen == 3 && axl_strncmp(p, "url", 3) == 0) {
                if (vlen >= sizeof(url_buf)) vlen = sizeof(url_buf) - 1;
                axl_memcpy(url_buf, v, vlen);
                url_buf[vlen] = '\0';
            } else if (klen == 4 && axl_strncmp(p, "size", 4) == 0) {
                for (size_t i = 0; i < vlen; i++) {
                    if (v[i] < '0' || v[i] > '9') break;
                    size_param = size_param * 10 + (size_t)(v[i] - '0');
                }
            } else if (klen == 4 && axl_strncmp(p, "mode", 4) == 0) {
                if (vlen >= sizeof(mode)) vlen = sizeof(mode) - 1;
                axl_memcpy(mode, v, vlen);
                mode[vlen] = '\0';
            }
        }
        p = (*amp == '&') ? amp + 1 : amp;
    }
    if (url_buf[0] == '\0') {
        axl_http_response_set_status(resp, 400);
        return 0;
    }

    AxlHttpClient *c = axl_http_client_new();
    if (c == NULL) {
        axl_http_response_set_status(resp, 500);
        return 0;
    }
    AxlHttpClientResponse *client_resp = NULL;
    int rc;

    if (axl_strcmp(mode, "file") == 0) {
        /* Materialize a temp file with the same byte pattern the
           callback would emit, then stream it via the file wrapper. */
        uint8_t *buf = axl_malloc(size_param);
        if (buf == NULL) {
            axl_http_client_free(c);
            axl_http_response_set_status(resp, 500);
            return 0;
        }
        for (size_t i = 0; i < size_param; i++) {
            buf[i] = (uint8_t)(i & 0xFF);
        }
        const char *path = "fs0:\\axl_stream_put.tmp";
        (void)axl_file_set_contents(path, buf, size_param);
        axl_free(buf);
        rc = axl_http_request_stream_file(c, "PUT", url_buf, path,
                                          "application/octet-stream",
                                          NULL, &client_resp);
        axl_file_delete(path);
    } else {
        StreamingPutCtx sctx = {
            .size       = size_param,
            .sent       = 0,
            .err_on_2nd = (axl_strcmp(mode, "err") == 0),
            .overshoot  = (axl_strcmp(mode, "overshoot") == 0),
            .pull_count = 0,
        };
        size_t total = (axl_strcmp(mode, "chunked") == 0)
                       ? (size_t)-1 : size_param;
        rc = axl_http_request_streaming(c, "PUT", url_buf,
                                        streaming_put_producer, &sctx,
                                        NULL, total,
                                        "application/octet-stream",
                                        NULL, &client_resp);
    }

    unsigned status = 0;
    if (client_resp != NULL) {
        status = (unsigned)client_resp->status_code;
        axl_http_client_response_free(client_resp);
    }
    axl_http_client_free(c);

    char body[128];
    axl_snprintf(body, sizeof(body),
                 "{\"rc\":%d,\"server_status\":%u,\"mode\":\"%s\",\"size\":%llu}",
                 rc, status, mode, (unsigned long long)size_param);
    axl_http_response_set_json(resp, body);
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
    if (axl_strcmp(auth, "Bearer admin-token") == 0) {
        auth_out->username = "admin";
        auth_out->role = AXL_ROUTE_ADMIN;
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
/* Cross-request counters that survive the per-upload reset. The
   integration test reads these via /upload-status to verify:
     - middleware-blocked uploads never reach the handler at all
       (chunks_lifetime stays flat across the rejected POST)
     - aborted uploads do reach the handler with aborted=true
       (aborts increments) */
static size_t upload_chunks_lifetime = 0;
static size_t upload_aborts          = 0;

static int
on_upload(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    const void      *chunk,
    size_t           chunk_size,
    void            *data,
    bool             aborted)
{
    (void)req;
    (void)resp;
    (void)data;

    if (aborted) {
        /* Connection torn down mid-upload. resp is NOT transmitted —
           just release per-request state. */
        upload_aborts++;
        upload_chunk_count = 0;
        upload_total_bytes = 0;
        return 0;
    }

    if (chunk == NULL && chunk_size == 0) {
        /* Clean-EOF final call — set response */
        char buf[128];
        axl_snprintf(buf, sizeof(buf),
            "{\"chunks\":%llu,\"total\":%llu}",
            (unsigned long long)upload_chunk_count,
            (unsigned long long)upload_total_bytes);
        axl_http_response_set_json(resp, buf);

        /* Reset per-upload counters; keep the lifetime totals. */
        upload_chunk_count = 0;
        upload_total_bytes = 0;
        return 0;
    }

    upload_chunk_count++;
    upload_total_bytes += chunk_size;
    upload_chunks_lifetime++;
    return 0;
}

/* GET /upload-status — exposes the cross-request counters so the
   integration test can verify "middleware blocked the upload"
   (chunks_lifetime stayed flat) and "abort fired" (aborts went up). */
static int
on_upload_status(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data)
{
    (void)req;
    (void)data;
    char buf[128];
    axl_snprintf(buf, sizeof(buf),
        "{\"chunks_lifetime\":%llu,\"aborts\":%llu}",
        (unsigned long long)upload_chunks_lifetime,
        (unsigned long long)upload_aborts);
    axl_http_response_set_json(resp, buf);
    return 0;
}

/* Middleware that rejects requests bearing the X-Test-Reject: 1
   header. Used by the integration test to verify middleware runs
   ahead of upload routes. Routes without that header pass through
   untouched. */
static int
test_reject_middleware(
    AxlHttpRequest  *req,
    AxlHttpResponse *resp,
    void            *data)
{
    (void)data;
    if (req->headers != NULL) {
        const char *flag = (const char *)axl_hash_table_lookup(
            req->headers, "x-test-reject");
        if (flag != NULL && axl_strcmp(flag, "1") == 0) {
            axl_http_response_set_text(resp, "rejected by middleware");
            resp->status_code = 403;
            return AXL_ERR;
        }
    }
    return AXL_OK;
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

// ---------------------------------------------------------------------------
// WebDAV test backend — tiny in-memory FS keyed by full path. Tracks
// existence + is_dir; does NOT track contents (W1 verbs don't need
// them). Used by the OPTIONS / MKCOL / DELETE / MOVE integration
// tests in test-http.sh.
// ---------------------------------------------------------------------------

static AxlHashTable *m_dav_fs = NULL;  /* path -> (void *)(uintptr_t)is_dir+1 */

static int
test_webdav_init(void)
{
    if (m_dav_fs != NULL) {
        return AXL_OK;
    }
    m_dav_fs = axl_hash_table_new_full(
        axl_str_hash, axl_str_equal,
        axl_free_impl, NULL);
    if (m_dav_fs == NULL) {
        return AXL_ERR;
    }
    /* Seed: root + a few pre-existing entries the integration test
       references. preset-file is consumed by the DELETE test;
       preset-stat is left alone so PROPFIND has a known entry to
       describe regardless of test order. */
    axl_hash_table_replace(m_dav_fs, axl_strdup("/"),
                           (void *)(uintptr_t)2);
    axl_hash_table_replace(m_dav_fs, axl_strdup("/preset-dir"),
                           (void *)(uintptr_t)2);
    axl_hash_table_replace(m_dav_fs, axl_strdup("/preset-file"),
                           (void *)(uintptr_t)1);
    axl_hash_table_replace(m_dav_fs, axl_strdup("/preset-stat"),
                           (void *)(uintptr_t)1);
    /* /preset-collection survives all the W1 verb tests so the W2
       PROPFIND-with-trailing-slash assertion has a directory to
       describe. preset-dir is consumed by the MOVE test. */
    axl_hash_table_replace(m_dav_fs, axl_strdup("/preset-collection"),
                           (void *)(uintptr_t)2);
    /* W5 COPY presets — copy-source-file is the basic COPY src,
       copy-existing-target stays put to exercise the overwrite=F
       refusal path. */
    axl_hash_table_replace(m_dav_fs, axl_strdup("/copy-source-file"),
                           (void *)(uintptr_t)1);
    axl_hash_table_replace(m_dav_fs, axl_strdup("/copy-existing-target"),
                           (void *)(uintptr_t)1);
    return AXL_OK;
}

static int
test_dav_mkdir(void *user, const char *path)
{
    (void)user;
    if (axl_hash_table_lookup(m_dav_fs, path) != NULL) {
        return AXL_ERR;  /* already exists */
    }
    char *key = axl_strdup(path);
    if (key == NULL) {
        return AXL_ERR;
    }
    axl_hash_table_replace(m_dav_fs, key, (void *)(uintptr_t)2);
    return AXL_OK;
}

static int
test_dav_remove(void *user, const char *path)
{
    (void)user;
    if (axl_hash_table_lookup(m_dav_fs, path) == NULL) {
        return AXL_ERR;
    }
    axl_hash_table_remove(m_dav_fs, path);
    return AXL_OK;
}

static int
test_dav_move(void *user, const char *src, const char *dst, bool overwrite)
{
    (void)user;
    void *v = axl_hash_table_lookup(m_dav_fs, src);
    if (v == NULL) {
        return AXL_ERR;
    }
    if (axl_hash_table_lookup(m_dav_fs, dst) != NULL && !overwrite) {
        return AXL_ERR;
    }
    char *new_key = axl_strdup(dst);
    if (new_key == NULL) {
        return AXL_ERR;
    }
    axl_hash_table_remove(m_dav_fs, src);
    axl_hash_table_replace(m_dav_fs, new_key, v);
    return AXL_OK;
}

/* COPY mirrors MOVE but keeps the source in place. Depth is
   forwarded by the SDK (0 = collection-only, infinity = deep);
   the hash-table fs is flat so we don't model child copying. */
static int
test_dav_copy(void *user, const char *src, const char *dst,
              bool overwrite, int depth)
{
    (void)user;
    (void)depth;
    void *v = axl_hash_table_lookup(m_dav_fs, src);
    if (v == NULL) {
        return AXL_ERR;
    }
    if (axl_hash_table_lookup(m_dav_fs, dst) != NULL && !overwrite) {
        return AXL_ERR;
    }
    char *new_key = axl_strdup(dst);
    if (new_key == NULL) {
        return AXL_ERR;
    }
    axl_hash_table_replace(m_dav_fs, new_key, v);
    return AXL_OK;
}

static int
test_dav_stat(void *user, const char *path, AxlFsEntry *out)
{
    (void)user;
    void *v = axl_hash_table_lookup(m_dav_fs, path);
    if (v == NULL) {
        return AXL_ERR;
    }
    bool is_dir = ((uintptr_t)v == 2);
    /* basename: last segment after the last '/'. */
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' && *(p + 1) != '\0') {
            base = p + 1;
        }
    }
    axl_strlcpy(out->name, base, sizeof(out->name));
    out->attributes = (is_dir) ? AXL_FS_ATTR_DIRECTORY : 0u;
    out->size       = is_dir ? 0 : 42;       /* fake file size */
    out->mtime_unix = 1762456800;            /* 2025-11-06T19:20:00Z */
    return AXL_OK;
}

/* Foreach helper: collect children of `parent` into the AxlFsEntry
   array. A child is any key that starts with `parent` + (optional /),
   contains exactly one more path segment, and isn't the parent
   itself. */
typedef struct {
    const char       *parent;
    size_t            parent_len;
    AxlFsEntry   *out;
    size_t            max;
    size_t            count;
} DavListCtx;

static void
test_dav_list_collect(const void *key, void *value, void *data)
{
    DavListCtx *ctx  = data;
    const char *path = key;
    bool        dir  = ((uintptr_t)value == 2);

    if (ctx->count >= ctx->max) return;
    /* Skip the parent itself. */
    if (axl_strcmp(path, ctx->parent) == 0) return;

    /* Must start with parent. For root parent="/" we want any path
       beginning with "/" but ending with at most ONE further segment
       past the parent. */
    bool root = (axl_strcmp(ctx->parent, "/") == 0);
    if (!root) {
        if (axl_strncmp(path, ctx->parent, ctx->parent_len) != 0) return;
        if (path[ctx->parent_len] != '/') return;
    } else {
        if (path[0] != '/') return;
    }
    const char *child = root ? path + 1 : path + ctx->parent_len + 1;
    /* Reject grandchildren (child path contains a /). */
    for (const char *p = child; *p != '\0'; p++) {
        if (*p == '/') return;
    }
    if (*child == '\0') return;

    AxlFsEntry *e = &ctx->out[ctx->count++];
    axl_strlcpy(e->name, child, sizeof(e->name));
    e->attributes = (dir) ? AXL_FS_ATTR_DIRECTORY : 0u;
    e->size       = dir ? 0 : 42;
    e->mtime_unix = 1762456800;
}

static int
test_dav_list_dir(void *user, const char *path,
                  AxlFsEntry *out, size_t max, size_t *count)
{
    (void)user;
    DavListCtx ctx = {
        .parent     = path,
        .parent_len = axl_strlen(path),
        .out        = out,
        .max        = max,
        .count      = 0,
    };
    axl_hash_table_foreach(m_dav_fs, test_dav_list_collect, &ctx);
    *count = ctx.count;
    return AXL_OK;
}

/* GET / PUT — backed by a fixed body string (read) and a tiny
   sink that records the last upload (write). Real consumers map
   onto their own filesystem. */
static const char DAV_FILE_BODY[] =
    "this is the contents of preset-stat exposed via WebDAV GET\n";

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
} DavReadCtx;

static int
test_dav_read_open(void *user, const char *path, uint64_t offset,
                   void **out_ctx)
{
    (void)user;
    /* Only preset-stat has a body for GET in the test backend.
       Everything else gets a stat-OK + read-fail to exercise the
       SDK's 500/error path. */
    if (axl_strcmp(path, "/preset-stat") != 0) {
        return AXL_ERR;
    }
    DavReadCtx *r = axl_calloc(1, sizeof(*r));
    if (r == NULL) {
        return AXL_ERR;
    }
    r->src = DAV_FILE_BODY;
    r->len = sizeof(DAV_FILE_BODY) - 1;
    if (offset > r->len) {
        axl_free(r);
        return AXL_ERR;
    }
    r->pos = (size_t)offset;
    *out_ctx = r;
    return AXL_OK;
}

static int
test_dav_read_chunk(void *vctx, void *buf, size_t buf_size,
                    size_t *bytes_read)
{
    DavReadCtx *r = vctx;
    size_t avail = (r->pos < r->len) ? (r->len - r->pos) : 0;
    size_t take  = (avail < buf_size) ? avail : buf_size;
    if (take > 0) {
        axl_memcpy(buf, r->src + r->pos, take);
        r->pos += take;
    }
    *bytes_read = take;
    return AXL_OK;
}

static void
test_dav_read_close(void *vctx)
{
    axl_free(vctx);
}

/* Write sink — records the last uploaded path + body so the
   integration test can verify PUT round-trips. Single in-flight
   matches the SDK's single-PUT-per-mount contract. */
static char  m_dav_last_put_path[256];
static char  m_dav_last_put_body[4096];
static size_t m_dav_last_put_len    = 0;
static bool   m_dav_last_put_aborted = false;

typedef struct {
    char  *target_path;
    size_t written;
} DavWriteCtx;

static int
test_dav_write_open(void *user, const char *path, void **out_ctx)
{
    (void)user;
    DavWriteCtx *w = axl_calloc(1, sizeof(*w));
    if (w == NULL) {
        return AXL_ERR;
    }
    w->target_path = axl_strdup(path);
    if (w->target_path == NULL) {
        axl_free(w);
        return AXL_ERR;
    }
    *out_ctx = w;
    return AXL_OK;
}

static int
test_dav_write_chunk(void *vctx, const void *data, size_t len)
{
    DavWriteCtx *w = vctx;
    size_t space = sizeof(m_dav_last_put_body) - w->written;
    size_t take  = (len < space) ? len : space;
    if (take > 0) {
        axl_memcpy(m_dav_last_put_body + w->written, data, take);
        w->written += take;
    }
    return AXL_OK;
}

static void
test_dav_write_close(void *vctx, bool aborted)
{
    DavWriteCtx *w = vctx;
    axl_strlcpy(m_dav_last_put_path, w->target_path,
                sizeof(m_dav_last_put_path));
    m_dav_last_put_len     = w->written;
    m_dav_last_put_aborted = aborted;
    /* On clean EOF, register the new file in the in-memory fs. */
    if (!aborted) {
        if (axl_hash_table_lookup(m_dav_fs, w->target_path) == NULL) {
            axl_hash_table_replace(m_dav_fs, axl_strdup(w->target_path),
                                   (void *)(uintptr_t)1);
        }
    }
    axl_free(w->target_path);
    axl_free(w);
}

static const char *
test_dav_content_type(void *user, const char *path)
{
    (void)user;
    /* Tiny extension sniff for the GET integration test. */
    const char *dot = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') dot = p;
    }
    if (dot != NULL) {
        if (axl_strcmp(dot, ".txt") == 0)  return "text/plain";
        if (axl_strcmp(dot, ".html") == 0) return "text/html";
    }
    return NULL;
}

/* Stub digest callback: returns a stable fake 64-char hex string
   for any FILE that exists in the in-memory fs (directories and
   missing paths get AXL_ERR). The hex itself isn't a real
   SHA-256 — fixed lowercase value the integration tests pin
   verbatim for the response-side Digest emission AND the
   request-side Content-Digest validation tests. */
static const char TEST_DAV_FAKE_HEX[] =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

static int
test_dav_digest(void *user, const char *path, const char *algo,
                char *out_hex, size_t hex_size)
{
    (void)user;
    if (axl_strcmp(algo, "sha-256") != 0) {
        return AXL_ERR;
    }
    void *v = axl_hash_table_lookup(m_dav_fs, path);
    if (v == NULL || (uintptr_t)v == 2) {
        return AXL_ERR;  /* missing or directory */
    }
    size_t need = 64 + 1;
    if (hex_size < need) {
        return AXL_ERR;
    }
    for (size_t i = 0; i < 64; i++) out_hex[i] = TEST_DAV_FAKE_HEX[i];
    out_hex[64] = '\0';
    return AXL_OK;
}

/* Last-call hook: stamps every WebDAV response with an
   X-Test-Dav-Hook header so the integration test can prove the
   hook fired across every verb. The integration test cycles
   through OPTIONS / PROPFIND / GET / HEAD / PUT-EOF / DELETE /
   MOVE / COPY / MKCOL and asserts the header is present. */
static void
test_dav_before_response(void *user, AxlHttpRequest *req,
                         AxlHttpResponse *resp)
{
    (void)user;
    if (resp->headers == NULL) {
        resp->headers = axl_hash_table_new_full(
            axl_str_hash, axl_str_equal,
            axl_free_impl, axl_free_impl);
        if (resp->headers == NULL) {
            return;
        }
    }
    /* Echo the request method so we can also prove the hook saw
       the right request — proves req is plumbed through, not just
       a generic stamp. */
    char *k = axl_strdup("X-Test-Dav-Hook");
    char *v = axl_strdup(req->method != NULL ? req->method : "?");
    if (k == NULL || v == NULL) {
        axl_free(k);
        axl_free(v);
        return;
    }
    axl_hash_table_replace(resp->headers, k, v);
}

static const AxlWebDavOps test_webdav_ops = {
    .stat            = test_dav_stat,
    .list_dir        = test_dav_list_dir,
    .read_open       = test_dav_read_open,
    .read_chunk      = test_dav_read_chunk,
    .read_close      = test_dav_read_close,
    .write_open      = test_dav_write_open,
    .write_chunk     = test_dav_write_chunk,
    .write_close     = test_dav_write_close,
    .mkdir           = test_dav_mkdir,
    .remove          = test_dav_remove,
    .move            = test_dav_move,
    .copy            = test_dav_copy,
    .content_type    = test_dav_content_type,
    .digest          = test_dav_digest,
    .before_response = test_dav_before_response,
};

/* GET /dav-status — exposes m_dav_last_put_* so the integration
   test can verify PUT round-trips. Cache-bust via query string. */
static int
on_dav_status(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    char buf[1024];
    /* Truncate body in the JSON payload to keep it small; the
       integration test only checks length + path + first bytes. */
    size_t show = m_dav_last_put_len < 64 ? m_dav_last_put_len : 64;
    char body_preview[129] = {0};
    for (size_t i = 0; i < show; i++) {
        unsigned char c = (unsigned char)m_dav_last_put_body[i];
        /* Hex-escape non-ASCII so the JSON doesn't break. */
        body_preview[i] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
            ? (char)c : '?';
    }
    axl_snprintf(buf, sizeof(buf),
        "{\"path\":\"%s\",\"len\":%llu,\"aborted\":%s,\"preview\":\"%s\"}",
        m_dav_last_put_path,
        (unsigned long long)m_dav_last_put_len,
        m_dav_last_put_aborted ? "true" : "false",
        body_preview);
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

// Per-connection echo (add_websocket_ex): reply only to the sending client
// via axl_ws_send, prefixed "ex:" so the test distinguishes it from a
// broadcast. CONNECT reads the peer to prove the handle carries identity.
static int
on_ws_echo_ex(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)data;
    if (event == AXL_WS_CONNECT) {
        uint8_t peer[4] = { 0 };
        if (axl_ws_conn_peer(conn, peer) == AXL_OK) {
            axl_printf("WS-EX: connect peer=%u.%u.%u.%u\r\n",
                       peer[0], peer[1], peer[2], peer[3]);
        }
        return 0;
    }
    if (event == AXL_WS_TEXT) {
        char   reply[256];
        int    n = (frame_size < 240) ? (int)frame_size : 240;
        axl_snprintf(reply, sizeof(reply), "ex:%.*s", n, (const char *)frame);
        axl_ws_send(conn, AXL_WS_TEXT, reply, axl_strlen(reply));
    }
    return 0;
}

// Server-initiated close from within a frame handler (axl_ws_conn_close):
// regression guard for the "handler tears down the conn mid-dispatch" path —
// process_websocket_data must not re-arm recv on the reset connection.
static int
on_ws_close(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)data;
    if (event == AXL_WS_TEXT && frame_size == 3
        && axl_memcmp(frame, "bye", 3) == 0) {
        axl_ws_conn_close(conn);
    }
    return 0;
}

// Burst endpoint (ws-broadcast-over-TLS desync repro): on ANY client frame,
// fire WS_BURST_N back-to-back ws_broadcast calls within one dispatch — exactly
// what the console mirror does echoing one keystroke (>=3 ConOut ops). Each
// frame carries a distinct, fixed-width payload so the client can assert all N
// arrive in order, byte-exact, with the TLS stream intact (no desync). `data`
// is the AxlHttpServer* (broadcast needs the server).
#define WS_BURST_N  8
static int
on_ws_burst(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)conn;
    (void)frame;
    (void)frame_size;
    AxlHttpServer *s = (AxlHttpServer *)data;
    if (event == AXL_WS_TEXT || event == AXL_WS_BINARY) {
        for (int i = 0; i < WS_BURST_N; i++) {
            char msg[16];
            /* Fixed 8-byte payload "WSBURSTn" — distinct per frame. */
            axl_snprintf(msg, sizeof(msg), "WSBURST%d", i);
            axl_http_server_ws_broadcast(s, "/ws-burst", msg, axl_strlen(msg));
        }
    }
    return 0;
}

// Authenticated endpoint (add_websocket_ex + AXL_ROUTE_AUTH): echoes the
// identity captured at upgrade, proving auth-on-upgrade + axl_ws_conn_auth.
static int
on_ws_auth(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)frame;
    (void)frame_size;
    (void)data;
    if (event == AXL_WS_TEXT) {
        AxlAuthInfo info;
        char        reply[128];
        if (axl_ws_conn_auth(conn, &info) == AXL_OK && info.username != NULL) {
            axl_snprintf(reply, sizeof(reply), "user:%s", info.username);
        } else {
            axl_strlcpy(reply, "user:?", sizeof(reply));
        }
        axl_ws_send(conn, AXL_WS_TEXT, reply, axl_strlen(reply));
    }
    return 0;
}

// P1 greet-on-connect: a banner sent from AXL_WS_CONNECT must reach the
// client AFTER the 101 (valid only because CONNECT now fires post-handshake).
static int
on_ws_greet(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)frame;
    (void)frame_size;
    (void)data;
    if (event == AXL_WS_CONNECT) {
        axl_ws_send(conn, AXL_WS_TEXT, "hi", 2);
    }
    return 0;
}

// P1 close-from-connect: calling axl_ws_conn_close from AXL_WS_CONNECT (then
// returning AXL_OK) must not leave on_response_sent arming recv on the
// torn-down conn — regression for the missing post-CONNECT active check.
static int
on_ws_connect_close(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)frame;
    (void)frame_size;
    (void)data;
    if (event == AXL_WS_CONNECT) {
        axl_ws_conn_close(conn);
    }
    return 0;   /* AXL_OK — close already requested */
}

// P1 reject-on-connect: returning AXL_ERR from AXL_WS_CONNECT must drop the
// connection (the 101 was sent, then the socket closes; no further events).
static int
on_ws_reject(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)conn;
    (void)frame;
    (void)frame_size;
    (void)data;
    if (event == AXL_WS_CONNECT) {
        return AXL_ERR;   /* refuse the connection */
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
    axl_http_server_add_route(s, "GET",  "/streaming-put-test",
                              on_streaming_put_test, NULL);
    axl_http_server_add_route(s, "GET",  "/static-asset", on_get_static_asset, NULL);

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
    axl_http_server_set_auth_challenge(s, "Basic", "axl-test");
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
    /* TTL must be above axl_time_get_ms()'s 1-second granularity on
       UEFI: GetTime returns 0 for nanosecond, so two requests in the
       same wall-clock second compute age=0 regardless of how much
       real time elapsed, and a sub-second TTL never expires. The
       integration test that follows sleeps 2 s before the second
       request to give us at least one second-boundary crossing. */
    axl_http_server_set_route_ttl(s, "/ttl-short", 1500);

    axl_http_server_add_route(s, "GET", "/api/users/1", on_users_1, NULL);
    axl_http_server_add_route(s, "GET", "/api/users/2", on_users_2, NULL);
    axl_http_server_add_route(s, "GET", "/api/posts/1", on_posts_1, NULL);
    axl_http_server_add_route(s, "GET", "/invalidate-users", on_invalidate_users, s);

    /* Header-gated middleware: rejects any request bearing
       X-Test-Reject: 1 with 403. Registered AFTER the auth middleware
       above; the integration test exercises it on /upload to verify
       middleware runs ahead of upload routes (was a silent bypass). */
    axl_http_server_use(s, test_reject_middleware, NULL);

    /* WebDAV test mount under /dav. The test backend is an in-memory
       hash table keyed by full path; the integration test exercises
       OPTIONS / MKCOL / DELETE / MOVE through it. */
    if (test_webdav_init() == AXL_OK) {
        axl_http_server_add_webdav(s, "/dav", &test_webdav_ops, NULL);
        axl_http_server_add_route(s, "GET", "/dav-status",
                                  on_dav_status, NULL);
    }

    /* Upload streaming endpoint + status probe (counters survive the
       per-upload reset so the integration test can verify
       middleware-blocked vs aborted vs completed). */
    axl_http_server_set(s, "upload.chunk.size", "1024");
    axl_http_server_add_upload_route(s, "POST", "/upload", on_upload, NULL);
    axl_http_server_add_route(s, "GET", "/upload-status", on_upload_status, NULL);

    /* WebSocket echo endpoint */
    ws_test_server = s;
    axl_http_server_add_websocket(s, "/ws-echo", on_ws_echo, NULL);
    axl_http_server_add_websocket_ex(s, "/ws-echo-ex", on_ws_echo_ex, NULL,
                                     AXL_ROUTE_NO_AUTH);
    axl_http_server_add_websocket_ex(s, "/ws-auth", on_ws_auth, NULL,
                                     AXL_ROUTE_AUTH);
    axl_http_server_add_websocket_ex(s, "/ws-close", on_ws_close, NULL,
                                     AXL_ROUTE_NO_AUTH);
    axl_http_server_add_websocket_ex(s, "/ws-greet", on_ws_greet, NULL,
                                     AXL_ROUTE_NO_AUTH);
    axl_http_server_add_websocket_ex(s, "/ws-reject", on_ws_reject, NULL,
                                     AXL_ROUTE_NO_AUTH);
    axl_http_server_add_websocket_ex(s, "/ws-connect-close", on_ws_connect_close,
                                     NULL, AXL_ROUTE_NO_AUTH);

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

    if (axl_tls_init() != AXL_OK) {
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

    if (axl_http_server_use_tls(s, cert, cert_len, key, key_len) != AXL_OK) {
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
    /* Per-connection WS over TLS — the inbound-frame-over-TLS regression
       (wss client echo). on_ws_echo_ex replies per-client via axl_ws_send. */
    axl_http_server_add_websocket_ex(s, "/ws-echo-ex", on_ws_echo_ex, NULL,
                                     AXL_ROUTE_NO_AUTH);

    axl_printf("HTTPS server listening on port 8443\n");
    axl_printf("READY\n");

    return axl_http_server_run(s);
}

// ---------------------------------------------------------------------------
// HTTPS server driven by a RESIDENT driver-tick loop — "serve-tls-driver".
//
// Same TLS server as serve-tls, but instead of a top-level axl_loop_run it
// uses axl_loop_attach_driver (the DXE-driver / AxlService dispatch: a
// periodic timer notify at TPL_CALLBACK drains the loop) and idles the
// foreground. This is the shape that exposed the TLS handshake stall — a
// synchronous handshake nesting an ephemeral axl_loop_run cannot run at the
// tick's raised TPL. With the async handshake it completes; the same curl
// that times out against the buggy build gets a 200 here.
// ---------------------------------------------------------------------------

static int
run_serve_tls_driver_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        return -1;
    }

    axl_net_auto_init(SIZE_MAX, 10);
    if (axl_tls_init() != AXL_OK) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len, &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s    = axl_http_server_new(8443);
    if (loop == NULL || s == NULL
        || axl_http_server_use_tls(s, cert, cert_len, key, key_len) != AXL_OK) {
        axl_printf("ERROR: TLS server setup failed\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }
    axl_free(cert);
    axl_free(key);

    axl_http_server_add_route(s, "GET", "/api/version", on_get_version, NULL);
    axl_http_server_add_route(s, "GET", "/plain",       on_get_plain,   NULL);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start server on loop\n");
        return -1;
    }

    /* Drive the loop the way a resident DXE driver / AxlService does — a
       periodic firmware timer at TPL_CALLBACK, NOT a foreground
       axl_loop_run. The foreground then idles. */
    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTPS server (resident driver-tick loop) on port 8443\n");
    axl_printf("READY\n");

    /* Idle: the driver tick pumps the loop in the background. */
    for (;;) {
        axl_msleep(1000);
    }
}

// ---------------------------------------------------------------------------
// HTTPS + per-client WebSocket under a RESIDENT driver-tick loop —
// "serve-tls-ws-driver".
//
// Regression for the WS-teardown wedge (SoftBMC console-mirror RemoteShell):
// the same TLS server as serve-tls-driver, plus a PER_CLIENT WebSocket
// endpoint (add_websocket_ex). A wss client connect + clean disconnect against
// the pumped server used to WEDGE the whole loop — process_websocket_data's
// WS_OP_CLOSE echo (and axl_ws_conn_close) did a SYNCHRONOUS axl_tls_write,
// which spins a nested ephemeral axl_loop_run that cannot progress at the
// tick's raised TPL (the adbf5461 / axl_tls_free hazard). The next HTTPS GET
// then timed out. With the async-pong + FIN-conveys-close fix the loop keeps
// serving after a WS connect/disconnect. Driven by test-ws-teardown-driver-qemu.sh.
// ---------------------------------------------------------------------------

static int
run_serve_tls_ws_driver_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        return -1;
    }

    axl_net_auto_init(SIZE_MAX, 10);
    if (axl_tls_init() != AXL_OK) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len, &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s    = axl_http_server_new(8443);
    if (loop == NULL || s == NULL
        || axl_http_server_use_tls(s, cert, cert_len, key, key_len) != AXL_OK) {
        axl_printf("ERROR: TLS server setup failed\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }
    axl_free(cert);
    axl_free(key);

    axl_http_server_add_route(s, "GET", "/api/version", on_get_version, NULL);
    axl_http_server_add_route(s, "GET", "/plain",       on_get_plain,   NULL);
    /* Per-client WebSocket endpoint — the console-mirror RemoteShell shape. */
    axl_http_server_add_websocket_ex(s, "/ws-console", on_ws_echo_ex, NULL,
                                     AXL_ROUTE_NO_AUTH);
    /* Burst endpoint — the ws-broadcast-over-TLS desync repro: on any client
       frame, fire WS_BURST_N back-to-back broadcasts within one dispatch. */
    axl_http_server_add_websocket_ex(s, "/ws-burst", on_ws_burst, s,
                                     AXL_ROUTE_NO_AUTH);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start server on loop\n");
        return -1;
    }

    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTPS+WS server (resident driver-tick loop) on port 8443\n");
    axl_printf("READY\n");

    /* Idle: the driver tick pumps the loop in the background. */
    for (;;) {
        axl_msleep(1000);
    }
}

/* on_get_srv2 (the plain second server's handler) is defined later. */
static int
on_get_srv2(AxlHttpRequest *req, AxlHttpResponse *resp, void *data);

// ---------------------------------------------------------------------------
// Consumer-emulator: the CANONICAL hazardous topology — "serve-hazard-driver".
//
// This is the SoftBMC execution model in one binary, combining every shape
// that surfaced a wedge: HTTPS (8443) AND plain HTTP (8081) on ONE shared
// loop (the adbf5461 second-server-dead-accept class), pumped from an
// axl_loop_attach_driver tick at raised TPL (the d249a9b6 / e90b87e4 sync-at-
// raised-TPL class), with a per-client WebSocket endpoint whose handlers do
// async I/O and a broadcast-burst endpoint (the 4563aabf ws-over-TLS desync).
// The unit suite only ever models one server on its own loop at TPL_APPLICATION;
// this mode is the in-repo stand-in for the consumer that found those bugs.
// Driven by test-consumer-emulator-qemu.sh, which probes liveness after each
// scenario. See docs/AXL-Concurrency.md "Testing the model".
// ---------------------------------------------------------------------------

static int
run_serve_hazard_driver_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        return -1;
    }

    axl_net_auto_init(SIZE_MAX, 10);
    if (axl_tls_init() != AXL_OK) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len, &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s1   = axl_http_server_new(8443);   /* HTTPS, started first */
    AxlHttpServer *s2   = axl_http_server_new(8081);   /* plain, started 2nd   */
    if (loop == NULL || s1 == NULL || s2 == NULL
        || axl_http_server_use_tls(s1, cert, cert_len, key, key_len) != AXL_OK) {
        axl_printf("ERROR: server setup failed\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }
    axl_free(cert);
    axl_free(key);

    /* HTTPS server: a plain route (liveness probe target), a per-client WS
       endpoint (handlers send/close), and a broadcast-burst endpoint. */
    axl_http_server_add_route(s1, "GET", "/api/version", on_get_version, NULL);
    axl_http_server_add_route(s1, "GET", "/plain",       on_get_plain,   NULL);
    axl_http_server_add_websocket_ex(s1, "/ws-console", on_ws_echo_ex, NULL,
                                     AXL_ROUTE_NO_AUTH);
    axl_http_server_add_websocket_ex(s1, "/ws-burst", on_ws_burst, s1,
                                     AXL_ROUTE_NO_AUTH);
    /* Plain second server on the SHARED loop — must still dispatch. */
    axl_http_server_add_route(s2, "GET", "/plain", on_get_srv2, NULL);

    if (axl_http_server_start(s1, loop) != 0
        || axl_http_server_start(s2, loop) != 0) {
        axl_printf("ERROR: failed to start servers on shared loop\n");
        return -1;
    }

    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTPS(8443)+WS + plain HTTP(8081), shared driver-tick loop\n");
    axl_printf("READY\n");

    /* Idle: the driver tick pumps the loop in the background. */
    for (;;) {
        axl_msleep(1000);
    }
}

// ---------------------------------------------------------------------------
// Shell coexistence spike — "serve-shell-coexist". The Console Mirror
// linchpin: a foreground real Shell.efi (StartImage blocks) MUST coexist with
// a background HTTP server pumped off a firmware timer (axl_loop_attach_driver,
// the resident-driver model). We start a plain HTTP server on a loop, attach
// the driver tick, then axl_shell_launch() — which blocks in the Shell. The
// Shell sits at its prompt in WaitForEvent(ConIn), which lowers TPL and lets
// the periodic timer fire and drain the loop. The harness curls /plain while
// the Shell is foreground: a 200 proves the two coexist. The inner Shell never
// gets input, so it never exits — the harness asserts on the curl and lets
// QEMU time out.
// ---------------------------------------------------------------------------

static int
run_serve_shell_coexist_mode(void)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s    = axl_http_server_new(8080);
    if (loop == NULL || s == NULL) {
        axl_printf("ERROR: server setup failed\n");
        return -1;
    }
    axl_http_server_add_route(s, "GET", "/plain", on_get_plain, NULL);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start server on loop\n");
        return -1;
    }

    /* Pump the loop from a firmware periodic timer at TPL_CALLBACK — the
       background HTTP heartbeat. The foreground then goes to the Shell. */
    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTP server (driver-tick) on 8080; launching foreground Shell\n");
    axl_printf("READY\n");

    /* StartImage(Shell.efi) — blocks. The HTTP server keeps serving off
       the timer the whole time. */
    int exit_code = 0;
    int rc = axl_shell_launch(&exit_code);
    if (rc != AXL_OK) {
        /* No Shell.efi staged on this runner: report so the harness can
           SKIP-balance rather than read it as a coexistence failure. */
        axl_printf("NO_SHELL\n");
        return 0;
    }

    /* Reached only if the Shell exits (it won't in the unattended test). */
    axl_loop_detach_driver(loop);
    axl_printf("SHELL_EXITED %d\n", exit_code);
    return 0;
}

// ---------------------------------------------------------------------------
// Console-mirror rung 5 (P3) — "serve-tls-shell-coexist". The deployment-
// faithful SoftBMC shape, all three at once: the AxlConsoleMirror wrapping the
// console, an HTTPS server pumped by axl_loop_attach_driver (TPL_CALLBACK), and
// a real foreground child Shell.efi. Proves the TLS handshake completes under
// the timer pump WHILE the mirrored Shell holds the foreground — the sharp
// async-TLS-at-raised-TPL envelope combined with a blocked StartImage. The
// harness curls HTTPS while the Shell is foreground; a 200 is the proof. The
// inner Shell gets no input, so the app idles in the launcher; QEMU times out.
// ---------------------------------------------------------------------------

static void
tls_coexist_sink(const char *bytes, size_t len, void *user)
{
    (void)bytes;
    (void)len;
    (void)user;   /* discard: rung 5 proves coexistence, not mirror content */
}

static int
run_serve_tls_shell_coexist_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        axl_printf("NO_TLS\n");
        return -1;
    }

    axl_net_auto_init(SIZE_MAX, 10);
    if (axl_tls_init() != AXL_OK) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len, &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s    = axl_http_server_new(8443);
    if (loop == NULL || s == NULL
        || axl_http_server_use_tls(s, cert, cert_len, key, key_len) != AXL_OK) {
        axl_printf("ERROR: TLS server setup failed\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }
    axl_free(cert);
    axl_free(key);
    axl_http_server_add_route(s, "GET", "/plain", on_get_plain, NULL);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start TLS server on loop\n");
        return -1;
    }

    /* Wrap the console (the child Shell runs mirrored), then pump the loop
       from the firmware timer in the background. */
    AxlConsoleMirror      *m   = NULL;
    AxlConsoleMirrorConfig cfg = {
        .sink = tls_coexist_sink, .user = NULL,
        .cols = 80, .rows = 25, .passthrough_local = true,
    };
    if (axl_console_mirror_install(&m, &cfg) != AXL_OK) {
        axl_printf("ERROR: mirror install failed\n");
        return -1;
    }

    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTPS (driver-tick) on 8443 + mirror; launching foreground Shell\n");
    axl_printf("READY\n");

    /* Foreground: the real Shell, console mirrored, while HTTPS keeps serving
       off the timer. Blocks (no input → never exits in the unattended test). */
    int exit_code = 0;
    if (axl_shell_launch(&exit_code) != AXL_OK) {
        axl_printf("NO_SHELL\n");
    }

    axl_loop_detach_driver(loop);
    axl_console_mirror_uninstall(m);
    for (;;) {
        axl_msleep(1000);
    }
    return 0;  /* unreachable */
}

// ---------------------------------------------------------------------------
// Console-mirror self-test — "mirror-selftest". The positive AxlConsoleMirror
// proof (install → wrap gST ConIn/ConOut → translate/inject → uninstall) run
// in ITS OWN QEMU boot. It can't run in the combined unit boot: wrapping the
// parent harness Shell's console wedges that Shell for the next binary (even
// after a clean uninstall). Here we drive the wrapped console directly with a
// capture sink, prove the VT translation + key injection, uninstall (restoring
// the console), print results, then idle forever — never returning to the
// (now-wedged) parent Shell. The harness greps the printed PASS lines.
// ---------------------------------------------------------------------------

static char   g_cm_cap[8192];
static size_t g_cm_cap_len;

static void
cm_selftest_sink(const char *bytes, size_t len, void *user)
{
    (void)user;
    for (size_t i = 0; i < len && g_cm_cap_len < sizeof(g_cm_cap) - 1; i++) {
        g_cm_cap[g_cm_cap_len++] = bytes[i];
    }
    g_cm_cap[g_cm_cap_len] = '\0';
}

static int
run_mirror_selftest_mode(void)
{
    AxlConsoleMirror      *m   = NULL;
    AxlConsoleMirrorConfig cfg = {
        .sink = cm_selftest_sink, .user = NULL,
        .cols = 80, .rows = 25, .passthrough_local = false,
    };

    g_cm_cap_len = 0;
    g_cm_cap[0]  = '\0';

    int rc = axl_console_mirror_install(&m, &cfg);

    bool dbl_blocked = false;
    bool got_clear = false, got_cursor = false, got_sgr = false, got_text = false;
    bool got_size = false;
    bool inj_up = false, inj_x = false, inj_f2 = false;

    if (rc == AXL_OK) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = gST->ConOut;
        EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *in  = gST->ConIn;

        AxlConsoleMirror *m2 = NULL;
        dbl_blocked = (axl_console_mirror_install(&m2, &cfg) == AXL_ERR);

        /* QueryMode on the CURRENT mode must report the remote size (this is
           what `edit` queries to lay itself out). */
        UINTN qc = 0, qr = 0;
        out->QueryMode(out, (UINTN)out->Mode->Mode, &qc, &qr);
        got_size = (qc == 80 && qr == 25);

        out->ClearScreen(out);
        out->SetCursorPosition(out, 4, 2);     /* col 4, row 2 -> ESC[3;5H */
        out->SetAttribute(out, EFI_LIGHTRED);   /* fg 12 -> SGR 91 */
        out->OutputString(out, (CHAR16 *)u"Hi");

        got_clear  = (axl_strstr(g_cm_cap, "\x1b[2J\x1b[H") != NULL);
        got_cursor = (axl_strstr(g_cm_cap, "\x1b[3;5H") != NULL);
        got_sgr    = (axl_strstr(g_cm_cap, "\x1b[0;91;40m") != NULL);
        got_text   = (axl_strstr(g_cm_cap, "Hi") != NULL);

        EFI_INPUT_KEY k1 = {0}, k2 = {0}, k3 = {0};
        axl_console_mirror_inject_text(m, "\x1b[A", 3);  /* Up  -> scan 0x01 */
        axl_console_mirror_inject_text(m, "x", 1);        /* 'x' -> unicode    */
        axl_console_mirror_inject_key(m, 0x0C, 0);        /* F2  -> scan 0x0C  */
        inj_up = (in->ReadKeyStroke(in, &k1) == EFI_SUCCESS
                  && k1.ScanCode == 0x01 && k1.UnicodeChar == 0);
        inj_x  = (in->ReadKeyStroke(in, &k2) == EFI_SUCCESS
                  && k2.ScanCode == 0 && k2.UnicodeChar == 'x');
        inj_f2 = (in->ReadKeyStroke(in, &k3) == EFI_SUCCESS
                  && k3.ScanCode == 0x0C && k3.UnicodeChar == 0);

        axl_console_mirror_uninstall(m);  /* restores the console */
    }

    /* Console restored: axl_printf reaches the real serial console now. */
    axl_printf("MIRROR_SELFTEST: install=%d\n", rc == AXL_OK);
    axl_printf("MIRROR_SELFTEST: dbl_install_blocked=%d\n", dbl_blocked);
    axl_printf("MIRROR_SELFTEST: query_size=%d\n", got_size);
    axl_printf("MIRROR_SELFTEST: clear=%d\n", got_clear);
    axl_printf("MIRROR_SELFTEST: cursor=%d\n", got_cursor);
    axl_printf("MIRROR_SELFTEST: sgr=%d\n", got_sgr);
    axl_printf("MIRROR_SELFTEST: text=%d\n", got_text);
    axl_printf("MIRROR_SELFTEST: inject_up=%d\n", inj_up);
    axl_printf("MIRROR_SELFTEST: inject_printable=%d\n", inj_x);
    axl_printf("MIRROR_SELFTEST: inject_key_f2=%d\n", inj_f2);

    bool all = (rc == AXL_OK) && dbl_blocked && got_size && got_clear
               && got_cursor && got_sgr && got_text && inj_up && inj_x && inj_f2;
    axl_printf("MIRROR_SELFTEST_DONE %s\n", all ? "PASS" : "FAIL");

    /* Do NOT return to the parent Shell (wrapping its console wedged it).
       Idle; the harness asserts on the printed lines and kills QEMU. */
    for (;;) {
        axl_msleep(1000);
    }
    return 0;  /* unreachable; satisfies non-void return */
}

// ---------------------------------------------------------------------------
// Console-mirror `edit` ACCEPTANCE GATE — "mirror-edit" (design §6 rung 3).
//
// The headline proof: the Shell's full-screen interactive `edit` runs OVER the
// mirror and actually saves a file — something a one-shot command shell can
// never do. We install the mirror, launch a real child Shell.efi via
// axl_shell_launch (foreground, blocks), and drive `edit` entirely through
// injected keystrokes from a background firmware timer (fires at TPL_CALLBACK
// while the child Shell idles in WaitForEvent): open the editor on a file, type
// a line, F2-save, F3-exit, then `exit` the child Shell. When the child Shell
// exits, axl_shell_launch returns; we uninstall, READ THE FILE BACK, and assert
// it contains the typed line. The mirror sink corroborates the full-screen VT
// framing (ClearScreen + cursor positioning). Needs a real Shell.efi staged.
// ---------------------------------------------------------------------------

#define EDIT_FILE_PATH  "fs0:\\mtxt.txt"
#define EDIT_LINE       "hello mirror"

static AxlConsoleMirror *g_edit_mirror;
static EFI_EVENT         g_edit_timer;
static volatile UINTN    g_edit_phase;
static UINTN             g_edit_phase_tick;
static UINT8             g_edit_csi;   /* CSI escape parser state for the sink */

/* Full-screen framing seen in the sink (corroboration). */
static volatile bool     g_edit_saw_clear;
static volatile bool     g_edit_saw_cursor;

/* Milestones the sink detects in edit's output to SYNC injection — robust to
   variable Shell/editor startup time instead of fixed delays. Each is an
   incremental substring match over the mirrored byte stream. */
static volatile bool g_ms_prompt;   /* "\>"           — Shell prompt          */
static volatile bool g_ms_editor;   /* "Ctrl-E"       — editor status bar up  */
static volatile bool g_ms_savep;    /* "File to Save" — F2 save dialog open   */
static size_t        g_mp_prompt, g_mp_editor, g_mp_savep;

static void
feed_needle(unsigned char b, const char *needle, size_t *pos, volatile bool *flag)
{
    if (b == (unsigned char)needle[*pos]) {
        (*pos)++;
        if (needle[*pos] == '\0') {
            *flag = true;
            *pos = 0;
        }
    } else {
        *pos = (b == (unsigned char)needle[0]) ? 1 : 0;
    }
}

/* Sink: (a) prove edit drove a full-screen redraw — a clear (ESC[..J) and a
   cursor-position escape (ESC[..H) — and (b) detect the prompt / editor /
   save-dialog milestones that gate injection. */
static void
edit_frame_sink(const char *bytes, size_t len, void *user)
{
    (void)user;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];

        feed_needle(b, "\\>",           &g_mp_prompt, &g_ms_prompt);
        feed_needle(b, "Ctrl-E",        &g_mp_editor, &g_ms_editor);
        feed_needle(b, "File to Save",  &g_mp_savep,  &g_ms_savep);

        switch (g_edit_csi) {
            case 0: if (b == 0x1B) { g_edit_csi = 1; } break;
            case 1: g_edit_csi = (b == '[') ? 2 : 0; break;
            default:  /* inside CSI: scan to the final byte */
                if (b == 'J') {
                    g_edit_saw_clear = true;   /* ESC[..J — clear screen */
                    g_edit_csi = 0;
                } else if (b == 'H' || b == 'f') {
                    g_edit_saw_cursor = true;  /* ESC[..H — cursor position */
                    g_edit_csi = 0;
                } else if ((b >= '@' && b <= '~')) {
                    g_edit_csi = 0;            /* some other final byte */
                }
                break;
        }
    }
}

/* Periodic (1s) injection driver. Milestone-gated: each phase waits for the
   sink to observe the state edit is in before injecting the next keystrokes,
   so it tolerates variable startup time. The two "settle" phases use a short
   tick delay (no distinct on-screen milestone to wait for). */
static void EFIAPI
edit_inject_cb(EFI_EVENT ev, void *ctx)
{
    (void)ev;
    (void)ctx;
    AxlConsoleMirror *m = g_edit_mirror;
    if (m == NULL) {
        return;
    }
    switch (g_edit_phase) {
        case 0:  /* wait for the Shell prompt, then open the editor */
            if (g_ms_prompt) {
                axl_console_mirror_inject_text(m, "edit " EDIT_FILE_PATH "\r",
                                               5 + (sizeof(EDIT_FILE_PATH) - 1) + 1);
                g_ms_editor  = false;
                g_edit_phase = 1;
            }
            break;
        case 1:  /* wait for the editor status bar, then type the line */
            if (g_ms_editor) {
                axl_console_mirror_inject_text(m, EDIT_LINE,
                                               sizeof(EDIT_LINE) - 1);
                g_edit_phase_tick = 0;
                g_edit_phase = 2;
            }
            break;
        case 2:  /* let the typed text settle, then F2 (Save) */
            if (++g_edit_phase_tick >= 1) {
                g_ms_savep = false;
                axl_console_mirror_inject_key(m, 0x0C, 0);
                g_edit_phase = 3;
            }
            break;
        case 3:  /* wait for the "File to Save" dialog, then ENTER to commit */
            if (g_ms_savep) {
                axl_console_mirror_inject_text(m, "\r", 1);
                g_edit_phase_tick = 0;
                g_edit_phase = 4;
            }
            break;
        case 4:  /* let the save settle, then F3 (Exit editor) */
            if (++g_edit_phase_tick >= 2) {
                g_ms_prompt = false;
                axl_console_mirror_inject_key(m, 0x0D, 0);
                g_edit_phase = 5;
            }
            break;
        case 5:  /* wait for the Shell prompt again, then exit the child Shell */
            if (g_ms_prompt) {
                axl_console_mirror_inject_text(m, "exit\r", 5);
                g_edit_phase = 6;
            }
            break;
        default:
            break;
    }
}

/* Read the edited file back and report whether it holds the typed line. */
static bool
edit_file_has_line(void)
{
    AxlFileView *v = axl_file_view_open(EDIT_FILE_PATH, 0, 4);
    if (v == NULL) {
        return false;
    }
    size_t sz = axl_file_view_size(v);
    if (sz == 0 || sz > 4096) {
        axl_file_view_close(v);
        return false;
    }
    char raw[4096];
    size_t n = axl_file_view_read(v, 0, raw, sz);
    axl_file_view_close(v);

    /* UEFI `edit` saves new files as UTF-16LE (the "UNICODE" file type), so the
       bytes are h\0e\0l\0l\0o\0... Strip NUL bytes to recover the ASCII text
       (also a no-op for an ASCII file), then match. */
    char ascii[4097];
    size_t a = 0;
    for (size_t i = 0; i < n && a < sizeof(ascii) - 1; i++) {
        if (raw[i] != '\0') {
            ascii[a++] = raw[i];
        }
    }
    ascii[a] = '\0';
    return axl_strstr(ascii, EDIT_LINE) != NULL;
}

static int
run_mirror_edit_mode(void)
{
    AxlConsoleMirrorConfig cfg = {
        .sink = edit_frame_sink, .user = NULL,
        .cols = 80, .rows = 25, .passthrough_local = true,
    };

    g_edit_phase      = 0;
    g_edit_phase_tick = 0;
    g_edit_saw_clear  = false;
    g_edit_saw_cursor = false;
    g_edit_csi        = 0;
    g_ms_prompt = g_ms_editor = g_ms_savep = false;
    g_mp_prompt = g_mp_editor = g_mp_savep = 0;

    if (axl_console_mirror_install(&g_edit_mirror, &cfg) != AXL_OK) {
        axl_printf("MIRROR_EDIT: install=0\n");
        axl_printf("MIRROR_EDIT_DONE FAIL\n");
        for (;;) { axl_msleep(1000); }
    }

    /* Background injection heartbeat: a 1s periodic firmware timer at
       TPL_CALLBACK, which fires while the child Shell idles in WaitForEvent. */
    EFI_STATUS st = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                     edit_inject_cb, NULL, &g_edit_timer);
    if (!EFI_ERROR(st)) {
        gBS->SetTimer(g_edit_timer, TimerPeriodic, 10000000);  /* 1s */
    }

    /* Foreground: launch the real child Shell. Blocks until it `exit`s (driven
       by the injected sequence above). */
    int exit_code = 0;
    int launched  = axl_shell_launch(&exit_code);

    /* Child Shell exited (or no Shell.efi). Stop injecting, restore console. */
    if (g_edit_timer != NULL) {
        gBS->SetTimer(g_edit_timer, TimerCancel, 0);
        gBS->CloseEvent(g_edit_timer);
        g_edit_timer = NULL;
    }
    bool saw_clear  = g_edit_saw_clear;
    bool saw_cursor = g_edit_saw_cursor;
    axl_console_mirror_uninstall(g_edit_mirror);
    g_edit_mirror = NULL;

    /* Console restored: read the file back and report. */
    bool file_ok = (launched == AXL_OK) && edit_file_has_line();

    axl_printf("MIRROR_EDIT: install=1\n");
    axl_printf("MIRROR_EDIT: shell_launched=%d\n", launched == AXL_OK);
    axl_printf("MIRROR_EDIT: saw_clear=%d\n", saw_clear);
    axl_printf("MIRROR_EDIT: saw_cursor=%d\n", saw_cursor);
    axl_printf("MIRROR_EDIT: file_saved=%d\n", file_ok);

    bool all = (launched == AXL_OK) && saw_clear && saw_cursor && file_ok;
    axl_printf("MIRROR_EDIT_DONE %s\n", all ? "PASS" : "FAIL");

    for (;;) { axl_msleep(1000); }
    return 0;  /* unreachable */
}

// ---------------------------------------------------------------------------
// Console-mirror P1 GATE — "serve-ws-shell-inject" (the SoftBMC RemoteShell
// shape end-to-end). Proves axl_console_mirror_inject_text wakes the foreground
// Shell when the injection happens FROM A WS HANDLER running inside the
// axl_loop_attach_driver dispatch (raised TPL) — not from a standalone timer
// (the edit gate above already proved the timer path). This is the exact gap
// the SoftBMC handoff flagged P1 against.
//
// A per-client WS endpoint's handler forwards every received text/binary frame
// straight into axl_console_mirror_inject_text. A foreground child Shell.efi
// (axl_shell_launch, blocks) idles in WaitForEvent(ConIn); the driver tick
// pumps the loop, so the WS recv -> handler -> inject_text -> SignalEvent(
// wait_key) all runs at TPL_CALLBACK. The harness's WS client sends a command
// that writes a unique marker to a file, then `exit\r`. When the Shell exits,
// axl_shell_launch returns; we read the file back. The marker present == the
// injected keys drove the Shell (same deterministic assertion style as the
// edit gate, but triggered from the pumped-loop WS handler). Needs a staged
// Shell.efi; SKIP-balances via NO_SHELL otherwise.
// ---------------------------------------------------------------------------

#define P1_FILE_PATH  "fs0:\\p1inj.txt"
#define P1_MARKER     "P1INJECTOK"

static AxlConsoleMirror *g_p1_mirror;

/* No-op sink: P1 asserts the inject->wake->execute path via file readback, not
   mirrored output, so the sink content is irrelevant here. */
static void
p1_sink(const char *bytes, size_t len, void *user)
{
    (void)bytes;
    (void)len;
    (void)user;
}

/* WS handler: forward each received frame's bytes into the console mirror.
   This runs inside the attach_driver loop dispatch (TPL_CALLBACK) — the P1
   variable under test. */
static int
on_ws_console_inject(
    AxlWsConn  *conn,
    size_t      event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)conn;
    (void)data;
    if ((event == AXL_WS_TEXT || event == AXL_WS_BINARY)
        && frame != NULL && frame_size > 0 && g_p1_mirror != NULL) {
        axl_console_mirror_inject_text(g_p1_mirror, (const char *)frame,
                                       frame_size);
    }
    return 0;
}

/* Read P1_FILE_PATH back and report whether it holds the marker. Mirrors
   edit_file_has_line: the Shell may write the redirected file as UTF-16LE, so
   strip NULs before matching. */
static bool
p1_file_has_marker(void)
{
    AxlFileView *v = axl_file_view_open(P1_FILE_PATH, 0, 4);
    if (v == NULL) {
        return false;
    }
    size_t sz = axl_file_view_size(v);
    if (sz == 0 || sz > 4096) {
        axl_file_view_close(v);
        return false;
    }
    char raw[4096];
    size_t n = axl_file_view_read(v, 0, raw, sz);
    axl_file_view_close(v);

    char ascii[4097];
    size_t a = 0;
    for (size_t i = 0; i < n && a < sizeof(ascii) - 1; i++) {
        if (raw[i] != '\0') {
            ascii[a++] = raw[i];
        }
    }
    ascii[a] = '\0';
    return axl_strstr(ascii, P1_MARKER) != NULL;
}

static int
run_serve_ws_shell_inject_mode(void)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s    = axl_http_server_new(8080);
    if (loop == NULL || s == NULL) {
        axl_printf("ERROR: server setup failed\n");
        return -1;
    }
    /* Per-client WS endpoint that injects received frames into the mirror. */
    axl_http_server_add_websocket_ex(s, "/ws-console", on_ws_console_inject,
                                     NULL, AXL_ROUTE_NO_AUTH);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start server on loop\n");
        return -1;
    }

    AxlConsoleMirrorConfig cfg = {
        .sink = p1_sink, .user = NULL,
        .cols = 80, .rows = 25, .passthrough_local = true,
    };
    if (axl_console_mirror_install(&g_p1_mirror, &cfg) != AXL_OK) {
        axl_printf("ERROR: mirror install failed\n");
        return -1;
    }

    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTP+WS server (driver-tick) on 8080 + mirror; launching Shell\n");
    axl_printf("READY\n");

    /* Foreground: the real child Shell, console mirrored. Blocks until the
       injected `exit` returns it. */
    int exit_code = 0;
    int launched  = axl_shell_launch(&exit_code);

    axl_loop_detach_driver(loop);
    axl_console_mirror_uninstall(g_p1_mirror);
    g_p1_mirror = NULL;

    if (launched != AXL_OK) {
        /* No Shell.efi staged: SKIP-balance rather than read as a failure. */
        axl_printf("NO_SHELL\n");
        for (;;) { axl_msleep(1000); }
    }

    bool file_ok = p1_file_has_marker();
    axl_printf("P1_INJECT: shell_launched=1\n");
    axl_printf("P1_INJECT: file_marker=%d\n", file_ok);
    axl_printf("P1_INJECT_DONE %s\n", file_ok ? "PASS" : "FAIL");

    for (;;) { axl_msleep(1000); }
    return 0;  /* unreachable */
}

// ---------------------------------------------------------------------------
// Multi-server mode — "serve-multi" starts TWO plain HTTP servers on ONE
// shared AxlLoop (8080 + 8081). Regression repro for the bug where only the
// first-started server on a loop dispatched HTTP (SoftBMC's :443 + :80
// redirect pattern). Both must answer.
// ---------------------------------------------------------------------------

static int
on_get_srv2(
    AxlHttpRequest *req,
    AxlHttpResponse *resp,
    void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_text(resp, "Hello from server2");
    return 0;
}

static int
run_serve_multi_mode(void)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("ERROR: failed to create loop\n");
        return -1;
    }

    AxlHttpServer *s1 = axl_http_server_new(8080);
    AxlHttpServer *s2 = axl_http_server_new(8081);
    if (s1 == NULL || s2 == NULL) {
        axl_printf("ERROR: failed to create servers\n");
        return -1;
    }
    axl_http_server_add_route(s1, "GET", "/plain", on_get_plain, NULL);
    axl_http_server_add_route(s2, "GET", "/plain", on_get_srv2,  NULL);

    if (axl_http_server_start(s1, loop) != 0
        || axl_http_server_start(s2, loop) != 0) {
        axl_printf("ERROR: failed to start servers on shared loop\n");
        return -1;
    }

    axl_printf("HTTP servers on 8080 (s1) and 8081 (s2), shared loop\n");
    axl_printf("READY\n");

    return axl_loop_run(loop);
}

/* serve-davfs — mount an axl-fs-backed WebDAV file server with
   axl_http_server_serve_fs: /dav read-write and /ro read-only, both
   over a clean subtree of the (writable) boot volume. Drives the
   full verb set + traversal-reject + readonly-405 from the host. */
static int
run_serve_davfs_common(bool tls)
{
    axl_net_auto_init(SIZE_MAX, 10);

    /* Fresh served subtree (disk image is rebuilt per run, so this
       starts empty). */
    axl_dir_mkdir("FS0:\\davroot");

    void   *cert = NULL, *key = NULL;
    size_t  cert_len = 0, key_len = 0;
    if (tls) {
        if (!axl_tls_available()) {
            axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
            return -1;
        }
        if (axl_tls_init() != AXL_OK) {
            axl_printf("ERROR: TLS init failed\n");
            return -1;
        }
        if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                         &cert, &cert_len,
                                         &key, &key_len) != 0) {
            axl_printf("ERROR: cert generation failed\n");
            return -1;
        }
    }

    AxlHttpServer *s = axl_http_server_new(tls ? 8443 : 8080);
    if (s == NULL) {
        axl_printf("ERROR: failed to create server\n");
        axl_free(cert);
        axl_free(key);
        return -1;
    }
    if (tls) {
        if (axl_http_server_use_tls(s, cert, cert_len, key, key_len) != AXL_OK) {
            axl_printf("ERROR: TLS setup failed\n");
            axl_http_server_free(s);
            axl_free(cert);
            axl_free(key);
            return -1;
        }
        axl_free(cert);
        axl_free(key);
    }
    if (axl_http_server_serve_fs(s, "/dav", "FS0:\\davroot", 0,
                                 AXL_ROUTE_NO_AUTH) != AXL_OK) {
        axl_printf("ERROR: serve_fs (rw) failed\n");
        return -1;
    }
    if (axl_http_server_serve_fs(s, "/ro", "FS0:\\davroot",
                                 AXL_SERVE_FS_READONLY,
                                 AXL_ROUTE_NO_AUTH) != AXL_OK) {
        axl_printf("ERROR: serve_fs (readonly) failed\n");
        return -1;
    }

    /* Auth-gated mount: every verb (GET / PROPFIND / ... and the
       streaming PUT) requires "Bearer test-token". Proves
       serve_fs auth_flags gate the whole mount, including the upload
       path that bypasses dispatch_request. */
    axl_http_server_use_auth(s, test_auth_callback, NULL);
    if (axl_http_server_serve_fs(s, "/auth", "FS0:\\davroot", 0,
                                 AXL_ROUTE_AUTH) != AXL_OK) {
        axl_printf("ERROR: serve_fs (auth) failed\n");
        return -1;
    }

    /* Standalone authed upload routes exercise
       axl_http_server_add_upload_route_auth directly: /upload-auth
       needs any authenticated user (401 without), /upload-admin needs
       the admin role (403 for a non-admin token). */
    if (axl_http_server_add_upload_route_auth(s, "POST", "/upload-auth",
                                              on_upload, NULL,
                                              AXL_ROUTE_AUTH) != AXL_OK) {
        axl_printf("ERROR: add_upload_route_auth (auth) failed\n");
        return -1;
    }
    if (axl_http_server_add_upload_route_auth(s, "POST", "/upload-admin",
                                              on_upload, NULL,
                                              AXL_ROUTE_ADMIN) != AXL_OK) {
        axl_printf("ERROR: add_upload_route_auth (admin) failed\n");
        return -1;
    }

    /* A normal whole-body route (non-streaming): the body is accumulated
       into conn->body and echoed back. Exercises the Content-Length /
       chunked request-body accumulation path (distinct from the streaming
       upload path the /dav PUT uses) — needed to prove large bodies that
       span multiple TLS records are read to completion over HTTPS. */
    axl_http_server_add_route(s, "POST", "/echo", on_echo, NULL);

    axl_printf("WebDAV file server (%s): /dav (rw) + /ro (readonly) + "
               "/auth (gated) + POST /echo over FS0:\\davroot\n",
               tls ? "https" : "http");
    axl_printf("READY\n");
    return axl_http_server_run(s);
}

static int
run_serve_davfs_mode(void)
{
    return run_serve_davfs_common(false);
}

static int
run_serve_davfs_tls_mode(void)
{
    return run_serve_davfs_common(true);
}

/* serve-multi-tls — the SoftBMC shape: a TLS server (8443) started first,
   a plain server (8081) started second, on ONE shared loop. The plain
   second server must still dispatch. */
static int
run_serve_multi_tls_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
        return -1;
    }
    axl_net_auto_init(SIZE_MAX, 10);
    if (axl_tls_init() != AXL_OK) {
        axl_printf("ERROR: TLS init failed\n");
        return -1;
    }

    void  *cert = NULL, *key = NULL;
    size_t cert_len = 0, key_len = 0;
    if (axl_tls_generate_self_signed("AXL-Test", NULL, 0,
                                     &cert, &cert_len, &key, &key_len) != 0) {
        axl_printf("ERROR: cert generation failed\n");
        return -1;
    }

    AxlLoop *loop = axl_loop_new();
    AxlHttpServer *s1 = axl_http_server_new(8443);   /* TLS, started first  */
    AxlHttpServer *s2 = axl_http_server_new(8081);   /* plain, started 2nd  */
    if (loop == NULL || s1 == NULL || s2 == NULL) {
        axl_printf("ERROR: alloc failed\n");
        return -1;
    }
    if (axl_http_server_use_tls(s1, cert, cert_len, key, key_len) != AXL_OK) {
        axl_printf("ERROR: TLS setup failed\n");
        return -1;
    }
    axl_free(cert);
    axl_free(key);

    axl_http_server_add_route(s1, "GET", "/plain", on_get_plain, NULL);
    axl_http_server_add_route(s2, "GET", "/plain", on_get_srv2,  NULL);

    if (axl_http_server_start(s1, loop) != 0
        || axl_http_server_start(s2, loop) != 0) {
        axl_printf("ERROR: failed to start servers on shared loop\n");
        return -1;
    }

    axl_printf("TLS server on 8443 (s1) + plain server on 8081 (s2), shared loop\n");
    axl_printf("READY\n");

    return axl_loop_run(loop);
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
// AxlHttpRequest helpers — content negotiation + JSON body parse
// (axl_http_request_accepts / _wants_json / _get_json)
// ---------------------------------------------------------------------------

static void
test_http_request_helpers(void)
{
    /* Build a synthetic AxlHttpRequest with an Accept header. The SDK
       uses an AxlHashTable<const char *, const char *> for headers
       (lowercased keys); axl_http_request_accepts looks up "accept". */
    AxlHashTable *headers = axl_hash_table_new_str();
    test_check(headers != NULL, "request_helpers: hash_table_new succeeds");

    axl_hash_table_insert(headers, (void *)"accept",
                          (void *)"text/html, application/json;q=0.9");

    AxlHttpRequest req = { 0 };
    req.headers = headers;

    /* axl_http_request_accepts routes through axl_http_accepts so
       wildcards / case / q-values all behave. */
    test_check(axl_http_request_accepts(&req, "application/json"),
               "request_accepts: matches in multi-type list");
    test_check(axl_http_request_accepts(&req, "text/html"),
               "request_accepts: matches first entry");
    test_check(!axl_http_request_accepts(&req, "image/png"),
               "request_accepts: rejects type not in Accept");

    /* wants_json convenience */
    test_check(axl_http_request_wants_json(&req),
               "request_wants_json: present in Accept");

    /* Switch headers to no Accept entry — wants_json returns false */
    AxlHashTable *no_accept = axl_hash_table_new_str();
    AxlHttpRequest req_no_accept = { 0 };
    req_no_accept.headers = no_accept;
    test_check(!axl_http_request_wants_json(&req_no_accept),
               "request_wants_json: false on missing Accept header");
    axl_hash_table_free(no_accept);

    /* NULL safety */
    test_check(!axl_http_request_accepts(NULL, "application/json"),
               "request_accepts: NULL request returns false");
    test_check(!axl_http_request_wants_json(NULL),
               "request_wants_json: NULL request returns false");

    axl_hash_table_free(headers);

    /* JSON body parse — happy path */
    const char     *body = "{\"port\":9090,\"name\":\"axl\"}";
    AxlHttpRequest  json_req = { 0 };
    json_req.body      = body;
    json_req.body_size = axl_strlen(body);
    AxlJsonReader   reader = { 0 };
    test_check(axl_http_request_get_json(&json_req, &reader),
               "request_get_json: parses valid JSON body");
    /* Spot-check the reader works — pull the port field back. */
    int64_t port = -1;
    test_check(axl_json_get_int(&reader, "port", &port) && port == 9090,
               "request_get_json: parsed port=9090 round-trip");
    axl_json_free(&reader);

    /* Empty body / NULL inputs */
    AxlHttpRequest empty_req = { 0 };
    AxlJsonReader  dummy = { 0 };
    test_check(!axl_http_request_get_json(&empty_req, &dummy),
               "request_get_json: rejects empty body");
    test_check(!axl_http_request_get_json(NULL, &dummy),
               "request_get_json: NULL request returns false");
    test_check(!axl_http_request_get_json(&json_req, NULL),
               "request_get_json: NULL out returns false");

    /* Malformed JSON body */
    const char    *bad = "{not-json";
    AxlHttpRequest bad_req = { 0 };
    bad_req.body      = bad;
    bad_req.body_size = axl_strlen(bad);
    AxlJsonReader  bad_reader = { 0 };
    test_check(!axl_http_request_get_json(&bad_req, &bad_reader),
               "request_get_json: rejects malformed JSON");
}

// ---------------------------------------------------------------------------
// HTTP Range parsing tests
// ---------------------------------------------------------------------------
// axl_http_server_add_routes — variadic batch route registration
// ---------------------------------------------------------------------------

static int
addroutes_h_get(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req; (void)resp; (void)data; return 0;
}
static int
addroutes_h_put(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req; (void)resp; (void)data; return 0;
}
static int
addroutes_h_del(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req; (void)resp; (void)data; return 0;
}

static void
test_http_add_routes_variadic(void)
{
    AxlHttpServer *s = axl_http_server_new(8889);
    test_check(s != NULL, "add_routes: server alloc");
    if (s == NULL) {
        return;
    }

    int marker = 0;
    int rc = axl_http_server_add_routes(s,
        "GET",    "/a",  addroutes_h_get, &marker,
        "GET",    "/b",  addroutes_h_get, NULL,
        "PUT",    "/c",  addroutes_h_put, &marker,
        "DELETE", "/d",  addroutes_h_del, &marker,
        NULL);
    test_check(rc == AXL_OK, "add_routes: 4-route batch returns AXL_OK");

    /* Empty list (just sentinel) is a no-op success — same shape as
       add_route never being called. */
    rc = axl_http_server_add_routes(s, NULL);
    test_check(rc == AXL_OK, "add_routes: empty list returns AXL_OK");

    axl_http_server_free(s);
}

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
// AxlNetOpts shape tests (struct layout + sentinel + selector bits)
// ---------------------------------------------------------------------------

static void
test_net_opts_validation(void)
{
    /* AXL_NET_NIC_AUTO sentinel value is documented as (uint64_t)-1.
       Pin it so future churn doesn't silently change the wire-level
       sentinel a consumer might have hard-coded. */
    test_check(AXL_NET_NIC_AUTO == (uint64_t)-1,
               "net-opts: AXL_NET_NIC_AUTO == (uint64_t)-1");

    /* port is uint16_t — keeps AXL_CFG_UINT auto-apply honest and
       documents the on-wire 16-bit reality of TCP/UDP ports. */
    AxlNetOpts probe = { 0 };
    test_check(sizeof(probe.port) == sizeof(uint16_t),
               "net-opts: port field is uint16_t");

    /* Single local_ip field replaces the v1 source/listen split.
       Both SOURCE_IP and LISTEN_IP enum bits target this same
       field offset (verified by the descs_net tests in
       axl-test-util.c) — same bind(2) operation, different
       CLI vocabulary. */
    test_check(sizeof(probe.local_ip) == sizeof(const char *),
               "net-opts: local_ip is a const char *");

    /* Group-selector masks: SERVER and CLIENT compose distinct
       conventional flag sets, neither including STATIC_IP (dropped
       in v2 — IP-config is the firmware ifconfig layer's job). */
    test_check(AXL_NET_OPT_CLIENT ==
                   (AXL_NET_OPT_NIC | AXL_NET_OPT_SOURCE_IP),
               "net-opts: CLIENT mask = NIC | SOURCE_IP");
    test_check(AXL_NET_OPT_SERVER ==
                   (AXL_NET_OPT_NIC | AXL_NET_OPT_PORT
                                    | AXL_NET_OPT_LISTEN_IP),
               "net-opts: SERVER mask = NIC | PORT | LISTEN_IP");

    /* NULL opts → AXL_ERR (no crash, no bring-up). */
    test_check(axl_net_init_from_opts(NULL, 10) == AXL_ERR,
               "net-opts: NULL opts -> AXL_ERR");
}

static void
test_net_resolve_ptr_validation(void)
{
    /* Safe negatives only — these return on the input guard before any DNS4
       protocol call, so they're deterministic with no network. The positive
       PTR round-trip is exercised in the tap+dnsmasq integration harness
       (a reverse zone is required, which SLIRP can't deterministically
       provide). */
    char buf[64];
    AxlIPv4Address ip = { { 8, 8, 8, 8 } };

    test_check(axl_net_resolve_ptr(NULL, buf, sizeof(buf)) == AXL_ERR,
               "resolve_ptr: NULL ip -> AXL_ERR");
    test_check(axl_net_resolve_ptr(&ip, NULL, sizeof(buf)) == AXL_ERR,
               "resolve_ptr: NULL out -> AXL_ERR");
    test_check(axl_net_resolve_ptr(&ip, buf, 0) == AXL_ERR,
               "resolve_ptr: zero cap -> AXL_ERR");

    /* get_dhcp_lease: NULL out returns on the guard before any protocol
       call. The live lease read is exercised in net-diag mode. */
    test_check(axl_net_get_dhcp_lease(0, NULL) == AXL_ERR,
               "dhcp-lease: NULL out -> AXL_ERR");

    /* ping_ex: NULL target / out return on the guard before any IP4 call.
       The live probe (end-to-end IP4 setup/transmit/timeout) is exercised in
       net-diag mode; SLIRP drops ICMP so it can only assert the probe COMPLETES
       with NO_REPLY. ECHO_REPLY + responder/RTT and the TIME_EXCEEDED /
       FRAG_NEEDED reply classification are real-hardware only. */
    AxlPingResult pr;
    test_check(axl_net_ping_ex(NULL, 1000, 64, false, 0, &pr) == AXL_ERR,
               "ping_ex: NULL target -> AXL_ERR");
    test_check(axl_net_ping_ex(&ip, 1000, 64, false, 0, NULL) == AXL_ERR,
               "ping_ex: NULL out -> AXL_ERR");

    /* sntp_query: NULL server / out return on the guard before any UDP call.
       The live round-trip against a host SNTP responder is in test-sntp-qemu.sh. */
    AxlSntpResult sr;
    test_check(axl_sntp_query(NULL, 0, 1000, &sr) == AXL_ERR,
               "sntp_query: NULL server -> AXL_ERR");
    test_check(axl_sntp_query("10.0.2.2", 0, 1000, NULL) == AXL_ERR,
               "sntp_query: NULL out -> AXL_ERR");

    /* arp_list: NULL count returns on the guard before any protocol call. The
       live cache read is exercised in net-diag mode. */
    AxlArpEntry ae[2];
    test_check(axl_net_arp_list(0, ae, 2, NULL) == AXL_ERR,
               "arp_list: NULL count -> AXL_ERR");

    /* get_link_stats: NULL out returns on the guard. The live read is in
       net-diag mode. */
    test_check(axl_net_get_link_stats(0, NULL) == AXL_ERR,
               "link_stats: NULL out -> AXL_ERR");
}

static void
test_ws_conn_api_validation(void)
{
    /* Safe negatives for the per-connection WebSocket API — each returns on
       its NULL guard before touching any connection state. The live
       per-client send + auth-on-upgrade behavior is exercised by the
       /ws-echo-ex and /ws-auth endpoints in test-http.sh. */
    char         payload[4] = "x";
    AxlAuthInfo  info;
    uint8_t      peer[4];

    test_check(axl_http_server_add_websocket_ex(NULL, "/x", NULL, NULL,
                                                AXL_ROUTE_NO_AUTH) == AXL_ERR,
               "ws_ex: NULL server -> AXL_ERR");
    test_check(axl_ws_send(NULL, AXL_WS_TEXT, payload, 1) == AXL_ERR,
               "ws_send: NULL conn -> AXL_ERR");
    test_check(axl_ws_conn_auth(NULL, &info) == AXL_ERR,
               "ws_conn_auth: NULL conn -> AXL_ERR");
    test_check(axl_ws_conn_peer(NULL, peer) == AXL_ERR,
               "ws_conn_peer: NULL conn -> AXL_ERR");
    test_check(axl_ws_conn_close(NULL) == AXL_ERR,
               "ws_conn_close: NULL conn -> AXL_ERR");
    test_check(axl_ws_conn_user_data(NULL) == NULL,
               "ws_conn_user_data: NULL conn -> NULL");
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
    test_check(axl_ipv4_parse("192.168.1.1", octets) == AXL_OK, "ipv4 parse: 192.168.1.1");
    test_check(octets[0] == 192 && octets[1] == 168 &&
               octets[2] == 1 && octets[3] == 1, "ipv4 parse: octets correct");

    test_check(axl_ipv4_parse("0.0.0.0", octets) == AXL_OK, "ipv4 parse: 0.0.0.0");
    test_check(octets[0] == 0 && octets[3] == 0, "ipv4 parse: all zeros");

    test_check(axl_ipv4_parse("255.255.255.255", octets) == AXL_OK, "ipv4 parse: 255.255.255.255");
    test_check(octets[0] == 255 && octets[3] == 255, "ipv4 parse: all 255");

    test_check(axl_ipv4_parse("10.0.0.1", octets) == AXL_OK, "ipv4 parse: 10.0.0.1");

    /* Parse invalid addresses */
    test_check(axl_ipv4_parse("256.0.0.1", octets) == AXL_ERR, "ipv4 parse: 256 octet");
    test_check(axl_ipv4_parse("1.2.3", octets) == AXL_ERR, "ipv4 parse: 3 octets");
    test_check(axl_ipv4_parse("abc", octets) == AXL_ERR, "ipv4 parse: letters");
    test_check(axl_ipv4_parse("", octets) == AXL_ERR, "ipv4 parse: empty");
    test_check(axl_ipv4_parse(NULL, octets) == AXL_ERR, "ipv4 parse: NULL str");
    test_check(axl_ipv4_parse("1.2.3.4", NULL) == AXL_ERR, "ipv4 parse: NULL octets");
    test_check(axl_ipv4_parse("1.2.3.4.5", octets) == AXL_ERR, "ipv4 parse: 5 octets");

    /* Format */
    uint8_t addr[] = { 10, 0, 0, 1 };
    test_check(axl_ipv4_format(addr, buf, sizeof(buf)) == AXL_OK, "ipv4 format: 10.0.0.1");
    test_check(axl_strcmp(buf, "10.0.0.1") == 0, "ipv4 format: string correct");

    /* Format roundtrip */
    axl_ipv4_parse("192.168.100.200", octets);
    axl_ipv4_format(octets, buf, sizeof(buf));
    test_check(axl_strcmp(buf, "192.168.100.200") == 0, "ipv4 format: roundtrip");

    /* Format small buffer */
    test_check(axl_ipv4_format(addr, buf, 4) == AXL_ERR, "ipv4 format: small buffer");

    /* Format NULL safety */
    test_check(axl_ipv4_format(NULL, buf, sizeof(buf)) == AXL_ERR, "ipv4 format: NULL octets");
    test_check(axl_ipv4_format(addr, NULL, 16) == AXL_ERR, "ipv4 format: NULL buf");

    /* IPv6 format — full address, no zero collapsing. */
    uint8_t v6_full[16] = {
        0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x01,
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    };
    char v6_buf[40];
    test_check(axl_ipv6_format(v6_full, v6_buf, sizeof(v6_buf)) == AXL_OK,
               "ipv6 format: 2001:db8 full");
    test_check(axl_strcmp(v6_buf, "2001:db8:85a3:1:1234:5678:9abc:def0") == 0,
               "ipv6 format: leading-zero suppression");

    /* IPv6 format — zero-run collapse `::`. */
    uint8_t v6_loopback[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    test_check(axl_ipv6_format(v6_loopback, v6_buf, sizeof(v6_buf)) == AXL_OK
               && axl_strcmp(v6_buf, "::1") == 0,
               "ipv6 format: ::1 loopback");

    uint8_t v6_unspec[16] = { 0 };
    test_check(axl_ipv6_format(v6_unspec, v6_buf, sizeof(v6_buf)) == AXL_OK
               && axl_strcmp(v6_buf, "::") == 0,
               "ipv6 format: :: unspecified");

    /* IPv6 format — single zero group is NOT collapsed (RFC 5952). */
    uint8_t v6_single_zero[16] = {
        0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    test_check(axl_ipv6_format(v6_single_zero, v6_buf, sizeof(v6_buf)) == AXL_OK
               && axl_strcmp(v6_buf, "2001:db8:0:1::1") == 0,
               "ipv6 format: longest-run collapse");

    /* IPv6 format — small buffer + NULL safety. */
    test_check(axl_ipv6_format(v6_full, v6_buf, 8) == AXL_ERR,
               "ipv6 format: small buffer");
    test_check(axl_ipv6_format(NULL, v6_buf, sizeof(v6_buf)) == AXL_ERR,
               "ipv6 format: NULL octets");
    test_check(axl_ipv6_format(v6_full, NULL, sizeof(v6_buf)) == AXL_ERR,
               "ipv6 format: NULL buf");

    /* axl_ipv4_equals — pure compare. */
    uint8_t a[4] = { 10, 0, 0, 1 };
    uint8_t b[4] = { 10, 0, 0, 1 };
    uint8_t c[4] = { 10, 0, 0, 2 };
    test_check(axl_ipv4_equals(a, b),  "ipv4_equals: identical");
    test_check(!axl_ipv4_equals(a, c), "ipv4_equals: differ in last");
    test_check(!axl_ipv4_equals(NULL, b), "ipv4_equals: NULL a is false");
    test_check(!axl_ipv4_equals(a, NULL), "ipv4_equals: NULL b is false");

    /* axl_ipv4_in_subnet — routing-decision predicate.
       The function rejects a 0.0.0.0 mask explicitly so an unconfigured
       interface (UEFI brings link UP before IP4Config2 policy applies)
       doesn't falsely match every destination. */
    uint8_t station[4] = { 169, 254, 1, 2 };
    uint8_t mask24[4]  = { 255, 255, 255, 0 };
    uint8_t same_subnet[4]  = { 169, 254, 1, 99 };
    uint8_t other_subnet[4] = { 10, 0, 0, 1 };
    test_check(axl_ipv4_in_subnet(same_subnet, station, mask24),
               "ipv4_in_subnet: same /24");
    test_check(!axl_ipv4_in_subnet(other_subnet, station, mask24),
               "ipv4_in_subnet: other /24");

    uint8_t mask23[4] = { 255, 255, 254, 0 };
    uint8_t station_23[4] = { 10, 9, 176, 1 };
    uint8_t in_23[4]      = { 10, 9, 177, 250 };
    uint8_t out_23[4]     = { 10, 9, 178, 1 };
    test_check(axl_ipv4_in_subnet(in_23, station_23, mask23),
               "ipv4_in_subnet: /23 includes second half");
    test_check(!axl_ipv4_in_subnet(out_23, station_23, mask23),
               "ipv4_in_subnet: /23 excludes outside");

    uint8_t zero_mask[4] = { 0, 0, 0, 0 };
    test_check(!axl_ipv4_in_subnet(same_subnet, station, zero_mask),
               "ipv4_in_subnet: zero mask rejected (no policy yet)");

    test_check(!axl_ipv4_in_subnet(NULL, station, mask24),
               "ipv4_in_subnet: NULL dest is false");
    test_check(!axl_ipv4_in_subnet(same_subnet, NULL, mask24),
               "ipv4_in_subnet: NULL station is false");
    test_check(!axl_ipv4_in_subnet(same_subnet, station, NULL),
               "ipv4_in_subnet: NULL mask is false");
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

    if (axl_ipv4_parse(host, octets) != AXL_OK) {
        axl_printf("UDP-ECHO: invalid host '%s'\n", host);
        return 1;
    }
    axl_memcpy(&dest, octets, 4);

    uint16_t port;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0 || port == 0) {
        axl_printf("UDP-ECHO: invalid port '%s'\n", port_str);
        return 1;
    }

    /* Init networking */
    axl_net_auto_init(SIZE_MAX, 10);

    AxlUdp *sock = NULL;
    if (axl_udp_open(&sock, 0) != AXL_OK) {
        axl_printf("UDP-ECHO: failed to open socket\n");
        return 1;
    }

    /* Verify the ephemeral-port readback. axl_udp_open(&s, 0) lets
       UEFI pick a port; without get_local_addr we had no way to
       read it. Print the result for the integration test to grep. */
    char     local_addr[16] = {0};
    uint16_t local_port     = 0;
    if (axl_udp_get_local_addr(sock, local_addr, sizeof(local_addr),
                               &local_port) == AXL_OK && local_port != 0)
    {
        axl_printf("PASS: udp-get-local-addr (bound %s:%u)\n",
                   local_addr, (unsigned)local_port);
    } else {
        axl_printf("FAIL: udp-get-local-addr (port=%u)\n",
                   (unsigned)local_port);
    }

    const char *msg = "hello from UEFI";
    char rx_buf[256];
    size_t rx_len = 0;

    axl_printf("UDP-ECHO: sending to %s:%u\n", host, (unsigned)port);

    int rc = axl_udp_sendrecv(sock, &dest, port,
                              msg, axl_strlen(msg),
                              rx_buf, sizeof(rx_buf) - 1, &rx_len,
                              5000);

    if (rc == AXL_OK && rx_len > 0) {
        rx_buf[rx_len] = '\0';
        axl_printf("UDP-ECHO: received %zu bytes: %s\n", rx_len, rx_buf);
        axl_printf("PASS: udp-echo-response\n");
    } else {
        axl_printf("UDP-ECHO: no response (rc=%d)\n", rc);
        axl_printf("FAIL: udp-echo-response\n");
    }

    axl_udp_close(sock);
    return (rc == AXL_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Raised-TPL TCP mode — "tcp-connect-rtpl <host> <port>" does a sync TCP
// connect + send + recv against a host echo server while at TPL_CALLBACK
// (the level a sync TCP op reaches when called from inside a driver-pump
// notify). Before the tcp4->Poll() tick landed in the sync TCP wrappers, the
// loop's raised-TPL CheckEvent spin held TPL_CALLBACK and starved the TCP4
// firmware notify, so the connect never advanced — it stalled to its full
// timeout and FAILED. With the tick it advances and connects promptly, so the
// return value cleanly separates fixed (AXL_OK) from broken (AXL_ERR/timeout).
// Used by test-tcp-connect-rtpl-qemu.sh.
// ---------------------------------------------------------------------------

static int
run_tcp_connect_rtpl_mode(const char *host, const char *port_str)
{
    uint16_t port;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0 || port == 0) {
        axl_printf("TCP-RTPL: invalid port '%s'\n", port_str);
        return 1;
    }

    /* Bring the stack up at TPL_APPLICATION before raising. */
    axl_net_auto_init(SIZE_MAX, 10);

    const char *msg = "hello from UEFI";
    char        rx_buf[256];
    size_t      rx_len = sizeof(rx_buf) - 1;

    axl_printf("TCP-RTPL: connecting to %s:%u at TPL_CALLBACK\n",
               host, (unsigned)port);

    /* Everything below runs at TPL_CALLBACK — the condition a sync TCP op
       hits inside a driver-pump notify. The 4 s connect timeout means a
       BROKEN build takes ~4 s here then fails; the fix returns in well
       under that. */
    EFI_TPL old = gBS->RaiseTPL(TPL_CALLBACK);

    AxlTcp *sock = NULL;
    int crc = axl_tcp_connect_timeout(host, port, NULL, 4000, &sock);

    int src = AXL_ERR, rrc = AXL_ERR;
    if (crc == AXL_OK && sock != NULL) {
        src = axl_tcp_send(sock, msg, axl_strlen(msg), 4000);
        rrc = axl_tcp_recv(sock, rx_buf, &rx_len, 4000);
    }

    gBS->RestoreTPL(old);

    /* The headline assertion: the connect advanced at raised TPL. */
    if (crc == AXL_OK) {
        axl_printf("PASS: tcp-connect-raised-tpl\n");
    } else {
        axl_printf("FAIL: tcp-connect-raised-tpl (rc=%d)\n", crc);
    }

    /* Bonus: send + recv at raised TPL too (exercises those wrappers' ticks).
       The host echo server replies "ECHO:<msg>". */
    if (crc == AXL_OK && src == AXL_OK && rrc == AXL_OK && rx_len > 0) {
        rx_buf[rx_len] = '\0';
        axl_printf("TCP-RTPL: echo '%s'\n", rx_buf);
        axl_printf("PASS: tcp-roundtrip-raised-tpl\n");
    } else {
        axl_printf("FAIL: tcp-roundtrip-raised-tpl (c=%d s=%d r=%d len=%zu)\n",
                   crc, src, rrc, rx_len);
    }

    if (sock != NULL) {
        axl_tcp_close(sock);
    }
    return (crc == AXL_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Async HTTP client mode — "get-async <url>" / "post-async <url> <body>".
//
// The faithful SoftBMC-webhook topology: the async request is ISSUED from
// inside a raised-TPL driver-pump tick (a one-shot loop timer kicks it; the
// axl_loop_attach_driver pump dispatches that timer at TPL_CALLBACK), and the
// whole request — resolve, connect, [TLS handshake], send, recv, redirects —
// runs as events on that ONE loop with NO nested ephemeral loop. The sync
// axl_http_get/post would nest a loop here and emit the "synchronous wait
// invoked from inside a loop callback" warning; the async path must not.
// Driven by test-http-async-qemu.sh, which asserts the body arrives via the
// callback AND that no such warning appears. https adds axl_tls_init().
// ---------------------------------------------------------------------------

typedef struct {
    AxlHttpClient *client;
    AxlLoop       *loop;
    const char    *method;
    const char    *url;
    const char    *body;
    size_t         body_len;
    bool           done;
} HttpAsyncMode;

static void
http_async_mode_done(AxlHttpClientResponse *resp, AxlStatus st, void *user)
{
    HttpAsyncMode *m = (HttpAsyncMode *)user;

    if (st == AXL_OK && resp != NULL) {
        axl_printf("ASYNC-STATUS: %u\n", (unsigned)resp->status_code);
        if (resp->body != NULL && resp->body_size > 0) {
            /* The body is not guaranteed NUL-terminated — bound the print. */
            axl_printf("ASYNC-BODY: %.*s\n",
                       (int)resp->body_size, (const char *)resp->body);
        } else {
            axl_printf("ASYNC-BODY: <empty>\n");
        }
        axl_http_client_response_free(resp);
        axl_printf("PASS: http-async-%s\n", m->method);
    } else {
        axl_printf("ASYNC-RC: %d\n", st);
        axl_printf("FAIL: http-async-%s\n", m->method);
    }
    m->done = true;
}

/* One-shot timer callback (dispatched at raised TPL by the driver pump) that
   initiates the async request — mirroring a webhook fired from a timer. */
static bool
http_async_mode_kick(void *user)
{
    HttpAsyncMode *m = (HttpAsyncMode *)user;
    int rc;

    if (axl_strcmp(m->method, "POST") == 0) {
        rc = axl_http_post_async(m->client, m->loop, m->url,
                                 m->body, m->body_len, "text/plain",
                                 NULL, http_async_mode_done, m);
    } else {
        rc = axl_http_get_async(m->client, m->loop, m->url,
                                NULL, http_async_mode_done, m);
    }
    if (rc != AXL_OK) {
        axl_printf("ASYNC-INIT-RC: %d\n", rc);
        axl_printf("FAIL: http-async-%s\n", m->method);
        m->done = true;
    }
    return AXL_SOURCE_REMOVE;
}

static int
run_http_async_mode(const char *method, const char *url, const char *body)
{
    axl_net_auto_init(SIZE_MAX, 10);

    bool https = axl_strncmp(url, "https://", 8) == 0;
    if (https) {
        if (!axl_tls_available() || axl_tls_init() != AXL_OK) {
            axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
            return 1;
        }
    }

    AxlLoop       *loop = axl_loop_new();
    AxlHttpClient *c    = axl_http_client_new();
    if (loop == NULL || c == NULL) {
        axl_printf("ERROR: loop/client alloc failed\n");
        return 1;
    }
    /* Self-signed host cert in the integration test — skip chain validation. */
    axl_http_client_set(c, "tls.verify", "false");

    HttpAsyncMode m = {
        .client   = c,
        .loop     = loop,
        .method   = method,
        .url      = url,
        .body     = body,
        .body_len = (body != NULL) ? axl_strlen(body) : 0,
        .done     = false,
    };

    /* Kick the request from a raised-TPL tick (see the header comment). */
    axl_loop_add_timeout(loop, 300, http_async_mode_kick, &m);
    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return 1;
    }

    axl_printf("READY\n");

    /* Idle in the foreground; the driver pump advances the request. Bound the
       wait (~30 s) so a wedge fails loudly instead of hanging the harness. */
    for (int i = 0; i < 600 && !m.done; i++) {
        axl_msleep(50);
    }
    if (!m.done) {
        axl_printf("FAIL: http-async-timeout\n");
    }

    /* Drain before teardown — modeling a real reactive consumer. The response
       arrived via the callback, but a Connection: close response left an
       in-flight async tcp4 Close registered on this loop; it finalizes on a
       LATER pump tick (the firmware TCP4 timer runs while the pump yields TPL
       between ticks — axl-tcp-sync.c's documented async-close contract). Tearing
       the loop down the instant the response lands would free it with that close
       still pending (the "caller-owned event source still active" warning + a
       socket/close-ctx leak). A continuously-running service never hits this; a
       one-shot must let the loop idle until the close drains. */
    for (int i = 0; i < 20; i++) {
        axl_msleep(50);
    }

    axl_loop_detach_driver(loop);
    axl_http_client_free(c);
    axl_loop_free(loop);
    return m.done ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Raised-TPL SYNC GET — "get-sync-rtpl <url>". Phase 3 reimplemented the sync
// axl_http_get as an ephemeral-loop wrapper over the async core; a blocking
// loop at a raised TPL starves the TCP4 firmware notify, so the wrapper arms a
// Poll tick over the client's current socket. This runs axl_http_get at
// TPL_CALLBACK (the level a sync call reaches inside a driver-pump notify) and
// asserts the body still arrives — i.e. the Poll tick drives it. Without the
// tick the request would stall to the deadline and the body never prints.
// ---------------------------------------------------------------------------

static int
run_get_sync_rtpl_mode(const char *url)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlHttpClient *c = axl_http_client_new();
    if (c == NULL) {
        axl_printf("ERROR: client alloc failed\n");
        return 1;
    }
    /* Bound the op so a BROKEN build (no Poll progress) fails in a few seconds
       rather than hanging the harness. */
    axl_http_client_set(c, "timeout.ms", "8000");

    axl_printf("RTPL-SYNC: GET %s at TPL_CALLBACK\n", url);

    EFI_TPL old = gBS->RaiseTPL(TPL_CALLBACK);
    AxlHttpClientResponse *resp = NULL;
    int rc = axl_http_get(c, url, &resp);
    gBS->RestoreTPL(old);

    if (rc == AXL_OK && resp != NULL) {
        axl_printf("RTPL-SYNC-STATUS: %u\n", (unsigned)resp->status_code);
        if (resp->body != NULL && resp->body_size > 0) {
            axl_printf("RTPL-SYNC-BODY: %.*s\n",
                       (int)resp->body_size, (const char *)resp->body);
        }
        axl_http_client_response_free(resp);
        axl_printf("PASS: http-get-sync-rtpl\n");
    } else {
        axl_printf("FAIL: http-get-sync-rtpl (rc=%d)\n", rc);
    }

    axl_http_client_free(c);
    return (rc == AXL_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Large single-GET mode — "get-size <url>". Reproduces the v2.0.0 regression
// (gBS->LoadImage of a ~1 MB .efi over an AxlFsProvider-over-HTTP volume hung):
// the async core capped a Content-Length response body at 1 MiB. This sync
// axl_http_get of the whole body in ONE call, at TPL_CALLBACK (the consumer's
// EFI_FILE_PROTOCOL.Read / LoadImage context), must return the FULL body. The
// host /large endpoint serves a deterministic body (byte i == i & 0xFF), so we
// verify size AND content (catches truncation/corruption, not just the cap).
// ---------------------------------------------------------------------------

static int
run_get_size_mode(const char *url)
{
    axl_net_auto_init(SIZE_MAX, 10);

    if (axl_strncmp(url, "https://", 8) == 0) {
        if (!axl_tls_available() || axl_tls_init() != AXL_OK) {
            axl_printf("ERROR: TLS not available (build with AXL_TLS=1)\n");
            return 1;
        }
    }

    AxlHttpClient *c = axl_http_client_new();
    if (c == NULL) {
        axl_printf("ERROR: client alloc failed\n");
        return 1;
    }
    /* Generous bound — a large body legitimately takes longer, but a wedge
       still fails the harness in time rather than hanging it. */
    axl_http_client_set(c, "timeout.ms", "20000");

    axl_printf("GET-SIZE: GET %s at TPL_CALLBACK\n", url);

    EFI_TPL old = gBS->RaiseTPL(TPL_CALLBACK);
    AxlHttpClientResponse *resp = NULL;
    int rc = axl_http_get(c, url, &resp);
    gBS->RestoreTPL(old);

    int ret = 1;
    if (rc == AXL_OK && resp != NULL) {
        size_t n = resp->body_size;
        const uint8_t *b = (const uint8_t *)resp->body;
        size_t bad = (size_t)-1;
        for (size_t i = 0; i < n; i++) {
            if (b[i] != (uint8_t)(i & 0xFF)) {
                bad = i;
                break;
            }
        }
        axl_printf("GET-SIZE-STATUS: %u\n", (unsigned)resp->status_code);
        axl_printf("GET-SIZE-BYTES: %zu\n", n);
        if (bad == (size_t)-1) {
            axl_printf("GET-SIZE-VERIFY: OK\n");
            axl_printf("PASS: http-get-size\n");
            ret = 0;
        } else {
            axl_printf("GET-SIZE-VERIFY: CORRUPT@%zu\n", bad);
            axl_printf("FAIL: http-get-size (corrupt)\n");
        }
        axl_http_client_response_free(resp);
    } else {
        axl_printf("GET-SIZE-RC: %d\n", rc);
        axl_printf("FAIL: http-get-size (rc=%d)\n", rc);
    }

    axl_http_client_free(c);
    return ret;
}

// ---------------------------------------------------------------------------
// sntp-query mode — query a host-side SNTP responder (test-sntp-qemu.sh) and
// print the result for the harness to grep. The mock responder returns a fixed
// known Unix time, so unix_secs is deterministic.
// ---------------------------------------------------------------------------

static int
run_sntp_query_mode(const char *host, const char *port_str)
{
    uint16_t port = 0;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0) {
        axl_printf("SNTP: invalid port '%s'\n", port_str);
        return 1;
    }

    axl_net_auto_init(SIZE_MAX, 10);

    AxlSntpResult r;
    int rc = axl_sntp_query(host, port, 5000, &r);
    axl_printf("SNTP: rc=%d reachable=%d unix_secs=%lld offset_ms=%d\n",
               rc, (int)r.reachable, (long long)r.unix_secs, r.offset_ms);
    if (rc == AXL_OK && r.reachable) {
        axl_printf("PASS: sntp-reachable\n");
        axl_printf("PASS: sntp-unix-secs=%lld\n", (long long)r.unix_secs);
    } else {
        axl_printf("FAIL: sntp-query (rc=%d)\n", rc);
    }
    return (rc == AXL_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// net-diag mode — exercise the DHCP-lease / reverse-DNS primitives against a
// live (DHCP'd) network. Driven by test-netdiag-qemu.sh, which greps the
// PASS:/FAIL: lines. SLIRP hands out a deterministic lease (10.0.2.15, gw/dns
// 10.0.2.2/10.0.2.3), so the values are assertable; renew/release land here
// next over a tap+dnsmasq server.
// ---------------------------------------------------------------------------

static int
run_net_diag_mode(void)
{
    int nd_pass = 0;
    int nd_fail = 0;
#define ND_CHECK(cond, name)                                  \
    do {                                                      \
        if (cond) { axl_printf("PASS: %s\r\n", (name)); nd_pass++; } \
        else      { axl_printf("FAIL: %s\r\n", (name)); nd_fail++; } \
    } while (0)

    axl_printf("=== net-diag mode ===\r\n");

    /* The startup script ran DHCP already; make sure the stack is up for a
       direct boot too (no-op if an address is already configured). */
    axl_net_auto_init(0, 10);

    AxlDhcpLease lease;
    int rc = axl_net_get_dhcp_lease(0, &lease);
    ND_CHECK(rc == AXL_OK, "dhcp-lease: get_dhcp_lease returns AXL_OK");
    if (rc == AXL_OK) {
        axl_printf("  lease: addr=%u.%u.%u.%u subnet=%u.%u.%u.%u "
                   "router=%u.%u.%u.%u dns_count=%u dns0=%u.%u.%u.%u\r\n",
                   lease.address[0], lease.address[1], lease.address[2], lease.address[3],
                   lease.subnet[0], lease.subnet[1], lease.subnet[2], lease.subnet[3],
                   lease.router[0], lease.router[1], lease.router[2], lease.router[3],
                   (unsigned)lease.dns_count,
                   lease.dns[0][0], lease.dns[0][1], lease.dns[0][2], lease.dns[0][3]);

        static const uint8_t exp_addr[4]   = { 10, 0, 2, 15 };
        static const uint8_t exp_router[4] = { 10, 0, 2, 2 };
        ND_CHECK(axl_memcmp(lease.address, exp_addr, 4) == 0,
                 "dhcp-lease: address == 10.0.2.15 (SLIRP lease)");
        ND_CHECK(axl_memcmp(lease.router, exp_router, 4) == 0,
                 "dhcp-lease: router == 10.0.2.2 (SLIRP gateway)");
        ND_CHECK(lease.dns_count >= 1,
                 "dhcp-lease: at least one DNS resolver");
    }

    /* ping_ex end-to-end over the live IP4 stack. SLIRP is a NAT that does NOT
       answer ICMP echo (verified: even the gateway never replies), so under
       QEMU every probe times out — the deterministic invariants are that the
       probe COMPLETES cleanly (AXL_OK, no hang/crash through the IP4
       setup/transmit/timeout path) and classifies as NO_REPLY. On real
       hardware the gateway probe would be ECHO_REPLY (with responder + RTT)
       and a real router/MTU bottleneck yields TIME_EXCEEDED / FRAG_NEEDED;
       those are not reproducible here. The gateway check accepts ECHO_REPLY
       too, so it stays correct if a future SLIRP gains ICMP support. */
    AxlIPv4Address gw = { { 10, 0, 2, 2 } };
    AxlPingResult  pr;
    int prc = axl_net_ping_ex(&gw, 3000, 64, false, 0, &pr);
    ND_CHECK(prc == AXL_OK, "ping_ex: probe completes (AXL_OK)");
    ND_CHECK(prc == AXL_OK
             && (pr.reply == AXL_PING_NO_REPLY
                 || pr.reply == AXL_PING_ECHO_REPLY),
             "ping_ex: gateway probe is NO_REPLY (SLIRP) or ECHO_REPLY (real ICMP)");

    /* DF + a large payload still completes cleanly (path-MTU probe shape). */
    AxlPingResult  prdf;
    int prcdf = axl_net_ping_ex(&gw, 1500, 64, true, 1400, &prdf);
    ND_CHECK(prcdf == AXL_OK,
             "ping_ex: DF + large payload probe completes (AXL_OK)");

    /* An unreachable host inside the subnet: nothing answers, on SLIRP or real
       hardware — the probe completes with NO_REPLY. */
    AxlIPv4Address dead = { { 10, 0, 2, 250 } };
    AxlPingResult  pr2;
    int prc2 = axl_net_ping_ex(&dead, 1500, 64, false, 0, &pr2);
    ND_CHECK(prc2 == AXL_OK && pr2.reply == AXL_PING_NO_REPLY,
             "ping_ex: unreachable host -> NO_REPLY (probe still completes)");

    /* ARP neighbor cache. The call must succeed; the count is best-effort
       (the cache only holds neighbors the firmware actually resolved). After
       DHCP the gateway is typically present. */
    AxlArpEntry arp[16];
    size_t      arp_n = 0;
    int arc = axl_net_arp_list(0, arp, 16, &arp_n);
    ND_CHECK(arc == AXL_OK, "arp-list: returns AXL_OK");
    axl_printf("  arp: %zu entries\r\n", arp_n);

    /* After DHCP the guest has resolved the gateway, so the shared ARP cache
       holds 10.0.2.2 with a non-zero MAC — proves the cache READ, not just
       that the call returns. */
    static const uint8_t gw_ip[4] = { 10, 0, 2, 2 };
    bool arp_has_gw = false;
    for (size_t i = 0; i < arp_n && i < 16; i++) {
        axl_printf("  arp[%zu]: %u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                   i, arp[i].ip[0], arp[i].ip[1], arp[i].ip[2], arp[i].ip[3],
                   arp[i].mac[0], arp[i].mac[1], arp[i].mac[2],
                   arp[i].mac[3], arp[i].mac[4], arp[i].mac[5]);
        bool mac_nz = (arp[i].mac[0] | arp[i].mac[1] | arp[i].mac[2]
                       | arp[i].mac[3] | arp[i].mac[4] | arp[i].mac[5]) != 0;
        if (axl_memcmp(arp[i].ip, gw_ip, 4) == 0 && mac_nz) {
            arp_has_gw = true;
        }
    }
    ND_CHECK(arp_has_gw, "arp-list: gateway 10.0.2.2 resolved in the cache");

    /* Link stats. link_up is authoritative (the QEMU NIC's media is present);
       speed/duplex/autoneg have no portable UEFI source and read as unknown. */
    AxlNetLinkStats ls;
    int lsrc = axl_net_get_link_stats(0, &ls);
    ND_CHECK(lsrc == AXL_OK, "link-stats: returns AXL_OK");
    ND_CHECK(lsrc == AXL_OK && ls.link_up, "link-stats: link is up");
    axl_printf("  link: up=%d speed_bps=%llu duplex=%u autoneg=%d\r\n",
               (int)ls.link_up, (unsigned long long)ls.speed_bps,
               (unsigned)ls.duplex, (int)ls.autoneg);

    axl_printf("=== net-diag Results: %d passed, %d failed ===\r\n",
               nd_pass, nd_fail);
#undef ND_CHECK
    return (nd_fail == 0) ? 0 : 1;
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
    /* Connect via the AxlSocketClient API to the runner-provided
       echo backend. See test_tcp_echo for the rationale. */
    AxlSocket *sock = NULL;
    AxlSocketClient *client;
    int ret;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket_client connect (no network)\n");
        return;
    }

    client = axl_socket_client_new();
    test_check(client != NULL, "socket_client: new");

    ret = axl_socket_client_connect_to_host(client,
        AXL_TEST_ECHO_HOST, 9994, &sock);
    test_check(ret == 0, "socket_client: connect_to_host");
    if (ret == 0) {
        test_check(axl_socket_get_type(sock) == AXL_SOCKET_STREAM,
                   "socket_client: type is stream");
        axl_socket_free(sock);
    }

    axl_socket_client_free(client);
}

static void
test_socket_client_invalid(void)
{
    AxlSocketClient *client = axl_socket_client_new();
    AxlSocket *sock = NULL;

    /* NULL host */
    test_check(axl_socket_client_connect_to_host(client, NULL, 80, &sock) == AXL_ERR,
               "socket_client: NULL host");

    /* NULL out_sock */
    test_check(axl_socket_client_connect_to_host(client, "10.0.0.1", 80, NULL) == AXL_ERR,
               "socket_client: NULL out_sock");

    axl_socket_client_free(client);
}

// ---------------------------------------------------------------------------
// AxlSocket Tests (network required)
// ---------------------------------------------------------------------------

static void
test_socket_stream_echo(void)
{
    /* Client-only against the runner-provided echo backend.
       See test_tcp_echo for the rationale. */
    AxlSocket *client;
    int    ret;
    char   send_buf[] = "Hello AxlSocket";
    char   recv_buf[64];
    size_t recv_size;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket stream echo (no network)\n");
        return;
    }

    client = axl_socket_new(AXL_SOCKET_STREAM);
    test_check(client != NULL, "socket stream: new");
    if (client == NULL) {
        return;
    }

    AxlSocketAddress *remote = axl_socket_address_new(
        axl_inet_address_new_from_string(AXL_TEST_ECHO_HOST), 9998);
    ret = axl_socket_connect(client, remote);
    axl_socket_address_free(remote);
    test_check(ret == 0, "socket stream: connect");
    if (ret != 0) {
        axl_socket_free(client);
        return;
    }

    ret = axl_socket_send(client, send_buf, axl_strlen(send_buf), 0);
    test_check(ret == 0, "socket stream: send");

    recv_size = sizeof(recv_buf) - 1;
    ret = axl_socket_receive(client, recv_buf, &recv_size, 0);
    test_check(ret == 0, "socket stream: receive");
    if (ret == 0) {
        recv_buf[recv_size] = '\0';
        test_check(axl_strcmp(recv_buf, "Hello AxlSocket") == 0,
                   "socket stream: echo match");
    }

    axl_socket_free(client);
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
    test_check(axl_socket_send(sock, "test", 4, 0) == AXL_ERR,
               "socket datagram: send (stream-only) fails");

    axl_socket_free(sock);
}

static void
test_socket_get_addresses(void)
{
    /* Client-only against the runner-provided echo backend (slirp
       guestfwd to host stream-echo). Verifies that
       axl_socket_get_local_address / _get_remote_address return
       sensible values once a connection is up. */
    AxlSocket *client;
    int ret;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket get_addresses (no network)\n");
        return;
    }

    client = axl_socket_new(AXL_SOCKET_STREAM);
    AxlSocketAddress *remote = axl_socket_address_new(
        axl_inet_address_new_from_string(AXL_TEST_ECHO_HOST), 9996);
    ret = axl_socket_connect(client, remote);
    axl_socket_address_free(remote);

    test_check(ret == 0, "socket get_addresses: connect");
    if (ret != 0) {
        axl_socket_free(client);
        return;
    }

    AxlSocketAddress *local_sa = axl_socket_get_local_address(client);
    test_check(local_sa != NULL, "socket get_local_address: not NULL");
    if (local_sa != NULL) {
        test_check(axl_socket_address_get_port(local_sa) != 0,
                   "socket get_local_address: port non-zero");
        axl_socket_address_free(local_sa);
    }

    AxlSocketAddress *remote_sa = axl_socket_get_remote_address(client);
    test_check(remote_sa != NULL, "socket get_remote_address: not NULL");
    if (remote_sa != NULL) {
        test_check(axl_socket_address_get_port(remote_sa) == 9996,
                   "socket get_remote_address: port 9996");
        axl_socket_address_free(remote_sa);
    }

    axl_socket_free(client);
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
        test_check(axl_socket_send_to(stream, "x", 1, dest) == AXL_ERR,
                   "socket type_errors: send_to on stream fails");
        axl_socket_address_free(dest);

        /* listen on datagram -> error */
        /* receive on unconnected stream -> error */
        size_t sz = 64;
        char buf[64];
        test_check(axl_socket_receive(stream, buf, &sz, 0) == AXL_ERR,
                   "socket type_errors: receive on unconnected fails");

        axl_socket_free(stream);
    }

    /* listen on datagram -> error */
    AxlSocket *dgram = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (dgram != NULL) {
        test_check(axl_socket_listen(dgram, 9995) == AXL_ERR,
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
on_udp_recv_test(AxlSocket *sock, AxlStatus status, void *data)
{
    UdpRecvTestCtx *ctx = (UdpRecvTestCtx *)data;

    ctx->received = (status == AXL_OK);
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
    /* One-sided test against the runner-provided UDP echo. The guest
       sends a datagram to AXL_TEST_UDP_ECHO_HOST:AXL_TEST_UDP_ECHO_PORT
       (slirp's gateway IP at a host-side echo port); slirp delivers
       it to the host loopback; the host echo replies; slirp NATs the
       reply back to the sender's source port. We arm async recv on
       that same socket and expect the echo to arrive as a callback. */
    AxlSocket *sock;
    char recv_buf[256];
    UdpRecvTestCtx ctx;
    const char *msg = "hello-udp-async";

    if (!axl_net_is_available()) {
        axl_printf("SKIP: socket UDP async recv (no network)\n");
        return;
    }

    sock = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (sock == NULL) {
        axl_printf("SKIP: socket UDP async recv (no UDP)\n");
        return;
    }

    /* Bind ephemerally — needed so the socket has a known local
       port that slirp can NAT the reply back to. */
    if (axl_socket_bind(sock, 9990) != AXL_OK) {
        axl_printf("SKIP: socket UDP async recv (bind 9990 failed)\n");
        axl_socket_free(sock);
        return;
    }

    AxlLoop *loop = axl_loop_new();
    ctx.loop = loop;
    ctx.received = false;
    ctx.recv_len = 0;

    int rc = axl_socket_receive_async(sock, recv_buf,
                                      sizeof(recv_buf) - 1,
                                      loop, on_udp_recv_test, &ctx);
    test_check(rc == AXL_OK, "socket UDP async: receive_async");
    if (rc != AXL_OK) {
        axl_loop_free(loop);
        axl_socket_free(sock);
        return;
    }

    /* Safety timeout — kept generous since the slirp UDP path
       traverses host loopback and back. */
    axl_loop_add_timeout(loop, 3000, on_udp_recv_timeout, &ctx);

    AxlSocketAddress *dest = axl_socket_address_new(
        axl_inet_address_new_from_string(AXL_TEST_UDP_ECHO_HOST),
        AXL_TEST_UDP_ECHO_PORT);
    axl_socket_send_to(sock, msg, axl_strlen(msg), dest);
    axl_socket_address_free(dest);

    axl_loop_run(loop);

    test_check(ctx.received, "socket UDP async: callback fired");
    if (ctx.received) {
        test_check(ctx.recv_len == axl_strlen(msg),
                   "socket UDP async: recv_len correct");
        recv_buf[ctx.recv_len] = '\0';
        test_check(axl_strcmp(recv_buf, msg) == 0,
                   "socket UDP async: data matches");
    }

    /* Free the socket BEFORE the loop — axl_socket_free drives
       axl_udp_close, which calls axl_loop_remove_source on the loop
       the socket was registered with. Freeing the loop first leaves
       the socket holding a dangling loop pointer and a stale source
       id; the subsequent close path would then access freed memory
       and crash inside UDP4 Cancel's token-event access. */
    axl_socket_free(sock);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// AxlUdp send_async — fire-and-forget completion via callback
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop  *loop;
    AxlStatus last_status;
    bool      fired;
} UdpSendCtx;

static bool
on_udp_send_done(AxlUdp *sock, AxlStatus status, void *data)
{
    (void)sock;
    UdpSendCtx *ctx = (UdpSendCtx *)data;
    ctx->last_status = status;
    ctx->fired       = true;
    axl_loop_quit(ctx->loop);
    return true;  /* send is one-shot; return ignored */
}

static bool
on_udp_send_timeout(void *data)
{
    UdpSendCtx *ctx = (UdpSendCtx *)data;
    ctx->fired = false;
    axl_loop_quit(ctx->loop);
    return AXL_SOURCE_REMOVE;
}

static void
test_udp_send_async(void)
{
    /* Send a datagram to localhost on a port nothing is listening
       on. UDP is connectionless — Transmit completes successfully
       at the wire level regardless of whether anyone receives it.
       Test verifies the async-completion machinery (event source +
       callback dispatch + status propagation), not the network
       reachability. */
    AxlUdp *sock;
    AxlIPv4Address dest;
    uint8_t loopback_octets[4] = {127, 0, 0, 1};
    const char *msg = "send-async-test";

    if (!axl_net_is_available()) {
        axl_printf("SKIP: udp send_async (no network)\n");
        return;
    }

    if (axl_udp_open(&sock, 0) != AXL_OK) {
        axl_printf("SKIP: udp send_async (open failed)\n");
        return;
    }
    axl_memcpy(&dest, loopback_octets, 4);

    AxlLoop *loop = axl_loop_new();
    UdpSendCtx ctx = { .loop = loop, .last_status = AXL_ERR, .fired = false };

    int rc = axl_udp_send_async(sock, &dest, 9984,
                                msg, axl_strlen(msg),
                                loop, NULL,
                                on_udp_send_done, &ctx);
    test_check(rc == AXL_OK, "udp send_async: initiate");

    /* Second send while first is in flight must fail. */
    int rc2 = axl_udp_send_async(sock, &dest, 9984,
                                 msg, axl_strlen(msg),
                                 loop, NULL,
                                 on_udp_send_done, &ctx);
    test_check(rc2 == AXL_ERR,
               "udp send_async: second send rejected while in flight");

    axl_loop_add_timeout(loop, 2000, on_udp_send_timeout, &ctx);
    axl_loop_run(loop);

    test_check(ctx.fired, "udp send_async: callback fired");
    test_check(ctx.last_status == AXL_OK,
               "udp send_async: status is AXL_OK");

    axl_udp_close(sock);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// AxlUdp connect / disconnect — peer-lock contract
// ---------------------------------------------------------------------------

static void
test_udp_connect(void)
{
    AxlUdp *sock;
    AxlIPv4Address peer;
    uint8_t loopback[4] = {127, 0, 0, 1};
    const char *msg = "connected-send";

    if (!axl_net_is_available()) {
        axl_printf("SKIP: udp connect (no network)\n");
        return;
    }
    if (axl_udp_open(&sock, 0) != AXL_OK) {
        axl_printf("SKIP: udp connect (open failed)\n");
        return;
    }
    axl_memcpy(&peer, loopback, 4);

    /* Pre-connect: NULL dest must be rejected. */
    test_check(axl_udp_send(sock, NULL, 0, msg, axl_strlen(msg)) == AXL_ERR,
               "udp connect: pre-connect, NULL dest rejected");

    /* Connect, then NULL dest works (uses configured peer). */
    test_check(axl_udp_connect(sock, &peer, 9983) == AXL_OK,
               "udp connect: connect succeeds");
    test_check(axl_udp_send(sock, NULL, 0, msg, axl_strlen(msg)) == AXL_OK,
               "udp connect: NULL dest send uses configured peer");

    /* Explicit dest still works (overrides peer for that packet). */
    AxlIPv4Address other;
    axl_memcpy(&other, loopback, 4);
    test_check(axl_udp_send(sock, &other, 9982, msg, axl_strlen(msg))
               == AXL_OK,
               "udp connect: explicit dest overrides peer");

    /* Disconnect: NULL dest must be rejected again. */
    test_check(axl_udp_disconnect(sock) == AXL_OK,
               "udp connect: disconnect succeeds");
    test_check(axl_udp_send(sock, NULL, 0, msg, axl_strlen(msg)) == AXL_ERR,
               "udp connect: post-disconnect, NULL dest rejected");

    /* Disconnect is idempotent. */
    test_check(axl_udp_disconnect(sock) == AXL_OK,
               "udp connect: disconnect is idempotent");

    axl_udp_close(sock);
}

// ---------------------------------------------------------------------------
// AxlUdp multicast / broadcast — surface-level (real packet exchange
// would require multi-host topology; these verify the API contract).
// ---------------------------------------------------------------------------

static void
test_udp_multicast_broadcast(void)
{
    AxlUdp        *sock;
    AxlIPv4Address group;
    AxlIPv4Address bad;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: udp multicast/broadcast (no network)\n");
        return;
    }
    if (axl_udp_open(&sock, 0) != AXL_OK) {
        axl_printf("SKIP: udp multicast/broadcast (open failed)\n");
        return;
    }

    /* mDNS multicast group 224.0.0.251. */
    uint8_t mdns[4] = {224, 0, 0, 251};
    axl_memcpy(&group, mdns, 4);

    test_check(axl_udp_join_multicast(sock, &group) == AXL_OK,
               "udp multicast: join 224.0.0.251");

    /* Reject obvious non-multicast addresses (224.0.0.0/4). */
    uint8_t unicast[4] = {192, 168, 1, 1};
    axl_memcpy(&bad, unicast, 4);
    test_check(axl_udp_join_multicast(sock, &bad) == AXL_ERR,
               "udp multicast: join rejects unicast address");

    test_check(axl_udp_leave_multicast(sock, &group) == AXL_OK,
               "udp multicast: leave specific group");

    /* leave-all (NULL group) must succeed even with no groups joined. */
    test_check(axl_udp_leave_multicast(sock, NULL) == AXL_OK,
               "udp multicast: leave-all succeeds when no groups joined");

    /* Broadcast toggle. */
    test_check(axl_udp_set_broadcast(sock, true) == AXL_OK,
               "udp broadcast: enable");
    test_check(axl_udp_set_broadcast(sock, true) == AXL_OK,
               "udp broadcast: enable is idempotent");
    test_check(axl_udp_set_broadcast(sock, false) == AXL_OK,
               "udp broadcast: disable");

    axl_udp_close(sock);
}

// ---------------------------------------------------------------------------
// AxlUdp recv_async cancel — drives the cancellable path directly
// (the AxlSocket bridge passes NULL cancel, so this exercises the
// underlying axl_udp_recv_async cancel-source wiring).
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop  *loop;
    AxlStatus last_status;
    bool      fired;
} UdpCancelCtx;

static bool
on_udp_cancel_recv(AxlUdp *sock, AxlStatus status,
                   const void *data, size_t len,
                   const AxlIPv4Address *from, uint16_t from_port,
                   void *user_data)
{
    (void)sock; (void)data; (void)len; (void)from; (void)from_port;
    UdpCancelCtx *ctx = (UdpCancelCtx *)user_data;
    ctx->last_status = status;
    ctx->fired       = true;
    axl_loop_quit(ctx->loop);
    return false;  /* cancel is terminal anyway; explicit for clarity */
}

static bool
on_udp_cancel_fire(void *data)
{
    AxlCancellable *cancel = (AxlCancellable *)data;
    axl_cancellable_cancel(cancel);
    return AXL_SOURCE_REMOVE;
}

static void
test_udp_recv_async_cancel(void)
{
    /* Bind a UDP socket on a port that nothing will send to. Arm
       recv_async with a cancellable. Schedule a timer to fire the
       cancellable. Verify the callback fires with AXL_CANCELLED. */
    AxlUdp *sock;

    if (!axl_net_is_available()) {
        axl_printf("SKIP: udp recv_async cancel (no network)\n");
        return;
    }

    if (axl_udp_open(&sock, 9986) != AXL_OK) {
        axl_printf("SKIP: udp recv_async cancel (open 9986 failed)\n");
        return;
    }

    AxlLoop        *loop   = axl_loop_new();
    AxlCancellable *cancel = axl_cancellable_new();
    UdpCancelCtx    ctx    = { .loop = loop, .last_status = AXL_OK,
                               .fired = false };

    int rc = axl_udp_recv_async(sock, loop, cancel,
                                on_udp_cancel_recv, &ctx);
    test_check(rc == AXL_OK,
               "udp recv_async cancel: arm with cancellable");

    /* Fire the cancel after 100ms (no datagram will ever arrive). */
    axl_loop_add_timeout(loop, 100, on_udp_cancel_fire, cancel);
    /* Safety timeout: if cancel doesn't propagate, exit anyway. */
    axl_loop_add_timeout(loop, 2000, on_udp_recv_timeout, &ctx);

    axl_loop_run(loop);

    test_check(ctx.fired,
               "udp recv_async cancel: callback fired");
    test_check(ctx.last_status == AXL_CANCELLED,
               "udp recv_async cancel: status is AXL_CANCELLED");

    axl_udp_close(sock);
    axl_cancellable_free(cancel);
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
    test_check(axl_socket_bind(sock, 9989) == AXL_OK,
               "socket bind: port 9989");

    /* Bind to a different port */
    test_check(axl_socket_bind(sock, 9988) == AXL_OK,
               "socket bind: rebind to 9988");

    /* Bind on stream should fail */
    AxlSocket *stream = axl_socket_new(AXL_SOCKET_STREAM);
    if (stream != NULL) {
        test_check(axl_socket_bind(stream, 9987) == AXL_ERR,
                   "socket bind: stream fails");
        axl_socket_free(stream);
    }

    axl_socket_free(sock);
}

// ---------------------------------------------------------------------------
// AxlBytes adoption: server set_bytes + client response get_bytes (pure,
// no network).
// ---------------------------------------------------------------------------

static void
test_http_response_set_bytes(void)
{
    AxlHttpResponse r;
    axl_memset(&r, 0, sizeof(r));

    const char payload[] = { 1, 2, 3, 0, 4 };  // embedded NUL
    AxlBytes *b = axl_bytes_new(payload, sizeof(payload));
    axl_http_response_set_bytes(&r, b, "application/octet-stream");

    test_check(r.body != NULL && !r.body_static,
        "set_bytes: installs an owned (copied) body");
    test_check(r.body != axl_bytes_get_data(b, NULL),
        "set_bytes: body is a copy, not the AxlBytes storage");
    test_check(r.body_size == sizeof(payload), "set_bytes: body_size matches");
    test_check(axl_memcmp(r.body, payload, sizeof(payload)) == 0,
        "set_bytes: content matches (incl. embedded NUL)");
    test_check(r.content_type != NULL
        && axl_strcmp(r.content_type, "application/octet-stream") == 0,
        "set_bytes: content_type captured");
    test_check(r.status_code == 200, "set_bytes: defaults status to 200");

    // The response owns its copy, so the caller may unref immediately.
    axl_bytes_unref(b);
    test_check(axl_memcmp(r.body, payload, sizeof(payload)) == 0,
        "set_bytes: body survives input unref");
    axl_free(r.body);

    // NULL content_type preserves prior; empty AxlBytes -> empty body.
    AxlHttpResponse r2;
    axl_memset(&r2, 0, sizeof(r2));
    r2.content_type = "text/plain";
    AxlBytes *empty = axl_bytes_new(NULL, 0);
    axl_http_response_set_bytes(&r2, empty, NULL);
    test_check(r2.content_type != NULL && axl_strcmp(r2.content_type, "text/plain") == 0,
        "set_bytes: NULL content_type preserves prior value");
    test_check(r2.body_size == 0, "set_bytes: empty AxlBytes -> empty body");
    axl_bytes_unref(empty);
    if (r2.body != NULL && !r2.body_static) {
        axl_free(r2.body);
    }
}

static void
test_http_client_response_get_bytes(void)
{
    AxlHttpClientResponse resp;
    axl_memset(&resp, 0, sizeof(resp));
    const char body[] = "response-body-\x00-with-nul-inside";
    resp.body = axl_malloc(sizeof(body));
    axl_memcpy(resp.body, body, sizeof(body));
    resp.body_size = sizeof(body);

    AxlBytes *b = axl_http_client_response_get_bytes(&resp);
    test_check(b != NULL && axl_bytes_get_size(b) == sizeof(body),
        "client get_bytes: size matches");
    size_t n = 0;
    const uint8_t *p = axl_bytes_get_data(b, &n);
    test_check(p != NULL && axl_memcmp(p, body, sizeof(body)) == 0,
        "client get_bytes: content matches (incl. embedded NUL)");

    // Snapshot outlives the response body being freed.
    axl_free(resp.body);
    resp.body = NULL;
    resp.body_size = 0;
    test_check(axl_memcmp(axl_bytes_get_data(b, NULL), body, sizeof(body)) == 0,
        "client get_bytes: snapshot survives response body free");
    axl_bytes_unref(b);

    // Empty / NULL.
    test_check(axl_http_client_response_get_bytes(&resp) == NULL,
        "client get_bytes: empty body -> NULL");
    test_check(axl_http_client_response_get_bytes(NULL) == NULL,
        "client get_bytes: NULL resp -> NULL");
}

// ---------------------------------------------------------------------------
// HTTP status line (reason-phrase table)
// ---------------------------------------------------------------------------

/* Internal helper from axl-http-core.c (declared in axl-net-internal.h,
   which isn't on the default-build include path). */
size_t http_build_status_line(char *buf, size_t buf_size, size_t status_code);

static void
test_http_status_line(void)
{
    char buf[64];

    http_build_status_line(buf, sizeof(buf), 402);
    test_check(axl_strcmp(buf, "HTTP/1.1 402 Payment Required\r\n") == 0,
               "http status: 402 Payment Required");

    /* Spot-check neighbors so the table edit didn't shift anything. */
    http_build_status_line(buf, sizeof(buf), 401);
    test_check(axl_strcmp(buf, "HTTP/1.1 401 Unauthorized\r\n") == 0,
               "http status: 401 Unauthorized");
    http_build_status_line(buf, sizeof(buf), 403);
    test_check(axl_strcmp(buf, "HTTP/1.1 403 Forbidden\r\n") == 0,
               "http status: 403 Forbidden");
}

static void
test_http_auth_challenge(void)
{
    AxlHttpServer *s = axl_http_server_new(0);
    test_check(s != NULL, "auth challenge: server created");
    if (s == NULL) {
        return;
    }

    test_check(axl_http_server_set_auth_challenge(NULL, "Basic", "r") == AXL_ERR,
               "auth challenge: NULL server -> AXL_ERR");
    test_check(axl_http_server_set_auth_challenge(s, "Basic", "axl") == AXL_OK,
               "auth challenge: Basic + realm -> AXL_OK");
    test_check(axl_http_server_set_auth_challenge(s, "Basic", NULL) == AXL_OK,
               "auth challenge: no realm -> AXL_OK");
    test_check(axl_http_server_set_auth_challenge(s, NULL, NULL) == AXL_OK,
               "auth challenge: clear (NULL scheme) -> AXL_OK");
    /* Header-injection guards. */
    test_check(axl_http_server_set_auth_challenge(s, "Ba sic", "r") == AXL_ERR,
               "auth challenge: scheme with space -> AXL_ERR");
    test_check(axl_http_server_set_auth_challenge(s, "Basic", "a\r\nb") == AXL_ERR,
               "auth challenge: realm with CRLF -> AXL_ERR");
    test_check(axl_http_server_set_auth_challenge(s, "Basic", "a\"b") == AXL_ERR,
               "auth challenge: realm with quote -> AXL_ERR");

    axl_http_server_free(s);
}

// ---------------------------------------------------------------------------
// Driver selection — bus-location decode + safe negatives
//
// _axl_net_bus_location is an internal helper (src/net/axl-net-driver-select.c)
// exposed for unit testing; declared extern here since unit tests include
// only <axl.h>. The synthetic device paths below exercise the real
// trim + firmware DevicePathToText path with deterministic, exact-string
// expectations. The live driver/bus accessors over a real NIC are covered
// by test/integration/test-nic-qemu.sh (which the iPXE-last / OEM-child /
// MediaPresent quirks are real-hardware-only for).
// ---------------------------------------------------------------------------

int _axl_net_bus_location(const void *device_path, char *out, size_t out_size);

/* Append one device-path node {type,sub,len=4+paylen, payload} at @p off,
   returning the new offset. */
static size_t
dp_append(uint8_t *buf, size_t off, uint8_t type, uint8_t sub,
          const uint8_t *payload, uint16_t paylen)
{
    uint16_t len = (uint16_t)(4 + paylen);
    buf[off + 0] = type;
    buf[off + 1] = sub;
    buf[off + 2] = (uint8_t)len;
    buf[off + 3] = (uint8_t)(len >> 8);
    if (paylen != 0 && payload != NULL) {
        axl_memcpy(buf + off + 4, payload, paylen);
    }
    return off + len;
}

/* ACPI _HID PciRoot(0x0): HID = EISA_PNP_ID(0x0A03) = 0x0A0341D0, UID = 0. */
static size_t
dp_pci_root(uint8_t *buf, size_t off)
{
    static const uint8_t acpi[8] = { 0xD0, 0x41, 0x03, 0x0A, 0, 0, 0, 0 };
    return dp_append(buf, off, 0x02, 0x01, acpi, sizeof(acpi));
}

/* PCI(device,function): struct order is {Function, Device}. */
static size_t
dp_pci(uint8_t *buf, size_t off, uint8_t device, uint8_t function)
{
    uint8_t pci[2] = { function, device };
    return dp_append(buf, off, 0x01, 0x01, pci, sizeof(pci));
}

/* USB(port,interface): {ParentPortNumber, InterfaceNumber}. */
static size_t
dp_usb(uint8_t *buf, size_t off, uint8_t port, uint8_t iface)
{
    uint8_t usb[2] = { port, iface };
    return dp_append(buf, off, 0x03, 0x05, usb, sizeof(usb));
}

/* MAC node — 32-byte MAC + 1-byte IfType (content irrelevant; trimmed). */
static size_t
dp_mac(uint8_t *buf, size_t off)
{
    static const uint8_t mac[33] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    return dp_append(buf, off, 0x03, 0x0B, mac, sizeof(mac));
}

static size_t
dp_end(uint8_t *buf, size_t off)
{
    return dp_append(buf, off, 0x7F, 0xFF, NULL, 0);
}

static void
test_bus_location(void)
{
    char loc[160];

    /* PCI NIC: PciRoot/Pci/MAC -> trim the MAC tail. */
    uint8_t pci[128];
    size_t n = dp_pci_root(pci, 0);
    n = dp_pci(pci, n, 0x03, 0x00);
    n = dp_mac(pci, n);
    (void)dp_end(pci, n);
    test_check(_axl_net_bus_location(pci, loc, sizeof(loc)) == AXL_OK
                   && axl_strcmp(loc, "PciRoot(0x0)/Pci(0x3,0x0)") == 0,
               "bus_location: PCI NIC trims MAC tail");

    /* USB NIC: PciRoot/Pci/USB/MAC -> keep the USB port, trim MAC. */
    uint8_t usb[128];
    n = dp_pci_root(usb, 0);
    n = dp_pci(usb, n, 0x01, 0x00);
    n = dp_usb(usb, n, 0x01, 0x00);
    n = dp_mac(usb, n);
    (void)dp_end(usb, n);
    test_check(_axl_net_bus_location(usb, loc, sizeof(loc)) == AXL_OK
                   && axl_strcmp(loc, "PciRoot(0x0)/Pci(0x1,0x0)/USB(0x1,0x0)") == 0,
               "bus_location: USB NIC keeps USB port, trims MAC");

    /* No network tail: the whole path IS the location. */
    uint8_t notail[64];
    n = dp_pci_root(notail, 0);
    n = dp_pci(notail, n, 0x03, 0x00);
    (void)dp_end(notail, n);
    test_check(_axl_net_bus_location(notail, loc, sizeof(loc)) == AXL_OK
                   && axl_strcmp(loc, "PciRoot(0x0)/Pci(0x3,0x0)") == 0,
               "bus_location: no MAC tail -> whole path");

    /* Argument validation. */
    test_check(_axl_net_bus_location(NULL, loc, sizeof(loc)) == AXL_ERR,
               "bus_location: NULL device path -> AXL_ERR");
    test_check(_axl_net_bus_location(pci, NULL, sizeof(loc)) == AXL_ERR,
               "bus_location: NULL out -> AXL_ERR");
    test_check(_axl_net_bus_location(pci, loc, 0) == AXL_ERR,
               "bus_location: zero out_size -> AXL_ERR");
}

/* axl_tcp_connect_timeout: a short connect timeout must bound the SYN wait on
   a silently-dropped destination, instead of the 10 s default. Arg validation
   needs no network; the timing guard needs an IP (as the suite's other socket
   tests do). */
static void
test_connect_timeout(void)
{
    AxlTcp *s = NULL;
    test_check(axl_tcp_connect_timeout(NULL, 80, NULL, 1000, &s) == AXL_ERR,
               "connect_timeout: NULL host -> AXL_ERR");
    test_check(axl_tcp_connect_timeout("10.0.2.99", 80, NULL, 1000, NULL) == AXL_ERR,
               "connect_timeout: NULL out -> AXL_ERR");

    /* 10.0.2.99 is unassigned in slirp's 10.0.2.0/24 subnet: the SYN goes
       unanswered (no RST), so the AXL-side connect deadline is what ends the
       wait. With a 1 s timeout the call must return in ~1 s, not the ~10 s the
       old hardcoded default took. The >= 500 ms lower bound proves the
       blackhole actually engaged the timeout (vs a vacuous fast-fail). */
    axl_net_init(AXL_NET_NIC_AUTO, 10);
    AxlTcp  *sock = NULL;
    uint64_t t0 = axl_time_get_ms();
    int      rc = axl_tcp_connect_timeout("10.0.2.99", 9999, NULL, 1000, &sock);
    uint64_t elapsed = axl_time_get_ms() - t0;
    test_check(rc == AXL_ERR,
               "connect_timeout: silently-dropped host -> AXL_ERR");
    test_check(elapsed >= 500 && elapsed < 5000,
               "connect_timeout: 1s timeout honored (not the 10s default)");
    if (sock != NULL) {
        axl_tcp_close(sock);
    }
}

static void
test_driver_select_negatives(void)
{
    AxlNetDriverInfo di;
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    test_check(axl_net_get_driver_info(NULL, &di) == AXL_ERR,
               "get_driver_info: NULL mac -> AXL_ERR");
    test_check(axl_net_get_driver_info(mac, NULL) == AXL_ERR,
               "get_driver_info: NULL out -> AXL_ERR");

    /* list_available_drivers: NULL count rejected; count-query succeeds. */
    test_check(axl_net_list_available_drivers(NULL, NULL) == AXL_ERR,
               "list_available_drivers: NULL count -> AXL_ERR");
    size_t dcount = 0;
    test_check(axl_net_list_available_drivers(NULL, &dcount) == AXL_OK,
               "list_available_drivers: count query -> AXL_OK");

    /* try_driver safe negatives — none load a driver into the shared boot.
       NULL / empty / a name that can't be located all fail before LoadImage. */
    AxlNetTryResult tr;
    test_check(axl_net_try_driver(NULL, &tr) == AXL_ERR && !tr.found,
               "try_driver: NULL name -> AXL_ERR, found=false");
    test_check(axl_net_try_driver("", &tr) == AXL_ERR && !tr.found,
               "try_driver: empty name -> AXL_ERR, found=false");
    test_check(axl_net_try_driver("axl-no-such-nic-driver-xyz.efi", &tr) == AXL_ERR
                   && !tr.found,
               "try_driver: unlocatable name -> AXL_ERR, found=false");

    /* connect_stack is idempotent and safe to call repeatedly. */
    test_check(axl_net_connect_stack() == AXL_OK,
               "connect_stack: returns AXL_OK");
}

// ---------------------------------------------------------------------------
// axl_net_resolve_async — deterministic IPv4-literal path (no network)
// ---------------------------------------------------------------------------

typedef struct {
    bool            fired;
    AxlIPv4Address  addr;
    AxlStatus       st;
    AxlLoop        *loop;
} ResolveAsyncTestCtx;

static void
resolve_async_test_cb(const AxlIPv4Address *addr, AxlStatus st, void *user)
{
    ResolveAsyncTestCtx *c = (ResolveAsyncTestCtx *)user;
    c->fired = true;
    c->st    = st;
    if (addr != NULL) {
        c->addr = *addr;
    }
    axl_loop_quit(c->loop);
}

// An IPv4 literal needs no DNS, so this is deterministic and network-free. It
// pins the async contract: the callback is ALWAYS deferred (never re-entrant
// from the call), fires exactly once, and carries the parsed address.
static void
test_resolve_async_literal(void)
{
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("resolve_async: loop alloc");
        return;
    }

    ResolveAsyncTestCtx ctx = { .fired = false, .st = AXL_ERR, .loop = loop };
    int rc = axl_net_resolve_async("127.0.0.1", loop, NULL,
                                   resolve_async_test_cb, &ctx);

    test_check(rc == AXL_OK, "resolve_async: literal initiated (AXL_OK)");
    test_check(!ctx.fired,
               "resolve_async: literal callback is deferred (not re-entrant)");

    axl_loop_run(loop);

    test_check(ctx.fired, "resolve_async: callback fired after loop run");
    test_check(ctx.st == AXL_OK, "resolve_async: literal status AXL_OK");
    test_check(ctx.addr.addr[0] == 127 && ctx.addr.addr[1] == 0 &&
               ctx.addr.addr[2] == 0   && ctx.addr.addr[3] == 1,
               "resolve_async: literal resolved to 127.0.0.1");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// axl_http_{get,post}_async — deterministic parameter-validation contract.
//
// The happy path needs a server (covered by test-http-async-qemu.sh). Here we
// pin only the network-free guarantees: a rejected call returns an error and
// the callback must NOT fire (the "AXL_OK iff cb fires later" contract). A
// fired callback would flip async_cb_must_not_fire.
// ---------------------------------------------------------------------------

static void
async_cb_must_not_fire(AxlHttpClientResponse *resp, AxlStatus st, void *user)
{
    (void)resp;
    (void)st;
    *(bool *)user = true;
    if (resp != NULL) {
        axl_http_client_response_free(resp);
    }
}

static void
test_http_async_param_validation(void)
{
    AxlLoop *loop = axl_loop_new();
    if (loop == NULL) {
        test_fail("http-async: loop alloc");
        return;
    }

    AxlHttpClient *c = axl_http_client_new();
    if (c == NULL) {
        test_fail("http-async: client alloc");
        axl_loop_free(loop);
        return;
    }

    bool fired = false;

    /* NULL client / loop / url are rejected before any work — and the
       callback must not fire (caller still owns everything). */
    test_check(axl_http_get_async(NULL, loop, "http://h/", NULL,
                                  async_cb_must_not_fire, &fired) == AXL_ERR,
               "http-async: NULL client -> AXL_ERR");
    test_check(axl_http_get_async(c, NULL, "http://h/", NULL,
                                  async_cb_must_not_fire, &fired) == AXL_ERR,
               "http-async: NULL loop -> AXL_ERR");
    test_check(axl_http_get_async(c, loop, NULL, NULL,
                                  async_cb_must_not_fire, &fired) == AXL_ERR,
               "http-async: NULL url -> AXL_ERR");
    test_check(axl_http_post_async(c, loop, NULL, "b", 1, "text/plain", NULL,
                                   async_cb_must_not_fire, &fired) == AXL_ERR,
               "http-async: POST NULL url -> AXL_ERR");

    /* Drain the loop briefly: a buggy impl that deferred the callback
       despite the error return would surface here. */
    axl_loop_iterate_until(loop, NULL, 20 * 1000);
    test_check(!fired, "http-async: rejected call never fires the callback");

    axl_http_client_free(c);
    axl_loop_free(loop);
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
    // "serve-tls-driver" -- HTTPS server driven by a resident driver-tick
    // loop (axl_loop_attach_driver, TPL_CALLBACK) instead of a top-level
    // axl_loop_run. Regression repro for the TLS handshake stalling when a
    // resident AxlService loop dispatches accept (the synchronous handshake
    // used to nest an ephemeral axl_loop_run that can't run at raised TPL).
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-tls-driver") == 0) {
        return run_serve_tls_driver_mode();
    }

    //
    // "serve-tls-ws-driver" -- HTTPS + per-client WebSocket on a resident
    // driver-tick loop. Regression repro for the WS-teardown wedge: a wss
    // connect/disconnect against the pumped server used to wedge the loop
    // (synchronous close-frame echo nesting an ephemeral loop at raised TPL).
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-tls-ws-driver") == 0) {
        return run_serve_tls_ws_driver_mode();
    }

    //
    // "serve-hazard-driver" -- the consumer-emulator: HTTPS + WS + a plain
    // second HTTP server on ONE shared loop, pumped from a driver tick at
    // raised TPL. The canonical SoftBMC topology that surfaced the wedge /
    // desync / dead-accept bug family. Driven by test-consumer-emulator-qemu.sh.
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-hazard-driver") == 0) {
        return run_serve_hazard_driver_mode();
    }

    //
    // "serve-shell-coexist" -- foreground real Shell.efi + background HTTP
    // pumped by axl_loop_attach_driver. The Console Mirror concurrency spike.
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-shell-coexist") == 0) {
        return run_serve_shell_coexist_mode();
    }

    //
    // "serve-tls-shell-coexist" -- rung 5: mirror + HTTPS-on-attach_driver +
    // foreground Shell (the SoftBMC-faithful HTTPS coexistence proof).
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-tls-shell-coexist") == 0) {
        return run_serve_tls_shell_coexist_mode();
    }

    //
    // "mirror-selftest" -- AxlConsoleMirror positive proof in its own boot.
    //
    if (argc >= 2 && axl_strcmp(argv[1], "mirror-selftest") == 0) {
        return run_mirror_selftest_mode();
    }

    //
    // "mirror-edit" -- the `edit` acceptance gate (design §6 rung 3).
    //
    if (argc >= 2 && axl_strcmp(argv[1], "mirror-edit") == 0) {
        return run_mirror_edit_mode();
    }

    //
    // "serve-ws-shell-inject" -- P1 gate: inject_text from a WS handler in the
    // attach_driver dispatch must wake the foreground Shell (SoftBMC RemoteShell).
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-ws-shell-inject") == 0) {
        return run_serve_ws_shell_inject_mode();
    }

    //
    // Check for "serve-multi" -- two plain servers on one shared loop
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-multi") == 0) {
        return run_serve_multi_mode();
    }

    if (argc >= 2 && axl_strcmp(argv[1], "serve-multi-tls") == 0) {
        return run_serve_multi_tls_mode();
    }

    if (argc >= 2 && axl_strcmp(argv[1], "serve-davfs") == 0) {
        return run_serve_davfs_mode();
    }

    if (argc >= 2 && axl_strcmp(argv[1], "serve-davfs-tls") == 0) {
        return run_serve_davfs_tls_mode();
    }

    //
    // Check for "udp-echo" argument -- send UDP datagram to host
    //
    if (argc >= 4 && axl_strcmp(argv[1], "udp-echo") == 0) {
        return run_udp_echo_mode(argv[2], argv[3]);
    }

    // Outbound sync TCP connect/send/recv at raised TPL — test-tcp-connect-rtpl-qemu.sh
    if (argc >= 3 && axl_strcmp(argv[1], "get-async") == 0) {
        return run_http_async_mode("GET", argv[2], NULL);
    }

    if (argc >= 4 && axl_strcmp(argv[1], "post-async") == 0) {
        return run_http_async_mode("POST", argv[2], argv[3]);
    }

    if (argc >= 3 && axl_strcmp(argv[1], "get-sync-rtpl") == 0) {
        return run_get_sync_rtpl_mode(argv[2]);
    }

    if (argc >= 3 && axl_strcmp(argv[1], "get-size") == 0) {
        return run_get_size_mode(argv[2]);
    }

    if (argc >= 4 && axl_strcmp(argv[1], "tcp-connect-rtpl") == 0) {
        return run_tcp_connect_rtpl_mode(argv[2], argv[3]);
    }

    //
    // "sntp-query <host> <port>" -- query a (mock) SNTP server
    //
    if (argc >= 4 && axl_strcmp(argv[1], "sntp-query") == 0) {
        return run_sntp_query_mode(argv[2], argv[3]);
    }

    //
    // "net-diag" -- exercise the DHCP-lease / reverse-DNS primitives
    // against the live (DHCP'd) network. Driven by test-netdiag-qemu.sh.
    //
    if (argc >= 2 && axl_strcmp(argv[1], "net-diag") == 0) {
        return run_net_diag_mode();
    }

    //
    // Normal unit test mode
    //
    test_print_header("AxlTestNet");

    //
    // Driver selection — bus-location decode (synthetic) + safe negatives.
    // Needs the firmware DevicePathToText protocol (always present under
    // OVMF) but no NIC / driver load.
    //
    axl_printf("--- Driver Select ---\n");
    test_bus_location();
    test_driver_select_negatives();
    test_connect_timeout();

    //
    // URL parsing (no network)
    //
    axl_printf("--- URL Parsing ---\n");
    test_url_parse_basic();
    test_url_parse_https();
    test_url_parse_minimal();
    test_url_parse_userinfo_full();
    test_url_parse_userinfo_user_only();
    test_url_parse_userinfo_empty_password();
    test_url_parse_no_userinfo();
    test_url_parse_fragment_only();
    test_url_parse_no_fragment();
    test_url_parse_at_in_path_not_userinfo();
    test_url_parse_userinfo_colon_in_password();
    test_url_parse_fragment_after_host();
    test_url_parse_port_nondigit();
    test_url_parse_port_overflow();
    test_url_parse_port_trailing_garbage();
    test_url_parse_port_empty();
    test_url_parse_port_max();
    test_url_parse_malformed();
    test_url_build();
    test_url_encode_decode();

    //
    // HTTP core parsing (no network)
    //
    axl_printf("\n--- HTTP Core Parsing ---\n");
    test_http_find_header_end();
    test_http_parse_status();
    test_http_status_line();
    test_http_auth_challenge();
    test_http_parse_request();
    test_http_parse_headers();
    test_http_parse_range();
    test_http_accepts();
    test_http_request_helpers();
    test_http_add_routes_variadic();

    //
    // IPv4 parse / format (no network)
    //
    axl_printf("\n--- IPv4 Parse/Format ---\n");
    test_ipv4_parse_format();

    //
    // AxlNetOpts (no network — validation only)
    //
    axl_printf("\n--- AxlNetOpts ---\n");
    test_net_opts_validation();
    test_net_resolve_ptr_validation();
    test_ws_conn_api_validation();

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
    test_http_response_set_static();
    test_http_response_set_bytes();
    test_http_client_response_get_bytes();
    test_http_response_set_range();
    test_http_response_set_content_range();
    test_http_response_set_streamer();

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
    test_udp_send_async();
    test_udp_connect();
    test_udp_multicast_broadcast();
    test_udp_recv_async_cancel();
    test_socket_udp_async_recv();

    //
    // AxlSocketClient (network required)
    //
    axl_printf("\n--- SocketClient ---\n");
    test_socket_client_connect();
    test_socket_client_invalid();

    test_resolve_async_literal();
    test_http_async_param_validation();

    return test_print_results();
}

AXL_APP(test_net_main)
