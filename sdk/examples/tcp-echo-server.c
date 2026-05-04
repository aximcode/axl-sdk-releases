/**
 * tcp-echo-server.c — Async TCP echo server built on the AxlTcp
 * primitives (thin wrapper over UEFI TCP4_PROTOCOL).
 *
 * Listens on port 7000, accepts connections via async events, echoes
 * data back. No polling timers — purely event-driven. Demonstrates
 * axl_tcp_listen, axl_tcp_accept_async, axl_tcp_recv_async,
 * axl_tcp_send_async, and the event loop.
 *
 * All I/O rides on a single event loop: no nested loops. Each recv
 * kicks off an async send; when the send completes its callback
 * re-arms the recv. Using `axl_tcp_send` (blocking) here would spin
 * up a temporary per-call loop inside the recv callback (see the
 * "Sync vs async" note on axl_tcp_send in the header).
 *
 * Re-arming is controlled by the callback's bool return: `true`
 * stays armed (loop re-issues recv on the same buffer, or accepts
 * the next client), `false` tears down. Returning false permits
 * closing the socket inside the callback — safe because the loop
 * doesn't access the socket after cb returns false.
 *
 * For the idiomatic version using the higher-level AxlSocket API
 * (unified stream+datagram, AxlSocketAddress, GSocket-shaped), see
 * echo-server.c. Prefer that layer for new code; this file exists
 * to show the primitives underneath.
 *
 * Build with: axl-cc tcp-echo-server.c -o tcp-echo-server.efi
 */

#include <axl.h>

typedef struct {
    char buf[256];
} EchoConn;

static AxlLoop *loop = NULL;

static bool on_data(AxlTcp *sock, AxlStatus status, void *data);

static bool
on_echo_sent(AxlTcp *sock, AxlStatus status, void *data)
{
    EchoConn *conn = (EchoConn *)data;

    if (status != AXL_OK) {
        /* Send failed — peer likely gone. Close and stop. */
        axl_tcp_close(sock);
        axl_free(conn);
        return false;
    }

    /* Echo finished transmitting; re-arm recv into the same buffer. */
    axl_tcp_recv_async(sock, conn->buf, sizeof(conn->buf) - 1,
                       loop, NULL, on_data, conn);
    return false;  /* send is one-shot; return value ignored anyway */
}

static bool
on_data(AxlTcp *sock, AxlStatus status, void *data)
{
    EchoConn *conn = (EchoConn *)data;
    size_t len = axl_tcp_recv_get_size(sock);

    if (status != AXL_OK || len == 0) {
        axl_printf("  disconnected\n");
        axl_tcp_close(sock);
        axl_free(conn);
        return false;  /* tear down; safe to close above */
    }

    conn->buf[len] = '\0';
    axl_printf("  recv: %s", conn->buf);

    /* Fire async echo on the same loop; on_echo_sent re-arms recv. */
    axl_tcp_send_async(sock, conn->buf, len, loop, NULL,
                       on_echo_sent, conn);

    /* Don't auto-rearm recv here — the send owns the buffer until
       on_echo_sent fires. on_echo_sent re-arms recv with the now-
       free buffer. */
    return false;
}

static bool
on_accept(AxlTcp *client, AxlStatus status, void *data)
{
    (void)data;

    if (status != AXL_OK || client == NULL) {
        return true;  /* per-accept error — keep listening */
    }

    char addr[46];
    uint16_t port;
    axl_tcp_get_remote_addr(client, addr, sizeof(addr), &port);
    axl_printf("  connected: %s:%u\n", addr, port);

    EchoConn *conn = axl_calloc(1, sizeof(EchoConn));
    if (conn == NULL) {
        axl_tcp_close(client);
        return true;  /* keep listening — next client may find a slot */
    }

    axl_tcp_recv_async(client, conn->buf, sizeof(conn->buf) - 1,
                       loop, NULL, on_data, conn);
    return true;  /* keep accepting more clients */
}

int
main(int argc, char **argv)
{
    AxlTcp *listener;

    (void)argc;
    (void)argv;

    /* Bring up networking before any axl_tcp_* call. Loads NIC
       drivers, runs ConnectController, and waits for DHCP — the
       same setup the UEFI shell would do for `connect -r &&
       ifconfig -s eth0 dhcp`. Idempotent. */
    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("error: network bring-up failed\n");
        return 1;
    }

    if (axl_tcp_listen(7000, &listener) != AXL_OK) {
        axl_printf("error: cannot listen on port 7000\n");
        return 1;
    }

    axl_printf("tcp-echo-server: listening on port 7000 (async)\n");

    loop = axl_loop_new();
    if (loop == NULL) {
        axl_printf("error: axl_loop_new failed\n");
        axl_tcp_close(listener);
        return 1;
    }
    if (axl_tcp_accept_async(listener, loop, NULL, on_accept, NULL) != AXL_OK) {
        axl_printf("error: axl_tcp_accept_async failed\n");
        axl_tcp_close(listener);
        axl_loop_free(loop);
        return 1;
    }
    axl_loop_run(loop);
    /* Close the listener BEFORE freeing the loop. axl_tcp_close drops
       the still-armed accept source against this loop, and the
       previous "free loop, then close" order left axl_tcp_close
       dereferencing freed loop memory. */
    axl_tcp_close(listener);
    axl_loop_free(loop);
    return 0;
}
