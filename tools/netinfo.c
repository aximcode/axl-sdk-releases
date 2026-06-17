/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file NetInfo.c
    Network interface diagnostics and ping (UEFI ifconfig/ping equivalent).

    Build with axl-cc:
      axl-cc NetInfo.c -o NetInfo.efi

    Usage:
      NetInfo.efi [-v] [-n] [-h] [list | ping <ip> [-c <count>] | list-bundle]
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// File-scope variables
// ---------------------------------------------------------------------------

static bool verbose = false;
static bool no_load = false;

/* Build-arch label for drivers/<arch>/ — must match the static
 * driver_arch in src/util/axl-driver.c. axl-sdk tools are built per-arch
 * so a compile-time choice is exact. */
#if defined(__x86_64__)
static const char netinfo_arch[] = "x64";
#elif defined(__aarch64__)
static const char netinfo_arch[] = "aa64";
#else
#  error "unknown arch"
#endif

static const AxlArgDesc global_flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output: device-path text, NII layer info, debug logs" },
    { .name = "no-load", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Skip auto-loading NIC drivers from drivers/<arch>/" },
    {0}
};

static const AxlArgDesc ping_flags[] = {
    { .name = "count", .short_name = 'c', .type = AXL_ARG_U32, .base = 10,
      .help = "Number of pings (default: 4)" },
    { .name = "nic",                                           .type = AXL_ARG_U64,
      .default_value = AXL_NET_NIC_AUTO_STR,
      .help = "NIC index for DHCP target (default: auto-select first usable)" },
    {0}
};

static const AxlArgDesc ping_pos[] = {
    { .name = "target", .type = AXL_ARG_STRING, .required = true,
      .help = "Target IP address" },
    {0}
};

/* Report the driver image bound to each NIC, plus (under -v) its bus
 * location, via the public axl_net_get_driver_info accessor. This was a
 * private EFI_NETWORK_INTERFACE_IDENTIFIER walk here until AxlNet grew
 * the accessor; netinfo now dogfoods it.
 *
 * axl_net_get_driver_info walks NII3.1 -> NII (legacy) -> SNP for the
 * binding layer that actually owns the hardware, and reports the
 * device-path topology (MAC tail trimmed) as a stable bus location. */
static void
show_nic_drivers(const char *label)
{
    size_t count = 0;
    if (axl_net_list_interfaces(NULL, &count) != AXL_OK || count == 0) {
        axl_printf("=== %s ===\n  (no network interfaces)\n\n", label);
        return;
    }

    AxlNetInterface *ifaces = axl_calloc(count, sizeof(AxlNetInterface));
    if (ifaces == NULL) {
        axl_printf("=== %s ===\n  (out of memory)\n\n", label);
        return;
    }
    axl_net_list_interfaces(ifaces, &count);

    axl_printf("=== %s ===\n\n", label);
    for (size_t i = 0; i < count; i++) {
        AxlNetDriverInfo di;
        bool have = (axl_net_get_driver_info(ifaces[i].mac, &di) == AXL_OK);

        const char *layer  = (have && di.layer[0]  != '\0') ? di.layer  : "-";
        const char *driver = (have && di.driver[0] != '\0')
            ? di.driver : "<no driver attached>";

        axl_printf("  NIC[%zu] [%s] driver=%s\n", i, layer, driver);

        if (verbose && have && di.bus_location[0] != '\0') {
            axl_printf("         bus=%s\n", di.bus_location);
        }
    }
    axl_printf("\n");

    axl_free(ifaces);
}

/* Auto-load NIC drivers so NetInfo works from a bare UEFI shell without
 * a startup.nsh that pre-loads them. Same shape as MkRd for RamDiskDxe.
 * Returns 0 on success-or-no-load, non-zero if hard fail (no NICs and
 * driver-load couldn't change that). */
static int
ensure_net_drivers_warn(void)
{
    if (no_load) {
        /* User opted out of disk-driver loading, but still want
           firmware-provided drivers to bind. UEFI doesn't run the
           equivalent of `connect -r` automatically — without it,
           NICs that have a firmware-provided driver bound at boot
           may not have SNP up yet. Trigger the connect explicitly
           so `--no-load` produces a useful "firmware baseline"
           instead of silently zero NICs. */
        axl_driver_connect(NULL);
        return 0;
    }
    switch (axl_net_ensure_drivers()) {
    case AXL_NET_DRIVERS_OK:
        return 0;
    case AXL_NET_DRIVERS_NOT_FOUND:
        axl_printf("NetInfo: warning: no NIC drivers found in "
                   "drivers/<arch>/ on any mounted volume.\n");
        return 1;
    case AXL_NET_DRIVERS_NO_LINK:
        axl_printf("NetInfo: warning: drivers loaded but no NIC "
                   "came up - is a NIC plugged in?\n");
        return 1;
    default:
        axl_printf("NetInfo: warning: failed to bring up networking.\n");
        return 1;
    }
}

// ---------------------------------------------------------------------------
// List network interfaces
// ---------------------------------------------------------------------------

static void
show_interfaces(void)
{
    size_t count = 0;
    if (axl_net_list_interfaces(NULL, &count) != AXL_OK || count == 0) {
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
// list-bundle: enumerate stageable NIC drivers via axl_net_list_available_drivers
// ---------------------------------------------------------------------------

static int
do_list_bundle_verb(AxlArgs *a)
{
    (void)a;

    size_t count = 0;
    if (axl_net_list_available_drivers(NULL, &count) != AXL_OK || count == 0) {
        axl_printf("=== Driver Bundle (drivers/%s/) ===\n"
                   "  (no drivers staged)\n\n", netinfo_arch);
        return 0;
    }

    AxlNetDriverFile *files = axl_calloc(count, sizeof(AxlNetDriverFile));
    if (files == NULL) {
        axl_printf("NetInfo: out of memory\n");
        return 1;
    }
    axl_net_list_available_drivers(files, &count);

    axl_printf("=== Driver Bundle (drivers/%s/ on each volume) ===\n\n",
               netinfo_arch);
    for (size_t i = 0; i < count; i++) {
        axl_printf("  %-32s %8llu bytes  %s\n",
                   files[i].name,
                   (unsigned long long)files[i].size,
                   files[i].path);
    }
    axl_printf("\n");
    axl_free(files);
    return 0;
}

// ---------------------------------------------------------------------------
// diag: composite report intended to be copy-pasted to a maintainer
// ---------------------------------------------------------------------------

static void
diag_show_firmware(void)
{
    AxlFirmwareInfo info = {0};
    if (axl_sys_get_firmware_info(&info) != AXL_OK) {
        axl_printf("=== Firmware ===\n  (info unavailable)\n\n");
        return;
    }
    axl_printf("=== Firmware ===\n");
    axl_printf("  vendor    : %s\n", info.vendor);
    axl_printf("  rev       : 0x%08x\n", info.firmware_revision);
    axl_printf("  uefi spec : %u.%u\n", info.spec_major, info.spec_minor);
    axl_printf("  arch      : %s\n", netinfo_arch);
    axl_printf("\n");
}

static void
diag_show_volumes(void)
{
    AxlVolume volumes[16];
    size_t n = 0;
    if (axl_volume_enumerate(volumes, 16, &n) != AXL_OK || n == 0) {
        axl_printf("=== Mounted Volumes ===\n  (none)\n\n");
        return;
    }
    axl_printf("=== Mounted Volumes ===\n");
    for (size_t i = 0; i < n; i++) {
        AXL_AUTO_FREE char *dp_text =
            axl_device_path_to_text(volumes[i].device_path);
        axl_printf("  %-6s %s\n",
                   volumes[i].name,
                   dp_text != NULL ? dp_text : "<no device path>");
    }
    axl_printf("\n");
}

/* PCI class 0x02 = Network controller. Walking by class shows every
 * NIC the firmware sees in config space — including ones with no UEFI
 * driver attached. The gap between PCI Network Controllers and
 * Simple Network Protocol handles is the "missing driver" smoking gun. */
static void
diag_show_pci_nics(void)
{
    axl_printf("=== PCI Network Controllers (class 0x02xxxx) ===\n");
    /* Walk every PCI function, filtering by base class 0x02.
     * axl_pci_find_by_class needs an exact 24-bit class match, which
     * misses subclasses; the cursor walk lets us match base alone. */
    AxlPciAddr *p = NULL;
    bool any = false;
    while ((p = axl_pci_next(p)) != NULL) {
        uint32_t class_code = 0;
        if (axl_pci_get_class_code(*p, &class_code) != AXL_OK) continue;
        if (((class_code >> 16) & 0xFF) != 0x02) continue;

        uint16_t vid = 0, did = 0;
        (void)axl_pci_get_vid_did(*p, &vid, &did);

        char addr_buf[AXL_PCI_ADDR_STR_MAX];
        axl_pci_addr_format(*p, addr_buf, sizeof(addr_buf));

        char id_buf[160];
        if (axl_pci_format_name(vid, did, id_buf, sizeof(id_buf)) <= 0) {
            axl_snprintf(id_buf, sizeof(id_buf), "%04X:%04X", vid, did);
        }

        char class_buf[80];
        (void)axl_pci_class_string(class_code, class_buf, sizeof(class_buf));

        axl_printf("  %-13s  %s  (%s)\n", addr_buf, id_buf, class_buf);
        any = true;
    }
    if (!any) {
        axl_printf("  (no network-class PCI functions enumerated)\n");
    }
    axl_printf("\n");
}

static int
do_diag_verb(AxlArgs *a)
{
    (void)a;

    /* diag implies verbose — every section gets its richer payload.
     * Library debug logs come from AXL_LOG_LEVEL=debug (or per-domain
     * filters); we don't override here. */
    verbose = true;

    axl_printf("\n=== NetInfo diag (paste this back to the maintainer) ===\n\n");

    diag_show_firmware();
    diag_show_volumes();
    diag_show_pci_nics();

    /* Driver bundle inventory — same shape as `list-bundle`. */
    (void)do_list_bundle_verb(a);

    /* NIC handle table BEFORE driver-load attempt — shows what
     * firmware natively provides. */
    show_nic_drivers("NIC Drivers (before driver-load)");

    /* Try to bring drivers up unless explicitly suppressed. */
    int rc = ensure_net_drivers_warn();
    axl_printf("=== ensure_drivers status ===\n");
    if (no_load) {
        axl_printf("  --no-load set: skipped\n\n");
    } else {
        axl_printf("  result: %s\n\n", rc == 0 ? "OK" : "no link / not found");
    }

    if (!no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    show_interfaces();
    return 0;
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

    axl_printf("PING %s - %zu packets\n", target, count);

    size_t sent = 0;
    size_t received = 0;
    size_t rtt_min = (size_t)-1;
    size_t rtt_max = 0;
    size_t rtt_total = 0;

    for (size_t i = 0; i < count; i++) {
        size_t rtt_ms = 0;
        int rc = axl_net_ping(&addr, 5000, &rtt_ms);
        sent++;

        if (rc == AXL_OK) {
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

/* Pre-run hook: pick up global flags. Per-verb logic owns the
 * driver-load decision so list-bundle never tries to bring up NICs and
 * `list` doesn't fail-closed when drivers can't be loaded. */
static void
netinfo_pre_run(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");
    no_load = axl_args_get_bool(a, "no-load");
    /* For library debug logs use AXL_LOG_LEVEL=debug rather than
     * a tool-level switch — frees -v to carry tool-specific
     * verbose-output semantics. */
}

static int
do_list_verb(AxlArgs *a)
{
    (void)a;

    if (verbose) {
        show_nic_drivers(no_load
            ? "NIC Drivers (firmware-provided, --no-load)"
            : "NIC Drivers (before driver-load)");
    }

    /* Try to bring up drivers, but never gate `list` on the result —
     * showing zero interfaces when nothing's there is itself a useful
     * diagnostic. */
    (void)ensure_net_drivers_warn();

    if (verbose && !no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    show_interfaces();
    return 0;
}

static int
do_ping_verb(AxlArgs *a)
{
    if (verbose) {
        show_nic_drivers(no_load
            ? "NIC Drivers (firmware-provided, --no-load)"
            : "NIC Drivers (before driver-load)");
    }

    /* ping requires a NIC; fail-closed if drivers can't be loaded. */
    if (ensure_net_drivers_warn() != 0) {
        return 1;
    }

    if (verbose && !no_load) {
        show_nic_drivers("NIC Drivers (after driver-load)");
    }

    const char *target     = axl_args_get_string(a, "target");
    uint32_t    ping_count = (uint32_t)axl_args_get_uint(a, "count");
    if (ping_count == 0) {
        ping_count = 4;
    }
    if (axl_net_init(axl_args_get_uint(a, "nic"), 10) != AXL_OK) {
        axl_printf("NetInfo: networking unavailable "
                   "(NIC driver, link, or DHCP setup failed).\n");
        return 1;
    }
    return do_ping(target, ping_count);
}

// ---------------------------------------------------------------------------
// try: load one specific driver and report whether it brought a NIC up
// ---------------------------------------------------------------------------

static const AxlArgDesc try_pos[] = {
    { .name = "driver", .type = AXL_ARG_STRING, .required = true,
      .help = "Driver path, or a name from `list-bundle`, to load and try" },
    {0}
};

static int
do_try_verb(AxlArgs *a)
{
    const char *name = axl_args_get_string(a, "driver");
    axl_printf("Trying driver: %s\n", name);

    AxlNetTryResult r;
    int rc = axl_net_try_driver(name, &r);

    if (!r.found) {
        axl_printf("  not found on the driver search path\n");
        return 1;
    }
    axl_printf("  loaded=%s  SNP handles added=%u  link=%s\n",
               r.loaded ? "yes" : "no",
               r.snp_handles_added,
               r.link_up ? "up" : "down");
    for (size_t i = 0; i < r.bound_nic_count; i++) {
        const uint8_t *m = r.bound_nic_macs[i];
        axl_printf("  bound NIC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                   m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    if (rc != AXL_OK) {
        axl_printf("  result: driver bound no NIC%s\n",
                   r.unloaded ? " (unloaded)" : "");
        return 1;
    }
    axl_printf("  result: OK\n");
    return 0;
}

// ---------------------------------------------------------------------------
// config: apply a static/DHCP policy bag (dogfoods axl_net_init_static)
// ---------------------------------------------------------------------------

static const AxlArgDesc config_flags[] = {
    { .name = "mode", .type = AXL_ARG_STRING, .default_value = "dhcp",
      .help = "Address mode: dhcp | static" },
    { .name = "ip",       .type = AXL_ARG_STRING, .help = "Static IPv4 (mode=static)" },
    { .name = "mask",     .type = AXL_ARG_STRING, .help = "Subnet mask (mode=static)" },
    { .name = "gw",       .type = AXL_ARG_STRING, .help = "Default gateway" },
    { .name = "dns",      .type = AXL_ARG_STRING, .help = "Primary DNS server" },
    { .name = "dns2",     .type = AXL_ARG_STRING, .help = "Secondary DNS server" },
    { .name = "hostname", .type = AXL_ARG_STRING, .help = "Hostname (persisted)" },
    { .name = "nic",      .type = AXL_ARG_U64, .default_value = AXL_NET_NIC_AUTO_STR,
      .help = "NIC index (default: auto-select first usable)" },
    {0}
};

static int
do_config_verb(AxlArgs *a)
{
    (void)ensure_net_drivers_warn();

    AxlNetStaticOpts cfg = {
        .mode     = axl_args_get_string(a, "mode"),
        .ip       = axl_args_get_string(a, "ip"),
        .netmask  = axl_args_get_string(a, "mask"),
        .gateway  = axl_args_get_string(a, "gw"),
        .dns      = axl_args_get_string(a, "dns"),
        .dns2     = axl_args_get_string(a, "dns2"),
        .hostname = axl_args_get_string(a, "hostname"),
    };
    uint64_t nic = axl_args_get_uint(a, "nic");

    axl_printf("Applying net config (mode=%s)...\n",
               (cfg.mode != NULL) ? cfg.mode : "dhcp");
    if (axl_net_init_static(&cfg, nic, 10) != AXL_OK) {
        axl_printf("NetInfo: net config failed\n");
        return 1;
    }

    AxlIPv4Address addr;
    if (axl_net_get_ip_address(&addr) == AXL_OK) {
        axl_printf("  address: %u.%u.%u.%u\n",
                   addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3]);
    } else {
        axl_printf("  address: (none)\n");
    }
    char host[64];
    if (axl_net_get_hostname(host, sizeof(host)) == AXL_OK && host[0] != '\0') {
        axl_printf("  hostname: %s\n", host);
    }
    return 0;
}

static const AxlArgsNode verbs[] = {
    { .name = "list", .handler = do_list_verb,
      .help = "List network interfaces and their state" },
    { .name = "ping", .handler = do_ping_verb,
      .flags = ping_flags, .positionals = ping_pos,
      .help = "Send ICMP echo to a target IP" },
    { .name = "list-bundle", .handler = do_list_bundle_verb,
      .help = "List staged drivers in drivers/<arch>/ on each mounted volume" },
    { .name = "config", .handler = do_config_verb, .flags = config_flags,
      .help = "Apply DHCP or static IP / DNS / hostname policy" },
    { .name = "try", .handler = do_try_verb, .positionals = try_pos,
      .help = "Load one driver and report whether it brought a NIC up" },
    { .name = "diag", .handler = do_diag_verb,
      .help = "Composite report (firmware, PCI NICs, drivers, interfaces)" },
    {0}
};

AXL_TOOL_MAIN(netinfo)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "NetInfo",
        .help         = "Network interface inventory and ICMP ping",
        .flags        = global_flags,
        .verbs        = verbs,
        .pre_run      = netinfo_pre_run,
    });
}
