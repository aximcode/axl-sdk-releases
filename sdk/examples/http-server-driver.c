/**
 * http-server-driver.c — DXE-driver-mode HTTP server.
 *
 * Mirrors http-server.c but lives inside a DXE driver image: no
 * foreground caller, no axl_http_server_run, no int main. Instead
 * uses AXL_DRIVER for the firmware-side scaffolding and
 * axl_loop_attach_driver to drive the loop from a periodic
 * firmware timer notify.
 *
 * Used by test/integration/test-driver-http.sh to verify the
 * fully-async send_response path actually delivers complete
 * responses across multiple requests in driver mode (the v1
 * sync send_response broke after request 1 in driver mode —
 * headers landed but body never did, server stuck in CLOSE-WAIT).
 *
 * Build with: axl-cc --type driver http-server-driver.c -o http-server-driver.efi
 */

#include <axl.h>

AXL_LOG_DOMAIN("http-srv-drv");

#define HTTP_PORT       8080
#define LOOP_TICK_MS    50

static AxlHttpServer *g_server;
static AxlLoop       *g_loop;
static bool           g_loop_attached;

static int
on_version(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"version\":\"1.0\",\"mode\":\"driver\"}");
    return 0;
}

static int
on_echo(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)data;
    resp->status_code   = 200;
    resp->content_type  = "text/plain";
    /* Copy the request body — the framework frees resp->body after
       send. Pointing resp->body at req->body would alias conn->body
       and double-free (the dispatcher's tail and reset_connection
       both free it). Matches the established echo-handler pattern
       in test/unit/axl-test-net.c. */
    if (req->body != NULL && req->body_size > 0) {
        resp->body = axl_malloc(req->body_size);
        if (resp->body != NULL) {
            axl_memcpy(resp->body, req->body, req->body_size);
            resp->body_size = req->body_size;
        }
    }
    return 0;
}

static int
on_hello(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    /* Body large enough to push past one TCP packet so we exercise
       the multi-segment send path that the v1 sync send_response
       failed on. */
    static const char body[] =
        "Hello from a DXE-driver-mode HTTP server.\n"
        "If you can read this entire body, axl_loop_attach_driver +\n"
        "the fully-async send_response path are working: the firmware\n"
        "delivered the headers AND the body without the consumer's\n"
        "TPL_CALLBACK notify ever calling gBS->WaitForEvent.\n"
        "\n"
        "The previous sync send_response would emit headers into\n"
        "TCP4's buffer, then fail to wait for completion (WaitForEvent\n"
        "returns EFI_UNSUPPORTED above TPL_APPLICATION), leaving\n"
        "sock->send_source stale and rejecting the body send. Headers\n"
        "would arrive at the client but the body would not. This\n"
        "longer body confirms both paths now go through.\n";
    axl_http_response_set_text(resp, body);
    return 0;
}

static int
driver_main(AxlHandle image, AxlSystemTable *st);
static int
driver_unload(AxlHandle image);

AXL_DRIVER(driver_main, driver_unload)

static int
driver_main(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("FAIL: net-init\n");
        return AXL_ERR;
    }
    axl_printf("PASS: net-init\n");

    g_server = axl_http_server_new(HTTP_PORT);
    if (g_server == NULL) {
        axl_printf("FAIL: server-new\n");
        return AXL_ERR;
    }

    if (axl_http_server_add_routes(g_server,
            "GET",  "/version", on_version, NULL,
            "POST", "/echo",    on_echo,    NULL,
            "GET",  "/",        on_hello,   NULL,
            NULL) != AXL_OK)
    {
        axl_printf("FAIL: routes\n");
        goto fail_server;
    }
    axl_printf("PASS: routes\n");

    g_loop = axl_loop_new();
    if (g_loop == NULL) {
        axl_printf("FAIL: loop-new\n");
        goto fail_server;
    }

    /* axl_http_server_start brings the server up: allocates the
       per-conn pool, opens the listener, and registers async
       accept on the loop. One call. */
    if (axl_http_server_start(g_server, g_loop) != 0) {
        axl_printf("FAIL: server-start\n");
        goto fail_loop;
    }
    axl_printf("PASS: server-start on port %d\n", HTTP_PORT);

    if (axl_loop_attach_driver(g_loop, LOOP_TICK_MS) != AXL_OK) {
        axl_printf("FAIL: loop-attach-driver\n");
        goto fail_loop;
    }
    g_loop_attached = true;
    axl_printf("PASS: loop-attach-driver (%dms tick)\n", LOOP_TICK_MS);

    /* DriverEntry returns; firmware-managed timer notify drives
       the loop until DriverUnload. */
    return AXL_OK;

fail_loop:
    axl_loop_free(g_loop);
    g_loop = NULL;
fail_server:
    axl_http_server_free(g_server);
    g_server = NULL;
    return AXL_ERR;
}

static int
driver_unload(AxlHandle image)
{
    (void)image;

    /* Detach the loop FIRST so no notify is in flight as we tear
       down server state. */
    if (g_loop_attached) {
        axl_loop_detach_driver(g_loop);
        g_loop_attached = false;
    }

    /* Server before loop — server holds events registered against
       the loop's source table. */
    if (g_server != NULL) {
        axl_http_server_free(g_server);
        g_server = NULL;
    }

    if (g_loop != NULL) {
        axl_loop_free(g_loop);
        g_loop = NULL;
    }

    axl_printf("PASS: driver-http-unload\n");
    return AXL_OK;
}
