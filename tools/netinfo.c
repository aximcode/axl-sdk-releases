/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file NetInfo.c
    Network interface diagnostics and ping (UEFI ifconfig/ping equivalent).

    Build with axl-cc:
      axl-cc NetInfo.c -o NetInfo.efi

    Usage:
      NetInfo.efi [-v] [-h] [list | ping <ip> [-c <count>]]
**/

#include <axl.h>

static bool verbose = false;

static const AxlConfigDesc descs[] = {
    { "verbose", AXL_CFG_BOOL,   "false", 'v', "Verbose output",                0, 0 },
    { "count",   AXL_CFG_STRING, NULL,    'c', "Number of pings (default: 4)",  0, 0 },
    { "help",    AXL_CFG_BOOL,   "false", 'h', "Show this help",                0, 0 },
    { 0 }
};

// ---------------------------------------------------------------------------
// List network interfaces
// ---------------------------------------------------------------------------

static void
show_interfaces(void)
{
    size_t count = 0;
    if (axl_net_list_interfaces(NULL, &count) != 0 || count == 0) {
        axl_printf("No network interfaces found.\n");
        return;
    }

    AxlNetInterface *ifaces = axl_calloc(count, sizeof(AxlNetInterface));
    if (ifaces == NULL) {
        axl_printf("NetInfo: out of memory\n");
        return;
    }
    axl_net_list_interfaces(ifaces, &count);

    axl_printf("=== Network Interfaces ===\n\n");
    axl_printf("  %-4s %-18s %-6s %-16s %s\n",
               "IF#", "MAC", "LINK", "IPv4", "MTU");
    axl_printf("  %-4s %-18s %-6s %-16s %s\n",
               "---", "------------------", "------",
               "----------------", "----");

    for (size_t i = 0; i < count; i++) {
        AxlNetInterface *iface = &ifaces[i];

        /* Format MAC */
        char mac_str[24];
        axl_snprintf(mac_str, sizeof(mac_str),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     iface->mac[0], iface->mac[1], iface->mac[2],
                     iface->mac[3], iface->mac[4], iface->mac[5]);

        /* Format IP */
        char ip_str[20] = "-";
        if (iface->has_ipv4) {
            axl_snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                         iface->ipv4[0], iface->ipv4[1],
                         iface->ipv4[2], iface->ipv4[3]);
        }

        axl_printf("  %-4zu %-18s %-6s %-16s %u\n",
                   i, mac_str,
                   iface->link_up ? "UP" : "DOWN",
                   ip_str,
                   (unsigned)iface->mtu);

        if (verbose && iface->has_ipv4) {
            axl_printf("       Netmask: %u.%u.%u.%u  Gateway: %u.%u.%u.%u\n",
                       iface->netmask[0], iface->netmask[1],
                       iface->netmask[2], iface->netmask[3],
                       iface->gateway[0], iface->gateway[1],
                       iface->gateway[2], iface->gateway[3]);
        }
    }

    axl_printf("\n");
    axl_free(ifaces);
}

// ---------------------------------------------------------------------------
// Ping
// ---------------------------------------------------------------------------

static bool
parse_ipv4(
    const char     *str,
    AxlIPv4Address *out
    )
{
    unsigned int a, b, c, d;
    const char *p = str;

    /* Simple manual parse: a.b.c.d */
    a = b = c = d = 0;
    while (*p >= '0' && *p <= '9') { a = a * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || a > 255) { return false; }
    while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || b > 255) { return false; }
    while (*p >= '0' && *p <= '9') { c = c * 10 + (*p - '0'); p++; }
    if (*p++ != '.' || c > 255) { return false; }
    while (*p >= '0' && *p <= '9') { d = d * 10 + (*p - '0'); p++; }
    if (*p != '\0' || d > 255) { return false; }

    out->addr[0] = (uint8_t)a;
    out->addr[1] = (uint8_t)b;
    out->addr[2] = (uint8_t)c;
    out->addr[3] = (uint8_t)d;
    return true;
}

static int
do_ping(
    const char *target,
    size_t      count
    )
{
    AxlIPv4Address addr;
    if (!parse_ipv4(target, &addr)) {
        axl_printf("NetInfo: invalid IPv4 address '%s'\n", target);
        return 1;
    }

    axl_printf("PING %s — %zu packets\n", target, count);

    size_t sent = 0;
    size_t received = 0;
    size_t rtt_min = (size_t)-1;
    size_t rtt_max = 0;
    size_t rtt_total = 0;

    for (size_t i = 0; i < count; i++) {
        size_t rtt_ms = 0;
        int rc = axl_net_ping(&addr, 5000, &rtt_ms);
        sent++;

        if (rc == 0) {
            received++;
            rtt_total += rtt_ms;
            if (rtt_ms < rtt_min) {
                rtt_min = rtt_ms;
            }
            if (rtt_ms > rtt_max) {
                rtt_max = rtt_ms;
            }
            axl_printf("  Reply from %s: time=%zums\n",
                       target, rtt_ms);
        } else {
            axl_printf("  Request timed out.\n");
        }
    }

    /* Statistics */
    axl_printf("\n--- %s ping statistics ---\n", target);
    axl_printf("%zu packets sent, %zu received", sent, received);
    if (sent > 0) {
        size_t loss = ((sent - received) * 100) / sent;
        axl_printf(", %zu%% packet loss", loss);
    }
    axl_printf("\n");

    if (received > 0) {
        size_t avg = rtt_total / received;
        axl_printf("rtt min/avg/max = %zu/%zu/%zums\n",
                   rtt_min, avg, rtt_max);
    }

    return (received > 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
main(
    int    argc,
    char **argv
    )
{
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, NULL);
    if (cfg == NULL || axl_config_parse_args(cfg, argc, argv) != 0) {
        axl_printf("NetInfo: invalid option\n");
        axl_config_usage(cfg, "NetInfo",
                         "[-v] [list | ping <ip> [-c <count>]]");
        return 1;
    }

    if (axl_config_get_bool(cfg, "help")) {
        axl_config_usage(cfg, "NetInfo",
                         "[-v] [list | ping <ip> [-c <count>]]");
        return 0;
    }

    verbose = axl_config_get_bool(cfg, "verbose");

    const char *cmd = axl_config_pos(cfg, 0);

    if (cmd == NULL || axl_strcmp(cmd, "list") == 0) {
        show_interfaces();
    } else if (axl_strcmp(cmd, "ping") == 0) {
        const char *target = axl_config_pos(cfg, 1);
        if (target == NULL) {
            axl_printf("NetInfo: ping requires an IP address\n");
            return 1;
        }

        size_t ping_count = 4;
        const char *count_str = axl_config_get(cfg, "count");
        if (count_str != NULL) {
            ping_count = (size_t)axl_strtou64(count_str);
            if (ping_count == 0) {
                ping_count = 4;
            }
        }

        return do_ping(target, ping_count);
    } else {
        axl_printf("NetInfo: unknown command '%s'\n", cmd);
        return 1;
    }

    return 0;
}
