/**
 * fetch.c — HTTP client: GET a URL and print the response.
 *
 * Demonstrates axl_http_client_new, axl_http_get, response handling,
 * and AXL_AUTOPTR for automatic resource cleanup.
 * Build with: axl-cc fetch.c -o fetch.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        axl_printf("usage: fetch <url>\n");
        return 1;
    }

    /* Bring up networking (load NIC drivers, ConnectController,
       wait for DHCP). Idempotent. */
    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("error: network bring-up failed\n");
        return 1;
    }

    AXL_AUTOPTR(AxlHttpClient) client = axl_http_client_new();
    if (client == NULL) {
        axl_printf("error: cannot create HTTP client\n");
        return 1;
    }

    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    int rc = axl_http_get(client, argv[1], &resp);
    if (rc != 0 || resp == NULL) {
        axl_printf("error: GET %s failed\n", argv[1]);
        return 1;
    }

    axl_printf("HTTP %llu\n", (unsigned long long)resp->status_code);

    if (resp->body != NULL && resp->body_size > 0) {
        for (size_t i = 0; i < resp->body_size; i++) {
            axl_printf("%c", ((char *)resp->body)[i]);
        }
        axl_printf("\n");
    }

    return 0;
}
