/**
 * @file loop-demo.c
 *
 * Advanced event loop: timers, timeout, idle source, cleanup,
 * and source removal.
 *
 * A 500ms timer ticks a counter. An idle source runs 3 times then
 * self-removes. After 2 seconds a one-shot timeout removes the timer
 * and quits the loop. A cleanup callback fires on exit.
 *
 * Build with: axl-cc loop-demo.c -o loop-demo.efi
 */

#include <axl.h>

/** Shared state passed between callbacks. */
typedef struct {
    AxlLoop  *loop;
    uint32_t  timer_id;
} DemoCtx;

/**
 * Repeating 500ms timer -- prints a tick counter.
 */
static bool
on_tick(void *data)
{
    int *count = (int *)data;
    (*count)++;
    axl_printf("  [timer] tick %d\n", *count);
    return AXL_SOURCE_CONTINUE;
}

/**
 * Idle source -- fires every iteration before blocking wait.
 * Removes itself after 3 runs.
 */
static bool
on_idle(void *data)
{
    int *count = (int *)data;
    (*count)++;
    axl_printf("  [idle]  run %d\n", *count);

    if (*count >= 3) {
        axl_printf("  [idle]  self-removing after 3 runs\n");
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

/**
 * One-shot timeout at 2 seconds -- removes the timer and quits.
 */
static bool
on_timeout(void *data)
{
    DemoCtx *ctx = (DemoCtx *)data;

    axl_printf("  [timeout] 2s elapsed -- removing timer and quitting\n");
    axl_loop_remove_source(ctx->loop, ctx->timer_id);
    axl_loop_quit(ctx->loop);

    return AXL_SOURCE_REMOVE;
}

/**
 * Cleanup callback -- fires when the loop exits (FIFO order).
 */
static bool
on_cleanup(void *data)
{
    (void)data;
    axl_printf("  [cleanup] shutting down\n");
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    DemoCtx ctx;
    int tick_count = 0;
    int idle_count = 0;

    (void)argc;
    (void)argv;

    axl_printf("loop-demo: advanced event loop features\n\n");

    ctx.loop = axl_loop_new();
    if (ctx.loop == NULL) {
        axl_printf("error: cannot create event loop\n");
        return 1;
    }

    /* Repeating 500ms timer */
    ctx.timer_id = axl_loop_add_timer(ctx.loop, 500, on_tick, &tick_count);

    /* Idle source -- fires every iteration */
    axl_loop_add_idle(ctx.loop, on_idle, &idle_count);

    /* One-shot 2s timeout -- removes timer and quits */
    axl_loop_add_timeout(ctx.loop, 2000, on_timeout, &ctx);

    /* Cleanup callback -- fires on loop exit */
    axl_loop_add_cleanup(ctx.loop, on_cleanup, NULL);

    axl_printf("  starting loop (will auto-quit after 2s)\n\n");
    axl_loop_run(ctx.loop);
    axl_loop_free(ctx.loop);

    axl_printf("\n  total ticks: %d, idle runs: %d\n", tick_count, idle_count);
    return 0;
}

/* FUSE-style manual loop (alternative to axl_loop_run):
 *
 * while (axl_loop_is_running(loop)) {
 *     int rc = axl_loop_next_event(loop, true);
 *     if (rc == -1) break;  // Ctrl-C
 *     axl_loop_dispatch_event(loop);
 * }
 */
