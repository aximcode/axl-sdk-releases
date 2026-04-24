/**
 * @file cancellable-demo.c
 *
 * Demonstrates AxlCancellable in the same-loop multi-source pattern
 * from the axl-cancellable.h header example.
 *
 * A 2s timer simulates a background task finishing -- it quits the
 * loop. Without arguments the task runs to completion. With `--cancel`
 * a second 500ms timer fires first, cancels an AxlCancellable, and
 * quits the loop before the task finishes.
 *
 * After the loop exits, axl_cancellable_is_cancelled() tells us which
 * path won. In real code the cancellable would also be passed to one
 * or more axl_tcp_*_async / axl_http_*_async calls so their callbacks
 * fire with AXL_CANCELLED when the token is signalled.
 *
 * Build with: axl-cc cancellable-demo.c -o cancellable-demo.efi
 * Run:        cancellable-demo              -> "task completed"
 *             cancellable-demo --cancel     -> "cancelled at 500ms"
 */

#include <axl.h>

typedef struct {
    AxlLoop        *loop;
    AxlCancellable *cancel;
} DemoCtx;

/** 2s timer: simulates a background op finishing. */
static bool
on_task_done(void *data)
{
    DemoCtx *ctx = data;
    axl_printf("  [task]   2s elapsed -- task finished\n");
    axl_loop_quit(ctx->loop);
    return AXL_SOURCE_REMOVE;
}

/** 500ms timer (optional): cancels before the task completes. */
static bool
on_cancel(void *data)
{
    DemoCtx *ctx = data;
    axl_printf("  [cancel] 500ms elapsed -- cancelling\n");
    axl_cancellable_cancel(ctx->cancel);
    axl_loop_quit(ctx->loop);
    return AXL_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
    DemoCtx ctx = {0};
    bool arm_cancel = false;

    for (int i = 1; i < argc; i++) {
        if (axl_strcmp(argv[i], "--cancel") == 0) {
            arm_cancel = true;
        }
    }

    axl_printf("cancellable-demo: %s\n\n",
               arm_cancel ? "with --cancel (expect early cancellation)"
                          : "without --cancel (expect task to complete)");

    ctx.loop   = axl_loop_new();
    ctx.cancel = axl_cancellable_new();
    if (ctx.loop == NULL || ctx.cancel == NULL) {
        axl_printf("error: allocation failed\n");
        return 1;
    }

    axl_loop_add_timeout(ctx.loop, 2000, on_task_done, &ctx);
    if (arm_cancel) {
        axl_loop_add_timeout(ctx.loop, 500, on_cancel, &ctx);
    }

    axl_printf("  starting loop\n");
    axl_loop_run(ctx.loop);

    if (axl_cancellable_is_cancelled(ctx.cancel)) {
        axl_printf("\nresult: cancelled at 500ms\n");
    } else {
        axl_printf("\nresult: task completed\n");
    }

    /* Ownership rule: free the cancellable AFTER the loop has stopped
     * and any op observing it has unwound. Here the timers have
     * already returned AXL_SOURCE_REMOVE. */
    axl_cancellable_free(ctx.cancel);
    axl_loop_free(ctx.loop);
    return 0;
}
