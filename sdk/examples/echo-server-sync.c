/**
 * echo-server-sync.c — Single-client synchronous TCP echo server.
 *
 * Accepts one client at a time, echoes every chunk back, and
 * re-accepts when the client disconnects. All I/O is top-level
 * linear code with the blocking socket API — no event loop, no
 * callbacks, no shared state between callbacks.
 *
 * This shape is right for:
 *   - Single-client diagnostic listeners (`nc -l`-style tools)
 *   - Test harnesses that expect one peer at a time
 *   - Teaching material that shows the blocking API on its own
 *
 * It is NOT right for concurrent multi-client servers: while one
 * connection is being serviced no other accept can happen. For
 * the multi-client version of the same task see echo-server.c
 * (one loop, async accept/recv/send, bool-return re-arm).
 *
 * Every blocking call here (accept, receive, send) spins its own
 * temporary AxlLoop per call — cheap in this program because
 * there is no outer event loop for the per-call loops to conflict
 * with. Inside an outer loop callback the same pattern would
 * freeze the outer loop for the duration of each call; see
 * docs/AXL-Runtime.md §5.4 for the rule.
 *
 * Ctrl-C: each blocking call observes the shell-break event via
 * its internal loop dispatch; the runtime's default policy ends
 * the program cleanly at the next yield point.
 *
 * Usage:  echo-server-sync [port]    (default port 7001)
 * Build:  axl-cc echo-server-sync.c -o echo-server-sync.efi
 */

#include <axl.h>

static int
parse_port(
    const char *s,     ///< decimal string
    uint16_t   *out    ///< [out] parsed port
)
{
    unsigned int v = 0;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10 + (unsigned int)(*p - '0');
        if (v > 65535) {
            return -1;
        }
    }
    if (v == 0) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

int
main(int argc, char **argv)
{
    uint16_t port = 7001;

    if (argc >= 2 && parse_port(argv[1], &port) != 0) {
        axl_printf("error: invalid port '%s'\n", argv[1]);
        return 1;
    }

    AXL_AUTOPTR(AxlSocket) listener = axl_socket_new(AXL_SOCKET_STREAM);
    if (listener == NULL || axl_socket_listen(listener, port) != 0) {
        axl_printf("error: cannot listen on port %u\n", (unsigned)port);
        return 1;
    }
    axl_printf("echo-server-sync: listening on port %u (single-client)\n",
               (unsigned)port);

    while (!axl_interrupted()) {
        AxlSocket *client = NULL;

        /* timeout_ms = 0 → wait forever for a client to connect.
           Ctrl-C still ends the wait via the loop's break observation. */
        if (axl_socket_accept(listener, &client, 0) != 0 || client == NULL) {
            if (axl_interrupted()) {
                break;
            }
            axl_printf("  accept failed\n");
            continue;
        }

        axl_printf("  connected\n");

        for (;;) {
            char   buf[256];
            size_t size = sizeof(buf) - 1;

            if (axl_socket_receive(client, buf, &size, 0) != 0
                || size == 0) {
                break;
            }
            buf[size] = '\0';
            axl_printf("  recv: %.*s\n", (int)size, buf);

            if (axl_socket_send(client, buf, size, 0) != 0) {
                break;
            }
        }

        axl_printf("  disconnected\n");
        axl_socket_free(client);
    }

    axl_printf("echo-server-sync: shutting down\n");
    return 0;
}
