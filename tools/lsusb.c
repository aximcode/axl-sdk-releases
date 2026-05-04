/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file lsusb.c
    USB device lister (UEFI lsusb(8) equivalent).

    Built on the AxlUsb public API (EFI_USB_IO_PROTOCOL enumeration
    via LocateHandleBuffer; descriptor reads; class triplet decode;
    JSON5 vendor/device-name sidecar). Output mirrors Linux lsusb
    with the same divergences AxlPci's lspci ships with:

      1. -v / -vv mean "more detail in the listing" — Linux lsusb's
         convention. Our other tools use -v for log verbosity; this
         tool uses --debug for that purpose so the muscle-memory
         flag stays useful.

      2. No 6 MB usb.ids text database. lsusb.efi reads a curated
         JSON5 sidecar (`usb-ids.json5`) auto-discovered next to
         the .efi or in cwd. To convert the canonical usb.ids file
         to this schema, see scripts/usb-ids-to-json5.py (future).

      3. The tree view (-t) walks the real USB hub-port chain via
         `axl_usb_tree_for_each` — depth comes from the EFI device
         path's USB messaging-node count, so a device behind one
         hub renders one indent deeper than a directly-attached
         device. (Linux `lsusb -t` synthesizes a root-hub line per
         bus; we use a `Bus NNN` heading to the same effect.)

    Build with axl-cc:
      axl-cc lsusb.c -o lsusb.efi

    Usage:
      lsusb [-t] [-s BBB[:DDD]] [-d V[:P]] [-n] [-v[v]]
            [--ids-file PATH] [--debug]
**/

#include <axl.h>
#include <axl/axl-log.h>
#include <axl/axl-usb.h>

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "addr",     .short_name = 's', .type = AXL_ARG_STRING,
      .help = "Filter to one BBB[:DDD] (decimal bus[:device])" },
    { .name = "id",       .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Filter by VID[:PID] in hex (e.g. 0627 or 0627:0001)" },
    { .name = "numeric",  .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Numeric IDs only (skip vendor/device-name lookup)" },
    { .name = "verbose",  .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose: per-interface class triplet + string descriptors" },
    { .name = "vv",       .type = AXL_ARG_BOOL,
      .help = "Very verbose: same as -v plus per-interface row in default mode" },
    { .name = "tree",     .short_name = 't', .type = AXL_ARG_BOOL,
      .help = "Tree view: bus → device → interface" },
    { .name = "ids-file", .type = AXL_ARG_STRING,
      .help = "Path to usb-ids.json5 (default: companion to .efi or cwd)" },
    { .name = "debug",    .type = AXL_ARG_BOOL,
      .help = "Set log level to DEBUG (replaces our usual -v)" },
    {0}
};

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

static bool      filter_addr_set;
static uint8_t   filter_addr_bus;
static bool      filter_addr_dev_set;
static uint8_t   filter_addr_dev;

static bool      filter_vid_set;
static uint16_t  filter_vid;
static bool      filter_pid_set;
static uint16_t  filter_pid;

static bool      opt_numeric;
static unsigned  opt_verbose;     /* 0 / 1 / 2 */
static bool      opt_tree;
static bool      g_ids_loaded;

/* Parse "BBB" or "BBB:DDD" (decimal, Linux lsusb -s convention). */
static int
parse_addr_filter(
    const char  *s
    )
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    /* Accept arbitrary leading digits, then optional ':DDD'. */
    unsigned bus = 0;
    int      i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        bus = bus * 10 + (unsigned)(s[i] - '0');
        i++;
        if (bus > 0xFF) {
            return -1;
        }
    }
    if (i == 0) {
        return -1;
    }
    filter_addr_bus = (uint8_t)bus;
    filter_addr_set = true;

    if (s[i] == '\0') {
        return 0;
    }
    if (s[i] != ':') {
        return -1;
    }
    i++;
    unsigned dev = 0;
    int      j = 0;
    while (s[i + j] >= '0' && s[i + j] <= '9') {
        dev = dev * 10 + (unsigned)(s[i + j] - '0');
        j++;
        if (dev > 0xFF) {
            return -1;
        }
    }
    if (j == 0 || s[i + j] != '\0') {
        return -1;
    }
    filter_addr_dev     = (uint8_t)dev;
    filter_addr_dev_set = true;
    return 0;
}

/* Parse "VVVV" or "VVVV:PPPP" (hex, no 0x). Mirrors lspci -d. */
static int
parse_id_filter(
    const char  *s
    )
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    uint32_t  v = 0;
    int       i = 0;
    while (i < 4 && s[i] != '\0' && s[i] != ':') {
        int d = axl_hex_nibble(s[i]);
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | (uint32_t)d;
        i++;
    }
    if (i == 0) {
        return -1;
    }
    filter_vid     = (uint16_t)v;
    filter_vid_set = true;

    if (s[i] == '\0') {
        return 0;
    }
    if (s[i] != ':') {
        return -1;
    }
    i++;
    uint32_t p = 0;
    int      j = 0;
    while (j < 4 && s[i + j] != '\0') {
        int d = axl_hex_nibble(s[i + j]);
        if (d < 0) {
            return -1;
        }
        p = (p << 4) | (uint32_t)d;
        j++;
    }
    if (j == 0 || s[i + j] != '\0') {
        return -1;
    }
    filter_pid     = (uint16_t)p;
    filter_pid_set = true;
    return 0;
}

static bool
match_filters(
    AxlUsbAddr  a,
    uint16_t    vid,
    uint16_t    pid
    )
{
    if (filter_addr_set && a.bus != filter_addr_bus) {
        return false;
    }
    if (filter_addr_dev_set && a.addr != filter_addr_dev) {
        return false;
    }
    if (filter_vid_set && vid != filter_vid) {
        return false;
    }
    if (filter_pid_set && pid != filter_pid) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static void
print_class_triplet(
    uint8_t  cls,
    uint8_t  sub,
    uint8_t  prot
    )
{
    if (opt_numeric) {
        axl_printf("%02x%02x%02x", cls, sub, prot);
        return;
    }
    char buf[AXL_USB_CLASS_NAME_MAX];
    if (axl_usb_class_string_fmt(cls, sub, prot,
                                 AXL_USB_CLASS_FMT_FULL,
                                 buf, sizeof(buf)) > 0)
    {
        axl_printf("%s", buf);
    } else {
        axl_printf("%02x%02x%02x", cls, sub, prot);
    }
}

/* Emit any device-level string descriptors the device exposes —
   manufacturer / product / serial. Each is silently skipped if the
   device declares no string at that slot (axl_usb_get_* returns -1).
   Indented under the device row. */
static void
print_device_strings(
    AxlUsbAddr  addr,
    const char *indent
    )
{
    char buf[AXL_USB_STRING_MAX];
    if (axl_usb_get_manufacturer(addr, buf, sizeof(buf)) > 0) {
        axl_printf("%siManufacturer  %s\n", indent, buf);
    }
    if (axl_usb_get_product(addr, buf, sizeof(buf)) > 0) {
        axl_printf("%siProduct       %s\n", indent, buf);
    }
    if (axl_usb_get_serial(addr, buf, sizeof(buf)) > 0) {
        axl_printf("%siSerial        %s\n", indent, buf);
    }
}

// ---------------------------------------------------------------------------
// Per-row rendering
// ---------------------------------------------------------------------------

/* Default Linux lsusb shape: one row per device, identifying it by
   bus + (synthesized) address. Verbose adds class triplet under
   each interface plus per-device strings. */
static void
list_default(
    void
    )
{
    AxlUsbAddr  *u = NULL;
    uint8_t      prev_bus = 0;
    uint8_t      prev_addr = 0;
    bool         have_prev = false;

    while ((u = axl_usb_next(u)) != NULL) {
        uint16_t vid = 0;
        uint16_t pid = 0;
        if (axl_usb_get_vid_pid(*u, &vid, &pid) != AXL_OK) {
            continue;
        }
        if (!match_filters(*u, vid, pid)) {
            continue;
        }

        bool device_changed = !have_prev
                              || u->bus != prev_bus
                              || u->addr != prev_addr;

        /* In default mode (no -vv) emit one row per physical device.
           In -vv mode emit one row per interface. */
        if (device_changed || opt_verbose >= 2) {
            axl_printf("Bus %03u Device %03u:",
                       (unsigned)u->bus, (unsigned)u->addr);
            if (opt_verbose >= 2) {
                axl_printf(" If %u", (unsigned)u->intf);
            }
            axl_printf(" ID %04x:%04x", (unsigned)vid, (unsigned)pid);
            /* Append vendor/device names when known. format_name's
               "vendor unknown" fallback would just repeat the vid:pid
               we already printed — gate on vendor-known to skip that. */
            if (!opt_numeric && axl_usb_vendor_name(vid) != NULL) {
                char buf[AXL_USB_NAME_COMPOSED_MAX];
                if (axl_usb_format_name(vid, pid, buf, sizeof(buf)) > 0) {
                    axl_printf(" %s", buf);
                }
            }
            axl_printf("\n");
        }

        if (opt_verbose >= 1) {
            uint8_t cls = 0, sub = 0, prot = 0;
            if (axl_usb_get_class(*u, &cls, &sub, &prot) == AXL_OK) {
                axl_printf("    If %u: ", (unsigned)u->intf);
                print_class_triplet(cls, sub, prot);
                axl_printf("\n");
            }
            /* Device-level strings only on the first interface row
               of each device — they're per-device, not per-interface. */
            if (device_changed) {
                print_device_strings(*u, "    ");
            }
        }

        prev_bus  = u->bus;
        prev_addr = u->addr;
        have_prev = true;
    }
}

/* Tree view: walk the real USB hub-port chain via
   axl_usb_tree_for_each. Depth comes from the device path's USB
   messaging-node count — a device directly on the controller's
   root hub is depth 0; one hub deep is depth 1; etc.

   Indents 2 spaces per level past the bus heading; uses ASCII
   connectors so the output stays readable on UEFI consoles that
   don't render UCS-2 box-drawing reliably. Linux lsusb -t emits a
   synthetic root-hub line per bus; we use a bare "Bus NNN" heading
   to the same effect. */
typedef struct {
    uint8_t  prev_bus;
    uint8_t  prev_addr;
    bool     have_prev;
} TreeRenderCtx;

static int
tree_render_cb(
    AxlUsbAddr   addr,
    unsigned     depth,
    void        *ctx
    )
{
    TreeRenderCtx *t = ctx;

    /* Filters apply: read vid/pid up front. axl_usb_tree_for_each
       can't be cancelled per-entry by filter mismatch — return 0 to
       continue the walk; the rendering just elides this row. */
    uint16_t vid = 0;
    uint16_t pid = 0;
    if (axl_usb_get_vid_pid(addr, &vid, &pid) != AXL_OK) {
        return 0;
    }
    if (!match_filters(addr, vid, pid)) {
        return 0;
    }

    bool bus_changed    = !t->have_prev || addr.bus != t->prev_bus;
    bool device_changed = bus_changed || addr.addr != t->prev_addr;

    if (bus_changed) {
        axl_printf("Bus %03u\n", (unsigned)addr.bus);
    }
    if (device_changed) {
        /* Two spaces of base indent + two per hub depth. The leading
           "\\- " connector at the device row visually attaches the
           device to its parent (bus heading or upstream hub). */
        for (unsigned i = 0; i < depth; i++) {
            axl_printf("  ");
        }
        axl_printf("  \\- Device %03u: ID %04x:%04x",
                   (unsigned)addr.addr, (unsigned)vid, (unsigned)pid);
        if (!opt_numeric && axl_usb_vendor_name(vid) != NULL) {
            char buf[AXL_USB_NAME_COMPOSED_MAX];
            if (axl_usb_format_name(vid, pid, buf, sizeof(buf)) > 0) {
                axl_printf(" %s", buf);
            }
        }
        axl_printf("\n");
    }

    /* Per-interface row, indented one level past the device row. */
    for (unsigned i = 0; i < depth; i++) {
        axl_printf("  ");
    }
    axl_printf("    \\- Interface %u", (unsigned)addr.intf);
    uint8_t cls = 0, sub = 0, prot = 0;
    if (axl_usb_get_class(addr, &cls, &sub, &prot) == AXL_OK) {
        axl_printf(": ");
        print_class_triplet(cls, sub, prot);
    }
    axl_printf("\n");

    t->prev_bus  = addr.bus;
    t->prev_addr = addr.addr;
    t->have_prev = true;
    return 0;
}

static void
list_tree(
    void
    )
{
    TreeRenderCtx ctx = { 0 };
    axl_usb_tree_for_each(tree_render_cb, &ctx);
}

// ---------------------------------------------------------------------------
// Verbose mode shape pre-expand: -vv → --vv (mirrors lspci)
// ---------------------------------------------------------------------------

static char **
expand_count_flags(
    int     argc,
    char  **argv,
    int    *out_argc
    )
{
    static char vv_buf[] = "--vv";
    int    n = 0;

    char **new_argv = axl_malloc((size_t)(argc + 1) * sizeof(char *));
    if (new_argv == NULL) {
        /* OOM on a few-pointer alloc is implausible in UEFI, but a
           NULL-deref in the loop below is unsafe. Fall back to the
           caller's argv with no -vv expansion; main() detects the
           equality and skips the free. */
        *out_argc = argc;
        return argv;
    }

    if (argc > 0) {
        new_argv[n++] = argv[0];
    }
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        char       *replacement = NULL;
        /* "-vv" → "--vv" (-vvv and longer rejected by AxlArgs). */
        if (s[0] == '-' && s[1] == 'v' && s[2] == 'v' && s[3] == '\0') {
            replacement = vv_buf;
        }
        new_argv[n++] = (replacement != NULL) ? replacement : (char *)s;
    }
    new_argv[n] = NULL;
    *out_argc = n;
    return new_argv;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

static int
run_lsusb(
    AxlArgs  *a
    )
{
    if (axl_args_get_bool(a, "debug")) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }
    opt_numeric = axl_args_get_bool(a, "numeric");
    opt_tree    = axl_args_get_bool(a, "tree");
    if (axl_args_get_bool(a, "vv")) {
        opt_verbose = 2;
    } else if (axl_args_get_bool(a, "verbose")) {
        opt_verbose = 1;
    }

    /* Reject -t with -v: tree view shows topology only; verbose
       extras render at the wrong indent level. lspci enforces the
       same combo. */
    if (opt_tree && opt_verbose > 0) {
        axl_printf("lsusb: -t cannot be combined with -v "
                   "(tree view shows topology only; use the flat "
                   "list for per-interface detail)\n");
        return 1;
    }

    /* Opportunistic vendor/device-name database load. -n suppresses
       the lookup, so don't bother loading when the user only wants
       numeric output. */
    if (!opt_numeric) {
        g_ids_loaded = (axl_usb_ids_load(
                            axl_args_get_string(a, "ids-file"))
                        == AXL_SIDECAR_OK);
    }

    const char *addr_str = axl_args_get_string(a, "addr");
    if (addr_str != NULL && parse_addr_filter(addr_str) != 0) {
        axl_printf("lsusb: invalid -s value '%s' "
                   "(expected BBB[:DDD] decimal)\n", addr_str);
        return 1;
    }
    const char *id_str = axl_args_get_string(a, "id");
    if (id_str != NULL && parse_id_filter(id_str) != 0) {
        axl_printf("lsusb: invalid -d value '%s' "
                   "(expected VID[:PID] hex)\n", id_str);
        return 1;
    }

    if (opt_tree) {
        list_tree();
    } else {
        list_default();
    }

    /* Tip: only suggest the user grab usb-ids.json5 when names are
       wanted and the file isn't loaded. Stay polite — two lines max,
       suppressed when -n is set. */
    if (!opt_numeric && !g_ids_loaded) {
        axl_printf("\n(usb-ids.json5 not loaded — vendor/device names unavailable.\n");
        axl_printf(" Stage axl-sdk's curated set next to lsusb.efi or pass --ids-file.)\n");
    }
    return 0;
}

int
main(int argc, char **argv)
{
    int    new_argc;
    char **new_argv = expand_count_flags(argc, argv, &new_argc);
    int rc = axl_args_run(new_argc, new_argv, &(AxlArgsNode){
        .name    = "lsusb",
        .help    = "List USB devices (UEFI lsusb equivalent)",
        .flags   = flags,
        .handler = run_lsusb,
    });
    /* On success new_argv is heap; its contents are either argv strings
       (caller owns) or pointers to static "-vv" buffer. Free only the
       array. On OOM, expand_count_flags returns argv unchanged — don't
       free in that case. */
    if (new_argv != argv) {
        axl_free(new_argv);
    }
    return rc;
}
