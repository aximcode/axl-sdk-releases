/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * svc-tdfail.c -- an AXL_SERVICE whose teardown fails a configurable number
 * of times before it succeeds (test-service-teardown-fail-qemu.sh).
 *
 * Compiled twice, like every AXL_SERVICE: the driver image, and a launcher
 * with that driver embedded. `svc_tdfail.efi start --detach --fails N` starts
 * the driver with N, which its teardown counts down: each of the first N
 * teardowns returns AXL_ERR having released nothing, and the next one frees
 * what setup allocated and returns AXL_OK.
 *
 * A teardown that fails keeps the image resident (its unload returns
 * EFI_ABORTED), and the service must stay whole: still published, so status
 * finds it and a later stop resumes the teardown. The teardown logs how many
 * times the loop has ticked, so the test can show the loop of a half-stopped
 * service does not run between two attempts.
 *
 * --attach-fail makes the driver's entry fail AFTER setup succeeded, through
 * the library's "attach" test point (the strong definition below), so the
 * framework rolls the setup back with a teardown -- which can itself fail.
 * --setup-held makes setup return AXL_SERVICE_SETUP_HELD, keeping what it
 * allocated for the next unload's teardown to free.
 *
 * --setup-rc N makes setup unwind and return N instead (a positive N that is
 * not AXL_SERVICE_SETUP_HELD must be an ordinary failure). --held-no-teardown
 * makes setup clear the descriptor's teardown and return
 * AXL_SERVICE_SETUP_HELD, which the framework must refuse.
 *
 * --degrade N exercises the degraded state: 1 sets it in setup, 2 sets it
 * from the first loop tick (TPL_CALLBACK), 3 sets it in setup and clears it
 * from the first tick. 4 and 5 publish the degraded tag BY HAND with a
 * reason struct of another size -- 4 larger than this SDK's, 5 smaller and
 * with no terminator -- as a driver built against another SDK version would.
 *
 * The launcher adds one verb to the standard ones: `reload` replaces the
 * running service with the embedded driver (axl_service_reload_buffer) and
 * prints what that returned.
 */

#include <axl.h>
#include <axl/axl-config.h>
#include <axl/axl-embed.h>
#include <axl/axl-loop.h>
#include <axl/axl-service.h>
#include <axl/axl-str.h>

#ifdef AXL_SERVICE_BUILD_DRIVER
#include "../../src/service/axl-service-internal.h"
#endif

#include <stddef.h>

AXL_LOG_DOMAIN("svc-tdfail");

typedef struct {
    uint32_t fails;         ///< teardowns to fail before one succeeds
    bool     attach_fail;   ///< fail the driver tick's attach after setup
    bool     setup_held;    ///< setup returns AXL_SERVICE_SETUP_HELD
    uint32_t degrade;       ///< see the file header
    uint32_t setup_rc;      ///< nonzero: setup unwinds and returns this
    bool     held_no_td;    ///< setup drops the teardown and returns HELD
} TdfailOpts;

static TdfailOpts g_opts;

static const AxlConfigDesc tdfail_descs[] = {
    { "fails", AXL_CFG_UINT, "1", "Teardowns to fail before one succeeds",
      offsetof(TdfailOpts, fails), sizeof(uint32_t) },
    { "attach-fail", AXL_CFG_BOOL, "false", "Fail the tick attach after setup",
      offsetof(TdfailOpts, attach_fail), sizeof(bool) },
    { "setup-held", AXL_CFG_BOOL, "false", "Setup fails but keeps what it took",
      offsetof(TdfailOpts, setup_held), sizeof(bool) },
    { "degrade", AXL_CFG_UINT, "0", "Report degraded (see the source)",
      offsetof(TdfailOpts, degrade), sizeof(uint32_t) },
    { "setup-rc", AXL_CFG_UINT, "0", "Setup unwinds and returns this",
      offsetof(TdfailOpts, setup_rc), sizeof(uint32_t) },
    { "held-no-teardown", AXL_CFG_BOOL, "false", "Setup holds, with no teardown",
      offsetof(TdfailOpts, held_no_td), sizeof(bool) },
    { 0 }
};

#ifdef AXL_SERVICE_BUILD_DRIVER

static void     *g_block;
static uint32_t  g_ticks;
static uint32_t  g_attempt;
static AxlGuid   g_deg_guid;
static void     *g_deg_iface;   /* a hand-published degraded tag, or NULL */
static AxlHandle g_handle;      /* the handle it is published on */

/* Degraded-tag layouts another SDK version could publish. */
static struct {
    uint32_t size;
    char     why[AXL_SERVICE_REASON_MAX + 1];
    char     more[16];      /* a field a later version added at the end */
} g_tag_grown;

/* `size` stops at the end of why[]: the reader must not look past it. The
   bytes right after it are NOT zero (alignment padding would be, and would
   hide a reader that ignores `size`), so an over-read shows as
   "shortOVERREAD". */
static struct {
    uint32_t size;
    char     why[5];        /* "short", no terminator */
    char     tail[8];       /* "OVERREAD", past `size` */
} g_tag_short;

static AxlService svc_tdfail;

/* Publish a degraded tag of another layout on this image's handle. */
static void
tdfail_hand_tag(void)
{
    if (g_opts.degrade == 4) {
        g_tag_grown.size = (uint32_t)sizeof(g_tag_grown);
        axl_strlcpy(g_tag_grown.why, "grown struct", sizeof(g_tag_grown.why));
        g_deg_iface = &g_tag_grown;
    } else {
        g_tag_short.size = (uint32_t)offsetof(__typeof__(g_tag_short), tail);
        axl_memcpy(g_tag_short.why, "short", sizeof(g_tag_short.why));
        axl_memcpy(g_tag_short.tail, "OVERREAD", sizeof(g_tag_short.tail));
        g_deg_iface = &g_tag_short;
    }
    /* On this image's own handle, where the framework puts its tags: the one
       handle publishing the service right now. */
    AxlGuid  svc_guid;
    void   **handles = NULL;
    size_t   n       = 0;
    int rc = axl_service_guid(&svc_tdfail, &svc_guid);
    if (rc == AXL_OK) {
        rc = axl_protocol_enumerate_guid(&svc_guid, &handles, &n);
    }
    if (rc == AXL_OK && n == 1) {
        g_handle = (AxlHandle)handles[0];
        rc = _axl_service_tag_guid(&svc_tdfail, "degraded", &g_deg_guid);
    } else if (rc == AXL_OK) {
        rc = AXL_ERR;
    }
    axl_free(handles);
    if (rc == AXL_OK) {
        AxlHandle h = g_handle;
        rc = axl_protocol_install(&g_deg_guid, g_deg_iface, &h);
    }
    axl_info("hand tag %u rc=%d", (unsigned)g_opts.degrade, rc);
    if (rc != AXL_OK) {
        g_deg_iface = NULL;
    }
}

/* Strong over the library's weak "never". */
bool
_axl_service_test_point(const char *point)
{
    return g_opts.attach_fail && axl_strcmp(point, "attach") == 0;
}

static bool
tdfail_tick(void *data)
{
    (void)data;
    g_ticks++;
    if (g_ticks == 1 && g_opts.degrade == 2) {
        axl_info("degrade from a tick rc=%d",
                 axl_service_set_degraded(true, "set in a tick"));
    } else if (g_ticks == 1 && g_opts.degrade == 3) {
        axl_info("clear from a tick rc=%d", axl_service_set_degraded(false, NULL));
    }
    return AXL_SOURCE_CONTINUE;
}

static int
tdfail_setup(AxlLoop *loop, void *user)
{
    (void)user;
    g_block = axl_malloc(64);
    if (g_block == NULL) {
        return AXL_ERR;
    }
    if (axl_loop_add_timer(loop, 20, tdfail_tick, NULL) == 0) {
        axl_free(g_block);
        g_block = NULL;
        return AXL_ERR;
    }
    axl_info("setup fails=%u attach-fail=%d", (unsigned)g_opts.fails,
             g_opts.attach_fail ? 1 : 0);
    if (g_opts.setup_rc != 0) {
        axl_free(g_block);
        g_block = NULL;
        axl_info("setup returning %u", (unsigned)g_opts.setup_rc);
        return (int)g_opts.setup_rc;
    }
    if (g_opts.degrade == 1 || g_opts.degrade == 3) {
        axl_info("degrade from setup rc=%d",
                 axl_service_set_degraded(true, "set in setup"));
    }
    if (g_opts.degrade == 4 || g_opts.degrade == 5) {
        tdfail_hand_tag();
    }
    if (g_opts.held_no_td) {
        svc_tdfail.teardown = NULL;
        axl_info("setup holding, with no teardown");
        return AXL_SERVICE_SETUP_HELD;
    }
    if (g_opts.setup_held) {
        axl_info("setup failing, holding its block");
        return AXL_SERVICE_SETUP_HELD;
    }
    return AXL_OK;
}

static int
tdfail_teardown(void *user)
{
    (void)user;
    g_attempt++;
    if (g_opts.fails > 0) {
        g_opts.fails--;
        axl_info("teardown %u failing on purpose ticks=%u", (unsigned)g_attempt,
                 (unsigned)g_ticks);
        return AXL_ERR;
    }
    if (g_deg_iface != NULL) {
        axl_protocol_uninstall(g_handle, &g_deg_guid, g_deg_iface);
        g_deg_iface = NULL;
    }
    axl_free(g_block);
    g_block = NULL;
    axl_info("teardown %u ok ticks=%u", (unsigned)g_attempt, (unsigned)g_ticks);
    return AXL_OK;
}

#endif /* AXL_SERVICE_BUILD_DRIVER */

/* Not const: --held-no-teardown clears the teardown from setup. */
static AxlService svc_tdfail = {
    .name           = "svc-tdfail",
#ifdef AXL_SERVICE_BUILD_DRIVER
    .setup          = tdfail_setup,
    .teardown       = tdfail_teardown,
#endif
    .opts_descs     = tdfail_descs,
    .user           = &g_opts,
    .driver_tick_ms = 20,
};

#ifdef AXL_SERVICE_BUILD_DRIVER

AXL_SERVICE_DRIVER(svc_tdfail);

#else

AXL_EMBED_DECLARE(svc_tdfail);

int
main(int argc, char **argv)
{
    if (argc == 2 && axl_strcmp(argv[1], "reload") == 0) {
        AxlStatus rc = axl_service_reload_buffer(&svc_tdfail,
                                                 AXL_EMBED_DATA(svc_tdfail),
                                                 AXL_EMBED_SIZE(svc_tdfail));
        if (rc == AXL_DENIED) {
            axl_printf("svc-tdfail: reload denied (state %d)\n",
                       (int)axl_service_state(&svc_tdfail));
        } else {
            axl_printf("svc-tdfail: reload rc=%d\n", (int)rc);
        }
        return rc == AXL_OK ? 0 : 1;
    }
    AxlServiceDeploy deploy = {
        .service         = &svc_tdfail,
        .driver_blob     = AXL_EMBED_DATA(svc_tdfail),
        .driver_blob_len = AXL_EMBED_SIZE(svc_tdfail),
        .driver_name     = "svc_tdfail-dxe.efi",
    };
    return axl_service_main(&deploy, argc, argv);
}

#endif
