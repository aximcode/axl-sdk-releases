/**
 * socket-demo.c — AXL Socket layer demo.
 *
 * Connects to a host using AxlSocketClient and sends an HTTP GET
 * request, then prints the response.
 *
 * Build with: axl-cc socket-demo.c -o socket-demo.efi
 * Usage:      socket-demo <host> [port]
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    const char *host;
    uint16_t    port = 80;
    char        request[256];
    char        response[4096];
    size_t      resp_len;

    if (argc < 2) {
        axl_printf("usage: socket-demo <host> [port]\n");
        return 1;
    }

    host = argv[1];
    if (argc >= 3 && axl_str_to_u16(argv[2], 10, &port, NULL) != 0) {
        axl_printf("invalid port: %s\n", argv[2]);
        return 1;
    }

    /* Initialize networking (DHCP) */
    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("Network not available\n");
        return 1;
    }

    /* Connect using AxlSocketClient. Both the client and the
       returned socket clean themselves up at scope exit. */
    AXL_AUTOPTR(AxlSocketClient) client = axl_socket_client_new();
    AXL_AUTOPTR(AxlSocket)       sock   = NULL;

    axl_printf("Connecting to %s:%u...\n", host, (unsigned)port);

    if (axl_socket_client_connect_to_host(client, host, port, &sock) != 0) {
        axl_printf("Connect failed\n");
        return 1;
    }

    axl_printf("Connected!\n");

    /* Send HTTP GET request */
    int n = axl_snprintf(request, sizeof(request),
        "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
    axl_socket_send(sock, request, (size_t)n, 5000);

    /* Receive response */
    resp_len = sizeof(response) - 1;
    if (axl_socket_receive(sock, response, &resp_len, 5000) == 0) {
        response[resp_len] = '\0';
        axl_printf("Response (%zu bytes):\n%s\n", resp_len, response);
    } else {
        axl_printf("Receive failed\n");
    }

    return 0;
}
