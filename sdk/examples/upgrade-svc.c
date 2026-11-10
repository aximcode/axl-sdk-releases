/**
 * upgrade-svc.c — a resident service that upgrades itself, unattended:
 * stage, reset, apply at the next start (axl-service.h, "Staged upgrade").
 *
 * One source, two images, as with AXL_SERVICE:
 *   - with -DAXL_SERVICE_BUILD_DRIVER: upgrade-svc-dxe.efi, the service;
 *   - without: upgrade-svc.efi, its launcher.
 * Both share the deploy below, which pins the service's image to
 * fs0:\upgrade-svc-dxe.efi -- the file a staged upgrade goes beside.
 *
 * The service serves HTTP on :8080:
 *   GET  /version   {"version":"1","last":"none|applied|retried|rolled-back"}
 *   POST /upgrade   the body is a new upgrade-svc-dxe.efi: it is staged
 *                   (axl_service_stage) and, once the reply is sent, the box
 *                   resets. 202 when staged; 200 {"rc":0,"staged":false},
 *                   and no reset, when it is the image already running;
 *                   400/403/409/500 otherwise, with the AxlStatus in
 *                   {"rc":N}.
 *   POST /shutdown  power off (for the tests that drive this example)
 *
 * startup.nsh runs the launcher on every boot:
 *
 *     fs0:\upgrade-svc.efi start --detach
 *
 * and its `start` applies a staged image: a trial from upgrade-svc-dxe.efi.new
 * under the watchdog, a grace period, then the promotion -- or, if the new
 * image does not come up, a rollback to the one that ran before. The service
 * reports the outcome from GET /version (axl_service_last_upgrade).
 *
 * Staging is all the service does itself: it runs at TPL_CALLBACK, where
 * UEFI forbids LoadImage and StartImage, and writing a file and resetting are
 * allowed there. Authenticating an upload is the service's job; this example
 * accepts any image for brevity -- AxlServiceStageOpts::verify is where a
 * real one checks a signer or refuses a downgrade.
 *
 * The test variants (-DUPGRADE_SVC_VERSION, -DUPGRADE_SVC_BROKEN,
 * -DUPGRADE_SVC_HANG, -DUPGRADE_SVC_NO_NET -- comes up without a network, to
 * keep a test boot short -- -DUPGRADE_SVC_WEDGE -- up, then a callback that
 * never returns, inside the grace period -- -DUPGRADE_SVC_IN_GRACE=1/2/3 --
 * up, then, inside the grace period, a restage of fs0:\restage.efi, an
 * unstage, or that file written over P.new directly -- and the launcher's
 * -DUPGRADE_SVC_WATCHDOG_SEC, -DUPGRADE_SVC_GRACE_SEC and -DUPGRADE_SVC_TRIES)
 * are driven by the test-service-stage-*.sh tests.
 */
#include <axl.h>

#ifndef UPGRADE_SVC_VERSION
#define UPGRADE_SVC_VERSION "1"
#endif
#ifndef UPGRADE_SVC_WATCHDOG_SEC
#define UPGRADE_SVC_WATCHDOG_SEC 0   /* the library's default */
#endif
#ifndef UPGRADE_SVC_GRACE_SEC
#define UPGRADE_SVC_GRACE_SEC    0   /* the library's default */
#endif
#ifndef UPGRADE_SVC_TRIES
#define UPGRADE_SVC_TRIES        0   /* the library's default */
#endif

#define UPGRADE_SVC_PORT 8080

static int  usvc_setup(AxlLoop *loop, void *user);
static int  usvc_teardown(void *user);

static const AxlService upgrade_svc = {
    .name           = "upgrade-svc",
    .setup          = usvc_setup,
    .teardown       = usvc_teardown,
    .driver_tick_ms = 20,
};

static const AxlServiceStartOpts upgrade_opts = {
    .tries            = UPGRADE_SVC_TRIES,
    .watchdog_sec     = UPGRADE_SVC_WATCHDOG_SEC,
    .commit_grace_sec = UPGRADE_SVC_GRACE_SEC,
};

static const AxlServiceDeploy upgrade_deploy = {
    .service     = &upgrade_svc,
    .driver_path = "fs0:\\upgrade-svc-dxe.efi",
    .staged_opts = &upgrade_opts,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static AxlHttpServer *g_server;
static AxlLoop       *g_loop;

static const char *
outcome_name(void)
{
    AxlServiceStagedResult r;
    if (axl_service_last_upgrade(&upgrade_svc, &r) != AXL_OK) {
        return "none";
    }
    switch (r) {
    case AXL_SERVICE_STAGED_APPLIED:     return "applied";
    case AXL_SERVICE_STAGED_RETRIED:     return "retried";
    case AXL_SERVICE_STAGED_ROLLED_BACK: return "rolled-back";
    case AXL_SERVICE_STAGED_NONE:        return "none";
    }
    return "none";
}

static int
on_version(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    char body[96];
    axl_snprintf(body, sizeof(body), "{\"version\":\"%s\",\"last\":\"%s\"}",
                 UPGRADE_SVC_VERSION, outcome_name());
    axl_http_response_set_json(resp, body);
    return 0;
}

static bool
reset_cold(void *data)
{
    (void)data;
    axl_printf("USVC: resetting to apply the staged upgrade\n");
    axl_reset(AXL_RESET_COLD);
    return AXL_SOURCE_REMOVE;
}

static bool
power_off(void *data)
{
    (void)data;
    axl_reset(AXL_RESET_SHUTDOWN);
    return AXL_SOURCE_REMOVE;
}

#ifdef UPGRADE_SVC_IN_GRACE
/* Up and published, and inside the trial's commit grace (the test launcher's
   is 3 s): touch the staged upgrade as a request handler could. 1 restages
   fs0:\restage.efi, 2 unstages, 3 writes restage.efi over P.new by hand --
   past axl_service_stage, as any writer of the file could. */
static bool
in_grace(void *data)
{
    (void)data;
    AxlServiceStagedResult last;
    int    lrc = axl_service_last_upgrade(&upgrade_svc, &last);
    bool   busy = axl_service_upgrade_in_flight(&upgrade_svc);
    int    rc  = AXL_ERR;
#if UPGRADE_SVC_IN_GRACE == 2
    rc = axl_service_unstage(&upgrade_deploy);
#else
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents("fs0:\\restage.efi", &buf, &len) == AXL_OK) {
#if UPGRADE_SVC_IN_GRACE == 1
        rc = axl_service_stage(&upgrade_deploy, NULL, buf, len);
#else
        rc = axl_file_write_atomic("fs0:\\upgrade-svc-dxe.efi.new", buf, len);
#endif
        axl_free(buf);
    }
#endif
    axl_printf("USVC: v%s in grace: in_flight=%d last_rc=%d rc=%d\n", UPGRADE_SVC_VERSION,
               busy ? 1 : 0, lrc, rc);
    return AXL_SOURCE_REMOVE;
}

/* Well after the grace (the test holds the Shell with `stall`): the launcher
   is done -- nothing in flight, and the verdict is this attempt's. */
static bool
after_grace(void *data)
{
    (void)data;
    AxlServiceStagedResult last = AXL_SERVICE_STAGED_NONE;
    int lrc = axl_service_last_upgrade(&upgrade_svc, &last);
    axl_printf("USVC: v%s after grace: in_flight=%d last_rc=%d last=%d\n",
               UPGRADE_SVC_VERSION, axl_service_upgrade_in_flight(&upgrade_svc) ? 1 : 0,
               lrc, (int)last);
    return AXL_SOURCE_REMOVE;
}
#endif

#ifdef UPGRADE_SVC_WEDGE
/* Up and published, then stuck in a callback: only the watchdog the trial
   re-armed for the grace period can end this. */
static bool
wedge(void *data)
{
    (void)data;
    axl_printf("USVC: v%s wedging a callback on purpose\n", UPGRADE_SVC_VERSION);
    for (volatile int spin = 1; spin != 0;) {
    }
    return AXL_SOURCE_REMOVE;
}
#endif

static int
on_upgrade(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)data;
    /* Stage only: this runs in a callback, where no image may be loaded. */
    int  rc = axl_service_stage(&upgrade_deploy, NULL, req->body, req->body_size);
    char body[48];
    axl_snprintf(body, sizeof(body), "{\"rc\":%d}", rc);
    axl_http_response_set_json(resp, body);
    axl_printf("USVC: v%s stage rc=%d\n", UPGRADE_SVC_VERSION, rc);
    if (rc == AXL_OK && !axl_service_is_staged(&upgrade_deploy)) {
        /* The image is the one running: nothing was staged, so a reset
           would only interrupt the service. */
        axl_http_response_set_json(resp, "{\"rc\":0,\"staged\":false}");
        axl_http_response_set_status(resp, 200);
    } else if (rc == AXL_OK) {
        axl_http_response_set_status(resp, 202);
        /* Reset once the reply has gone out. */
        axl_loop_add_timeout(g_loop, 500, reset_cold, NULL);
    } else {
        axl_http_response_set_status(resp, rc == AXL_INVALID ? 400
                                         : rc == AXL_DENIED  ? 403
                                         : rc == AXL_UNSUPPORTED ? 409 : 500);
    }
    return 0;
}

static int
on_shutdown(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    axl_http_response_set_json(resp, "{}");
    axl_loop_add_timeout(g_loop, 500, power_off, NULL);
    return 0;
}

static int
usvc_setup(AxlLoop *loop, void *user)
{
    (void)user;
    g_loop = loop;
#ifdef UPGRADE_SVC_BROKEN
    axl_printf("USVC: v%s setup failing on purpose\n", UPGRADE_SVC_VERSION);
    return AXL_ERR;
#endif
#ifdef UPGRADE_SVC_HANG
    axl_printf("USVC: v%s setup hanging on purpose\n", UPGRADE_SVC_VERSION);
    for (;;) {
        axl_msleep(1000);
    }
#endif
#ifdef UPGRADE_SVC_NO_NET
    axl_printf("USVC: v%s up, no network (last upgrade: %s)\n", UPGRADE_SVC_VERSION,
               outcome_name());
#ifdef UPGRADE_SVC_WEDGE
    axl_loop_add_timeout(loop, 1000, wedge, NULL);
#endif
#ifdef UPGRADE_SVC_IN_GRACE
    axl_loop_add_timeout(loop, 1000, in_grace, NULL);
    axl_loop_add_timeout(loop, 6000, after_grace, NULL);
#endif
    return AXL_OK;
#endif
    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("USVC: v%s net FAIL\n", UPGRADE_SVC_VERSION);
        return AXL_ERR;
    }
    g_server = axl_http_server_new(UPGRADE_SVC_PORT);
    if (g_server == NULL) {
        return AXL_ERR;
    }
    axl_http_server_add_route(g_server, "GET", "/version", on_version, NULL);
    axl_http_server_add_route(g_server, "POST", "/upgrade", on_upgrade, NULL);
    axl_http_server_add_route(g_server, "POST", "/shutdown", on_shutdown, NULL);
    if (axl_http_server_start(g_server, loop) != 0) {
        return AXL_ERR;
    }
    axl_printf("USVC: v%s serving on :%d (last upgrade: %s)\n", UPGRADE_SVC_VERSION,
               UPGRADE_SVC_PORT, outcome_name());
    return AXL_OK;
}

static int
usvc_teardown(void *user)
{
    (void)user;
    if (g_server != NULL) {
        axl_http_server_free(g_server, AXL_TEARDOWN_RESET);
        g_server = NULL;
    }
    return AXL_OK;
}

AXL_SERVICE_DRIVER(upgrade_svc)

#else  /* the launcher */

static int
usvc_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    return AXL_OK;
}

static int
usvc_teardown(void *user)
{
    (void)user;
    return AXL_OK;
}

int
main(int argc, char **argv)
{
    return axl_service_main(&upgrade_deploy, argc, argv);
}

#endif
