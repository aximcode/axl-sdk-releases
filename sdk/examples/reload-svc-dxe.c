/**
 * reload-svc-dxe.c — the service half of the axl_service_reload example.
 *
 * A minimal AXL_SERVICE_DRIVER service: it serves :8080 and reports its
 * generation. It never reloads itself. A resident service's own code runs in
 * event callbacks, at TPL_CALLBACK, where UEFI forbids LoadImage and
 * StartImage (2.11 Table 7.3) -- so the replacement is asked for by an
 * application: reload-svc.c, run from the Shell, calls axl_service_reload().
 * That loads the new image, stops this one (its teardown abortive-frees the
 * server, so the port is free) and starts the replacement, which rebinds it.
 *
 * A generation counter kept in a volatile UEFI variable (test-only) lets the
 * harness tell OLD (gen 0) from NEW (gen 1); the launcher bumps it. Driven by
 * test-service-reload-qemu.sh.
 */
#include <axl.h>

#define SVC_PORT 8080

static const AxlGuid RSVC_NS_GUID =
    AXL_GUID(0x71c0de11, 0x4a22, 0x4e55,
             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01);

static AxlHttpServer *g_server;
static uint32_t       g_gen;

static const AxlService reload_svc;   /* forward: setup names it */

static int
on_version(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    char body[48];
    axl_snprintf(body, sizeof(body), "{\"gen\":%u}", (unsigned)g_gen);
    axl_http_response_set_json(resp, body);
    return 0;
}

static int
rsvc_setup(AxlLoop *loop, void *user)
{
    (void)user;

    /* The generation counter lives in this namespace — without it the
       reload demo cannot count, so a failed bind is fatal to setup. */
    if (axl_nvstore_register_namespace("rsvc", &RSVC_NS_GUID) != AXL_OK) {
        axl_printf("RSVC: nvstore namespace 'rsvc' unavailable\n");
        return AXL_ERR;
    }
    uint32_t g  = 0;
    size_t   sz = sizeof(g);
    if (axl_nvstore_get("rsvc", "gen", &g, &sz) == 0 && sz == sizeof(g)) {
        g_gen = g;
    }

    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("RSVC: gen%u net FAIL\n", (unsigned)g_gen);
        return AXL_ERR;
    }
    g_server = axl_http_server_new(SVC_PORT);
    if (g_server == NULL) {
        return AXL_ERR;
    }
    axl_http_server_add_route(g_server, "GET", "/version", on_version, NULL);
    if (axl_http_server_start(g_server, loop) != 0) {
        axl_printf("RSVC: gen%u start FAIL (port busy?)\n", (unsigned)g_gen);
        return AXL_ERR;
    }
    axl_printf("RSVC: gen%u SETUP on :%d\n", (unsigned)g_gen, SVC_PORT);

    /* What a service must not do: ask for its own reload. Even here, at
       TPL_APPLICATION (setup runs inside the loader's StartImage), the call
       is refused -- this image cannot unload itself -- and nothing changes. */
    if (g_gen == 0) {
        int self = axl_service_reload(&reload_svc, "fs0:\\reload-svc-dxe.efi");
        axl_printf("RSVC: gen%u self-reload rc=%d\n", (unsigned)g_gen, self);
    }
    return AXL_OK;
}

static int
rsvc_teardown(void *user)
{
    (void)user;
    if (g_server != NULL) {
        axl_http_server_free(g_server, AXL_TEARDOWN_RESET);
        g_server = NULL;
    }
    axl_printf("RSVC: gen%u TEARDOWN (abortive)\n", (unsigned)g_gen);
    return AXL_OK;
}

static const AxlService reload_svc = {
    .name           = "reload-svc",
    .setup          = rsvc_setup,
    .teardown       = rsvc_teardown,
    .driver_tick_ms = 20,
};

AXL_SERVICE_DRIVER(reload_svc)
