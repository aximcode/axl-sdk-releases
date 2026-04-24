/**
 * sysinfo.c — Print system information: time, network, memory.
 *
 * Demonstrates axl_time_format, axl_net_get_ip_address,
 * axl_net_is_available, axl_mem_get_stats.
 *
 * Build with: axl-cc sysinfo.c -o sysinfo.efi
 */

#include <axl.h>

int
main(int argc, char **argv)
{
    char           timestamp[32];
    AxlIPv4Address addr;
    AxlMemStats    stats;

    (void)argc;
    (void)argv;

    /* Current time */
    axl_time_format(timestamp, sizeof(timestamp));
    axl_printf("Time: %s\n\n", timestamp);

    /* Network */
    if (axl_net_is_available()) {
        if (axl_net_get_ip_address(&addr) == 0) {
            axl_printf("IP Address: %d.%d.%d.%d\n",
                       addr.addr[0], addr.addr[1],
                       addr.addr[2], addr.addr[3]);
        }
    } else {
        axl_printf("Network: not available\n");
    }

    /* Memory stats */
    axl_mem_get_stats(&stats);
    axl_printf("\nMemory:\n");
    axl_printf("  Current: %llu allocations, %llu bytes\n",
               (unsigned long long)stats.count,
               (unsigned long long)stats.bytes);
    axl_printf("  Total:   %llu allocations, %llu bytes\n",
               (unsigned long long)stats.total_count,
               (unsigned long long)stats.total_bytes);

    return 0;
}
