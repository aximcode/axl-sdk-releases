/**
 * @file pubsub-demo.c
 *
 * Publish/subscribe topics with deferred delivery.
 *
 * Two subscribers listen for "data-ready". The first publish reaches
 * both; subscriber A is then unsubscribed, so the second publish
 * reaches only B. Also demonstrates axl_defer() and axl_defer_cancel().
 *
 * Build with: axl-cc pubsub-demo.c -o pubsub-demo.efi
 */

#include <axl.h>

static uint32_t sub_a_handle;
static uint32_t sub_b_handle;

/* -- Subscriber callbacks ------------------------------------------------ */

static void
on_data_ready_a(void *event_data, void *user_data)
{
    const char *msg = (const char *)event_data;
    (void)user_data;
    axl_printf("  subscriber A: got data: %s\n", msg);
}

static void
on_data_ready_b(void *event_data, void *user_data)
{
    const char *msg = (const char *)event_data;
    (void)user_data;
    axl_printf("  subscriber B: got data: %s\n", msg);
}

/* -- Deferred work callbacks --------------------------------------------- */

static void
deferred_work(void *data)
{
    (void)data;
    axl_printf("  deferred work done\n");
}

static void
deferred_cancelled(void *data)
{
    (void)data;
    /* This should never print -- the work is cancelled before it fires. */
    axl_printf("  ERROR: cancelled work should not run!\n");
}

/* -- Timer callbacks ----------------------------------------------------- */

/**
 * First timeout (500ms): publish "data-ready" to both subscribers.
 */
static bool
on_first_publish(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    axl_printf("\n  --- publishing 'data-ready' (both subscribers) ---\n");
    axl_pubsub_publish(loop, "data-ready", (void *)"first-payload");
    return AXL_SOURCE_REMOVE;
}

/**
 * Quit helper -- used as a delayed timeout so deferred callbacks
 * from the final publish have one tick to fire before exit.
 */
static bool
on_quit(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    axl_loop_quit(loop);
    return AXL_SOURCE_REMOVE;
}

/**
 * Second timeout (1500ms): unsubscribe A, publish again, then quit.
 */
static bool
on_second_publish(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;

    axl_printf("\n  unsubscribing A\n");
    axl_pubsub_unsubscribe(loop, sub_a_handle);

    axl_printf("  --- publishing 'data-ready' (only B remains) ---\n");
    axl_pubsub_publish(loop, "data-ready", (void *)"second-payload");

    /* Give deferred callbacks one tick to fire, then quit. */
    axl_loop_add_timeout(loop, 100, on_quit, loop);
    return AXL_SOURCE_REMOVE;
}

/**
 * Cleanup: reset the pub/sub system.
 */
static bool
on_cleanup(void *data)
{
    AxlLoop *loop = (AxlLoop *)data;
    axl_pubsub_reset(loop);
    axl_printf("\n  [cleanup] pubsub system reset\n");
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    AxlLoop *loop;
    uint32_t h;

    (void)argc;
    (void)argv;

    axl_printf("pubsub-demo: publish/subscribe with deferred delivery\n\n");

    loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("error: cannot create event loop\n");
        return 1;
    }

    /* --- Demonstrate axl_defer directly --- */
    axl_printf("  scheduling deferred work...\n");
    axl_defer(loop, deferred_work, NULL);

    h = axl_defer(loop, deferred_cancelled, NULL);
    if (axl_defer_cancel(loop, h)) {
        axl_printf("  cancelled deferred work before it fired\n");
    }

    /* --- Subscribe to "data-ready" topic --- */
    sub_a_handle = axl_pubsub_subscribe(loop, "data-ready", on_data_ready_a, NULL);
    sub_b_handle = axl_pubsub_subscribe(loop, "data-ready", on_data_ready_b, NULL);
    axl_printf("  subscribed A (handle %u) and B (handle %u)\n",
               sub_a_handle, sub_b_handle);

    /* Schedule two publishes at different times. */
    axl_loop_add_timeout(loop, 500, on_first_publish, loop);
    axl_loop_add_timeout(loop, 1500, on_second_publish, loop);
    axl_loop_add_cleanup(loop, on_cleanup, loop);

    axl_printf("  running loop...\n");
    axl_loop_run(loop);
    axl_loop_free(loop);

    return 0;
}
