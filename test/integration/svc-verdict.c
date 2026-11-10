/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * svc-verdict.c -- an AXL_SERVICE whose driver image is stopped so that its
 * OWN teardown leak verdict can be checked (test-service-leak-verdict-qemu.sh).
 *
 * Compiled twice, like every AXL_SERVICE (driver image + launcher with the
 * driver embedded), and in two variants:
 *
 *   svc_verdict       a clean service: setup allocates, teardown frees.
 *   svc_verdict_leak  -DSVC_VERDICT_LEAK: teardown forgets the free, so the
 *                     driver's unload must report exactly one leaked block,
 *                     attributed to this file.
 *
 * Setup also asks for axl_loop_default(), which a service's code may well do:
 * the driver image's default loop is the runtime's to free, as an app's is, so
 * it must not read as a leak when the driver is stopped.
 *
 * The launcher's own verdict is printed by its AXL_APP teardown either way;
 * this fixture is about the DRIVER image's.
 */

#include <axl.h>
#include <axl/axl-driver.h>
#include <axl/axl-loop.h>
#include <axl/axl-service.h>

#ifdef SVC_VERDICT_LEAK
AXL_LOG_DOMAIN("svc-vleak");   /* log domains are cut at 15 chars */
#else
AXL_LOG_DOMAIN("svc-verdict");
#endif

static void *g_block;

/* Runs once, well after the launcher's start returned: by then the launcher
   has taken its copy of our LoadOptions back, and must not have left our
   LoadedImage pointing at the freed bytes. */
static bool
verdict_options_after_entry(void *data)
{
    (void)data;
    const void *buf  = NULL;
    size_t      size = 0;
    axl_info("options after entry: %s",
             axl_driver_get_load_options_raw(&buf, &size) == AXL_OK ? "present" : "none");
    return AXL_SOURCE_REMOVE;
}

static int
verdict_setup(AxlLoop *loop, void *user)
{
    (void)user;
    const void *buf  = NULL;
    size_t      size = 0;
    axl_info("options at entry: %s",
             axl_driver_get_load_options_raw(&buf, &size) == AXL_OK ? "present" : "none");
    axl_info("default loop: %s", axl_loop_default() != NULL ? "in use" : "none");
    g_block = axl_malloc(48);
    if (g_block == NULL) {
        return AXL_ERR;
    }
    if (axl_loop_add_timeout(loop, 200, verdict_options_after_entry, NULL) == 0) {
        axl_free(g_block);
        g_block = NULL;
        return AXL_ERR;
    }
    axl_info("setup");
    return AXL_OK;
}

static int
verdict_teardown(void *user)
{
    (void)user;
#ifndef SVC_VERDICT_LEAK
    axl_free(g_block);
#endif
    g_block = NULL;
    axl_info("teardown");
    return AXL_OK;
}

/* Spelled out per variant: AXL_SERVICE stringifies its argument for the
   driver's file name, so it must be the identifier itself, not a macro. */
#ifdef SVC_VERDICT_LEAK
static const AxlService svc_verdict_leak = {
    .name     = "svc-verdict-leak",
    .setup    = verdict_setup,
    .teardown = verdict_teardown,
};

AXL_SERVICE(svc_verdict_leak);
#else
static const AxlService svc_verdict = {
    .name     = "svc-verdict",
    .setup    = verdict_setup,
    .teardown = verdict_teardown,
};

AXL_SERVICE(svc_verdict);
#endif
