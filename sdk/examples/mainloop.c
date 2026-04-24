/**
 * mainloop.c — Event loop with timer and keyboard input.
 *
 * Prints a counter every second. Press 'q' to quit, or Ctrl-C.
 * Demonstrates axl_loop_new, axl_loop_add_timer,
 * axl_loop_add_key_press, axl_loop_run.
 *
 * Build with: axl-cc mainloop.c -o mainloop.efi
 */

#include <axl.h>

static int counter = 0;

static bool
on_tick(void *data)
{
    (void)data;
    counter++;
    axl_printf("\r  tick %d", counter);
    return AXL_SOURCE_CONTINUE;
}

static bool
on_key(AxlInputKey key, void *data)
{
    AxlLoop *loop = (AxlLoop *)data;

    if (key.unicode_char == 'q' || key.unicode_char == 'Q') {
        axl_printf("\n  'q' pressed — quitting\n");
        axl_loop_quit(loop);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

int
main(int argc, char **argv)
{
    AxlLoop *loop;

    (void)argc;
    (void)argv;

    axl_printf("mainloop: press 'q' to quit\n");

    loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("error: cannot create event loop\n");
        return 1;
    }

    axl_loop_add_timer(loop, 1000, on_tick, NULL);
    axl_loop_add_key_press(loop, on_key, loop);
    axl_loop_run(loop);
    axl_loop_free(loop);

    axl_printf("total ticks: %d\n", counter);
    return 0;
}
