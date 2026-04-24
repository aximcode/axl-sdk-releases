/**
 * axlk-echo-server.c — axl-kernel TCP echo server.
 *
 * The K3 validation demo. Listens on port 7000, forks a handler
 * process per connection, each handler runs a straight-line
 * read/write loop until the peer disconnects. This is the code
 * AXL-Kernel-Design.md §1 promises: sequential, stateful per-
 * connection code — no callbacks, no Init/Poll/Cleanup trio, no
 * per-client sidecar state.
 *
 * Compare to sdk/examples/echo-server.c (the existing callback-
 * shaped equivalent) to see the delta.
 *
 * Build with axl-cc once axl-kernel ships standalone. For now it
 * builds via the in-tree Makefile target `kernel-poc` alongside
 * the K1+K2 driver. See experiments/axl-kernel/README.md.
 */

#include <axl.h>
#include "axl-kernel.h"

#define ECHO_PORT   7000
#define ECHO_BUFSZ  256

static int
handle_client(int argc, char **argv)
{
    (void)argc;
    int fd = (int)(intptr_t)argv;
    char buf[ECHO_BUFSZ];

    axl_printf("  pid %d handling fd %d\n", (int)axlk_getpid(), fd);

    for (;;) {
        int n = axlk_read(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        if (axlk_write(fd, buf, (size_t)n) != 0) {
            break;
        }
    }

    axl_printf("  pid %d closing fd %d\n", (int)axlk_getpid(), fd);
    axlk_close(fd);
    return 0;
}

static int
echo_service(int argc, char **argv)
{
    (void)argc; (void)argv;

    int listener = axlk_listen(ECHO_PORT);
    if (listener < 0) {
        axl_printf("FAIL: axlk_listen\n");
        return 1;
    }

    axl_printf("axlk-echo-server: listening on port %u\n",
               (unsigned)ECHO_PORT);

    /* POC cutoff: service 3 connections then exit. A real server
     * would loop forever; bounded here so the integration test
     * exits cleanly. */
    for (int i = 0; i < 3; i++) {
        int client = axlk_accept(listener);
        if (client < 0) {
            axl_printf("FAIL: axlk_accept\n");
            axlk_close(listener);
            return 1;
        }

        axl_printf("axlk-echo-server: accepted client fd %d\n", client);

        AxlkPid handler = axlk_spawn(handle_client, 0,
                                     (char **)(intptr_t)client, 0);
        if (handler < 0) {
            axl_printf("FAIL: axlk_spawn\n");
            axlk_close(client);
            axlk_close(listener);
            return 1;
        }
    }

    /* Reap the three handlers we spawned. */
    for (int i = 0; i < 3; i++) {
        int status = 0;
        AxlkPid reaped = axlk_wait(AXLK_PID_ANY, &status);
        if (reaped < 0) {
            break;
        }
        axl_printf("axlk-echo-server: reaped handler pid=%d status=%d\n",
                   (int)reaped, status);
    }

    axlk_close(listener);
    axl_printf("PASS: axlk-echo-server served 3 clients\n");
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("axlk-echo-server: starting\n");

    /* Bring up networking via DHCP before the kernel starts.
     * axl_net_auto_init is idempotent and safe even if startup.nsh
     * already ran ifconfig. */
    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("FAIL: network not available\n");
        return 1;
    }

    if (axlk_init() != 0) {
        axl_printf("FAIL: axlk_init\n");
        return 1;
    }

    int rc = axlk_run(echo_service, 0, NULL);
    axl_printf("axlk-echo-server: kernel exited rc=%d\n", rc);
    return rc;
}
