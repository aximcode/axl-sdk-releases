/**
 * reload-svc-dxe.c — verification driver for axl_service_reload.
 *
 * A minimal AXL_SERVICE_DRIVER service that self-reloads via the SDK built-in:
 * serves :8080, and ~3 s in calls axl_service_reload() to hot-swap itself to a
 * fresh copy of the same image, handing off the port. Its teardown
 * abortive-frees the server so the port is free when the replacement binds it.
 *
 * A generation counter kept in a volatile UEFI variable (test-only; the
 * framework's own handoff rides LoadOptions) lets the harness tell OLD (gen 0)
 * from NEW (gen 1). Driven by test-service-reload-qemu.sh.
 */
#include <axl.h>

#define SVC_PORT 8080

/* Staged only by the start-failure harness — see reload_trigger_cb. */
#define POISON_PATH "fs0:\\reload-svc-fail-dxe.efi"

static const AxlGuid RSVC_NS_GUID =
    AXL_GUID(0x71c0de11, 0x4a22, 0x4e55,
             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01);

static AxlHttpServer *g_server;
static AxlLoop       *g_loop;
static uint32_t       g_gen;

static const AxlService reload_svc;                 /* forward */
static bool reload_trigger_cb(void *data);          /* forward */

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
    g_loop = loop;

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

    if (g_gen == 0) {
        axl_loop_add_timeout(loop, 3000, reload_trigger_cb, NULL);
    }
    return AXL_OK;
}

static bool
reload_trigger_cb(void *data)
{
    (void)data;
    axl_printf("RSVC: gen%u triggering self-reload\n", (unsigned)g_gen);

    /* A replacement that cannot be LOADED tears nothing down — this service is
       still serving afterwards, and the return code says so (distinct from the
       start-failure code below). */
    int miss = axl_service_reload(&reload_svc, "fs0:\\no-such-image.efi");
    axl_printf("RSVC: gen%u load-miss rc=%d\n", (unsigned)g_gen, miss);

    /* Poison image staged? Then this boot is the start-failure scenario
       (test-service-reload-fail-qemu.sh): the replacement loads fine but its
       setup fails on purpose, so it never attaches. This service is DOWN
       afterwards, so there is nothing left to do but report the rc. */
    AxlFsEntry poison;
    if (axl_file_info(POISON_PATH, &poison) == AXL_OK
        && !axl_fs_entry_is_dir(&poison)) {
        int frc = axl_service_reload(&reload_svc, POISON_PATH);
        axl_printf("RSVC: gen%u start-fail rc=%d\n", (unsigned)g_gen, frc);
        return AXL_SOURCE_REMOVE;
    }

    /* Bump the generation the replacement will read (test observability). */
    uint32_t next = g_gen + 1;
    axl_nvstore_set("rsvc", "gen", &next, sizeof(next), AXL_NV_VOLATILE);

    int rc = axl_service_reload(&reload_svc, "fs0:\\reload-svc-dxe.efi");
    if (rc != AXL_OK) {
        axl_printf("RSVC: gen%u reload FAIL rc=%d\n", (unsigned)g_gen, rc);
    }
    /* On success the replacement reclaims us shortly. */
    return AXL_SOURCE_REMOVE;
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
