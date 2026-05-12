/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * service-demo-custom.c — AxlService consumer with a hand-written
 * `main()` showing consumer-visible AxlArgs + AxlConfig usage.
 *
 * Same shape as `service-demo.c` (single-file, dual-compiled — the
 * driver image is embedded in the launcher) BUT the consumer's
 * `main()` is written explicitly instead of via the `AXL_SERVICE`
 * macro. Useful when you want to:
 *
 *   - mix the standard `start` / `stop` / `status` verbs with
 *     custom verbs of your own;
 *   - take direct control of the AxlArgs verb tree (custom help
 *     prolog, parent-level flags, multi-tool composition);
 *   - call `axl_args_get_*` and walk `AxlConfigDesc` from your own
 *     code.
 *
 * The custom verb here is `config` — parses argv via AxlArgs,
 * walks the AxlConfig descriptor, prints the parsed values
 * alongside the descriptor metadata. Foreground-only; doesn't
 * touch the driver. Demonstrates the AxlArgs ↔ AxlConfig
 * connection that `axl_service_main` packages internally.
 *
 * Build (in-tree):
 *     make service-demo-custom
 *     # → service_demo_custom.efi + service_demo_custom-dxe.efi
 *
 * Build (consumer):
 *     axl-cc --service service_demo_custom service-demo-custom.c
 *
 * Run:
 *     service_demo_custom.efi start --port 9090 --verbose
 *     service_demo_custom.efi stop
 *     service_demo_custom.efi status
 *     service_demo_custom.efi config --port 9090 --name foo
 *
 * For consumers who don't need custom verbs, `AXL_SERVICE(svc)` in
 * service-demo.c is a one-liner that does the equivalent of this
 * file's main() (minus the `config` verb).
 */

#include <axl.h>

AXL_LOG_DOMAIN("service-demo-custom");

typedef struct {
    uint64_t    port;
    bool        verbose;
    const char *name;
} DemoOpts;

static DemoOpts g_opts;

/* AxlConfig descriptor — the source of truth for option types,
   defaults, and offsets into g_opts. Used in two ways below:
     1. AxlService.opts_descs — drives the cross-binary LoadOptions
        round-trip (axl_service_start_embedded serializes from here,
        AXL_SERVICE_DRIVER decodes into the driver-side g_opts).
     2. The `config` verb walks this directly to print values. */
static const AxlConfigDesc demo_descs[] = {
    { "port",    AXL_CFG_UINT,   "8080",  "Listen port",
      offsetof(DemoOpts, port),    sizeof(uint64_t) },
    { "verbose", AXL_CFG_BOOL,   "false", "Verbose mode",
      offsetof(DemoOpts, verbose), sizeof(bool) },
    { "name",    AXL_CFG_STRING, "demo",  "Service name",
      offsetof(DemoOpts, name),    sizeof(const char *) },
    { 0 }
};

static bool
demo_idle_ready(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    axl_info("READY: port=%llu name=%s",
             (unsigned long long)g_opts.port,
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

/* Different name from service-demo.c so both demos can run side-by-
   side without colliding on axl_service_is_running — identity is
   derived from `name` via axl_guid_v5. */
static const AxlService service_demo_custom = {
    .name           = "service-demo-custom",
    .opts_descs     = demo_descs,
    .setup          = demo_setup,
    .teardown       = demo_teardown,
    .user           = &g_opts,
    .driver_tick_ms = 50,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

/* Driver-image compile: emit the standard DriverEntry. The custom
   verb tree is foreground-only — the driver image just runs the
   service body. */
AXL_SERVICE_DRIVER(service_demo_custom);

#else

/* Foreground compile: hand-written main() with custom verb tree.
   The driver blob is embedded by `axl-cc --service` (or the
   in-tree Makefile via EMBED_BLOB) under the symbol
   axl_embedded_service_demo_custom. */
AXL_EMBED_DECLARE(service_demo_custom);

/* === Helper: populate g_opts from args via opts_descs ===
   axl_service_main does this internally; here it's spelled out
   so consumers can see the AxlArgs ↔ g_opts mechanism. */
static void
populate_opts(AxlArgs *a)
{
    g_opts.port    = axl_args_get_uint(a, "port");
    g_opts.verbose = axl_args_get_bool(a, "verbose");
    g_opts.name    = axl_args_get_string(a, "name");
}

static AxlServiceDeploy
make_deploy(void)
{
    AxlServiceDeploy d = {
        .service          = &service_demo_custom,
        .driver_blob      = AXL_EMBED_DATA(service_demo_custom),
        .driver_blob_len  = AXL_EMBED_SIZE(service_demo_custom),
        .driver_name      = "service-demo-custom-dxe.efi",
    };
    return d;
}

// ----------------------------------------------------------------------------
// Standard verbs — same behavior as axl_service_main's defaults, written
// out so consumers can see the lifecycle they're stitching together.
// ----------------------------------------------------------------------------

static int
verb_start(AxlArgs *a)
{
    populate_opts(a);
    AxlServiceDeploy d = make_deploy();

    if (axl_service_is_running(&d)) {
        axl_printf("%s: already running\n", service_demo_custom.name);
        return 0;
    }
    int rc = axl_service_start_embedded(&d);
    if (rc != AXL_OK) {
        axl_printf("%s: start failed (rc=%d)\n",
                   service_demo_custom.name, rc);
        return 1;
    }
    if (axl_args_get_bool(a, "detach")) {
        return 0;
    }

    /* Supervise: block on the default loop until Ctrl-C, then stop.
       Custom mains that want extra loop sources (ESC handler, status
       printer, etc.) register them on axl_loop_default() before this
       call. */
    return axl_service_supervise(&d);
}

static int
verb_stop(AxlArgs *a)
{
    (void)a;
    AxlServiceDeploy d = make_deploy();
    if (!axl_service_is_running(&d)) {
        axl_printf("%s: not running\n", service_demo_custom.name);
        return 0;
    }
    int rc = axl_service_stop(&d);
    axl_printf("%s: %s\n", service_demo_custom.name,
               rc == AXL_OK ? "stopped" : "stop failed");
    return rc == AXL_OK ? 0 : 1;
}

static int
verb_status(AxlArgs *a)
{
    (void)a;
    AxlServiceDeploy d = make_deploy();
    bool running = axl_service_is_running(&d);
    axl_printf("%s: %s\n", service_demo_custom.name,
               running ? "running" : "stopped");
    return running ? 0 : 1;
}

// ----------------------------------------------------------------------------
// Custom verb — `config`: parse argv via AxlArgs, walk the AxlConfig
// descriptor, print the parsed values. Foreground-only; doesn't load the
// driver. Demonstrates direct consumer use of axl_args_get_* and the
// AxlConfigDesc walk that axl_service_main does internally.
// ----------------------------------------------------------------------------

static int
verb_config(AxlArgs *a)
{
    populate_opts(a);

    axl_printf("%s configuration (parsed from argv):\n",
               service_demo_custom.name);

    for (const AxlConfigDesc *d = demo_descs; d->key != NULL; d++) {
        const uint8_t *field = (const uint8_t *)&g_opts + d->offset;
        axl_printf("  %-10s = ", d->key);
        switch (d->type) {
        case AXL_CFG_BOOL:
            axl_printf("%-8s",
                       *(const bool *)field ? "true" : "false");
            break;
        case AXL_CFG_UINT:
            if (d->field_size == sizeof(uint64_t)) {
                axl_printf("%-8llu",
                           (unsigned long long)*(const uint64_t *)field);
            }
            break;
        case AXL_CFG_STRING: {
            const char *s = *(const char *const *)field;
            axl_printf("%-8s", s != NULL ? s : "(null)");
            break;
        }
        default:
            axl_printf("%-8s", "?");
            break;
        }
        axl_printf("  (%s, default=%s)\n",
                   d->description != NULL ? d->description : "",
                   d->default_value != NULL ? d->default_value : "");
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Verb tree
// ----------------------------------------------------------------------------

/* Service flags, hand-written. axl_service_main synthesizes the
   equivalent at runtime from opts_descs; this example shows the
   shape consumers can also write directly when they want full
   control over the AxlArgs tree. */
static const AxlArgDesc service_flags[] = {
    { .name = "port",    .short_name = 'p', .type = AXL_ARG_U64,
      .default_value = "8080", .help = "Listen port" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose mode" },
    { .name = "name",                       .type = AXL_ARG_STRING,
      .default_value = "demo", .help = "Service name" },
    { 0 }
};

static const AxlArgDesc start_flags[] = {
    { .name = "port",    .short_name = 'p', .type = AXL_ARG_U64,
      .default_value = "8080", .help = "Listen port" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose mode" },
    { .name = "name",                       .type = AXL_ARG_STRING,
      .default_value = "demo", .help = "Service name" },
    { .name = "detach",  .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Start the driver and exit (skip supervise loop)" },
    { 0 }
};

static const AxlArgsNode verbs[] = {
    { .name = "start",  .help = "Start the service driver",
      .flags = start_flags,   .handler = verb_start },
    { .name = "stop",   .help = "Stop a running driver",
      .handler = verb_stop },
    { .name = "status", .help = "Show service state",
      .handler = verb_status },
    { .name = "config", .help = "Parse + print options (no driver involvement)",
      .flags = service_flags, .handler = verb_config },
    { 0 }
};

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "service-demo-custom",
        .help        = "AxlService demo — consumer-side AxlArgs + AxlConfig",
        .help_prolog =
            "Same shape as service-demo.efi (built via AXL_SERVICE) "
            "but with a hand-written main() that mixes the standard "
            "start/stop/status verbs with a custom `config` verb. "
            "Use this pattern when you need consumer control of the "
            "verb tree.",
        .verbs       = verbs,
    });
}

#endif /* !AXL_SERVICE_BUILD_DRIVER */
