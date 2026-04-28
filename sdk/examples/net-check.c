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

    /* Bring up networking before probing — load NIC drivers, run
       ConnectController, wait up to 10 s for DHCP. axl_net_auto_init
       is idempotent and short-circuits if SNP is already up
       (typical when the firmware shell already ran `connect -r`). */
    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("Network: bring-up failed (no NIC, or no DHCP)\n");
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
