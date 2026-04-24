/**
 * http-server.c — HTTP server with JSON routes.
 *
 * Registers GET and POST routes, returns JSON responses.
 * Demonstrates axl_http_server_new, axl_http_server_add_route,
 * axl_http_response_set_json, axl_http_server_run.
 *
 * Build with: axl-cc http-server.c -o http-server.efi
 */

#include <axl.h>

static int
on_version(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{\"version\":\"1.0\"}");
    return 0;
}

static int
on_echo(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)data;
    resp->status_code = 200;
    resp->body = (void *)req->body;
    resp->body_size = req->body_size;
    resp->content_type = "text/plain";
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AXL_AUTOPTR(AxlHttpServer) server = axl_http_server_new(8080);
    if (server == NULL) {
        axl_printf("error: cannot create server\n");
        return 1;
    }

    axl_http_server_add_route(server, "GET", "/version", on_version, NULL);
    axl_http_server_add_route(server, "POST", "/echo", on_echo, NULL);

    axl_printf("HTTP server on port 8080\n");
    axl_printf("  GET  /version  — JSON version info\n");
    axl_printf("  POST /echo     — echo request body\n");
    axl_printf("Press Ctrl-C to stop.\n");

    axl_http_server_run(server);
    return 0;
}
