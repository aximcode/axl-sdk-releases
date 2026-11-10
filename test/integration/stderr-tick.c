/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * stderr-tick.c -- a resident service that writes to standard error on its
 * own clock, after the command that started it has ended: from 1 s after
 * it starts, one `SR-TICK <n>` line every 100 ms, 70 of them, the way a
 * server's log line arrives while the Shell runs some other command
 * (test-stderr-row-qemu.sh, `resident` case). The first line is written
 * well after `start --detach` returns, so it comes while the command the
 * test types next is running -- the first time the image writes at all.
 *
 * Each line is also written raw (`SR-RAWTICK <n>`), through the Shell
 * handle itself: none, once the starting command has ended.
 *
 * It also publishes a shared-driver interface of its own, as a second
 * resident shared driver does: a launcher's stdio bridge aimed at ANOTHER
 * driver's interface must still not lend this image its handles.
 */

#include <axl.h>
#include <axl/axl-loop.h>
#include <axl/axl-service.h>
#include <axl/axl-shared-driver.h>

AXL_LOG_DOMAIN("stderr-tick");

#define TICKS        70u
#define TICK_MS      100u
#define FIRST_MS     1000u

static unsigned  g_ticks;
static int       g_probe_iface;    /* the published interface: its address */
static AxlHandle g_probe_handle;

static bool
tick(void *data)
{
    (void)data;
    g_ticks++;
    axl_printerr("SR-TICK %u\n", g_ticks);
    /* And raw, which goes THROUGH the image's Shell StdErr handle rather
       than only comparing it: with none (the command that started us has
       ended) it writes nothing; with a handle the Shell has since freed it
       would write through freed memory (test-stderr-stale-qemu.sh). */
    axl_fprintf(axl_stderr_raw, "SR-RAWTICK %u\n", g_ticks);
    return (g_ticks < TICKS) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
}

static bool
start_ticking(void *data)
{
    if (axl_loop_add_timer(data, TICK_MS, tick, NULL) == 0) {
        axl_printerr("SR-TICK failed to start\n");
    }
    return AXL_SOURCE_REMOVE;
}

static int
tick_setup(AxlLoop *loop, void *user)
{
    (void)user;
    g_ticks = 0;
    g_probe_handle = NULL;
    if (axl_shared_driver_publish("stderr-tick-probe", &g_probe_iface,
                                  &g_probe_handle) != AXL_OK) {
        return AXL_ERR;
    }
    return (axl_loop_add_timeout(loop, FIRST_MS, start_ticking, loop) != 0) ? AXL_OK : AXL_ERR;
}

static const AxlService stderr_tick = {
    .name  = "stderr-tick",
    .setup = tick_setup,
};

AXL_SERVICE(stderr_tick);
