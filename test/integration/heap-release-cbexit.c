/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * heap-release-cbexit.c — a driver whose TIMER CALLBACK exits with an EFI
 * error while a child image it started is running, for
 * test-heap-release-qemu.sh.
 *
 * During the child, the child is the firmware's current image, so a
 * gBS->Exit from this image fails. axl_exit used to spin there, at callback
 * TPL, which starved the child: the machine deadlocked. Now the exit is
 * deferred -- the callback is abandoned, the child finishes ("SLEEP done"),
 * and the exit happens when axl_image_run returns, from the entry, where it
 * unloads the image and gives its heap back like any failing entry.
 *
 *   CBEXIT exiting from a callback      the callback fired during the child
 *   CBEXIT child returned without exit  the deferred exit did NOT happen
 *   CBEXIT image_run failed             the child never ran (a setup fault)
 *
 * The driver variant ALSO has a second callback, later and still during the
 * child, call axl_exit_status(EFI_SUCCESS): the FIRST request decides, so the
 * image must still exit with the error (a success would leave it resident).
 * And the teardown's atexit handler sleeps 1 ms: if the abandoned callback
 * had left AxlLoop's callback depth raised, that wait would warn it was
 * nesting a loop inside a callback.
 *
 * -DHEAP_REL_APP builds the same thing as an APPLICATION, which hung the same
 * way: its callback exits with 0, so the observer's app mode can count it.
 *
 * Its child is named relative to the Shell's current directory, as the
 * observer names this image -- fs0:\ on a booted disk, the test's directory
 * on a shared volume (a leased pool guest). Not beside this image: launched
 * by the observer, it has no file path axl_app_image_dir_path resolves
 * (measured: that call fails in it).
 */

#include <stdlib.h>

#include <axl.h>

static int   cbexit_body(void);
static bool  cbexit_timeout(void *data) AXL_CB_NOEXCEPT;
static bool  cbexit_late(void *data) AXL_CB_NOEXCEPT;
static void  cbexit_free_loop(void *data) AXL_CB_NOEXCEPT;

static char    *volatile g_held;
static AxlLoop *g_loop;

/* The second, later request: must not change the first one's status. */
static bool
cbexit_late(
    void *data
    ) AXL_CB_NOEXCEPT
{
    (void)data;
    axl_printf("CBEXIT second request\n");
    axl_exit_status(AXL_EFI_SUCCESS);
}

static bool
cbexit_timeout(
    void *data
    ) AXL_CB_NOEXCEPT
{
    (void)data;
    axl_printf("CBEXIT exiting from a callback\n");
#ifdef HEAP_REL_APP
    axl_exit(0);
#else
    axl_exit_status(AXL_EFI_ABORTED);
#endif
}

/* The consumer's own cleanup of what it attached: on the exit's teardown the
   loop's tick must stop before the image is gone. */
static void
cbexit_free_loop(
    void *data
    ) AXL_CB_NOEXCEPT
{
    (void)data;
    axl_msleep(1);      /* would warn if the callback depth were still raised */
    if (g_loop != NULL) {
        axl_loop_detach_driver(g_loop);
        axl_loop_free(g_loop);
        g_loop = NULL;
    }
}

static int
cbexit_body(
    void
    )
{
    int exit_code = -1;

    g_held = malloc(100);
    g_loop = axl_loop_new();
    /* 200 ms in, against a 1 s child: well after even a slow LoadImage
       (aa64 under TCG), well before the child ends. */
    if (g_loop == NULL || axl_atexit(cbexit_free_loop, NULL) == 0
        || axl_loop_add_timeout(g_loop, 200, cbexit_timeout, NULL) == 0
#ifndef HEAP_REL_APP
        || axl_loop_add_timeout(g_loop, 500, cbexit_late, NULL) == 0
#endif
        || axl_loop_attach_driver(g_loop, 10) != AXL_OK) {
        return 1;
    }
    if (axl_image_run("heap-rel-sleep.efi", NULL, &exit_code) != AXL_OK) {
        axl_printf("CBEXIT image_run failed\n");
        return 1;
    }
    axl_printf("CBEXIT child returned without exit\n");
    return 1;
}

#ifdef HEAP_REL_APP

int
main(
    void
    )
{
    return cbexit_body();
}

#else

static int
cbexit_entry(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    (void)image;
    (void)st;
    return cbexit_body();
}

static int
cbexit_unload(
    AxlHandle image
    )
{
    (void)image;
    return 0;
}

AXL_DRIVER(cbexit_entry, cbexit_unload)

#endif
