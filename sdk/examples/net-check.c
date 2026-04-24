/**
 * net-check.c — Verify AXL networking module links correctly.
 *
 * Checks whether UEFI networking is available and prints the IP address.
 * Build: axl-cc net-check.c -o net-check.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!axl_net_is_available()) {
        axl_printf("Network: not available\n");
        return 1;
    }

    AxlIPv4Address addr;
    if (axl_net_get_ip_address(&addr) == 0) {
        axl_printf("Network: %d.%d.%d.%d\n",
                   addr.addr[0], addr.addr[1],
                   addr.addr[2], addr.addr[3]);
    } else {
        axl_printf("Network: available but no IP yet\n");
    }

    return 0;
}
