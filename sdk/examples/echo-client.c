/**
 * echo-client.c — Sync TCP echo client.
 *
 * Connects to a TCP echo server, sends a message, prints the
 * echoed reply, exits. Demonstrates the blocking socket API as a
 * top-level linear program — no event loop, no callbacks. This
 * shape is the right default for CLI tools and one-shot network
 * utilities.
 *
 * Every blocking call here (connect, send, receive) spins its own
 * temporary AxlLoop per call (see the "Blocking operations" block
 * in axl-socket.h). That cost is invisible for a program like
 * this because there is no outer event loop for the per-call
 * loops to conflict with — every call is from main. Inside an
 * outer loop callback the nesting would freeze the outer loop
 * for the duration of each call; see echo-server.c (multi-client
 * async) and docs/AXL-Lifecycle.md §5.4 for the rule.
 *
 * Ctrl-C: each blocking call observes the shell-break event via
 * its internal loop dispatch and returns -1 with
 * axl_interrupted() true; the runtime's default policy takes
 * the program down cleanly at the next yield point.
 *
 * Usage:
 *   echo-client <host> <port> [message]
 *
 * Build with: axl-cc echo-client.c -o echo-client.efi
 */

#include <axl.h>

static const char DEFAULT_MESSAGE[] = "hello from UEFI";

static int
parse_port(
    const char *s,     ///< decimal string
    uint16_t   *out    ///< [out] parsed port
)
{
    if (axl_str_to_u16(s, 10, out, NULL) != 0 || *out == 0) {
        return -1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        axl_printf("usage: echo-client <host> <port> [message]\n");
        return 1;
    }

    uint16_t port;
    if (parse_port(argv[2], &port) != 0) {
        axl_printf("error: invalid port '%s'\n", argv[2]);
        return 1;
    }

    const char *host    = argv[1];
    const char *msg     = (argc >= 4) ? argv[3] : DEFAULT_MESSAGE;
    size_t      msg_len = axl_strlen(msg);

    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("error: network not available\n");
        return 1;
    }

    AXL_AUTOPTR(AxlSocketClient) client = axl_socket_client_new();
    AXL_AUTOPTR(AxlSocket)       sock   = NULL;

    axl_printf("connecting to %s:%u\n", host, (unsigned)port);
    if (axl_socket_client_connect_to_host(client, host, port, &sock) != 0) {
        axl_printf("error: connect failed\n");
        return 1;
    }
    axl_printf("connected\n");

    if (axl_socket_send(sock, msg, msg_len, 5000) != 0) {
        axl_printf("error: send failed\n");
        return 1;
    }
    axl_printf("sent: %.*s\n", (int)msg_len, msg);

    char   buf[512];
    size_t size = sizeof(buf) - 1;
    if (axl_socket_receive(sock, buf, &size, 5000) != 0) {
        axl_printf("error: receive failed\n");
        return 1;
    }
    buf[size] = '\0';
    axl_printf("recv: %.*s\n", (int)size, buf);

    return 0;
}
