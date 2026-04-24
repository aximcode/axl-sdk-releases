/**
 * echo-server.c — Async TCP echo server using the AxlSocket layer.
 *
 * Listens on port 7000, accepts connections via async events, echoes
 * data back. Uses the unified AxlSocket API (stream + datagram,
 * AxlSocketAddress, GLib-GSocket-shaped) — this is the idiomatic
 * layer for new code.
 *
 * All I/O rides on a single event loop: no nested loops. Each recv
 * kicks off an async send; when the send completes its callback
 * re-arms the recv. Running the echo on a fully-async pipeline is
 * the reason we don't just call `axl_socket_send` (blocking) inline
 * — the sync send helpers spin up their own per-call temporary loop
 * (see the "Sync vs async" note on axl_socket_send in the header),
 * which would nest inside the main loop's callback.
 *
 * Re-arming is controlled by the callback's bool return: `true`
 * stays armed (loop re-issues recv on the same buffer), `false`
 * tears down. Returning false permits closing the socket inside
 * the callback — safe because the loop doesn't access the socket
 * after cb returns false.
 *
 * Related examples:
 *   - tcp-echo-server.c — same task, same async shape, but against
 *     the lower-level AxlTcp primitives (thin wrapper over UEFI
 *     TCP4). Prefer the AxlSocket version (this file) for new code.
 *   - echo-server-sync.c — single-client sync variant. Shows the
 *     blocking API as top-level linear code. Right shape for CLI
 *     tools; wrong shape for multi-client servers.
 *   - echo-client.c — the sync client counterpart.
 *
 * Build with: axl-cc echo-server.c -o echo-server.efi
 */

#include <axl.h>

typedef struct {
    AxlSocket *sock;
    char       buf[256];
} EchoConn;

static AxlLoop *loop = NULL;

static bool on_data(AxlSocket *sock, int status, void *data);

static bool
on_echo_sent(AxlSocket *sock, int status, void *data)
{
    EchoConn *conn = (EchoConn *)data;

    (void)sock;

    if (status != 0) {
        /* Send failed — peer likely gone. Close and stop. */
        axl_socket_free(conn->sock);
        axl_free(conn);
        return false;
    }

    /* Echo finished transmitting; re-arm recv into the same buffer. */
    axl_socket_receive_async(conn->sock, conn->buf, sizeof(conn->buf) - 1,
                             loop, on_data, conn);
    return false;  /* send is one-shot; return value ignored anyway */
}

static bool
on_data(AxlSocket *sock, int status, void *data)
{
    EchoConn *conn = (EchoConn *)data;
    size_t len = axl_socket_receive_get_size(sock);

    (void)sock;

    if (status != 0 || len == 0) {
        axl_printf("  disconnected\n");
        axl_socket_free(conn->sock);
        axl_free(conn);
        return false;
    }

    conn->buf[len] = '\0';
    axl_printf("  recv: %s", conn->buf);

    /* Fire async echo on the same loop; on_echo_sent re-arms recv. */
    axl_socket_send_async(conn->sock, conn->buf, len,
                          loop, on_echo_sent, conn);

    /* Don't auto-rearm recv here — the send owns the buffer until
       on_echo_sent fires. on_echo_sent re-arms recv with the now-
       free buffer. */
    return false;
}

static bool
on_accept(AxlSocket *client, int status, void *data)
{
    (void)data;

    if (status != 0 || client == NULL) {
        return true;  /* per-accept error — keep listening */
    }

    AxlSocketAddress *remote = axl_socket_get_remote_address(client);
    if (remote != NULL) {
        const char *addr = axl_inet_address_to_string(
            axl_socket_address_get_address(remote));
        uint16_t port = axl_socket_address_get_port(remote);
        axl_printf("  connected: %s:%u\n", addr, (unsigned)port);
        axl_socket_address_free(remote);
    }

    EchoConn *conn = axl_calloc(1, sizeof(EchoConn));
    if (conn == NULL) {
        axl_socket_free(client);
        return true;  /* keep listening */
    }

    conn->sock = client;
    axl_socket_receive_async(client, conn->buf, sizeof(conn->buf) - 1,
                             loop, on_data, conn);
    return true;  /* keep accepting more clients */
}

int
main(int argc, char **argv)
{
    AxlSocket *listener;

    (void)argc;
    (void)argv;

    listener = axl_socket_new(AXL_SOCKET_STREAM);
    if (listener == NULL || axl_socket_listen(listener, 7000) != 0) {
        axl_printf("error: cannot listen on port 7000\n");
        axl_socket_free(listener);
        return 1;
    }

    axl_printf("echo-server: listening on port 7000 (async)\n");

    loop = axl_loop_new();
    axl_socket_accept_async(listener, loop, on_accept, NULL);
    axl_loop_run(loop);
    axl_loop_free(loop);
    axl_socket_free(listener);
    return 0;
}
