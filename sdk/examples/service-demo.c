/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * service-demo.c — single-file AxlService demo.
 *
 * AXL_SERVICE(service_demo) emits whichever entry point the current
 * compile needs:
 *   - When -DAXL_SERVICE_BUILD_DRIVER is defined → expands to
 *     AXL_SERVICE_DRIVER (driver-image DriverEntry).
 *   - Otherwise → expands to a main() that delegates to
 *     axl_service_main with the embedded driver baked in.
 *
 * Build (consumer):
 *
 *     axl-cc --service service_demo service-demo.c
 *     # produces: service_demo.efi (launcher) + service_demo-dxe.efi (driver)
 *
 * Run:
 *
 *     service_demo.efi start               # start + supervise (Ctrl-C to stop)
 *     service_demo.efi start --detach      # start and exit
 *     service_demo.efi start --port 9090
 *     service_demo.efi stop                # stop a started driver
 *     service_demo.efi status              # query
 *
 * The actual service body — setup, the driver-tick loop, teardown —
 * runs inside the driver image (service_demo-dxe.efi). The launcher
 * (service_demo.efi) is just glue: it parses argv, installs the
 * driver, and supervises until Ctrl-C.
 */

#include <axl.h>

AXL_LOG_DOMAIN("service-demo");

typedef struct {
    uint64_t    port;
    bool        verbose;
    const char *name;
} DemoOpts;

static DemoOpts g_opts;

/* short_name + choices + min/max (the trailing AxlConfigDesc fields,
   ignored by AxlConfig parsing) flow through axl_service_main's
   synthesizer into the AxlArgDesc[] it builds for the `start` verb. They
   give the demo a real-shaped CLI: `start -p 9090 -v --name foo` parses
   the same as the long form, `--name` is a CHOICE so `--name bogus`
   errors out at parse time, and `port` carries a [1,65535] range so
   `--port 99999` is rejected at parse time rather than reaching setup. */
static const char *const demo_names[] = { "demo", "alpha", "beta", NULL };
static const AxlConfigDesc demo_descs[] = {
    { .key = "port",    .type = AXL_CFG_UINT,   .default_value = "8080",
      .description = "Listen port",
      .offset = offsetof(DemoOpts, port),    .field_size = sizeof(uint64_t),
      .short_name = 'p', .min = 1, .max = 65535 },
    { .key = "verbose", .type = AXL_CFG_BOOL,   .default_value = "false",
      .description = "Verbose mode",
      .offset = offsetof(DemoOpts, verbose), .field_size = sizeof(bool),
      .short_name = 'v' },
    { .key = "name",    .type = AXL_CFG_STRING, .default_value = "demo",
      .description = "Service name",
      .offset = offsetof(DemoOpts, name),    .field_size = sizeof(const char *),
      .choices = demo_names },
    { 0 }
};

/* Setup adds a one-shot idle that prints READY (so the integration
   test can grep-assert the loop is actually running) and then quits.
   A real consumer's setup would register persistent sources (TCP
   listeners, timers, etc.) here and not quit. */
static bool
demo_idle_ready(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    axl_info("READY: port=%llu verbose=%d name=%s",
             (unsigned long long)g_opts.port,
             (int)g_opts.verbose,
             g_opts.name != NULL ? g_opts.name : "(null)");
    axl_loop_quit(loop);
    return AXL_SOURCE_REMOVE;
}

static int
demo_setup(AxlLoop *loop, void *user)
{
    (void)user;
    axl_info("setup: port=%llu verbose=%d name=%s",
             (unsigned long long)g_opts.port,
             (int)g_opts.verbose,
             g_opts.name != NULL ? g_opts.name : "(null)");
    if (axl_loop_add_idle(loop, demo_idle_ready, loop) == 0) {
        return AXL_ERR;
    }
    return AXL_OK;
}

static int
demo_teardown(void *user)
{
    (void)user;
    axl_info("teardown");
    return AXL_OK;
}

/* The service's identity GUID is derived from `name` via
   axl_guid_v5 — same name in both binaries -> same GUID, no
   uuidgen needed. */
static const AxlService service_demo = {
    .name           = "service-demo",
    .opts_descs     = demo_descs,
    .setup          = demo_setup,
    .teardown       = demo_teardown,
    .user           = &g_opts,
    .driver_tick_ms = 50,
};

AXL_SERVICE(service_demo);
