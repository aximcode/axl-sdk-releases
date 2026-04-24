/**
 * @file event-demo.c
 *
 * Demonstrates AxlEvent's signal/wait/reset state machine and the
 * three return-code paths of axl_event_wait_timeout:
 *
 *     0              -- event was (or became) signalled
 *    -1              -- timeout elapsed first
 *    AXL_CANCELLED   -- Ctrl-C or a signalled cancellable interrupted
 *
 * In real code the signal comes from an async callback (HTTP
 * response handler, TCP completion token, MP worker finishing)
 * while the main thread is parked inside axl_event_wait_timeout.
 * This demo fakes that by signalling the event synchronously
 * between waits, which is enough to exercise every return path.
 *
 * Build with: axl-cc event-demo.c -o event-demo.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    AxlEvent *e = axl_event_new();
    if (e == NULL) {
        axl_printf("error: allocation failed\n");
        return 1;
    }

    axl_printf("event-demo: signal/wait/reset state machine\n\n");

    /* 1. Signal-before-wait -- wait returns immediately with 0. This
     * is the common case when the async op completes before the main
     * thread parks. */
    axl_event_signal(e);
    int rc = axl_event_wait_timeout(e, NULL, 1000 * 1000);
    axl_printf("  [1] signal -> wait(1s):    rc=%d   (expect 0, already signalled)\n", rc);

    /* 2. Reset, then wait with a short timeout and no signal. Wait
     * blocks the CPU idle for the full timeout and returns -1. */
    axl_event_reset(e);
    rc = axl_event_wait_timeout(e, NULL, 200 * 1000);
    axl_printf("  [2] reset -> wait(200ms):  rc=%d  (expect -1, timed out)\n", rc);

    /* 3. Signal-after-reset -- wait returns immediately again. */
    axl_event_signal(e);
    rc = axl_event_wait_timeout(e, NULL, 1000 * 1000);
    axl_printf("  [3] signal -> wait(1s):    rc=%d   (expect 0, signalled)\n", rc);

    /* 4. Cancellable interruption. Wait with a pre-signalled
     * cancellable returns AXL_CANCELLED immediately without
     * observing the event's state. */
    axl_event_reset(e);
    AxlCancellable *cancel = axl_cancellable_new();
    axl_cancellable_cancel(cancel);
    rc = axl_event_wait_timeout(e, cancel, 1000 * 1000);
    axl_printf("  [4] cancel -> wait(1s):    rc=%d  (expect %d = AXL_CANCELLED)\n",
               rc, AXL_CANCELLED);

    axl_cancellable_free(cancel);
    axl_event_free(e);

    axl_printf("\nIn real code the signal would come from an async callback\n"
               "(axl_tcp_*_async, axl_http_*_async, MP worker completion, ...)\n"
               "while the main thread is parked in axl_event_wait_timeout.\n");
    return 0;
}
