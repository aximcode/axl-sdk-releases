/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * net-latency-selftest.c -- a TCP echo server under one axl_net_set_latency
 * policy, for test-net-latency-qemu.sh to time round trips against.
 *
 * Usage: net-latency-selftest.efi <firmware|window|always> <async|driver|sync|sink> <port> [window_ms]
 *        net-latency-selftest.efi exit-arm     (see exit_arm)
 *        net-latency-selftest.efi period       PERIOD=<us>
 *
 *   async  the shape a server really has: an event loop that sleeps between
 *          events (the CPU halts until the firmware's tick). Accept, receive
 *          and send are loop callbacks.
 *   driver the async server with its loop under axl_loop_attach_driver (a
 *          10 ms driver tick) while the foreground only waits: a resident
 *          service's shape, sshd's
 *   sink   the async server receiving and discarding until the peer closes,
 *          then `SINK bytes=<n>`: an upload, for throughput
 *   sync   axl_tcp_recv/axl_tcp_send in a plain loop: each call runs a
 *          nested loop that polls TCP while it waits rather than sleeping.
 *
 * Prints `LATENCY-READY mode=<m> shape=<s> period=<us>` once listening, echoes
 * every line of ONE connection back until `quit` or a close, then prints
 * `LATENCY-DONE period=<us>` -- the tick once the policy has let go.
 */

#include <axl.h>

#define ECHO_BUF 256

typedef struct {
    AxlLoop      *loop;
    AxlTcp       *client;
    volatile bool done;    /* driver shape: the foreground waits on it */
    bool          sink;    /* receive and discard, counting */
    uint64_t      bytes;
    char          buf[16384];
} Echo;

static void
echo_done(
    Echo *e
    )
{
    if (e->client != NULL) {
        axl_tcp_close(e->client, AXL_TEARDOWN_GRACEFUL);
        e->client = NULL;
    }
    e->done = true;
    axl_loop_quit(e->loop);
}

static bool
on_sent(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    (void)sock;
    if (status != AXL_OK) {
        echo_done(data);
    }
    return false;
}

static bool
on_recv(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    Echo  *e = data;
    size_t n = axl_tcp_recv_get_size(sock);
    if (status != AXL_OK || n == 0 || (n >= 4 && axl_memcmp(e->buf, "quit", 4) == 0)) {
        echo_done(e);
        return false;
    }
    if (axl_tcp_send_async(sock, e->buf, n, e->loop, NULL, on_sent, e) != AXL_OK) {
        echo_done(e);
        return false;
    }
    return true;   /* re-arm the receive */
}

/* sink: receive and discard until the peer closes (an upload's shape). */
static bool
on_sink(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    Echo  *e = data;
    size_t n = axl_tcp_recv_get_size(sock);
    if (status != AXL_OK || n == 0) {
        echo_done(e);
        return false;
    }
    e->bytes += n;
    return true;
}

static bool
on_accept(
    AxlTcp   *sock,
    AxlStatus status,
    void     *data
    )
{
    Echo *e = data;
    if (status != AXL_OK || sock == NULL) {
        e->done = true;
        axl_loop_quit(e->loop);
        return false;
    }
    e->client = sock;
    if (axl_tcp_recv_async(sock, e->buf, sizeof(e->buf), e->loop, NULL,
                           e->sink ? on_sink : on_recv, e) != AXL_OK) {
        echo_done(e);
    }
    return false;   /* one connection */
}

/* @p driver: the loop is attached at a 10 ms driver tick and the foreground
   only waits, so every callback is dispatched by that tick -- the shape of
   a resident service such as sshd. */
static int
serve_async(
    AxlTcp *listener,
    bool    driver,
    bool    sink
    )
{
    static Echo e;   /* 16 KB buffer: not on the stack */
    axl_memset(&e, 0, sizeof(e));
    e.sink = sink;
    e.loop = axl_loop_new();
    if (axl_tcp_accept_async(listener, e.loop, NULL, on_accept, &e) != AXL_OK) {
        axl_tcp_close(listener, AXL_TEARDOWN_RESET);
        axl_loop_free(e.loop);
        return 1;
    }
    if (driver) {
        if (axl_loop_attach_driver(e.loop, 10) != AXL_OK) {
            axl_tcp_close(listener, AXL_TEARDOWN_RESET);
            axl_loop_free(e.loop);
            return 1;
        }
        while (!e.done) {
            axl_msleep(1);
        }
        axl_loop_detach_driver(e.loop);
    } else {
        axl_loop_run(e.loop);
    }
    /* The listener's sources live on this loop: it goes first. */
    axl_tcp_close(listener, AXL_TEARDOWN_RESET);
    axl_loop_free(e.loop);
    if (sink) {
        axl_printf("SINK bytes=%llu\n", (unsigned long long)e.bytes);
    }
    return 0;
}

static int
serve_sync(
    AxlTcp *listener
    )
{
    AxlTcp *client = NULL;
    if (axl_tcp_accept(listener, &client, 120000) != AXL_OK) {
        axl_tcp_close(listener, AXL_TEARDOWN_RESET);
        return 1;
    }
    char buf[ECHO_BUF];
    for (;;) {
        size_t n = sizeof(buf);
        if (axl_tcp_recv(client, buf, &n, 30000) != AXL_OK || n == 0
            || (n >= 4 && axl_memcmp(buf, "quit", 4) == 0)
            || axl_tcp_send(client, buf, n, 30000) != AXL_OK) {
            break;
        }
    }
    axl_tcp_close(client, AXL_TEARDOWN_GRACEFUL);
    axl_tcp_close(listener, AXL_TEARDOWN_RESET);
    return 0;
}

/* exit-arm: an exit handler registered BEFORE the first socket runs AFTER
   the latency policy's own (LIFO), and opens and resets a listener --
   activity once the policy has let go for good. */
static void
exit_arm(
    void *data
    )
{
    (void)data;
    AxlTcp *late = NULL;
    if (axl_tcp_listen(7778, &late) == AXL_OK) {
        axl_tcp_close(late, AXL_TEARDOWN_RESET);
    }
}

int
main(
    int   argc,
    char *argv[]
    )
{
    if (argc >= 2 && axl_strcmp(argv[1], "period") == 0) {
        axl_printf("PERIOD=%u\n", axl_timer_period_us());
        return 0;
    }
    if (argc >= 2 && axl_strcmp(argv[1], "exit-arm") == 0) {
        axl_atexit(exit_arm, NULL);
        AxlNetLatency always = { .mode = AXL_NET_LATENCY_ALWAYS };
        AxlTcp *listener = NULL;
        if (axl_net_set_latency(&always) != AXL_OK
            || axl_net_auto_init(AXL_NET_NIC_AUTO, 30) != AXL_OK
            || axl_tcp_listen(7777, &listener) != AXL_OK) {
            axl_printf("EXIT-ARM failed to listen\n");
            return 1;
        }
        axl_printf("EXIT-ARM period=%u\n", axl_timer_period_us());
        return 0;   /* the listener is left for the exit path */
    }
    if (argc < 4) {
        axl_printf("usage: net-latency-selftest.efi <firmware|window|always> <async|sync> <port>\n");
        return 2;
    }
    AxlNetLatency lat = { 0 };
    if (axl_strcmp(argv[1], "window") == 0) {
        lat.mode = AXL_NET_LATENCY_WINDOW;
    } else if (axl_strcmp(argv[1], "always") == 0) {
        lat.mode = AXL_NET_LATENCY_ALWAYS;
    } else if (axl_strcmp(argv[1], "firmware") != 0) {
        axl_printf("net-latency-selftest: unknown mode %s\n", argv[1]);
        return 2;
    }
    if (argc >= 5) {
        lat.window_ms = (uint32_t)axl_strtou64(argv[4]);
    }
    bool async  = axl_strcmp(argv[2], "async") == 0;
    bool driver = axl_strcmp(argv[2], "driver") == 0;
    bool sink   = axl_strcmp(argv[2], "sink") == 0;
    if (axl_net_set_latency(&lat) != AXL_OK) {
        axl_printf("net-latency-selftest: set_latency failed\n");
        return 1;
    }
    if (axl_net_auto_init(AXL_NET_NIC_AUTO, 30) != AXL_OK) {
        axl_printf("net-latency-selftest: no network\n");
        return 1;
    }
    AxlTcp *listener = NULL;
    if (axl_tcp_listen((uint16_t)axl_strtou64(argv[3]), &listener) != AXL_OK) {
        axl_printf("net-latency-selftest: listen failed\n");
        return 1;
    }
    axl_printf("LATENCY-READY mode=%s shape=%s period=%u\n", argv[1], argv[2],
               axl_timer_period_us());
    int rc = (async || driver || sink) ? serve_async(listener, driver, sink)
                                       : serve_sync(listener);   /* each closes it */
    AxlNetLatency now;
    axl_net_get_latency(&now);
    axl_msleep(now.window_ms + 150);   /* past the window in force: let WINDOW let go */
    axl_printf("LATENCY-DONE period=%u\n", axl_timer_period_us());
    return rc;
}
