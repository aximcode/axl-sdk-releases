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
#include <axl/axl-9p.h>
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

    //
    // A URL longer than the 512-byte STACK buffer axl_url_build formats
    // into. axl_snprintf returns the length it WOULD have written, so the
    // old code allocated that much and then copied it back out of the
    // 512-byte buffer -- reading past the end of the stack frame and
    // splicing whatever was there into the returned string. An over-read,
    // and an information leak into a value the caller hands onward.
    //
    // Found by test/fuzz/url_fuzz once it started calling the BUILDER; the
    // parser-only harness could not reach this function at all.
    //
    {
        char   longpath[700];
        size_t i;

        longpath[0] = '/';
        for (i = 1; i < sizeof(longpath) - 1; i++) {
            longpath[i] = 'a' + (char)(i % 26);
        }
        longpath[sizeof(longpath) - 1] = '\0';

        url = axl_url_build("http", "example.com", 0, longpath);
        test_check(url != NULL, "URL build oversize non-NULL");
        if (url != NULL) {
            /* Exact: scheme + host + the path we asked for, nothing else.
               A stack over-read shows up as trailing garbage, which a
               length check alone would miss if the garbage happened to
               start with a NUL. */
            const size_t want = axl_strlen("http://example.com")
                                + axl_strlen(longpath);

            test_check(axl_strlen(url) == want,
                       "URL build oversize: length is exactly the URL, not "
                       "the stack buffer's worth");
            test_check(axl_strncmp(url, "http://example.com", 18) == 0
                           && axl_strcmp(url + 18, longpath) == 0,
                       "URL build oversize: the whole path survives, with no "
                       "stack residue spliced in");
            axl_free(url);
        }
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

    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
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

// ---------------------------------------------------------------------------
// axl_tcp_send_async: a second send submitted while the first is in flight.
//
// The transport queues it; the caller is NOT required to serialize. Until the
// token queue landed, this returned AXL_BUSY and every caller had to invent its
// own serialisation — three did (axl-http-ws.c's frame queue, the
// axl_tls_write_async floor, handshake_flush_async's resume) and four call
// sites in axl-http-response.c did not, testing `!= AXL_OK` and resetting a
// healthy connection instead. That dropped ~1 request in 3 under concurrent
// TLS handshakes. See docs/AXL-Tcp-Queue-Design.md §1a.
//
// The assertion is on BOTH callbacks firing with AXL_OK, not merely on the
// submit returning AXL_OK: a queue that accepts a token and then loses it
// would pass the weaker check while hanging the caller forever.
//
// The third assertion pins FIFO callback ORDER. Retirement no longer calls the
// consumer inline — it appends to a per-socket list drained on the next loop
// tick (the SIGNAL_TOKEN port) — and a list that pushed at the head instead of
// the tail would deliver a stream's completions backwards while every
// status stayed AXL_OK.
// ---------------------------------------------------------------------------
typedef struct {
    int       fires;
    AxlStatus status[2];
    int       order[2];       /* slot ids, in the order their callbacks fired */
    AxlLoop  *loop;
} TcpSendQCtx;

/* Per-send context so a callback can say WHICH send it belongs to. */
typedef struct {
    TcpSendQCtx *ctx;
    int          id;          /* 1 = submitted first, 2 = submitted second */
} TcpSendQSlot;

static bool
on_tcp_sendq_done(AxlTcp *sock, AxlStatus status, void *data)
{
    TcpSendQSlot *slot = (TcpSendQSlot *)data;
    TcpSendQCtx  *c    = slot->ctx;
    (void)sock;
    if (c->fires < 2) {
        c->status[c->fires] = status;
        c->order[c->fires]  = slot->id;
    }
    c->fires++;
    if (c->fires >= 2) {
        axl_loop_quit(c->loop);
    }
    return AXL_SOURCE_REMOVE;
}

static bool
on_tcp_sendq_timeout(void *data)
{
    TcpSendQCtx *c = (TcpSendQCtx *)data;
    axl_loop_quit(c->loop);
    return AXL_SOURCE_REMOVE;
}

static void
test_tcp_send_async_queued(void)
{
    AxlTcp     *client = NULL;
    TcpSendQCtx ctx    = { 0 };
    /* Distinct payloads, both alive until their callbacks fire — the buffers
       are BORROWED by the transport for the whole queued lifetime, not copied
       (design §3.1), so statics rather than stack temporaries. */
    static const char first[]  = "queued-send-one";
    static const char second[] = "queued-send-two";

    if (!axl_net_is_available()) {
        test_skip_n(3, "TCP send_async queue (no network)");
        return;
    }
    if (axl_tcp_connect(AXL_TEST_ECHO_HOST, 9999, &client) != AXL_OK) {
        test_skip_n(3, "TCP send_async queue (connect failed)");
        return;
    }

    ctx.loop = axl_loop_new();
    TcpSendQSlot slot1 = { .ctx = &ctx, .id = 1 };
    TcpSendQSlot slot2 = { .ctx = &ctx, .id = 2 };

    int rc1 = axl_tcp_send_async(client, first, sizeof(first) - 1,
                                 ctx.loop, NULL, on_tcp_sendq_done, &slot1);
    /* No loop iteration between the two: the first send is still in flight. */
    int rc2 = axl_tcp_send_async(client, second, sizeof(second) - 1,
                                 ctx.loop, NULL, on_tcp_sendq_done, &slot2);

    test_check(rc1 == AXL_OK && rc2 == AXL_OK,
               "TCP send_async: second submit is queued");

    axl_loop_add_timeout(ctx.loop, 5000, on_tcp_sendq_timeout, &ctx);
    axl_loop_run(ctx.loop);

    test_check(ctx.fires == 2
                   && ctx.status[0] == AXL_OK
                   && ctx.status[1] == AXL_OK,
               "TCP send_async: both sends complete AXL_OK");

    test_check(ctx.order[0] == 1 && ctx.order[1] == 2,
               "TCP send_async: callbacks fire in submission order");

    /* Close before freeing the loop — axl_tcp_close removes sources stamped
       with it (see test_tcp_recv_async_rearm). */
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
    axl_loop_free(ctx.loop);
}

// ---------------------------------------------------------------------------
// Closing with a send still QUEUED must retire it, not strand it.
//
// EDK2 SockConnFlush runs SockFlushPendingToken over SndTokenList as well as
// the processing list (design §3.5). Without that, a caller whose send never
// reached the wire waits forever on a callback that cannot come, and its token
// leaks. Sabotage found this untested: deleting the flush left the suite green,
// because the queue-ordering test above closes only after both sends complete,
// so the queue is empty by then.
//
// Deterministic without running the loop: no loop iteration happens between the
// two submits, so the first still owns the active slot and the second is
// necessarily queued; the flush is synchronous inside axl_tcp_close.
//
// BOTH halves are pinned, because the queue makes both easy to get wrong: the
// QUEUED send and the ACTIVE one must each be retired with AXL_CANCELLED.
// Until send callbacks were deferred, close could not fire the active one —
// calling a consumer inline from inside teardown re-entered it
// (on_response_sent -> reset_connection -> axl_tcp_close) and wedged the run —
// so the active send's callback was silently dropped and its ciphertext copy
// leaked with it. Retirement now schedules the callback instead of calling it,
// which is what makes the symmetric contract (EDK2 SockConnFlush runs
// SockFlushPendingToken over the processing list as well) implementable.
// ---------------------------------------------------------------------------
typedef struct {
    int       fires;
    AxlStatus last;
} TcpFlushCtx;

static bool
on_tcp_flush_cb(AxlTcp *sock, AxlStatus status, void *data)
{
    TcpFlushCtx *c = (TcpFlushCtx *)data;
    (void)sock;
    c->fires++;
    c->last = status;
    return AXL_SOURCE_REMOVE;
}

static void
test_tcp_send_async_flush_on_close(void)
{
    AxlTcp     *client = NULL;
    TcpFlushCtx active = { 0 };
    TcpFlushCtx queued = { 0 };
    AxlLoop    *loop;
    static const char a[] = "flush-active-send";
    static const char b[] = "flush-queued-send";

    if (!axl_net_is_available()) {
        test_skip_n(2, "TCP send_async close-flush (no network)");
        return;
    }
    if (axl_tcp_connect(AXL_TEST_ECHO_HOST, 9999, &client) != AXL_OK) {
        test_skip_n(2, "TCP send_async close-flush (connect failed)");
        return;
    }

    loop = axl_loop_new();
    axl_tcp_send_async(client, a, sizeof(a) - 1, loop, NULL,
                       on_tcp_flush_cb, &active);
    axl_tcp_send_async(client, b, sizeof(b) - 1, loop, NULL,
                       on_tcp_flush_cb, &queued);

    /* Tear down with the second send still on the queue. */
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);

    test_check(queued.fires == 1 && queued.last == AXL_CANCELLED,
               "TCP send_async: close cancels a queued send");

    /* The ACTIVE send across the same close. Its callback fires too — a send
       that was ACCEPTED always gets exactly one callback, whichever list it
       sat on when teardown started. The close drains what it scheduled before
       returning, so this holds even though the loop is never run and is freed
       immediately below. */
    test_check(active.fires == 1 && active.last == AXL_CANCELLED,
               "TCP send_async: close cancels the active send");

    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// A send callback that closes its socket must not strand the send behind it.
//
// Shape: send A is on the wire, send B is queued behind it. A completes, B is
// PROMOTED into the active slot, and only then does A's callback run and close
// the socket. B is active by that point, so a teardown that retires only the
// queued list drops its callback entirely — its caller waits forever with its
// borrowed buffer pinned (design §6b defect 1). Barring promotion during close
// does not help: the promotion already happened.
//
// The same test pins the re-entrancy half (defect 2). B's callback fires from
// INSIDE axl_tcp_close and closes the socket again, which is ordinary consumer
// code (on_response_sent -> reset_connection -> axl_tcp_close; s9p_on_send;
// sdk/examples/tcp-echo-server.c). The nested close must not run a second
// teardown over firmware state the outer one already released, and must not
// free the socket while the outer close is still using it.
//
// Deterministic ordering: the deferred-callback drain runs at the TOP of a loop
// iteration, before any event is waited on or dispatched, so A's completion
// (iteration N) and A's callback (iteration N+1) can never be separated by B's
// completion.
// ---------------------------------------------------------------------------
typedef struct {
    AxlLoop  *loop;
    int       a_fires;
    int       b_fires;
    AxlStatus a_status;
    AxlStatus b_status;
} TcpCloseCbCtx;

static bool
on_tcp_closecb_b(AxlTcp *sock, AxlStatus status, void *data)
{
    TcpCloseCbCtx *c = (TcpCloseCbCtx *)data;
    c->b_fires++;
    c->b_status = status;
    /* Re-entrant close, from a callback the close itself fired. RESET so the
       teardown finalizes inline rather than deferring to a close event on a
       loop this test is about to quit. */
    axl_tcp_close(sock, AXL_TEARDOWN_RESET);
    axl_loop_quit(c->loop);
    return AXL_SOURCE_REMOVE;
}

static bool
on_tcp_closecb_a(AxlTcp *sock, AxlStatus status, void *data)
{
    TcpCloseCbCtx *c = (TcpCloseCbCtx *)data;
    c->a_fires++;
    c->a_status = status;
    axl_tcp_close(sock, AXL_TEARDOWN_RESET);
    return AXL_SOURCE_REMOVE;
}

static bool
on_tcp_closecb_timeout(void *data)
{
    TcpCloseCbCtx *c = (TcpCloseCbCtx *)data;
    axl_loop_quit(c->loop);
    return AXL_SOURCE_REMOVE;
}

static void
test_tcp_send_close_from_send_callback(void)
{
    AxlTcp       *client = NULL;
    TcpCloseCbCtx ctx    = { 0 };
    static const char a[] = "closecb-send-active";
    static const char b[] = "closecb-send-promoted";

    if (!axl_net_is_available()) {
        test_skip_n(2, "TCP send_async close-from-callback (no network)");
        return;
    }
    if (axl_tcp_connect(AXL_TEST_ECHO_HOST, 9999, &client) != AXL_OK) {
        test_skip_n(2, "TCP send_async close-from-callback (connect failed)");
        return;
    }

    ctx.loop = axl_loop_new();
    axl_tcp_send_async(client, a, sizeof(a) - 1, ctx.loop, NULL,
                       on_tcp_closecb_a, &ctx);
    axl_tcp_send_async(client, b, sizeof(b) - 1, ctx.loop, NULL,
                       on_tcp_closecb_b, &ctx);

    axl_loop_add_timeout(ctx.loop, 5000, on_tcp_closecb_timeout, &ctx);
    axl_loop_run(ctx.loop);

    test_check(ctx.a_fires == 1 && ctx.a_status == AXL_OK,
               "TCP send_async: completed send reports AXL_OK to its callback");
    test_check(ctx.b_fires == 1 && ctx.b_status == AXL_CANCELLED,
               "TCP send_async: a close from a send callback retires the "
               "promoted send");

    /* No close here — on_tcp_closecb_a already closed the socket. */
    axl_loop_free(ctx.loop);
}

// ---------------------------------------------------------------------------
// The SYNC send must not block behind another caller's send.
//
// axl_tcp_send builds an ephemeral loop and runs it until its own send
// completes. If another caller's send already owns the transport, this one is
// queued — and the promotion that would start it is driven by the active
// send's completion source, which sits on the OTHER caller's loop. That loop
// is not running (this one is), so nothing advances and the wrapper waits out
// its entire timeout, 10 s by default, before reporting failure (design §6b
// defect 4).
//
// It refuses instead. A sync wrapper's contract is "done when I return", and
// it cannot honour that behind a send whose progress it does not drive; the
// caller learns immediately and can use the async API, which DOES queue. This
// is the sync shell declining to queue, not the transport refusing a send —
// axl_tcp_send_async still never refuses (design §3.2).
//
// Deterministic: `bg` is never run, so the first send necessarily still owns
// the transport. Timed, because the failure mode being pinned is the DELAY —
// a 3 s timeout burned to the last millisecond returns the same status.
// ---------------------------------------------------------------------------
static void
test_tcp_send_sync_behind_async(void)
{
    AxlTcp     *client = NULL;
    AxlLoop    *bg;
    TcpFlushCtx bgctx = { 0 };
    uint64_t    t0;
    uint64_t    elapsed_us;
    int         rc;
    static const char first[]  = "sync-behind-async-first";
    static const char second[] = "sync-behind-async-second";

    if (!axl_net_is_available()) {
        test_skip_n(2, "TCP sync send behind async (no network)");
        return;
    }
    if (axl_tcp_connect(AXL_TEST_ECHO_HOST, 9999, &client) != AXL_OK) {
        test_skip_n(2, "TCP sync send behind async (connect failed)");
        return;
    }

    bg = axl_loop_new();
    axl_tcp_send_async(client, first, sizeof(first) - 1, bg, NULL,
                       on_tcp_flush_cb, &bgctx);

    t0         = axl_time_get_us();
    rc         = axl_tcp_send(client, second, sizeof(second) - 1, 3000);
    elapsed_us = axl_time_get_us() - t0;

    test_check(rc == AXL_ERR,
               "TCP send (sync): refuses behind another caller's send");
    test_check(elapsed_us < 1000ULL * 1000ULL,
               "TCP send (sync): refusal is immediate, not a burnt timeout");

    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
    axl_loop_free(bg);
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
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
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
        axl_http_server_free(server, AXL_TEARDOWN_GRACEFUL);
        axl_printf("SKIP: Loop alloc failed\n");
        return;
    }

    ret = axl_http_server_start(server, loop);
    if (ret != 0) {
        axl_http_server_free(server, AXL_TEARDOWN_GRACEFUL);
        axl_loop_free(loop);
        axl_printf("SKIP: HTTP server attach failed\n");
        return;
    }

    //
    // Create client and make a request
    //
    client = axl_http_client_new();
    if (client == NULL) {
        axl_http_server_free(server, AXL_TEARDOWN_GRACEFUL);
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
    axl_http_server_free(server, AXL_TEARDOWN_GRACEFUL);
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

static int
test_dav_write_close(void *vctx, bool aborted)
{
    DavWriteCtx *w = vctx;
    axl_strlcpy(m_dav_last_put_path, w->target_path,
                sizeof(m_dav_last_put_path));
    m_dav_last_put_len     = w->written;
    m_dav_last_put_aborted = aborted;
    /* A target named "flush-fails*" stands in for a backend whose final
       flush could not make the bytes durable — every chunk was accepted
       and only the close fails. The integration test uses it to pin PUT
       answering 500 rather than 201. Deliberately NOT registered in the
       in-memory fs: a failed store must not become visible to PROPFIND. */
    bool flush_fails =
        axl_strncmp(w->target_path, "/flush-fails", 12) == 0;
    /* On clean EOF, register the new file in the in-memory fs. */
    if (!aborted && !flush_fails) {
        if (axl_hash_table_lookup(m_dav_fs, w->target_path) == NULL) {
            axl_hash_table_replace(m_dav_fs, axl_strdup(w->target_path),
                                   (void *)(uintptr_t)1);
        }
    }
    axl_free(w->target_path);
    axl_free(w);
    return flush_fails ? AXL_ERR : AXL_OK;
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
    AxlWsEvent event,
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
    AxlWsEvent      event,
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
        /* Oversized-frame guard (WS wedge regression): a payload whose framed
           size exceeds the per-connection outbound byte budget must be REJECTED
           (AXL_ERR) at axl_ws_send, not admitted and handed to the transport as
           one giant Transmit. Report the rc on serial so the harness can assert
           it, then keep replying normally to prove the server stays responsive. */
        if (frame_size == 8 && axl_memcmp(frame, "OVERSIZE", 8) == 0) {
            size_t   big = (600u * 1024u);   /* > WS_OUT_MAX_BYTES (512 KB) */
            uint8_t *buf = axl_malloc(big);
            if (buf != NULL) {
                axl_memset(buf, 'Z', big);
                int rc = axl_ws_send(conn, AXL_WS_BINARY, buf, big);
                axl_printf("WS-OVERSIZE-RC:%d\r\n", rc);
                axl_free(buf);
            } else {
                /* Distinct marker so the harness attributes a failed probe to
                   OOM, not to a fix regression (an absent RC line looks like
                   the reject silently vanished). */
                axl_printf("WS-OVERSIZE-OOM\r\n");
            }
            return 0;
        }
        /* Multi-chunk transport guard (WS wedge fix, Part B): a 200 KB frame is
           under the outbound budget (512 KB) but larger than the transport's
           per-Transmit chunk (32 KB), so axl_tcp_send_async must chunk-chain it
           (~7 Transmits) and still deliver the payload byte-exact. Fill with a
           position-dependent pattern the client verifies, and report the rc on
           serial. Guards correctness of the bounded-Transmit rewrite. */
        if (frame_size == 8 && axl_memcmp(frame, "BIGFRAME", 8) == 0) {
            size_t   big = (200u * 1024u);   /* > 32 KB chunk, < 512 KB budget */
            uint8_t *buf = axl_malloc(big);
            if (buf != NULL) {
                for (size_t i = 0; i < big; i++) {
                    buf[i] = (uint8_t)(i & 0xFF);
                }
                int rc = axl_ws_send(conn, AXL_WS_BINARY, buf, big);
                axl_printf("WS-BIGFRAME-RC:%d\r\n", rc);
                axl_free(buf);
            } else {
                axl_printf("WS-BIGFRAME-OOM\r\n");
            }
            return 0;
        }
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
    AxlWsEvent      event,
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
    AxlWsEvent      event,
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
    AxlWsEvent      event,
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
    AxlWsEvent      event,
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
    AxlWsEvent      event,
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
    AxlWsEvent      event,
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
        axl_printf("ERROR: TLS not available\n");
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
        axl_http_server_free(s, AXL_TEARDOWN_GRACEFUL);
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
        axl_printf("ERROR: TLS not available\n");
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
        axl_printf("ERROR: TLS not available\n");
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

// ---------------------------------------------------------------------------
// "serve-tls-ws-close-pendtx-driver" — regression for the graceful-close wedge
// (docs/axl-sdk-console-reshape-snapshot-wedge-handoff.md in softbmc).
//
// A resident HTTPS+WS server pumped by axl_loop_attach_driver (TPL_CALLBACK)
// sends a LARGE frame to each /ws-console client on connect. A client that reads
// the 101, then sends a WS CLOSE WITHOUT draining, leaves the server holding
// un-flushed outbound TCP data when process_websocket_data reset_connections the
// conn at raised TPL. Before the fix, the graceful EFI_TCP4.Close() then spins in
// firmware flushing that send buffer (its FIN/ACK needs the MNP timer to fire
// below TPL_CALLBACK, which the pump holds) -> the loop hard-wedges (curl 000).
// With the fix (abortive close at raised TPL) the RST discards the buffer and the
// loop keeps serving. Driven by test-tcp-close-pendtx-driver-qemu.sh.
// ---------------------------------------------------------------------------

#define WS_PENDTX_FRAME_BYTES  (400u * 1024u)   /* > a socket window, < WS_OUT_MAX_BYTES */

// On connect, push a large frame so the client's non-drain leaves the server
// with un-flushed TX when it later resets the connection on the WS CLOSE.
static int
on_ws_pendtx(
    AxlWsConn  *conn,
    AxlWsEvent  event,
    const void *frame,
    size_t      frame_size,
    void       *data)
{
    (void)frame;
    (void)frame_size;
    (void)data;
    if (event == AXL_WS_CONNECT) {
        uint8_t *buf = axl_malloc(WS_PENDTX_FRAME_BYTES);
        if (buf != NULL) {
            axl_memset(buf, 'Z', WS_PENDTX_FRAME_BYTES);
            (void)axl_ws_send(conn, AXL_WS_BINARY, buf, WS_PENDTX_FRAME_BYTES);
            axl_free(buf);
        }
    }
    return 0;
}

static int
run_serve_tls_ws_close_pendtx_driver_mode(void)
{
    if (!axl_tls_available()) {
        axl_printf("ERROR: TLS not available\n");
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
    axl_http_server_add_websocket_ex(s, "/ws-console", on_ws_pendtx, NULL,
                                     AXL_ROUTE_NO_AUTH);

    if (axl_http_server_start(s, loop) != 0) {
        axl_printf("ERROR: failed to start server on loop\n");
        return -1;
    }

    /* 50 ms — SoftBMC's console-mirror driver cadence. */
    if (axl_loop_attach_driver(loop, 50) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    axl_printf("HTTPS+WS pending-TX close server (resident driver-tick loop) "
               "on port 8443\n");
    axl_printf("READY\n");

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
        axl_printf("ERROR: TLS not available\n");
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

    /* With a Shell.efi staged AND OVMF's FV Shell present, axl_shell_sources
       must report BOTH independently — the case the file-first axl_shell_locate
       collapses to FILE, hiding the FV. Emit the flags so the harness can pin
       the un-masking (see test-shell-coexist-qemu.sh). */
    AxlShellSources src = { 0 };
    if (axl_shell_sources(&src) != AXL_OK) {
        src = (AxlShellSources){ 0 };   /* query failed -> report no sources */
    }
    axl_printf("SOURCES:file=%d,fv=%d,fvn=%u\n",
               (int)src.file, (int)src.fv, (unsigned)src.fv_count);

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
// FV-embedded Shell coexistence — "serve-shell-fv-coexist". The no-file-staged
// counterpart of serve-shell-coexist: instead of axl_shell_launch (which needs
// a Shell.efi file), report axl_shell_locate() and launch the firmware-embedded
// Shell out of a Firmware Volume via axl_shell_launch_fv. Under OVMF/AAVMF the
// Shell lives in a readable FV (no file staged), so this is the real round-trip
// proof: a 200 returned WHILE the FV Shell holds the foreground means
// LoadImage+StartImage transferred control to the FV-embedded image, and the
// driver-tick timer keeps pumping the background HTTP server underneath it.
// ---------------------------------------------------------------------------

static int
run_serve_shell_fv_coexist_mode(void)
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

    if (axl_loop_attach_driver(loop, 10) != AXL_OK) {
        axl_printf("ERROR: attach_driver failed\n");
        return -1;
    }

    /* Report where a Shell is locatable so the harness can assert FIRMWARE. */
    axl_printf("LOCATE=%d\n", (int)axl_shell_locate());

    axl_printf("HTTP server (driver-tick) on 8080; launching FV Shell\n");
    axl_printf("READY\n");

    /* StartImage(FV-embedded Shell) — blocks. The HTTP server keeps serving
       off the timer the whole time. -nostartup avoids startup.nsh recursion. */
    int exit_code = 0;
    int rc = axl_shell_launch_fv("-nostartup", &exit_code);
    if (rc != AXL_OK) {
        /* No readable FV carries the Shell on this firmware: report so the
           harness can SKIP-balance rather than read it as a coexistence
           failure. */
        axl_printf("NO_FV_SHELL\n");
        return 0;
    }

    /* Reached only if the Shell exits (it won't in the unattended test). */
    axl_loop_detach_driver(loop);
    axl_printf("FV_SHELL_EXITED %d\n", exit_code);
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
        axl_printf("ERROR: TLS not available\n");
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
    if (axl_console_mirror_install(&cfg, &m) != AXL_OK) {
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

/* Fake EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL standing in for a firmware StdErr
   that is genuinely a distinct protocol instance from ConOut (some real
   firmware wires a separate error-device splitter). Used only to give
   axl_console_mirror_install/uninstall a non-NULL, non-aliased StdErr to
   save/restore in the regression test below -- real no-op methods, NOT a
   zeroed struct: axl_console_mirror_uninstall's own axl_info() call fires
   AFTER restoring gST->StdErr, so log_dispatch's OutputString/SetAttribute
   calls land on this object for real and must not be NULL function
   pointers. */
static EFI_STATUS EFIAPI
fake_stderr_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This; (void)ext;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This; (void)String;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This; (void)String;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
                       UINTN *Columns, UINTN *Rows)
{
    (void)This; (void)ModeNumber;
    if (Columns != NULL) {
        *Columns = 80;
    }
    if (Rows != NULL) {
        *Rows = 25;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber)
{
    (void)This; (void)ModeNumber;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{
    (void)This; (void)Attribute;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
    (void)This;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_set_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
    (void)This; (void)Column; (void)Row;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
fake_stderr_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
    (void)This; (void)Visible;
    return EFI_SUCCESS;
}

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL g_fake_stderr = {
    .Reset             = fake_stderr_reset,
    .OutputString      = fake_stderr_output_string,
    .TestString        = fake_stderr_test_string,
    .QueryMode         = fake_stderr_query_mode,
    .SetMode           = fake_stderr_set_mode,
    .SetAttribute      = fake_stderr_set_attribute,
    .ClearScreen       = fake_stderr_clear_screen,
    .SetCursorPosition = fake_stderr_set_cursor,
    .EnableCursor      = fake_stderr_enable_cursor,
    .Mode              = NULL,
};

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

    int rc = axl_console_mirror_install(&cfg, &m);

    bool dbl_blocked = false;
    bool got_clear = false, got_cursor = false, got_sgr = false, got_text = false;
    bool got_size = false, got_stderr = false;
    bool inj_up = false, inj_x = false, inj_f2 = false;

    if (rc == AXL_OK) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = gST->ConOut;
        EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *in  = gST->ConIn;

        AxlConsoleMirror *m2 = NULL;
        dbl_blocked = (axl_console_mirror_install(&cfg, &m2) == AXL_ERR);

        /* QueryMode on the CURRENT mode must report the remote size (this is
           what `edit` queries to lay itself out). */
        UINTN qc = 0, qr = 0;
        out->QueryMode(out, (UINTN)out->Mode->Mode, &qc, &qr);
        got_size = (qc == 80 && qr == 25);

        out->ClearScreen(out);
        out->SetCursorPosition(out, 4, 2);     /* col 4, row 2 -> ESC[3;5H */
        out->SetAttribute(out, EFI_LIGHTRED);   /* fg 12 -> SGR 91 */
        out->OutputString(out, (CHAR16 *)u"Hi");

        /* Real production path (axl_printerr -> gST->StdErr, falling back to
           ConOut only if StdErr is NULL): proves stderr text reaches the
           mirror sink, not just ConOut. */
        axl_printerr("StdErrMark");

        got_clear  = (axl_strstr(g_cm_cap, "\x1b[2J\x1b[H") != NULL);
        got_cursor = (axl_strstr(g_cm_cap, "\x1b[3;5H") != NULL);
        got_sgr    = (axl_strstr(g_cm_cap, "\x1b[0;91;40m") != NULL);
        got_text   = (axl_strstr(g_cm_cap, "Hi") != NULL);
        got_stderr = (axl_strstr(g_cm_cap, "StdErrMark") != NULL);

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

    /* Regression test for the StdErr save/restore bug: simulate firmware
       where StdErr is a genuinely distinct protocol instance from ConOut
       (some real firmware wires a separate error-device splitter) using
       g_fake_stderr. Prove install repoints BOTH to the (shared) wrapper
       and uninstall restores the EXACT original StdErr pointer -- not
       silently coerced to ConOut's original, which is what a naive
       "gST->StdErr = orig_conout" restore does (invisible on firmware
       where StdErr already aliases ConOut, which is why this needs a
       synthetic distinct instance to catch). Its own install/uninstall
       cycle so it can't perturb the assertions above. */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *real_conout_before = gST->ConOut;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *real_stderr_before = gST->StdErr;
    gST->StdErr = &g_fake_stderr;

    bool stderr_wrapped = false, stderr_restored = false;
    AxlConsoleMirror *m3 = NULL;
    if (axl_console_mirror_install(&cfg, &m3) == AXL_OK) {
        stderr_wrapped = (gST->StdErr == gST->ConOut
                          && gST->StdErr != &g_fake_stderr);
        axl_console_mirror_uninstall(m3);
        stderr_restored = (gST->StdErr == &g_fake_stderr
                           && gST->ConOut == real_conout_before);
    }
    gST->StdErr = real_stderr_before;  /* true original back, regardless */

    /* Console restored: axl_printf reaches the real serial console now. */
    axl_printf("MIRROR_SELFTEST: install=%d\n", rc == AXL_OK);
    axl_printf("MIRROR_SELFTEST: dbl_install_blocked=%d\n", dbl_blocked);
    axl_printf("MIRROR_SELFTEST: query_size=%d\n", got_size);
    axl_printf("MIRROR_SELFTEST: clear=%d\n", got_clear);
    axl_printf("MIRROR_SELFTEST: cursor=%d\n", got_cursor);
    axl_printf("MIRROR_SELFTEST: sgr=%d\n", got_sgr);
    axl_printf("MIRROR_SELFTEST: text=%d\n", got_text);
    axl_printf("MIRROR_SELFTEST: stderr_mirrored=%d\n", got_stderr);
    axl_printf("MIRROR_SELFTEST: stderr_wrap=%d\n", stderr_wrapped);
    axl_printf("MIRROR_SELFTEST: stderr_restore=%d\n", stderr_restored);
    axl_printf("MIRROR_SELFTEST: inject_up=%d\n", inj_up);
    axl_printf("MIRROR_SELFTEST: inject_printable=%d\n", inj_x);
    axl_printf("MIRROR_SELFTEST: inject_key_f2=%d\n", inj_f2);

    bool all = (rc == AXL_OK) && dbl_blocked && got_size && got_clear
               && got_cursor && got_sgr && got_text && got_stderr
               && stderr_wrapped && stderr_restored
               && inj_up && inj_x && inj_f2;
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

    if (axl_console_mirror_install(&cfg, &g_edit_mirror) != AXL_OK) {
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
    AxlWsEvent      event,
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
    if (axl_console_mirror_install(&cfg, &g_p1_mirror) != AXL_OK) {
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
            axl_printf("ERROR: TLS not available\n");
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
            axl_http_server_free(s, AXL_TEARDOWN_GRACEFUL);
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
        axl_printf("ERROR: TLS not available\n");
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

    /* STRICT, and until now nothing checked it. The docstring promises RFC
       8259 on the grounds that a request body comes from a client that may be
       hostile — but "{not-json" above is refused by EVERY dialect, so it
       cannot tell strict from liberal. This body is valid JSON5 and invalid
       JSON, which is the only input that can. */
    {
        const char    *json5 = "{/*c*/ name: 'axl', port: 0x23, }";
        AxlHttpRequest j5_req = { 0 };
        AxlJsonReader  j5_reader = { 0 };

        j5_req.body      = json5;
        j5_req.body_size = axl_strlen(json5);
        test_check(!axl_http_request_get_json(&j5_req, &j5_reader),
                   "request_get_json: refuses JSON5 — a request body is "
                   "strict RFC 8259, whatever dialect the caller could "
                   "have named");
    }
}

/* axl_http_response_get_json — the client-side mirror.
 *
 * Same contract as the request-side helper and the same reason for it: a
 * response body arrives from a peer this process does not control. The point
 * of the function existing at all is that the EASY call is the strict one, so
 * the JSON5 row is the one that matters — the rest is NULL hygiene. */
static void
test_http_response_get_json(void)
{
    /* Happy path, and the reader really works afterwards. */
    {
        const char           *body = "{\"port\":9090,\"name\":\"axl\"}";
        AxlHttpClientResponse resp = { 0 };
        AxlJsonReader         r = { 0 };
        int64_t               port = -1;

        resp.body      = (void *)(size_t)body;
        resp.body_size = axl_strlen(body);
        test_check(axl_http_response_get_json(&resp, &r),
                   "response_get_json: parses a valid JSON body");
        test_check(axl_json_get_int(&r, "port", &port) && port == 9090,
                   "response_get_json: the reader it fills is usable");
        axl_json_free(&r);
    }

    /* THE row this function exists for. */
    {
        const char           *json5 = "{/*c*/ name: 'axl', port: 0x23, }";
        AxlHttpClientResponse resp = { 0 };
        AxlJsonReader         r = { 0 };

        resp.body      = (void *)(size_t)json5;
        resp.body_size = axl_strlen(json5);
        test_check(!axl_http_response_get_json(&resp, &r),
                   "response_get_json: refuses JSON5 from the network, which "
                   "an AXL_JSON_RELAXED parse would have accepted");
    }

    /* Malformed under every dialect. */
    {
        const char           *bad = "{not-json";
        AxlHttpClientResponse resp = { 0 };
        AxlJsonReader         r = { 0 };

        resp.body      = (void *)(size_t)bad;
        resp.body_size = axl_strlen(bad);
        test_check(!axl_http_response_get_json(&resp, &r),
                   "response_get_json: rejects malformed JSON");
    }

    /* NULL and empty. A 204 No Content has a NULL body and must not be a
       crash — it is the ordinary outcome of a DELETE. */
    {
        AxlHttpClientResponse empty = { 0 };
        AxlJsonReader         r = { 0 };

        test_check(!axl_http_response_get_json(&empty, &r),
                   "response_get_json: an empty body (204) is false, "
                   "not a crash");
        test_check(!axl_http_response_get_json(NULL, &r),
                   "response_get_json: NULL response returns false");
        {
            const char           *ok = "{\"a\":1}";
            AxlHttpClientResponse resp = { 0 };

            resp.body      = (void *)(size_t)ok;
            resp.body_size = axl_strlen(ok);
            test_check(!axl_http_response_get_json(&resp, NULL),
                       "response_get_json: NULL out returns false");
        }
    }
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

    axl_http_server_free(s, AXL_TEARDOWN_GRACEFUL);
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

    /* get_dhcp_lease_by_mac: NULL mac / NULL out return on the guard before
       any protocol call. The live by-MAC read + unknown-MAC rejection are
       exercised in net-diag mode. */
    AxlDhcpLease lease_guard;
    static const uint8_t mac_guard[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    test_check(axl_net_get_dhcp_lease_by_mac(NULL, &lease_guard) == AXL_ERR,
               "dhcp-lease-by-mac: NULL mac -> AXL_ERR");
    test_check(axl_net_get_dhcp_lease_by_mac(mac_guard, NULL) == AXL_ERR,
               "dhcp-lease-by-mac: NULL out -> AXL_ERR");

    /* set_static_ip_by_mac: NULL mac / ip / netmask return on the guard
       before any protocol call. A valid MAC would reconfigure live firmware
       under this test binary (feedback_uefi_firmware_test_hazards), so the
       unknown-MAC no-fallback contract is exercised in net-diag mode instead
       (mirrors dhcp-lease-by-mac's unknown-MAC assertion), and the positive
       round-trip -- does it configure the RIGHT NIC -- rides netload's
       two-NIC BUG B integration boot (test-netload-qemu.sh), where driving
       static config is already the point. */
    static const uint8_t sip_ip_guard[4]      = { 192, 168, 1, 100 };
    static const uint8_t sip_netmask_guard[4] = { 255, 255, 255, 0 };
    test_check(axl_net_set_static_ip_by_mac(NULL, sip_ip_guard, sip_netmask_guard, NULL) == AXL_ERR,
               "set-static-ip-by-mac: NULL mac -> AXL_ERR");
    test_check(axl_net_set_static_ip_by_mac(mac_guard, NULL, sip_netmask_guard, NULL) == AXL_ERR,
               "set-static-ip-by-mac: NULL ip -> AXL_ERR");
    test_check(axl_net_set_static_ip_by_mac(mac_guard, sip_ip_guard, NULL, NULL) == AXL_ERR,
               "set-static-ip-by-mac: NULL netmask -> AXL_ERR");

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
    AxlNetSntpResult sr;
    test_check(axl_net_sntp_query(NULL, 0, 1000, &sr) == AXL_ERR,
               "sntp_query: NULL server -> AXL_ERR");
    test_check(axl_net_sntp_query("10.0.2.2", 0, 1000, NULL) == AXL_ERR,
               "sntp_query: NULL out -> AXL_ERR");

    /* arp_list: NULL count returns on the guard before any protocol call. The
       live cache read is exercised in net-diag mode. */
    AxlArpEntry ae[2];
    test_check(axl_net_arp_list(0, ae, 2, NULL) == AXL_ERR,
               "arp_list: NULL count -> AXL_ERR");

    /* Out-of-range nic must ERROR, not answer for a different NIC. Safe
       negative: returns on our own bounds check before CreateChild. */
    AxlArpEntry ae_oob[2];
    size_t      ae_oob_n = 0;
    test_check(axl_net_arp_list(SIZE_MAX - 1, ae_oob, 2, &ae_oob_n) == AXL_ERR,
               "arp_list: out-of-range nic -> AXL_ERR");

    /* get_link_stats: NULL out returns on the guard. The live read is in
       net-diag mode. */
    test_check(axl_net_get_link_stats(0, NULL) == AXL_ERR,
               "link_stats: NULL out -> AXL_ERR");

    /* Out-of-range nic must ERROR, not clamp. The `>= count -> 0` clamp is the
       wrong-NIC bug: it silently answered for NIC 0. Safe negative -- returns
       on our own bounds check before any firmware call. */
    AxlNetLinkStats ls_oob;
    test_check(axl_net_get_link_stats(SIZE_MAX - 1, &ls_oob) == AXL_ERR,
               "link_stats: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");
}

/* NIC registry contract, observed through the public API (tests never include
   src/net/axl-net-internal.h). A standalone AxlTestNet boot with no NIC (e.g.
   run-qemu.sh without --net) takes the SKIP branch, balanced against the
   populated-path check count per the balancer rule. The ratcheted
   test-axl.sh harness always attaches one live virtio NIC for AxlTestNet
   (test_add_network_with_echo), so under that harness this call already
   exercises the populated path directly. */
static void
test_nic_registry_contract(void)
{
    /* NULL count returns on the guard, at any NIC count. */
    test_check(axl_net_list_interfaces(NULL, NULL) == AXL_ERR,
               "list_interfaces: NULL count -> AXL_ERR");

    size_t n = 0;
    test_check(axl_net_list_interfaces(NULL, &n) == AXL_OK,
               "list_interfaces: count query -> AXL_OK");

    if (n == 0) {
        axl_printf("SKIP: list_interfaces MACs unique (no NIC)\n");
        axl_printf("SKIP: list_interfaces count == filled (no NIC)\n");
        return;
    }

    AxlNetInterface *ifs = axl_calloc(n, sizeof *ifs);
    if (ifs == NULL) {
        axl_printf("SKIP: list_interfaces MACs unique (alloc failed)\n");
        axl_printf("SKIP: list_interfaces count == filled (alloc failed)\n");
        return;
    }
    size_t filled = n;
    int rc = axl_net_list_interfaces(ifs, &filled);

    /* Dedup: every MAC in the listing is unique. Pre-registry this fails --
       a single NIC repeats across its SNP child handles. */
    bool unique = (rc == AXL_OK);
    for (size_t i = 0; unique && i < filled; i++) {
        for (size_t j = i + 1; j < filled; j++) {
            if (axl_memcmp(ifs[i].mac, ifs[j].mac, 6) == 0) {
                unique = false;
                break;
            }
        }
    }
    test_check(unique, "list_interfaces: every MAC is unique (one row per NIC)");

    /* The count query must agree with what the fill actually produces. */
    test_check(rc == AXL_OK && filled == n,
               "list_interfaces: count query == filled count");

    axl_free(ifs);
}

/* axl_net_list_interfaces_alloc: the allocating counterpart of
   axl_net_list_interfaces, promoted from netload's local count/alloc/
   re-query helper (formerly duplicated 4x across netload.c and netinfo.c).
   Same live-NIC assumption and SKIP-balance shape as
   test_nic_registry_contract above -- the ratcheted test-axl.sh harness
   always attaches one live virtio NIC, so the populated path is exercised
   directly under that harness. */
static void
test_net_list_interfaces_alloc_contract(void)
{
    /* NULL-arg negatives are safe at any NIC count -- they return on our
       own guard before axl_net_list_interfaces is ever called. */
    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    test_check(axl_net_list_interfaces_alloc(NULL, &count) == AXL_ERR,
               "list_interfaces_alloc: NULL out -> AXL_ERR");
    test_check(axl_net_list_interfaces_alloc(&ifs, NULL) == AXL_ERR,
               "list_interfaces_alloc: NULL count -> AXL_ERR");

    /* Zero interfaces is a normal result, not a failure -- AXL_OK either way. */
    ifs = NULL;
    count = 0;
    int rc = axl_net_list_interfaces_alloc(&ifs, &count);
    test_check(rc == AXL_OK, "list_interfaces_alloc: AXL_OK");

    if (count == 0) {
        axl_printf("SKIP: list_interfaces_alloc non-NULL array (no NIC)\n");
        axl_printf("SKIP: list_interfaces_alloc count matches list_interfaces (no NIC)\n");
        return;
    }

    test_check(ifs != NULL, "list_interfaces_alloc: non-NULL array when count > 0");

    /* Cross-check against the query-only axl_net_list_interfaces -- both
       walk the same NIC registry, so the counts must agree. */
    size_t n2 = 0;
    test_check(axl_net_list_interfaces(NULL, &n2) == AXL_OK && n2 == count,
               "list_interfaces_alloc: count matches list_interfaces");

    axl_free(ifs);
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
// MAC parse / format tests
// ---------------------------------------------------------------------------

static void
test_mac_format_parse(void)
{
    uint8_t mac[6];
    char    buf[24];

    /* Format */
    uint8_t addr[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    test_check(axl_mac_format(addr, buf, sizeof buf) == AXL_OK,
               "mac format: aa:bb:cc:dd:ee:ff");
    test_check(axl_strcmp(buf, "aa:bb:cc:dd:ee:ff") == 0,
               "mac format: string correct");

    uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };
    test_check(axl_mac_format(zero, buf, sizeof buf) == AXL_OK
               && axl_strcmp(buf, "00:00:00:00:00:00") == 0,
               "mac format: all zeros");

    /* Format small buffer / NULL safety */
    test_check(axl_mac_format(addr, buf, 17) == AXL_ERR,
               "mac format: buffer one byte too small");
    test_check(axl_mac_format(NULL, buf, sizeof buf) == AXL_ERR,
               "mac format: NULL mac");
    test_check(axl_mac_format(addr, NULL, sizeof buf) == AXL_ERR,
               "mac format: NULL buf");
    test_check(axl_mac_format(addr, buf, 0) == AXL_ERR,
               "mac format: zero size");

    /* Parse valid: lowercase, canonical two-digit octets */
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:ff", mac) == AXL_OK,
               "mac parse: lowercase");
    test_check(mac[0] == 0xaa && mac[1] == 0xbb && mac[2] == 0xcc
               && mac[3] == 0xdd && mac[4] == 0xee && mac[5] == 0xff,
               "mac parse: octets correct");

    /* Parse valid: uppercase and mixed case (case-insensitive) */
    test_check(axl_mac_parse("AA:BB:CC:DD:EE:FF", mac) == AXL_OK
               && mac[0] == 0xaa && mac[5] == 0xff,
               "mac parse: uppercase");
    test_check(axl_mac_parse("Aa:bB:Cc:dD:eE:fF", mac) == AXL_OK
               && mac[0] == 0xaa && mac[5] == 0xff,
               "mac parse: mixed case");

    /* Parse valid: 1-digit octets (matches the CLI --mac flag's long-
       standing behavior -- each octet accepts 1 or 2 hex digits). */
    test_check(axl_mac_parse("1:2:3:4:5:6", mac) == AXL_OK
               && mac[0] == 1 && mac[1] == 2 && mac[2] == 3
               && mac[3] == 4 && mac[4] == 5 && mac[5] == 6,
               "mac parse: single-digit octets");

    /* Parse valid: all zeros */
    test_check(axl_mac_parse("00:00:00:00:00:00", mac) == AXL_OK
               && mac[0] == 0 && mac[5] == 0,
               "mac parse: all zeros");

    /* Reject: wrong separator */
    test_check(axl_mac_parse("aa-bb-cc-dd-ee-ff", mac) == AXL_ERR,
               "mac parse: reject hyphen separator");
    test_check(axl_mac_parse("aa.bb.cc.dd.ee.ff", mac) == AXL_ERR,
               "mac parse: reject dot separator");
    test_check(axl_mac_parse("aabbccddeeff", mac) == AXL_ERR,
               "mac parse: reject bare hex run (no separator)");

    /* Reject: wrong octet count */
    test_check(axl_mac_parse("aa:bb:cc:dd:ee", mac) == AXL_ERR,
               "mac parse: reject 5 octets");
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:ff:00", mac) == AXL_ERR,
               "mac parse: reject 7 octets");

    /* Reject: non-hex digit */
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:zz", mac) == AXL_ERR,
               "mac parse: reject non-hex digit");
    test_check(axl_mac_parse("gg:bb:cc:dd:ee:ff", mac) == AXL_ERR,
               "mac parse: reject non-hex first octet");

    /* Reject: octet overflows a byte (3 hex digits) */
    test_check(axl_mac_parse("aaa:bb:cc:dd:ee:ff", mac) == AXL_ERR,
               "mac parse: reject 3-digit octet");

    /* Reject: trailing garbage after the sixth octet */
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:ff:", mac) == AXL_ERR,
               "mac parse: reject trailing colon");
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:ffX", mac) == AXL_ERR,
               "mac parse: reject trailing garbage");

    /* Reject: empty / NULL */
    test_check(axl_mac_parse("", mac) == AXL_ERR,
               "mac parse: reject empty string");
    test_check(axl_mac_parse(NULL, mac) == AXL_ERR,
               "mac parse: NULL str");
    test_check(axl_mac_parse("aa:bb:cc:dd:ee:ff", NULL) == AXL_ERR,
               "mac parse: NULL mac");

    /* Format/parse roundtrip */
    test_check(axl_mac_parse("12:34:56:78:9a:bc", mac) == AXL_OK, "mac roundtrip: parse");
    test_check(axl_mac_format(mac, buf, sizeof buf) == AXL_OK
               && axl_strcmp(buf, "12:34:56:78:9a:bc") == 0,
               "mac roundtrip: format matches input");
}

static void
test_ipv4_parse_cidr(void)
{
    uint8_t oct[4], mask[4];
    bool    hp;

    /* Bare address: octets set, had_prefix false, mask untouched. */
    axl_memset(mask, 0xAB, 4);
    test_check(axl_ipv4_parse_cidr("10.0.0.5", oct, mask, &hp) == AXL_OK,
               "cidr: bare parses");
    test_check(oct[0] == 10 && oct[1] == 0 && oct[2] == 0 && oct[3] == 5,
               "cidr: bare octets");
    test_check(hp == false, "cidr: bare has no prefix");
    test_check(mask[0] == 0xAB, "cidr: bare leaves mask untouched");

    /* /24 -> 255.255.255.0 */
    test_check(axl_ipv4_parse_cidr("192.168.1.1/24", oct, mask, &hp) == AXL_OK,
               "cidr: /24 parses");
    test_check(hp == true, "cidr: /24 has prefix");
    test_check(mask[0] == 255 && mask[1] == 255 && mask[2] == 255 && mask[3] == 0,
               "cidr: /24 mask");

    /* /0 -> 0.0.0.0, /32 -> 255.255.255.255, /1 -> 128.0.0.0 */
    test_check(axl_ipv4_parse_cidr("1.2.3.4/0", oct, mask, &hp) == AXL_OK
               && mask[0] == 0 && mask[3] == 0, "cidr: /0 mask all-zero");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/32", oct, mask, &hp) == AXL_OK
               && mask[0] == 255 && mask[3] == 255, "cidr: /32 mask all-ones");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/1", oct, mask, &hp) == AXL_OK
               && mask[0] == 128 && mask[1] == 0, "cidr: /1 mask 128.0.0.0");

    /* Rejections. */
    test_check(axl_ipv4_parse_cidr("1.2.3.4/33", oct, mask, &hp) == AXL_ERR,
               "cidr: /33 rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/", oct, mask, &hp) == AXL_ERR,
               "cidr: trailing slash rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/x", oct, mask, &hp) == AXL_ERR,
               "cidr: non-numeric prefix rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/24x", oct, mask, &hp) == AXL_ERR,
               "cidr: digits-then-garbage prefix rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/4294967328", oct, mask, &hp) == AXL_ERR,
               "cidr: 10-digit prefix rejected (no wrap)");
    test_check(axl_ipv4_parse_cidr("255.255.255.2555/24", oct, mask, &hp) == AXL_ERR,
               "cidr: over-long address rejected");
    test_check(axl_ipv4_parse_cidr("256.0.0.1/24", oct, mask, &hp) == AXL_ERR,
               "cidr: bad octet rejected");
    test_check(axl_ipv4_parse_cidr(NULL, oct, mask, &hp) == AXL_ERR,
               "cidr: NULL str rejected");
    test_check(axl_ipv4_parse_cidr("1.2.3.4/24", NULL, mask, &hp) == AXL_ERR,
               "cidr: NULL octets rejected");

    /* had_prefix may be NULL. */
    test_check(axl_ipv4_parse_cidr("8.8.8.8/8", oct, mask, NULL) == AXL_OK
               && mask[0] == 255 && mask[1] == 0, "cidr: NULL had_prefix ok");
}

// ---------------------------------------------------------------------------
// axl_net_driver_is_ipxe — filename heuristic (pure predicate, no network)
// ---------------------------------------------------------------------------

static void
test_net_driver_is_ipxe(void)
{
    /* Recognized: the real driver names axl_net_ensure_drivers /
       axl_net_try_driver actually see on staged volumes (see
       net_drivers_ipxe[] in axl-net-dhcp.c), a full path, a bare
       mid-word occurrence, and case variants — case folding is
       ASCII-only, same as the axl_strcasestr this delegates to. */
    test_check(axl_net_driver_is_ipxe("ipxe-intel.efi"),
               "is_ipxe: ipxe-intel.efi recognized");
    test_check(axl_net_driver_is_ipxe("ipxe-all.efidrv"),
               "is_ipxe: ipxe-all.efidrv recognized");
    test_check(axl_net_driver_is_ipxe("ipxe-broadcom.efi"),
               "is_ipxe: ipxe-broadcom.efi recognized");
    test_check(axl_net_driver_is_ipxe("IPXE.EFI"),
               "is_ipxe: all-caps recognized");
    test_check(axl_net_driver_is_ipxe("Ipxe-Intel.Efi"),
               "is_ipxe: mixed case recognized");
    test_check(axl_net_driver_is_ipxe("fs0:\\drivers\\x64\\ipxe-intel.efi"),
               "is_ipxe: full path recognized");
    test_check(axl_net_driver_is_ipxe("myIPXEdriver.efi"),
               "is_ipxe: substring mid-word recognized");

    /* Not recognized: real non-iPXE driver names, and a deliberate near-miss
       that shares every letter of "ipxe" but not the contiguous substring
       (i-p-x-e in that exact order) — pins this is a substring match, not
       a letter-set / anagram check. */
    test_check(!axl_net_driver_is_ipxe("Rtk.efi"),
               "is_ipxe: Rtk.efi not recognized");
    test_check(!axl_net_driver_is_ipxe("RtkUndiDxe.efi"),
               "is_ipxe: RtkUndiDxe.efi not recognized");
    test_check(!axl_net_driver_is_ipxe("UsbRndis.efi"),
               "is_ipxe: UsbRndis.efi not recognized");
    test_check(!axl_net_driver_is_ipxe("NetworkCommon.efi"),
               "is_ipxe: NetworkCommon.efi not recognized");
    test_check(!axl_net_driver_is_ipxe("pixel.efi"),
               "is_ipxe: pixel.efi (same letters, wrong order) not recognized");
    test_check(!axl_net_driver_is_ipxe(""),
               "is_ipxe: empty string not recognized");

    /* NULL-safe: axl_strcasestr is NULL-safe and this predicate inherits it
       rather than adding its own guard — a bare "does this string look
       like iPXE" question has an honest false answer for "no string". */
    test_check(!axl_net_driver_is_ipxe(NULL),
               "is_ipxe: NULL -> false");
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
                              msg, axl_strlen(msg), 5000,
                              rx_buf, sizeof(rx_buf) - 1, &rx_len);

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
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
    }
    return (crc == AXL_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// §7 spike — "tcp-multi-tx <ip> <port>": submit FOUR concurrent
// EFI_TCP4.Transmit tokens on ONE connection and record what the firmware
// actually does with them.
//
// docs/AXL-Tcp-Queue-Design.md §7 asks why AXL queues sends at all, when
// EFI_TCP4.Transmit is specified as "Queue outgoing data into the transmit
// queue". The one-send-in-flight limit is OURS: struct AxlTcp holds a single
// EFI_TCP4_IO_TOKEN. The design's stated reason for keeping it is that the
// spec does not GUARANTEE the completion order of multiple outstanding
// tokens -- caution that had never been measured. This measures it.
//
// RAW EFI_TCP4 deliberately, not AxlTcp: routing this through AXL's queue
// would measure AXL, which is the thing in question.
//
// Three observations, because the interesting failure modes differ:
//   1. the status each Transmit returns AT SUBMIT (does the firmware accept
//      four outstanding tokens at all, or reject after the first?)
//   2. the order the completion events signal, and each token's final Status
//   3. the order the BYTES arrive, which the events cannot answer -- the host
//      recorder (test/integration/tcp-multi-tx-server.py) reports that half.
//
// Each token carries a distinct repeated marker ('A'..'D') so the receiver
// can reconstruct order from the stream alone.
// ---------------------------------------------------------------------------

/* Concurrency is a KNOB, not a rebuild. The 32-token / 2 MB run cited in
   AXL-Tcp-Queue-Design §2a was originally taken by editing this constant and
   rebuilding, which made the cited evidence unreproducible from the committed
   harness -- the same failure as the §4.1 printf rig that had to be rebuilt
   from scratch. MTX_TOKENS_MAX only sizes the arrays. */
#define MTX_TOKENS_MAX   32u
#define MTX_TOKENS       4u
/* 64 KB per token, above EFI_TCP4's 32 KB default send buffer, so a token
   cannot be absorbed and retired between two submits -- see MTX-OUTSTANDING. */
#define MTX_CHUNK_BYTES  (64u * 1024u)
#define MTX_WAIT_MS      15000u

typedef struct {
    EFI_TCP4_IO_TOKEN       token;
    EFI_TCP4_TRANSMIT_DATA  tx;
    uint8_t                *buf;
    EFI_STATUS              submit;   /* status Transmit returned at submit */
} MtxSlot;

/* Completion order, recorded BY THE FIRMWARE'S OWN SIGNAL rather than by a
   poller.
 *
 * The first version of this polled CheckEvent over slots 0..3 in index order
 * every 10 ms, and therefore could not observe an out-of-order completion at
 * all: any two tokens that retired within the same 10 ms window were recorded
 * ascending because that is the order the scan visited them. It printed
 * inorder=yes for a reason that had nothing to do with the firmware -- the
 * measurement equivalent of a test that passes for the wrong reason.
 *
 * So the transmit tokens use EVT_NOTIFY_SIGNAL events instead: the firmware
 * calls this the moment it retires a token, at its own TPL, in its own order.
 * (CheckEvent cannot be used on a NOTIFY_SIGNAL event, which is fine -- the
 * recorded order IS the observation. The connect/close tokens keep plain
 * events: nothing is being ordered there.)
 *
 * Records only. No printing: this runs at TPL_CALLBACK. */
static volatile unsigned mtx_done_count;
static volatile unsigned mtx_done_order[MTX_TOKENS_MAX];

static void EFIAPI
mtx_on_transmit_done(
    EFI_EVENT event,
    void     *context
    )
{
    (void)event;
    unsigned index = (unsigned)(uintptr_t)context;
    if (mtx_done_count < MTX_TOKENS_MAX) {
        mtx_done_order[mtx_done_count] = index;
        mtx_done_count++;
    }
}

/* Poll the transport and report whether @p event has signalled. tcp4->Poll is
   what drives the firmware's state machine in a polled (no timer at our TPL)
   application -- without it the tokens can sit unprocessed. */
static bool
mtx_event_signalled(EFI_TCP4_PROTOCOL *tcp4, EFI_EVENT event)
{
    tcp4->Poll(tcp4);
    return gBS->CheckEvent(event) == EFI_SUCCESS;
}

static int
run_tcp_multi_transmit_mode(
    const char *host,
    const char *port_str,
    const char *gap_ms_str,
    bool        reverse,
    const char *tokens_str
    )
{
    uint16_t port;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0 || port == 0) {
        axl_printf("MTX-FAIL:port\n");
        return 1;
    }

    /* Milliseconds to wait BETWEEN submits. 0 is the measurement; a non-zero
       gap is the control that proves MTX-OUTSTANDING can report fewer than
       four. Without that control, "4 outstanding" is unfalsifiable -- it would
       read the same if the firmware only ever wrote Status from inside our own
       Poll, in which case the count would be measuring our call pattern rather
       than the firmware's concurrency. */
    uint16_t gap_ms = 0;
    if (gap_ms_str != NULL
        && axl_str_to_u16(gap_ms_str, 10, &gap_ms, NULL) != 0) {
        axl_printf("MTX-FAIL:gap\n");
        return 1;
    }

    /* How many concurrent tokens to submit. Raising this is how the firmware's
       queue DEPTH gets probed -- the spec sanctions EFI_NOT_READY ("the
       transmit queue is full"), which OVMF has never returned here. */
    uint16_t n_tokens = MTX_TOKENS;
    if (tokens_str != NULL
        && (axl_str_to_u16(tokens_str, 10, &n_tokens, NULL) != 0
            || n_tokens == 0 || n_tokens > MTX_TOKENS_MAX)) {
        axl_printf("MTX-FAIL:tokens (1..%u)\n", MTX_TOKENS_MAX);
        return 1;
    }

    AXL_AUTOPTR(AxlInetAddress) dest = axl_inet_address_new_from_string(host);
    if (dest == NULL) {
        axl_printf("MTX-FAIL:addr\n");
        return 1;
    }

    axl_net_auto_init(SIZE_MAX, 10);

    /* --- locate the TCP4 service binding ---------------------------------- */
    EFI_HANDLE *handles = NULL;
    UINTN       hc      = 0;
    EFI_STATUS  st      = gBS->LocateHandleBuffer(
                              ByProtocol, &gEfiTcp4ServiceBindingProtocolGuid,
                              NULL, &hc, &handles);
    if (EFI_ERROR(st) || hc == 0 || handles == NULL) {
        axl_printf("MTX-FAIL:no-service-binding\n");
        return 1;
    }

    EFI_SERVICE_BINDING_PROTOCOL *sb = NULL;
    gBS->HandleProtocol(handles[0], &gEfiTcp4ServiceBindingProtocolGuid, (void **)&sb);
    gBS->FreePool(handles);
    if (sb == NULL) {
        axl_printf("MTX-FAIL:no-service-binding\n");
        return 1;
    }

    EFI_HANDLE child = NULL;
    if (EFI_ERROR(sb->CreateChild(sb, &child))) {
        axl_printf("MTX-FAIL:create-child\n");
        return 1;
    }

    EFI_TCP4_PROTOCOL *tcp4 = NULL;
    gBS->HandleProtocol(child, &gEfiTcp4ProtocolGuid, (void **)&tcp4);
    if (tcp4 == NULL) {
        sb->DestroyChild(sb, child);
        axl_printf("MTX-FAIL:no-tcp4\n");
        return 1;
    }

    /* --- configure as an active (client) endpoint ------------------------- */
    EFI_TCP4_CONFIG_DATA cfg;
    axl_memset(&cfg, 0, sizeof(cfg));
    cfg.TimeToLive                    = 64;
    cfg.AccessPoint.ActiveFlag        = true;
    cfg.AccessPoint.UseDefaultAddress = true;
    cfg.AccessPoint.StationPort       = 0;
    cfg.AccessPoint.RemotePort        = port;
    axl_memcpy(cfg.AccessPoint.RemoteAddress.Addr,
               axl_inet_address_to_bytes(dest), 4);

    /* EFI_NO_MAPPING while DHCP is still settling -- same retry the library's
       own connect path carries, for the same reason. */
    for (unsigned retry = 0; retry < 20; retry++) {
        st = tcp4->Configure(tcp4, &cfg);
        if (st != EFI_NO_MAPPING) {
            break;
        }
        axl_msleep(500);
    }
    if (EFI_ERROR(st)) {
        axl_printf("MTX-FAIL:configure:0x%llx\n", (unsigned long long)st);
        sb->DestroyChild(sb, child);
        return 1;
    }

    /* --- connect ---------------------------------------------------------- */
    /* Non-zero until the run completes: every MTX-FAIL path must exit non-zero,
       so a firmware failure cannot read as a successful measurement. */
    int rc = 1;
    EFI_TCP4_CONNECTION_TOKEN ct;
    axl_memset(&ct, 0, sizeof(ct));
    if (EFI_ERROR(gBS->CreateEvent((UINT32)0, TPL_APPLICATION, (void *)NULL, (void *)NULL, &ct.CompletionToken.Event))) {
        axl_printf("MTX-FAIL:event\n");
        tcp4->Configure(tcp4, (void *)NULL);
        sb->DestroyChild(sb, child);
        return 1;
    }
    ct.CompletionToken.Status = EFI_ABORTED;

    st = tcp4->Connect(tcp4, &ct);
    if (EFI_ERROR(st)) {
        axl_printf("MTX-FAIL:connect-submit:0x%llx\n", (unsigned long long)st);
        goto out_connect_event;
    }
    for (unsigned ms = 0; ms < MTX_WAIT_MS; ms += 10) {
        if (mtx_event_signalled(tcp4, ct.CompletionToken.Event)) {
            break;
        }
        axl_msleep(10);
    }
    if (EFI_ERROR(ct.CompletionToken.Status)) {
        axl_printf("MTX-FAIL:connect:0x%llx\n",
                   (unsigned long long)ct.CompletionToken.Status);
        goto out_connect_event;
    }
    axl_printf("MTX-CONNECTED\n");

    /* --- build and submit MTX_TOKENS transmits, back to back -------------- */
    MtxSlot slots[MTX_TOKENS_MAX];
    axl_memset(slots, 0, sizeof(slots));

    /* The recorder is file-scope (the notify has nowhere else to put it), so
       reset it here rather than relying on one invocation per boot. */
    mtx_done_count = 0;

    unsigned built = 0;
    for (; built < n_tokens; built++) {
        MtxSlot *s = &slots[built];
        s->buf = axl_malloc(MTX_CHUNK_BYTES);
        if (s->buf == NULL) {
            break;
        }
        axl_memset(s->buf, (int)('A' + built), MTX_CHUNK_BYTES);
        if (EFI_ERROR(gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                       mtx_on_transmit_done,
                                       (void *)(uintptr_t)built,
                                       &s->token.CompletionToken.Event))) {
            axl_free(s->buf);
            s->buf = NULL;
            break;
        }
        s->token.CompletionToken.Status = EFI_ABORTED;
        s->tx.Push                      = true;
        s->tx.DataLength                = MTX_CHUNK_BYTES;
        s->tx.FragmentCount             = 1;
        s->tx.FragmentTable[0].FragmentLength = MTX_CHUNK_BYTES;
        s->tx.FragmentTable[0].FragmentBuffer = s->buf;
        s->token.Packet.TxData          = &s->tx;
    }
    if (built != n_tokens) {
        axl_printf("MTX-FAIL:setup built=%u\n", built);
    }

    /* No Poll between submits, on purpose: the point is to have all four
       outstanding at once, not to hand the firmware one at a time.
     *
     * `reverse` submits 3,2,1,0 instead of 0,1,2,3. It is the control for the
       ORDER measurement, the way gap_ms is the control for MTX-OUTSTANDING: if
       the firmware retires in submission order, the recorded order becomes
       3,2,1,0 and inorder flips to no. Without ever seeing that, "inorder=yes"
       would be an unfalsifiable claim about a collector that might simply be
       incapable of printing anything else -- which is exactly what the first
       version of this code turned out to be. */
    unsigned accepted = 0;
    for (unsigned n = 0; n < built; n++) {
        unsigned i = reverse ? (built - 1u - n) : n;
        if (n > 0 && gap_ms > 0) {
            axl_msleep(gap_ms);
        }
        slots[i].submit = tcp4->Transmit(tcp4, &slots[i].token);
        axl_printf("MTX-SUBMIT:%u:0x%llx\n", i,
                   (unsigned long long)slots[i].submit);
        if (!EFI_ERROR(slots[i].submit)) {
            accepted++;
        }
    }

    /* How many are STILL outstanding now that all four are submitted.
       Without this the run cannot distinguish "the firmware queued four" from
       "the firmware finished each one before the next was submitted" -- we sit
       at TPL_APPLICATION, so TcpDxe's periodic timer is free to run between
       two of these calls and retire a token. The firmware writes Status before
       signalling the event, so a token still holding the EFI_ABORTED sentinel
       we seeded has not completed. Only a count >= 2 makes this a measurement
       of CONCURRENT transmits at all. */
    unsigned outstanding = 0;
    for (unsigned i = 0; i < built; i++) {
        if (!EFI_ERROR(slots[i].submit)
            && slots[i].token.CompletionToken.Status == EFI_ABORTED) {
            outstanding++;
        }
    }
    axl_printf("MTX-OUTSTANDING:%u\n", outstanding);

    /* --- wait for the firmware to retire them ----------------------------- */
    /* The ORDER is whatever mtx_on_transmit_done recorded; this loop only
       drives Poll and waits. Nothing here may decide the order, which is the
       bug the notify exists to avoid. */
    for (unsigned ms = 0; ms < MTX_WAIT_MS && mtx_done_count < accepted; ms += 10) {
        tcp4->Poll(tcp4);
        axl_msleep(10);
    }

    unsigned completed = mtx_done_count;
    for (unsigned k = 0; k < completed; k++) {
        unsigned i = mtx_done_order[k];
        axl_printf("MTX-DONE:%u:%u:0x%llx\n", k, i,
                   (unsigned long long)slots[i].token.CompletionToken.Status);
    }

    /* in-order means: every accepted token completed, and the k'th completion
       was the k'th submission. A run where nothing was accepted proves
       nothing, so it is not "in order" either. */
    bool in_order = (completed == accepted) && (accepted > 0);
    for (unsigned k = 0; k < completed; k++) {
        unsigned submitted_kth = reverse ? (built - 1u - k) : k;
        if (mtx_done_order[k] != submitted_kth) {
            in_order = false;
        }
    }
    axl_printf("MTX-RESULT:submitted=%u accepted=%u completed=%u inorder=%s\n",
               built, accepted, completed, in_order ? "yes" : "no");

    /* Graceful close so the receiver sees EOF and can report its byte order.
       Abortive would race the last bytes out of existence. */
    EFI_TCP4_CLOSE_TOKEN clt;
    axl_memset(&clt, 0, sizeof(clt));
    if (!EFI_ERROR(gBS->CreateEvent((UINT32)0, TPL_APPLICATION, (void *)NULL, (void *)NULL, &clt.CompletionToken.Event))) {
        clt.CompletionToken.Status = EFI_ABORTED;
        clt.AbortOnClose           = false;
        if (!EFI_ERROR(tcp4->Close(tcp4, &clt))) {
            for (unsigned ms = 0; ms < MTX_WAIT_MS; ms += 10) {
                if (mtx_event_signalled(tcp4, clt.CompletionToken.Event)) {
                    break;
                }
                axl_msleep(10);
            }
        }
        gBS->CloseEvent(clt.CompletionToken.Event);
    }

    /* RESET FIRST, then free. If Close timed out with data still queued, the
       firmware still owns every un-retired token -- its 64 KB fragment buffer
       and its event. Configure(NULL) resets the instance and aborts those
       tokens (signalling their events), which is what makes the frees below
       safe; doing it the other way round hands the firmware freed memory to
       abort into. This is the ordering the library's own finalize_sock uses,
       and the reason it uses it. */
    tcp4->Configure(tcp4, (void *)NULL);

    for (unsigned i = 0; i < built; i++) {
        if (slots[i].token.CompletionToken.Event != NULL) {
            gBS->CloseEvent(slots[i].token.CompletionToken.Event);
        }
        axl_free(slots[i].buf);
    }
    rc = 0;

out_connect_event:
    /* Same ordering: the connect token can still be parked in the firmware on
       the timeout path, and Configure(NULL) is what retires it. Closing its
       event first would leave the reset signalling a freed event. */
    tcp4->Configure(tcp4, (void *)NULL);
    gBS->CloseEvent(ct.CompletionToken.Event);
    sb->DestroyChild(sb, child);
    return rc;
}

// ---------------------------------------------------------------------------
// 9P client mode — "9p-client <host> <port>" connects + attaches a 9P2000.L
// session against a host p9-server.py, printing 9P-CONNECT-OK on success,
// then reads /hello.txt (9P-READ:<contents>) and attempts a missing leaf in
// an existing dir (9P-READ-MISSING-OK on a clean AXL_ERR), lists /dir
// (9P-LIST:<name>:<size>,...), then writes a new file and reads it back
// (WRITE-RB:<contents>), overwrites it a second time to exercise the
// truncate-existing branch (TRUNC-RB:<contents>), then round-trips a
// 20000-byte buffer through /big.txt to exercise the chunked Twrite loop
// (MULTICHUNK-OK). Used by test-9p-qemu.sh.
// ---------------------------------------------------------------------------

static int
run_9p_client_mode(const char *host, const char *port_str)
{
    uint16_t port;
    if (axl_str_to_u16(port_str, 10, &port, NULL) != 0 || port == 0) {
        axl_printf("9P-CLIENT-FAIL:port\n");
        return 1;
    }
    axl_net_auto_init(SIZE_MAX, 10);

    Axl9pClient *c = NULL;
    if (axl_9p_connect(host, port, "axl", "/", &c) != AXL_OK) {
        axl_printf("9P-CLIENT-FAIL:connect\n");
        return 1;
    }
    axl_printf("9P-CONNECT-OK\n");

    AxlBytes *fb = NULL;
    if (axl_9p_read_file(c, "/hello.txt", &fb) == AXL_OK && fb != NULL) {
        size_t n = 0;
        const uint8_t *d = axl_bytes_get_data(fb, &n);
        axl_printf("9P-READ:%.*s\n", (int)n, (const char *)d);
        axl_bytes_unref(fb);
    } else {
        axl_printf("9P-READ-FAIL\n");
    }

    /* Missing leaf in an existing dir: read_file must fail cleanly (no
       hang, no crash) via the partial-Twalk path client_walk clunks. */
    AxlBytes *fb2 = NULL;
    if (axl_9p_read_file(c, "/dir/nope.txt", &fb2) != AXL_OK) {
        axl_printf("9P-READ-MISSING-OK\n");
    } else {
        axl_printf("9P-READ-MISSING-FAIL\n");
        axl_bytes_unref(fb2);
    }

    /* Non-root directory: this is the ONLY caller in the tree that reaches
       join_child_path's non-root branch, so it carries the size too -- a
       join bug ("/dira.txt") would otherwise leave the names right and
       every size silently 0. */
    AxlArray *entries = NULL;
    if (axl_9p_list(c, "/dir", &entries) == AXL_OK && entries != NULL) {
        axl_printf("9P-LIST:");
        for (size_t i = 0; i < axl_array_len(entries); i++) {
            AxlFsEntry *e = (AxlFsEntry *)axl_array_get(entries, i);
            axl_printf("%s:%llu,", e->name, (unsigned long long)e->size);
        }
        axl_printf("\n");
        axl_array_free(entries);
    } else {
        axl_printf("9P-LIST-FAIL\n");
    }

    /* Write a new file, then read it back to prove the round-trip. */
    if (axl_9p_write_file(c, "/wtest.txt", "hello-9p-write", 14) == AXL_OK) {
        AxlBytes *wb = NULL;
        if (axl_9p_read_file(c, "/wtest.txt", &wb) == AXL_OK && wb != NULL) {
            size_t n = 0;
            const uint8_t *d = axl_bytes_get_data(wb, &n);
            axl_printf("WRITE-RB: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(wb);
        } else {
            axl_printf("WRITE-RB-FAIL:readback\n");
        }
    } else {
        axl_printf("WRITE-RB-FAIL:write\n");
    }

    /* Write /wtest.txt a SECOND time with shorter, distinct content. Proves
       the walk-succeeds -> O_TRUNC-open branch, and that the old 14-byte
       tail from the first write doesn't survive the shrink. */
    if (axl_9p_write_file(c, "/wtest.txt", "trunc", 5) == AXL_OK) {
        AxlBytes *tb = NULL;
        if (axl_9p_read_file(c, "/wtest.txt", &tb) == AXL_OK && tb != NULL) {
            size_t n = 0;
            const uint8_t *d = axl_bytes_get_data(tb, &n);
            axl_printf("TRUNC-RB: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(tb);
        } else {
            axl_printf("TRUNC-RB-FAIL:readback\n");
        }
    } else {
        axl_printf("TRUNC-RB-FAIL:write\n");
    }

    /* Multi-chunk Twrite: 20000 bytes forces multiple Twrite iterations at
       the default 8192 msize (chunk = msize - AXL_9P_TWRITE_HDR_LEN, ~8169).
       Verify the round-trip by length + sampled bytes instead of dumping
       20 KB to serial. */
    {
        const size_t big_len = 20000;
        uint8_t *big = (uint8_t *)axl_malloc(big_len);
        if (big != NULL) {
            for (size_t i = 0; i < big_len; i++) {
                big[i] = (uint8_t)('A' + (i % 26));
            }
            if (axl_9p_write_file(c, "/big.txt", big, big_len) == AXL_OK) {
                AxlBytes *bb = NULL;
                if (axl_9p_read_file(c, "/big.txt", &bb) == AXL_OK && bb != NULL) {
                    size_t n = 0;
                    const uint8_t *d = axl_bytes_get_data(bb, &n);
                    bool ok = (n == big_len)
                        && d[0]     == (uint8_t)('A' + (0     % 26))
                        && d[8191]  == (uint8_t)('A' + (8191  % 26))
                        && d[8192]  == (uint8_t)('A' + (8192  % 26))
                        && d[19999] == (uint8_t)('A' + (19999 % 26));
                    axl_printf(ok ? "MULTICHUNK-OK\n" : "MULTICHUNK-FAIL:mismatch\n");
                    axl_bytes_unref(bb);
                } else {
                    axl_printf("MULTICHUNK-FAIL:readback\n");
                }
            } else {
                axl_printf("MULTICHUNK-FAIL:write\n");
            }
            axl_free(big);
        } else {
            axl_printf("MULTICHUNK-FAIL:alloc\n");
        }
    }

    /* mkdir: create then confirm via the marker (a real client would
       axl_9p_list the parent, but the marker alone proves Tmkdir round-trips
       and the harness stays consistent with the other steps' style). */
    if (axl_9p_mkdir(c, "/newdir") == AXL_OK) {
        axl_printf("MKDIR-OK: /newdir\n");
    } else {
        axl_printf("MKDIR-FAIL: /newdir\n");
    }

    /* remove: delete the file written above, confirm a subsequent read
       now fails cleanly. */
    axl_9p_remove(c, "/wtest.txt");
    AxlBytes *rb = NULL;
    if (axl_9p_read_file(c, "/wtest.txt", &rb) != AXL_OK) {
        axl_printf("REMOVE-GONE: /wtest.txt\n");
    } else {
        axl_printf("REMOVE-FAIL: /wtest.txt still readable\n");
        axl_bytes_unref(rb);
    }

    /* rename: write a fresh source (the earlier one was just removed),
       rename it, then read back the destination. */
    axl_9p_write_file(c, "/ren-src.txt", "hello-9p-write", 14);
    if (axl_9p_rename(c, "/ren-src.txt", "/ren-dst.txt") == AXL_OK) {
        AxlBytes *renb = NULL;
        if (axl_9p_read_file(c, "/ren-dst.txt", &renb) == AXL_OK) {
            size_t n;
            const uint8_t *d = axl_bytes_get_data(renb, &n);
            axl_printf("RENAME-RB: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(renb);
        }
    }

    /* Cross-directory rename: the server answers Rlerror(EXDEV) rather than
       moving the bytes itself (an unbounded synchronous copy on its loop), so
       the CLIENT must degrade to copy-then-unlink the way every POSIX client
       does. Proves three things at once: the destination has the source's
       bytes, the source is gone, and the call reported success only because
       both actually happened. */
    axl_9p_write_file(c, "/dir/xdev-src.txt", "xdev-payload", 12);
    if (axl_9p_rename(c, "/dir/xdev-src.txt", "/xdev-dst.txt") == AXL_OK) {
        AxlBytes *xb = NULL;
        if (axl_9p_read_file(c, "/xdev-dst.txt", &xb) == AXL_OK) {
            size_t         n = 0;
            const uint8_t *d = axl_bytes_get_data(xb, &n);
            axl_printf("XDEV-RB: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(xb);
        } else {
            axl_printf("XDEV-FAIL: destination unreadable\n");
        }
        AxlBytes *sb = NULL;
        if (axl_9p_read_file(c, "/dir/xdev-src.txt", &sb) != AXL_OK) {
            axl_printf("XDEV-SRC-GONE\n");
        } else {
            axl_printf("XDEV-FAIL: source survived the move\n");
            axl_bytes_unref(sb);
        }
    } else {
        axl_printf("XDEV-FAIL: rename returned an error\n");
    }

    /* Cross-directory rename of a DIRECTORY must be refused outright, not
       silently treated as a file by the copy-then-unlink fallback (the
       directory guard in rename_xdev_copy, axl-9p-client.c). Pick endpoints
       in different directories so the server bounces it to EXDEV the same
       way it does for the file case above, then confirm the client refuses
       the fallback rather than moving the tree. */
    if (axl_9p_mkdir(c, "/dir/xsub") == AXL_OK) {
        if (axl_9p_rename(c, "/dir/xsub", "/xsub") != AXL_OK) {
            axl_printf("XDEV-DIR-REFUSED\n");
        } else {
            axl_printf("XDEV-FAIL: directory rename should have been refused\n");
        }
    } else {
        axl_printf("XDEV-FAIL: mkdir /dir/xsub\n");
    }

    /* The EXDEV fallback REFUSES a destination that already exists, before
       it reads or writes anything (rename_xdev_copy's leading walk-then-
       clunk in axl-9p-client.c). rename(2)'s permission to clobber is only
       safe because the replacement is atomic; copy-then-unlink is not, so a
       session drop mid-copy would leave a REAL file truncated or half
       written. The refusal is the whole guarantee, and the guarantee is not
       "the call returned AXL_ERR" -- it is that BOTH files still hold their
       original bytes afterwards. Distinct contents so neither read-back can
       be satisfied by the other file's payload, and so a clobber that
       happened to preserve the length is still visible.

       The fixture is deliberately more permissive here (p9-server.py's own
       docstring: it overwrites at the destination rather than answering
       EEXIST), so if this guard were deleted the rename would SUCCEED and
       destroy /xdst2.txt -- which is exactly why the assertion can fail. */
    axl_9p_write_file(c, "/xdst2.txt",     "dst-original", 12);
    axl_9p_write_file(c, "/dir/xsrc2.txt", "src-original", 12);
    if (axl_9p_rename(c, "/dir/xsrc2.txt", "/xdst2.txt") != AXL_OK) {
        axl_printf("XDEV-EXIST-REFUSED\n");
    } else {
        axl_printf("XDEV-FAIL: taken destination was overwritten\n");
    }
    {
        AxlBytes *db = NULL;
        AxlBytes *sb2 = NULL;
        if (axl_9p_read_file(c, "/xdst2.txt", &db) == AXL_OK && db != NULL) {
            size_t         n = 0;
            const uint8_t *d = axl_bytes_get_data(db, &n);
            axl_printf("XDEV-EXIST-DST: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(db);
        } else {
            axl_printf("XDEV-FAIL: destination unreadable after the refusal\n");
        }
        if (axl_9p_read_file(c, "/dir/xsrc2.txt", &sb2) == AXL_OK && sb2 != NULL) {
            size_t         n = 0;
            const uint8_t *d = axl_bytes_get_data(sb2, &n);
            axl_printf("XDEV-EXIST-SRC: %.*s\n", (int)n, (const char *)d);
            axl_bytes_unref(sb2);
        } else {
            axl_printf("XDEV-FAIL: source unreadable after the refusal\n");
        }
    }

    axl_9p_disconnect(c);
    axl_printf("9P-CLIENT-OK\n");
    return 0;
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
            axl_printf("ERROR: TLS not available\n");
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
            axl_printf("ERROR: TLS not available\n");
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

    AxlNetSntpResult r;
    int rc = axl_net_sntp_query(host, port, 5000, &r);
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
// axl_net_auto_init_opts / quarantine — validation only (no live reconfigure).
// The live "brings a NIC online" assertions run in net-diag mode where a real
// DHCP lease exists; here we pin the safe negatives and the zero-init defaults,
// which need no NIC and cannot perturb live firmware state.
// ---------------------------------------------------------------------------
static void
test_auto_init_opts_validation(void)
{
    /* NULL opts is rejected before anything is touched. */
    test_check(axl_net_auto_init_opts(NULL, NULL) == AXL_ERR,
               "auto_init_opts: NULL opts -> AXL_ERR");

    /* Zero-init IS the documented default (AUTO NIC + DHCP + SWEEP_DIR): pin the
       enum values so a reorder that silently changes what a zeroed struct means
       fails here. */
    test_check(AXL_NET_NIC_SEL_AUTO == 0 && AXL_NET_IP_DHCP == 0
                   && AXL_NET_DRV_SWEEP_DIR == 0,
               "auto_init_opts: zero-init == AUTO + DHCP + SWEEP_DIR");

    /* A STATIC request missing its address/mask is rejected on validation,
       BEFORE any firmware bring-up -- so this is a safe negative even when a NIC
       is already up (the engine must not answer 'online' to a malformed static
       request just because some DHCP lease exists). */
    static const uint8_t ip[4]   = { 192, 168, 5, 5 };
    AxlNetAutoOpts st_no_ip = { 0 };
    st_no_ip.ip_mode = AXL_NET_IP_STATIC;   /* static_ipv4 left NULL */
    test_check(axl_net_auto_init_opts(&st_no_ip, NULL) == AXL_ERR,
               "auto_init_opts: STATIC without static_ipv4 -> AXL_ERR");
    AxlNetAutoOpts st_no_mask = { 0 };
    st_no_mask.ip_mode     = AXL_NET_IP_STATIC;
    st_no_mask.static_ipv4 = ip;             /* static_mask left NULL */
    test_check(axl_net_auto_init_opts(&st_no_mask, NULL) == AXL_ERR,
               "auto_init_opts: STATIC without static_mask -> AXL_ERR");

    /* Shared driver quarantine: clear is best-effort AXL_OK; the init helper
       rejects a NULL descriptor and binds a real one. */
    test_check(axl_net_clear_driver_quarantine() == AXL_OK,
               "clear_driver_quarantine: AXL_OK (best-effort)");
    test_check(axl_net_driver_quarantine_init(NULL) == AXL_ERR,
               "driver_quarantine_init: NULL -> AXL_ERR");
    AxlAttempt qa;
    test_check(axl_net_driver_quarantine_init(&qa) == AXL_OK,
               "driver_quarantine_init: binds the shared namespace");
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

    /* BUG regression: auto_init's short-circuit must ask "is THIS NIC
       already up", not "is ANY NIC up", for an explicit nic_index. The
       auto_init(0, 10) call above already established a real lease on NIC
       0 -- the only physical NIC in this profile -- so the first ordinal
       PAST it is out of range. Pre-fix, the short-circuit was
       axl_net_get_ip_address() (NIC-agnostic: "does ANY NIC have an IP")
       ahead of any bounds check, so it answered AXL_OK for this
       out-of-range NIC purely because NIC 0 already leased -- "configure
       NIC 1" reported success having configured nothing. Derived from the
       live NIC count, never hardcoded, so it holds at any NIC count. */
    size_t nd_nic_count = 0;
    if (axl_net_list_interfaces(NULL, &nd_nic_count) == AXL_OK && nd_nic_count > 0) {
        ND_CHECK(axl_net_auto_init(nd_nic_count, 10) == AXL_ERR,
                 "auto_init: explicit out-of-range nic -> AXL_ERR "
                 "(no ANY-NIC short-circuit leak)");
        ND_CHECK(axl_net_bring_up(nd_nic_count, NULL, NULL, NULL, 10, NULL) == AXL_ERR,
                 "bring_up: explicit out-of-range nic -> AXL_ERR (DHCP path)");
    }

    /* BUG regression: axl_net_bring_up's addr_out must report the address
       of the NIC it actually configured, not merely "some configured
       NIC" (axl_net_get_ip_address, by design, answers "does ANY NIC have
       an address" -- first configured IP4Config2 wins, which can be a
       DIFFERENT NIC than the one requested on a multi-NIC box). Both calls
       below short-circuit inside auto_init (NIC 0 is already up from the
       earlier call), so they touch no live firmware state -- purely a
       read-back check. This single-NIC profile canNOT discriminate a
       wrong-NIC misattribution (there is only one NIC to misattribute to),
       so it does not re-prove the multi-NIC bug; it pins that the new
       registry-attributed read-back still reports the right address for
       both an explicit ordinal and AXL_NET_NIC_AUTO, and that the two
       agree with each other. */
    static const uint8_t bu_exp_addr[4] = { 10, 0, 2, 15 };
    AxlIPv4Address bu_addr0;
    ND_CHECK(axl_net_bring_up(0, NULL, NULL, NULL, 10, &bu_addr0) == AXL_OK
             && axl_memcmp(bu_addr0.addr, bu_exp_addr, 4) == 0,
             "bring_up: explicit nic 0 addr_out == 10.0.2.15 "
             "(registry-attributed read-back)");
    AxlIPv4Address bu_addr_auto;
    ND_CHECK(axl_net_bring_up(AXL_NET_NIC_AUTO, NULL, NULL, NULL, 10, &bu_addr_auto) == AXL_OK
             && axl_memcmp(bu_addr_auto.addr, bu_exp_addr, 4) == 0,
             "bring_up: AXL_NET_NIC_AUTO addr_out == 10.0.2.15 "
             "(same NIC AUTO configured)");

    /* axl_net_auto_init_opts -- the library form of `netload -a`. NIC 0 is
       already up from the bring_up calls above, so the firmware-first pass
       satisfies the request without loading anything (drivers_tried == 0): a
       safe read-back like the bring_up checks, not a live reconfigure. The
       SWEEP_DIR driver loop itself is real-driver territory, covered by
       netload's integration suite. */
    AxlNetAutoOpts opts0 = { 0 };   /* zero-init == AUTO + DHCP + SWEEP_DIR */
    AxlNetBringUpResult res0;
    int rc_opts0 = axl_net_auto_init_opts(&opts0, &res0);
    ND_CHECK(rc_opts0 == AXL_OK && res0.online,
             "auto_init_opts: zero-init brings a NIC online (firmware-first)");
    ND_CHECK(rc_opts0 == AXL_OK && axl_memcmp(res0.ipv4, bu_exp_addr, 4) == 0,
             "auto_init_opts: result address == 10.0.2.15");
    ND_CHECK(rc_opts0 == AXL_OK && res0.have_nic,
             "auto_init_opts: result names a specific NIC");
    ND_CHECK(rc_opts0 == AXL_OK && res0.drivers_tried == 0,
             "auto_init_opts: firmware-first won, no drivers swept");

    /* Explicit ordinal 0 + DHCP resolves to the same already-up NIC. */
    AxlNetAutoOpts opts_idx = { 0 };
    opts_idx.nic_select = AXL_NET_NIC_SEL_INDEX;
    opts_idx.nic_index  = 0;
    AxlNetBringUpResult res_idx;
    ND_CHECK(axl_net_auto_init_opts(&opts_idx, &res_idx) == AXL_OK
                 && res_idx.online && res_idx.have_nic && res_idx.nic_index == 0,
             "auto_init_opts: SEL_INDEX 0 -> online, nic_index 0");

    /* Select by MAC (eth0's own MAC) -> the same NIC. Exercises MAC resolution. */
    AxlNetInterface od_if[4];
    size_t od_nif = 4;
    if (axl_net_list_interfaces(od_if, &od_nif) == AXL_OK && od_nif >= 1) {
        AxlNetAutoOpts opts_mac = { 0 };
        opts_mac.nic_select = AXL_NET_NIC_SEL_MAC;
        axl_memcpy(opts_mac.nic_mac, od_if[0].mac, 6);
        AxlNetBringUpResult res_mac;
        ND_CHECK(axl_net_auto_init_opts(&opts_mac, &res_mac) == AXL_OK
                     && res_mac.online
                     && axl_memcmp(res_mac.mac, od_if[0].mac, 6) == 0,
                 "auto_init_opts: SEL_MAC resolves eth0 and reports its MAC");
    }

    /* by-MAC lease accessor. In the single-NIC QEMU profile the IP4Config2 and
       SNP index spaces coincide, so the MAC-resolved lease must equal the
       index-0 lease byte-for-byte (same NIC, two lookup paths). The decisive
       difference from the index path is the unknown-MAC case: by-MAC must fail
       cleanly rather than clamp to NIC 0. */
    AxlNetInterface nd_ifaces[4];
    size_t nd_nif = 4;
    if (axl_net_list_interfaces(nd_ifaces, &nd_nif) == AXL_OK && nd_nif >= 1) {
        AxlDhcpLease lease_mac;
        int rcm = axl_net_get_dhcp_lease_by_mac(nd_ifaces[0].mac, &lease_mac);
        ND_CHECK(rcm == AXL_OK,
                 "dhcp-lease-by-mac: returns AXL_OK for eth0 MAC");
        if (rcm == AXL_OK && rc == AXL_OK) {
            ND_CHECK(axl_memcmp(&lease_mac, &lease, sizeof(lease)) == 0,
                     "dhcp-lease-by-mac: matches index-0 lease byte-for-byte");
        }
        static const uint8_t bogus_mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 };
        AxlDhcpLease lease_bogus;
        ND_CHECK(axl_net_get_dhcp_lease_by_mac(bogus_mac, &lease_bogus) == AXL_ERR,
                 "dhcp-lease-by-mac: unknown MAC -> AXL_ERR (no clamp)");
    }

    /* AXL_NET_NIC_AUTO through the same registry resolver the explicit-index
       calls above use. get_dhcp_lease carries no "already configured"
       short-circuit (unlike auto_init), so this genuinely exercises
       ip4cfg_for(AUTO) -> _axl_net_nic_resolve_ip4cfg's AUTO branch rather
       than short-circuiting before ever reaching it. In the single-NIC QEMU
       profile AUTO has only one candidate, so it must match the index-0
       lease byte-for-byte. */
    AxlDhcpLease lease_auto;
    int rca = axl_net_get_dhcp_lease(AXL_NET_NIC_AUTO, &lease_auto);
    ND_CHECK(rca == AXL_OK,
             "dhcp-lease: AXL_NET_NIC_AUTO resolves (registry AUTO rule)");
    if (rca == AXL_OK && rc == AXL_OK) {
        ND_CHECK(axl_memcmp(&lease_auto, &lease, sizeof(lease)) == 0,
                 "dhcp-lease: AUTO matches index-0 lease byte-for-byte");
    }

    /* Out-of-range must ERROR, not clamp to NIC 0. Safe negatives -- each
       returns on our own bounds check before any firmware call
       (feedback_uefi_firmware_test_hazards); a VALID index here would
       reconfigure live firmware, which is why only the out-of-range case is
       driven. SIZE_MAX-1 rather than SIZE_MAX: SIZE_MAX IS AXL_NET_NIC_AUTO
       and means auto-select, not out-of-range. These run here (post-DHCP),
       not in the default-suite validation function: a clamp to NIC 0
       answering AXL_OK is only distinguishable from the correct AXL_ERR once
       a real lease exists on NIC 0 for it to leak. */

    /* Pins the contract, but does NOT discriminate a clamp: IP4Config2 makes
       the DNS list read-only under the DHCP policy this NIC is on, so the
       SetData fails on its own merits whichever NIC the index resolves to.
       Kept as a real assertion (not a regression guard, and not a tautology
       -- it does pin "out-of-range never succeeds"). */
    static const uint8_t oob_dns[4] = { 10, 0, 2, 3 };
    ND_CHECK(axl_net_set_dns(SIZE_MAX - 1, oob_dns, NULL) == AXL_ERR,
             "set-dns: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");

    /* set_static_ip_by_mac has no ordinal to go out-of-range, so its
       equivalent safe negative is a MAC that names no NIC -- same hazard as
       above (a VALID MAC would reconfigure live firmware under this test
       binary; feedback_uefi_firmware_test_hazards), so only the unknown-MAC
       rejection is driven here. This is the decisive pin of the "no
       fallback" contract: ip4cfg_for_mac must not silently resolve to some
       OTHER NIC when the MAC it was asked for isn't present. */
    static const uint8_t sip_bogus_mac[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x02 };
    static const uint8_t sip_oob_ip[4]      = { 192, 168, 1, 100 };
    static const uint8_t sip_oob_netmask[4] = { 255, 255, 255, 0 };
    ND_CHECK(axl_net_set_static_ip_by_mac(sip_bogus_mac, sip_oob_ip, sip_oob_netmask, NULL) == AXL_ERR,
             "set-static-ip-by-mac: unknown MAC -> AXL_ERR (no fallback)");

    /* Genuine clamp regression guards -- both answered AXL_OK for NIC 0 under
       the deleted `>= count -> 0` clamp, because NIC 0 has a real lease for a
       clamp to leak. */
    AxlDhcpLease oob_lease;
    ND_CHECK(axl_net_get_dhcp_lease(SIZE_MAX - 1, &oob_lease) == AXL_ERR,
             "dhcp-lease: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");

    /* expect_ipv4 = NULL is the documented "any non-zero address" mode, and is
       what makes this discriminate: a clamp resolves NIC 0, whose station
       address IS non-zero (the SLIRP lease), so the buggy path settles
       immediately with AXL_OK. Passing a deliberately-wrong address instead
       would mask the clamp -- it would time out on the mismatch either way. */
    ND_CHECK(axl_net_wait_ip_settled(SIZE_MAX - 1, NULL, 1) == AXL_ERR,
             "wait-ip-settled: out-of-range nic -> AXL_ERR (no clamp to NIC 0)");

    /* test-netdiag-qemu.sh drives AxlTestNet.efi with argv[1] == "net-diag",
       which returns out of test_net_main() via run_net_diag_mode() before
       ever reaching the default suite's registration of this same check
       (below), so that boot needs its own call to exercise the dedup
       contract. Its PASS:/FAIL: lines feed this mode's own "FAIL:" grep in
       test-netdiag-qemu.sh. */
    test_nic_registry_contract();

    /* Config method: OVMF provides IP4Config2, so the bring-up at the top of
       this mode used the standard path (the IP4Config2-free Dhcp4-SB / PXE
       fallbacks are real-HW-only — OVMF can't exercise them). */
    ND_CHECK(axl_net_last_config_method() == AXL_NET_CONFIG_IP4CONFIG2,
             "config-method: IP4Config2 path on OVMF");

    /* NIC takeover must be a NO-OP when SNP is already present (OVMF has SNP) —
       the guard that prevents destroying a working firmware stack. It must
       return AXL_OK and leave networking intact. */
    ND_CHECK(axl_net_takeover_if_no_snp() == AXL_OK,
             "takeover: no-op AXL_OK when SNP present");
    AxlIPv4Address post_takeover_addr;
    ND_CHECK(axl_net_get_ip_address(&post_takeover_addr) == AXL_OK,
             "takeover: networking still up after no-op takeover");

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

    /* Ordinal consistency: get_link_stats(i) and list_interfaces()[i] must
       describe the SAME NIC. Before the registry they indexed different
       spaces (and computed link_up by different rules), so agreement was
       coincidence at NIC 0 and wrong beyond it. */
    size_t lc_n = 0;
    if (axl_net_list_interfaces(NULL, &lc_n) == AXL_OK && lc_n > 0) {
        AxlNetInterface *lc_ifs = axl_calloc(lc_n, sizeof *lc_ifs);
        if (lc_ifs != NULL) {
            size_t lc_filled = lc_n;
            if (axl_net_list_interfaces(lc_ifs, &lc_filled) == AXL_OK) {
                bool agree = true;
                for (size_t i = 0; i < lc_filled; i++) {
                    AxlNetLinkStats st_i;
                    if (axl_net_get_link_stats(i, &st_i) != AXL_OK
                        || st_i.link_up != lc_ifs[i].link_up) {
                        agree = false;
                        break;
                    }
                }
                ND_CHECK(agree,
                    "ordinal: get_link_stats(i).link_up == list_interfaces()[i].link_up");
            }
            axl_free(lc_ifs);
        }

        /* The decisive ordinal guard, and the one that does NOT depend on
           media detection: the first index PAST the last physical NIC must be
           rejected. One physical NIC publishes 2-3 SNP child handles here, so
           pre-registry get_link_stats indexed a 3-entry raw handle buffer and
           happily answered AXL_OK for ordinal 1 and 2 -- SNP children of the
           SAME NIC that list_interfaces does not expose. Post-registry only
           ordinals [0, lc_n) exist. Derived from the live count, never
           hardcoded, so it holds at any NIC count. */
        AxlNetLinkStats st_past;
        ND_CHECK(axl_net_get_link_stats(lc_n, &st_past) == AXL_ERR,
            "ordinal: get_link_stats(nic_count) -> AXL_ERR (SNP child handles are not ordinals)");
    }

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
        axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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
        axl_socket_free(client, AXL_TEARDOWN_GRACEFUL);
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

    axl_socket_free(client, AXL_TEARDOWN_GRACEFUL);
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

    axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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
        axl_socket_free(client, AXL_TEARDOWN_GRACEFUL);
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

    axl_socket_free(client, AXL_TEARDOWN_GRACEFUL);
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

        axl_socket_free(stream, AXL_TEARDOWN_GRACEFUL);
    }

    /* listen on datagram -> error */
    AxlSocket *dgram = axl_socket_new(AXL_SOCKET_DATAGRAM);
    if (dgram != NULL) {
        test_check(axl_socket_listen(dgram, 9995) == AXL_ERR,
                   "socket type_errors: listen on datagram fails");
        axl_socket_free(dgram, AXL_TEARDOWN_GRACEFUL);
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
        axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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
        axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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
    axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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
        axl_socket_free(stream, AXL_TEARDOWN_GRACEFUL);
    }

    axl_socket_free(sock, AXL_TEARDOWN_GRACEFUL);
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

    axl_http_server_free(s, AXL_TEARDOWN_GRACEFUL);
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
        axl_tcp_close(sock, AXL_TEARDOWN_GRACEFUL);
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
    /* On a pre-LoadImage error the result carries no owned resources: the MAC
       array and driver handle are both NULL and the count is 0, so the caller's
       uniform `axl_free(tr.bound_nic_macs)` cleanup is a safe no-op. (The
       populated alloc path — one small heap array of newly-bound MACs plus a
       resident driver handle — needs a real driver load and is
       consumer/real-hardware verified, not exercised here; see
       feedback_uefi_firmware_test_hazards.) */
    test_check(tr.bound_nic_macs == NULL && tr.driver == NULL
                   && tr.bound_nic_count == 0 && tr.snp_handles_added == 0,
               "try_driver: error path owns nothing (macs/driver NULL, count 0)");
    axl_free(tr.bound_nic_macs);   /* NULL -> no-op; documents the free contract */

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

    /* Sync variants surface the specific AxlStatus on validation (A2a part 2):
       bad args -> AXL_INVALID, not the generic AXL_ERR. Both paths reject
       before any network work. */
    AxlHttpClientResponse *sresp = NULL;
    test_check(axl_http_get(NULL, "http://h/", &sresp) == AXL_INVALID,
               "http-sync: NULL client -> AXL_INVALID");
    test_check(axl_http_get(c, NULL, &sresp) == AXL_INVALID,
               "http-sync: NULL url -> AXL_INVALID");

    /* Drain the loop briefly: a buggy impl that deferred the callback
       despite the error return would surface here. */
    axl_loop_iterate_until(loop, NULL, 20 * 1000);
    test_check(!fired, "http-async: rejected call never fires the callback");

    axl_http_client_free(c);
    axl_loop_free(loop);
}

// ---------------------------------------------------------------------------
// Port-releasing teardown — "serve-rebind <graceful|abortive>". Proves
// axl_http_server_free(RESET) releases the listen port synchronously so an
// immediate rebind on the same port succeeds even with an in-flight connection
// and WITHOUT pumping the loop.
//
// The teardown + rebind run INSIDE a loop callback, so axl_loop_is_running() is
// true — the exact condition under which the graceful axl_http_server_free
// DEFERS its listener/conn finalization to the loop. Because the rebind runs
// synchronously in the same callback (no loop tick between free and rebind):
//   graceful -> the port is still held -> REBIND-RC != 0 (the SoftBMC re-exec bug)
//   abortive -> RST + inline finalize -> port free -> REBIND-RC == 0
// Driven by test-http-rebind-qemu.sh, which holds an in-flight connection open
// across the teardown and (abortive) re-curls the rebound server.
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop       *loop;
    AxlHttpServer *s1;
    AxlHttpServer *s2;
    bool           abortive;
    int            rebind_rc;
} RebindCtx;

static bool
rebind_quit_cb(void *data)
{
    axl_loop_quit(((RebindCtx *)data)->loop);
    return AXL_SOURCE_REMOVE;
}

static bool
rebind_probe_cb(void *data)
{
    RebindCtx *c = (RebindCtx *)data;

    /* Inside a loop callback: axl_loop_is_running(loop) is true, so a graceful
       free defers finalization to this loop. We rebind synchronously here with
       no intervening tick. */
    if (c->abortive) {
        axl_http_server_free(c->s1, AXL_TEARDOWN_RESET);
    } else {
        axl_http_server_free(c->s1, AXL_TEARDOWN_GRACEFUL);
    }
    c->s1 = NULL;

    c->s2 = axl_http_server_new(8080);
    if (c->s2 != NULL) {
        axl_http_server_add_route(c->s2, "GET", "/plain", on_get_plain, NULL);
        c->rebind_rc = axl_http_server_start(c->s2, c->loop);
    } else {
        c->rebind_rc = -1;
    }
    axl_printf("REBIND-RC:%d\n", c->rebind_rc);
    axl_printf(c->rebind_rc == 0 ? "REBIND-OK\n" : "REBIND-FAIL\n");
    axl_printf("READY2\n");

    /* Keep serving the rebound server ~3 s so the host can curl it, then quit. */
    axl_loop_add_timeout(c->loop, 3000, rebind_quit_cb, c);
    return AXL_SOURCE_REMOVE;
}

static int
run_serve_rebind_mode(const char *variant)
{
    RebindCtx c;
    axl_memset(&c, 0, sizeof(c));
    c.abortive = (axl_strcmp(variant, "abortive") == 0);

    axl_net_auto_init(SIZE_MAX, 10);

    c.loop = axl_loop_new();
    c.s1   = axl_http_server_new(8080);
    if (c.loop == NULL || c.s1 == NULL) {
        axl_printf("ERROR: rebind setup failed\n");
        return -1;
    }
    axl_http_server_add_route(c.s1, "GET", "/plain", on_get_plain, NULL);
    if (axl_http_server_start(c.s1, c.loop) != 0) {
        axl_printf("ERROR: rebind initial start failed\n");
        return -1;
    }

    /* Fire the teardown+rebind ~2 s in — enough for the host to open an
       in-flight connection first. */
    axl_loop_add_timeout(c.loop, 2000, rebind_probe_cb, &c);
    axl_printf("READY\n");
    axl_loop_run(c.loop);

    if (c.s2 != NULL) {
        axl_http_server_free(c.s2, AXL_TEARDOWN_RESET);
    }
    axl_loop_free(c.loop);
    axl_printf("REBIND-DONE\n");
    return 0;
}

// ---------------------------------------------------------------------------
// serve-rebind-load — the under-load port-release regression.
//
// Unlike serve-rebind (one accepted, delivered connection), this NEVER pumps
// the accept loop before teardown: it starts the listener, prints READY, then
// axl_msleep()s while the host opens SEVERAL connections. The firmware completes
// their handshakes on its own periodic timer and queues them in the listen
// socket's ACCEPT BACKLOG — established, but never delivered to on_accept_ready
// (the loop never ran), so the server tracks NONE of them in s->conns. Then it
// frees + rebinds :8080 synchronously with no pump.
//
// The backlog children hold PCBs on :8080. A correct abortive teardown must RST
// + DestroyChild them synchronously, so:
//   abortive (GREEN): REBIND-RC == 0 — port released even with a full backlog.
//   abortive, unfixed (RED): REBIND-RC != 0 — backlog still holds the port.
// Driven by test-http-rebind-load-qemu.sh, which opens the backlog on READY.
// ---------------------------------------------------------------------------

static int
run_serve_rebind_load_mode(const char *variant)
{
    RebindCtx c;
    axl_memset(&c, 0, sizeof(c));
    c.abortive = (axl_strcmp(variant, "abortive") == 0);

    axl_net_auto_init(SIZE_MAX, 10);

    c.loop = axl_loop_new();
    c.s1   = axl_http_server_new(8080);
    if (c.loop == NULL || c.s1 == NULL) {
        axl_printf("ERROR: rebind-load setup failed\n");
        return -1;
    }
    axl_http_server_add_route(c.s1, "GET", "/plain", on_get_plain, NULL);
    if (axl_http_server_start(c.s1, c.loop) != 0) {
        axl_printf("ERROR: rebind-load initial start failed\n");
        return -1;
    }
    axl_printf("READY\n");

    /* Let the host's connections complete their handshakes into the firmware
       accept backlog WITHOUT pumping our accept loop — nothing is delivered to
       on_accept_ready, so the server tracks none of them. */
    axl_msleep(4000);

    /* Teardown + immediate synchronous rebind, NO loop pump — the code path the
       backlog must not defeat. */
    if (c.abortive) {
        axl_http_server_free(c.s1, AXL_TEARDOWN_RESET);
    } else {
        axl_http_server_free(c.s1, AXL_TEARDOWN_GRACEFUL);
    }
    c.s1 = NULL;

    c.s2 = axl_http_server_new(8080);
    if (c.s2 != NULL) {
        axl_http_server_add_route(c.s2, "GET", "/plain", on_get_plain, NULL);
        c.rebind_rc = axl_http_server_start(c.s2, c.loop);
    } else {
        c.rebind_rc = -1;
    }
    axl_printf("REBIND-RC:%d\n", c.rebind_rc);
    axl_printf(c.rebind_rc == 0 ? "REBIND-OK\n" : "REBIND-FAIL\n");
    axl_printf("READY2\n");

    /* Prove the rebound server actually serves on the reused port. */
    if (c.s2 != NULL && c.rebind_rc == 0) {
        axl_loop_add_timeout(c.loop, 3000, rebind_quit_cb, &c);
        axl_loop_run(c.loop);
        axl_http_server_free(c.s2, AXL_TEARDOWN_RESET);
    }
    axl_loop_free(c.loop);
    axl_printf("REBIND-DONE\n");
    return 0;
}

// ---------------------------------------------------------------------------
// serve-rebind-churn — the pending-deferred-close port-release regression.
//
// serve-rebind-load exercises the accept BACKLOG (never-accepted connections).
// This exercises the third port-holder category: connections that were accepted
// and SERVED, then GRACEFULLY closed (Connection: close), whose close is still
// in flight at teardown — its s->conns slot already freed, its AxlTcpCloseCtx /
// on_close_event now owned by the LOOP, not the server. Those deferred closes
// keep a PCB on the port until finalize_close_ctx runs (~2 s TIME_WAIT, needs a
// pump), and hold a caller-owned loop source.
//
// It pumps briefly so the server accepts + serves + graceful-closes the host's
// churn connections (leaving deferred closes in flight), then STOPS pumping and
// frees + rebinds synchronously. A correct abortive teardown must finalize those
// deferred closes synchronously and loop-free, so:
//   abortive (GREEN): REBIND-RC == 0 AND axl_loop_free reports zero still-active
//                     caller-owned sources.
//   abortive, unfixed (RED): REBIND-RC != 0 and the deferred-close sources leak.
// Driven by test-http-rebind-churn-qemu.sh.
// ---------------------------------------------------------------------------

static int
run_serve_rebind_churn_mode(const char *variant)
{
    RebindCtx c;
    axl_memset(&c, 0, sizeof(c));
    c.abortive = (axl_strcmp(variant, "abortive") == 0);

    axl_net_auto_init(SIZE_MAX, 10);

    c.loop = axl_loop_new();
    c.s1   = axl_http_server_new(8080);
    if (c.loop == NULL || c.s1 == NULL) {
        axl_printf("ERROR: rebind-churn setup failed\n");
        return -1;
    }
    axl_http_server_add_route(c.s1, "GET", "/plain", on_get_plain, NULL);
    if (axl_http_server_start(c.s1, c.loop) != 0) {
        axl_printf("ERROR: rebind-churn initial start failed\n");
        return -1;
    }
    axl_printf("READY\n");

    /* Pump ~1.2 s so the server accepts + serves + graceful-closes the host's
       churn connections. Their closes go DEFERRED on the loop (TIME_WAIT ~2 s),
       so when the pump stops they are still in flight. */
    axl_loop_add_timeout(c.loop, 1200, rebind_quit_cb, &c);
    axl_loop_run(c.loop);

    /* Teardown + rebind synchronously, NO further pump — the deferred closes
       are still in flight and must be finalized by the abortive free itself. */
    if (c.abortive) {
        axl_http_server_free(c.s1, AXL_TEARDOWN_RESET);
    } else {
        axl_http_server_free(c.s1, AXL_TEARDOWN_GRACEFUL);
    }
    c.s1 = NULL;

    c.s2 = axl_http_server_new(8080);
    if (c.s2 != NULL) {
        axl_http_server_add_route(c.s2, "GET", "/plain", on_get_plain, NULL);
        c.rebind_rc = axl_http_server_start(c.s2, c.loop);
    } else {
        c.rebind_rc = -1;
    }
    axl_printf("REBIND-RC:%d\n", c.rebind_rc);
    axl_printf(c.rebind_rc == 0 ? "REBIND-OK\n" : "REBIND-FAIL\n");

    /* Free the rebound server (no conns) and the loop with NO intervening pump.
       axl_loop_free logs a "caller-owned event source still active" error for
       every deferred close the abortive free failed to finalize. */
    if (c.s2 != NULL) {
        axl_http_server_free(c.s2, AXL_TEARDOWN_RESET);
    }
    axl_loop_free(c.loop);
    axl_printf("REBIND-DONE\n");
    return 0;
}

// ---------------------------------------------------------------------------
// serve-rebind-multi — deferred-close finalize must be SCOPED to one listener.
//
// TWO servers (A :8080, B :8081) on ONE loop, the SoftBMC multi-server topology.
// Both are pumped so both accumulate in-flight loop-deferred graceful closes.
// Then ONLY server A is abortive-freed + rebound, with NO pump. The fix finalizes
// A's deferred closes scoped by A's listener_id — it must NOT touch B's. Proof:
//   - A's port :8080 rebinds immediately (REBIND-A-RC:0), and
//   - server B is UNHARMED: after a pump (which finalizes B's deferred closes the
//     normal way via on_close_event), B still serves on :8081, both A' and B
//     answer 200, and axl_loop_free reports zero still-active sources.
// A broken scope (finalizing B's ctxs during A's teardown) would double-finalize
// B's closes when the loop later pumps on_close_event -> crash/hang/leak, and B
// would not serve. Driven by test-http-rebind-multi-qemu.sh.
// ---------------------------------------------------------------------------

static bool
multi_quit_cb(void *data)
{
    axl_loop_quit((AxlLoop *)data);
    return AXL_SOURCE_REMOVE;
}

static int
run_serve_rebind_multi_mode(void)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *a    = axl_http_server_new(8080);
    AxlHttpServer *b    = axl_http_server_new(8081);
    if (loop == NULL || a == NULL || b == NULL) {
        axl_printf("ERROR: rebind-multi setup failed\n");
        return -1;
    }
    axl_http_server_add_route(a, "GET", "/plain", on_get_plain, NULL);
    axl_http_server_add_route(b, "GET", "/plain", on_get_plain, NULL);
    if (axl_http_server_start(a, loop) != 0 || axl_http_server_start(b, loop) != 0) {
        axl_printf("ERROR: rebind-multi start failed\n");
        return -1;
    }
    axl_printf("READY\n");

    /* Pump ~1.2 s so BOTH servers serve + graceful-close the host's churn on
       :8080 AND :8081 — deferred closes for both listeners now sit on the loop. */
    axl_loop_add_timeout(loop, 1200, multi_quit_cb, loop);
    axl_loop_run(loop);

    /* Abortive-free ONLY server A + rebind :8080, NO pump. The finalize must
       clear A's deferred closes (scoped by A's listener_id) and leave B's. */
    axl_http_server_free(a, AXL_TEARDOWN_RESET);
    AxlHttpServer *a2 = axl_http_server_new(8080);
    int rc_a = -1;
    if (a2 != NULL) {
        axl_http_server_add_route(a2, "GET", "/plain", on_get_plain, NULL);
        rc_a = axl_http_server_start(a2, loop);
    }
    axl_printf("REBIND-A-RC:%d\n", rc_a);
    axl_printf(rc_a == 0 ? "REBIND-A-OK\n" : "REBIND-A-FAIL\n");
    axl_printf("READY2\n");

    /* Pump: B's deferred closes finalize the normal way (on_close_event); A' and B
       both serve the host's verification GETs. If A's teardown had corrupted B's
       deferred ctxs this pump would fault or B would stop serving. */
    axl_loop_add_timeout(loop, 3000, multi_quit_cb, loop);
    axl_loop_run(loop);

    if (a2 != NULL) {
        axl_http_server_free(a2, AXL_TEARDOWN_RESET);
    }
    axl_http_server_free(b, AXL_TEARDOWN_RESET);
    axl_loop_free(loop);
    axl_printf("MULTI-DONE\n");
    return 0;
}

// ---------------------------------------------------------------------------
// socket-rebind-load — axl_socket_free(RESET) parity with the HTTP path.
//
// The BSD-style AxlSocket API is a veneer over AxlTcp. axl_socket_free(RESET)
// must deliver the same port-releasing teardown as axl_http_server_free(RESET):
// a stream-socket listener with a firmware accept backlog at teardown rebinds
// immediately, no pump. Same reproduction as serve-rebind-load, via the socket
// API: listen + arm async accept (a real async socket server), never pump, let
// the host queue a backlog, then free + re-listen synchronously.
//   abortive (GREEN): REBIND-RC == 0 — the wrapper reached axl_tcp_close(RESET).
//   graceful (RED): REBIND-RC != 0 — the backlog still holds the port.
// Driven by test-socket-rebind-load-qemu.sh.
// ---------------------------------------------------------------------------

static bool
socket_accept_noop_cb(AxlSocket *client, AxlStatus status, void *data)
{
    (void)data;
    /* Never actually fires (the loop is not pumped before teardown); if it did,
       release the accepted client rather than leak it. */
    if (status == AXL_OK && client != NULL) {
        axl_socket_free(client, AXL_TEARDOWN_GRACEFUL);
    }
    return true;   /* keep accepting */
}

static int
run_socket_rebind_load_mode(const char *variant)
{
    bool abortive = (axl_strcmp(variant, "abortive") == 0);

    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop   *loop = axl_loop_new();
    AxlSocket *s1   = axl_socket_new(AXL_SOCKET_STREAM);
    if (loop == NULL || s1 == NULL) {
        axl_printf("ERROR: socket rebind-load setup failed\n");
        return -1;
    }
    if (axl_socket_listen(s1, 8080) != AXL_OK) {
        axl_printf("ERROR: socket listen failed\n");
        return -1;
    }
    /* Arm async accept (a real async socket server) but never pump the loop, so
       the host's connections queue in the firmware accept backlog undelivered. */
    axl_socket_accept_async(s1, loop, socket_accept_noop_cb, NULL);
    axl_printf("READY\n");

    axl_msleep(4000);

    /* Teardown + immediate synchronous re-listen, NO pump. */
    if (abortive) {
        axl_socket_free(s1, AXL_TEARDOWN_RESET);
    } else {
        axl_socket_free(s1, AXL_TEARDOWN_GRACEFUL);
    }

    AxlSocket *s2 = axl_socket_new(AXL_SOCKET_STREAM);
    int rc = (s2 != NULL) ? axl_socket_listen(s2, 8080) : -1;
    axl_printf("REBIND-RC:%d\n", rc);
    axl_printf(rc == AXL_OK ? "REBIND-OK\n" : "REBIND-FAIL\n");
    axl_printf("READY2\n");

    if (s2 != NULL) {
        axl_socket_free(s2, AXL_TEARDOWN_RESET);
    }
    axl_loop_free(loop);
    axl_printf("REBIND-DONE\n");
    return 0;
}

// ---------------------------------------------------------------------------
// serve-rebind-storm — AXL_TEARDOWN_RESET must RETURN BOUNDED under a
// connect-storm. Unlike serve-rebind-load (a fixed backlog opened before
// teardown), here the host hammers NEW connections continuously THROUGH the
// free: the firmware keeps completing handshakes and refilling the accept
// backlog while the drain runs. A drain that re-arms Accept and chases fresh
// arrivals never converges and the free wedges. The free MUST quiesce new
// accepts / bound itself and return.
//
// Brackets the free with FREE-START / FREE-DONE so the harness can measure its
// wall-clock from the serial timestamps. Driven by test-http-rebind-storm-qemu.sh.
// ---------------------------------------------------------------------------

static int
run_serve_rebind_storm_mode(void)
{
    axl_net_auto_init(SIZE_MAX, 10);

    AxlLoop       *loop = axl_loop_new();
    AxlHttpServer *s1   = axl_http_server_new(8080);
    if (loop == NULL || s1 == NULL) {
        axl_printf("ERROR: rebind-storm setup failed\n");
        return -1;
    }
    axl_http_server_add_route(s1, "GET", "/plain", on_get_plain, NULL);
    if (axl_http_server_start(s1, loop) != 0) {
        axl_printf("ERROR: rebind-storm initial start failed\n");
        return -1;
    }
    axl_printf("READY\n");

    /* Host storms new connections from here through the free below. */
    axl_msleep(4000);

    axl_printf("FREE-START\n");
    axl_http_server_free(s1, AXL_TEARDOWN_RESET);
    axl_printf("FREE-DONE\n");

    AxlHttpServer *s2 = axl_http_server_new(8080);
    int rc = -1;
    if (s2 != NULL) {
        axl_http_server_add_route(s2, "GET", "/plain", on_get_plain, NULL);
        rc = axl_http_server_start(s2, loop);
    }
    axl_printf("REBIND-RC:%d\n", rc);
    axl_printf(rc == 0 ? "REBIND-OK\n" : "REBIND-FAIL\n");
    if (s2 != NULL && rc == 0) {
        axl_http_server_free(s2, AXL_TEARDOWN_RESET);
    }
    axl_loop_free(loop);
    axl_printf("REBIND-DONE\n");
    return 0;
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

    if (argc >= 3 && axl_strcmp(argv[1], "serve-rebind") == 0) {
        return run_serve_rebind_mode(argv[2]);
    }

    if (argc >= 2 && axl_strcmp(argv[1], "serve-rebind-storm") == 0) {
        return run_serve_rebind_storm_mode();
    }

    if (argc >= 3 && axl_strcmp(argv[1], "serve-rebind-load") == 0) {
        return run_serve_rebind_load_mode(argv[2]);
    }

    if (argc >= 3 && axl_strcmp(argv[1], "serve-rebind-churn") == 0) {
        return run_serve_rebind_churn_mode(argv[2]);
    }

    if (argc >= 2 && axl_strcmp(argv[1], "serve-rebind-multi") == 0) {
        return run_serve_rebind_multi_mode();
    }

    if (argc >= 3 && axl_strcmp(argv[1], "socket-rebind-load") == 0) {
        return run_socket_rebind_load_mode(argv[2]);
    }

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
    if (argc >= 2 && axl_strcmp(argv[1], "serve-tls-ws-close-pendtx-driver") == 0) {
        return run_serve_tls_ws_close_pendtx_driver_mode();
    }

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
    // "serve-shell-fv-coexist" -- foreground FV-embedded Shell (no file staged)
    // + background HTTP pumped by axl_loop_attach_driver. The axl_shell_launch_fv
    // round-trip proof.
    //
    if (argc >= 2 && axl_strcmp(argv[1], "serve-shell-fv-coexist") == 0) {
        return run_serve_shell_fv_coexist_mode();
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

    // Four concurrent raw EFI_TCP4.Transmit tokens on one connection —
    // the AXL-Tcp-Queue-Design §7 spike. test-tcp-multi-transmit-qemu.sh
    if (argc >= 4 && axl_strcmp(argv[1], "tcp-multi-tx") == 0) {
        return run_tcp_multi_transmit_mode(
            argv[2], argv[3],
            (argc >= 5) ? argv[4] : NULL,
            (argc >= 6) && axl_strcmp(argv[5], "reverse") == 0,
            (argc >= 7) ? argv[6] : NULL);
    }

    //
    // "9p-client <host> <port>" -- 9P2000.L connect + attach round-trip
    //
    if (argc >= 4 && axl_strcmp(argv[1], "9p-client") == 0) {
        return run_9p_client_mode(argv[2], argv[3]);
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
    test_http_response_get_json();
    test_http_add_routes_variadic();

    //
    // IPv4 parse / format (no network)
    //
    axl_printf("\n--- IPv4 Parse/Format ---\n");
    test_ipv4_parse_format();
    test_ipv4_parse_cidr();

    //
    // MAC parse / format (no network)
    //
    axl_printf("\n--- MAC Parse/Format ---\n");
    test_mac_format_parse();

    //
    // Driver selection — pure predicates (no network)
    //
    axl_printf("\n--- Driver Selection ---\n");
    test_net_driver_is_ipxe();

    //
    // AxlNetOpts (no network — validation only)
    //
    axl_printf("\n--- AxlNetOpts ---\n");
    test_net_opts_validation();
    test_net_resolve_ptr_validation();
    test_ws_conn_api_validation();
    test_nic_registry_contract();
    test_net_list_interfaces_alloc_contract();
    test_auto_init_opts_validation();

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
    test_tcp_send_async_queued();
    test_tcp_send_async_flush_on_close();
    test_tcp_send_close_from_send_callback();
    test_tcp_send_sync_behind_async();
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
