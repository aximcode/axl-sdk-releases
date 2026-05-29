/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file lspci.c
    PCI/PCIe device lister (UEFI lspci(8) equivalent).

    Built on the AxlPci public API (ECAM-based config-space access
    via the ACPI MCFG table). Output mirrors Linux lspci with two
    intentional divergences:

      1. -v / -vv / -vvv mean "more detail in the listing" — the same
         meaning Linux lspci has used for ~25 years. Our other tools
         use -v as a log-verbosity toggle; this tool uses --debug for
         that purpose so the muscle-memory flag stays useful.

      2. No 6 MB pci.ids text database. lspci.efi reads a curated
         JSON5 sidecar (`pci-ids.json5`) auto-discovered next to
         the .efi or in cwd; the starter set covers QEMU + common
         server hardware. To convert the canonical pci.ids file
         to this schema, see `scripts/pci-ids-to-json5.py`.

    Build with axl-cc:
      axl-cc lspci.c -o lspci.efi

    Usage:
      lspci [-t] [-s ADDR] [-d V[:D]] [-n] [-D] [-v[v[v]]] [-x[x[x]]]
            [--ids-file PATH] [--debug]
**/

#include <axl.h>
#include <axl/axl-pci.h>
#include <axl/axl-log.h>

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "addr",    .short_name = 's', .type = AXL_ARG_STRING,
      .help = "Filter to one BDF (e.g. 00:1f.0 or 0001:aa:1f.7)" },
    { .name = "id",      .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Filter by VID[:DID] in hex (e.g. 8086 or 8086:100e)" },
    { .name = "numeric", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "Numeric class IDs (skip class-string decode)" },
    { .name = "domain",  .short_name = 'D', .type = AXL_ARG_BOOL,
      .help = "Always show segment (domain) prefix" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose: list legacy capabilities" },
    { .name = "vv",      .type = AXL_ARG_BOOL,
      .help = "Very verbose: also subsystem + extended capabilities" },
    { .name = "vvv",     .type = AXL_ARG_BOOL,
      .help = "Maximum: also decode each capability (currently same as --vv)" },
    { .name = "hex",     .short_name = 'x', .type = AXL_ARG_BOOL,
      .help = "Hex dump 64 bytes (standard header)" },
    { .name = "xx",      .type = AXL_ARG_BOOL,
      .help = "Hex dump 256 bytes (full legacy config)" },
    { .name = "xxx",     .type = AXL_ARG_BOOL,
      .help = "Hex dump 4 KiB (PCIe ECAM)" },
    { .name = "tree",    .short_name = 't', .type = AXL_ARG_BOOL,
      .help = "Tree view (SoftBMC-style: indented per bridge)" },
    { .name = "ids-file", .type = AXL_ARG_STRING,
      .help = "Path to pci-ids.json5 (default: companion to .efi or cwd)" },
    /* Use AXL_LOG_LEVEL=debug (or AXL_LOG_LEVEL=pci:debug) to enable
     * library debug logs; the tool no longer carries its own switch. */
    {0}
};

// ---------------------------------------------------------------------------
// Filters parsed once at startup
// ---------------------------------------------------------------------------

static bool        filter_addr_set;
static AxlPciAddr  filter_addr;
static bool        filter_vid_set;
static uint16_t    filter_vid;
static bool        filter_did_set;
static uint16_t    filter_did;

static bool         opt_numeric;
static bool         opt_show_domain;
static unsigned     opt_verbose;
static unsigned     opt_hex;
static bool         opt_tree;
static bool         g_ids_loaded;

static int
parse_id_filter(
    const char  *s
    )
{
    /* Form: VVVV or VVVV:DDDD (hex, no 0x). Empty/NULL is "no filter". */
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    uint64_t v = 0;
    int      n = axl_hex_parse_u64(s, 4, &v);
    if (n < 0 || (s[n] != '\0' && s[n] != ':')) {
        return -1;
    }
    filter_vid     = (uint16_t)v;
    filter_vid_set = true;

    if (s[n] == '\0') {
        return 0;
    }
    /* s[n] == ':' — parse the device-id field after it. */
    const char *did_str = s + n + 1;
    uint64_t    d_val   = 0;
    int         m       = axl_hex_parse_u64(did_str, 4, &d_val);
    if (m < 0 || did_str[m] != '\0') {
        return -1;
    }
    filter_did     = (uint16_t)d_val;
    filter_did_set = true;
    return 0;
}

static bool
match_filters(
    AxlPciAddr  a,
    uint16_t    vid,
    uint16_t    did
    )
{
    if (filter_addr_set) {
        if (a.seg != filter_addr.seg || a.bus != filter_addr.bus
            || a.dev != filter_addr.dev || a.func != filter_addr.func)
        {
            return false;
        }
    }
    if (filter_vid_set && vid != filter_vid) {
        return false;
    }
    if (filter_did_set && did != filter_did) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

/* Format the BDF prefix the way Linux lspci does: "BB:DD.F" by default,
   "SSSS:BB:DD.F" when domain is non-zero or -D was given. */
static void
print_bdf(
    AxlPciAddr  a
    )
{
    if (a.seg != 0 || opt_show_domain) {
        axl_printf("%04x:%02x:%02x.%x",
                   (unsigned)a.seg, (unsigned)a.bus,
                   (unsigned)a.dev, (unsigned)a.func);
    } else {
        axl_printf("%02x:%02x.%x",
                   (unsigned)a.bus, (unsigned)a.dev, (unsigned)a.func);
    }
}

static void
print_class(
    uint32_t  class_code
    )
{
    if (opt_numeric) {
        axl_printf("%06x", (unsigned)class_code);
        return;
    }
    char buf[96];
    if (axl_pci_class_string(class_code, buf, sizeof(buf)) > 0) {
        axl_printf("%s", buf);
    } else {
        axl_printf("%06x", (unsigned)class_code);
    }
}

/* Format the vendor/device tail: " <name> [vid:did]" via the
   composed-name helper (every consumer renders the same string
   for the same (vid, did) pair). With -n the name is suppressed
   and only the numeric form is emitted (matches Linux lspci -n). */
static void
print_vendor_device(
    uint16_t  vid,
    uint16_t  did
    )
{
    if (!opt_numeric) {
        char buf[AXL_PCI_NAME_COMPOSED_MAX];
        if (axl_pci_format_name(vid, did, buf, sizeof(buf)) > 0) {
            axl_printf(" %s", buf);
            /* axl_pci_format_name already includes the numeric pair
               for the "vendor unknown" fallback; for the other two
               fallback paths (vendor known, with or without device)
               append the [vid:did] suffix here so users can always
               paste it elsewhere. */
            const char *vname = axl_pci_vendor_name(vid);
            if (vname != NULL) {
                axl_printf(" [%04X:%04X]\n", (unsigned)vid, (unsigned)did);
            } else {
                axl_printf("\n");
            }
            return;
        }
    }
    axl_printf(" [%04X:%04X]\n", (unsigned)vid, (unsigned)did);
}

static void
print_caps(
    AxlPciAddr  a
    )
{
    /* Legacy capability list — first probe terminates cleanly via the
       VID precheck if the function is absent (post-fix; see commit
       8b90954). */
    uint16_t off = 0;
    uint16_t id;
    int      walked = 0;
    while (axl_pci_cap_next(a, off, &off, &id) == AXL_OK) {
        axl_printf("\tCapabilities: [%02x] %s\n",
                   (unsigned)off, axl_pci_cap_id_str((uint8_t)id));
        if (++walked > 64) {
            axl_printf("\tCapabilities: <walk truncated at 64>\n");
            break;
        }
    }
}

static void
print_ext_caps(
    AxlPciAddr  a
    )
{
    uint16_t off = 0;
    uint16_t id;
    int      walked = 0;
    while (axl_pci_ext_cap_next(a, off, &off, &id) == AXL_OK) {
        axl_printf("\tExtended Capabilities: [%03x v?] %s\n",
                   (unsigned)off, axl_pci_ext_cap_id_str(id));
        if (++walked > 64) {
            axl_printf("\tExtended Capabilities: <walk truncated at 64>\n");
            break;
        }
    }
}

static void
print_subsystem(
    AxlPciAddr  a
    )
{
    /* Subsystem VID/DID at offsets 0x2C/0x2E for header type 0
       (endpoint). Header type 1 (PCI bridge) has subsystem in a
       capability instead, not at 0x2C — skip on bridges to avoid
       printing junk. */
    uint8_t htype;
    if (axl_pci_read_config_8(a, 0x0E, &htype) != AXL_OK) {
        return;
    }
    if ((htype & 0x7Fu) != 0) {
        return;
    }
    uint16_t svid, sdid;
    if (axl_pci_read_config_16(a, 0x2C, &svid) != 0
        || axl_pci_read_config_16(a, 0x2E, &sdid) != 0)
    {
        return;
    }
    if (svid == 0 && sdid == 0) {
        /* Not populated — many emulated devices leave these zero. */
        return;
    }
    /* Decode the OEM card name from the subsystems sidecar table
       when available (axl_pci_subsys_name); always print the
       numeric pair so the SVID:SDID is greppable. */
    const char *sname = opt_numeric ? NULL
                                    : axl_pci_subsys_name(svid, sdid);
    if (sname != NULL) {
        axl_printf("\tSubsystem: %s [%04X:%04X]\n",
                   sname, (unsigned)svid, (unsigned)sdid);
    } else {
        axl_printf("\tSubsystem: %04X:%04X\n",
                   (unsigned)svid, (unsigned)sdid);
    }
}

static void
print_hex_dump(
    AxlPciAddr  a
    )
{
    /* -x = 64 bytes (standard header), -xx = 256 bytes (full
       legacy config space), -xxx = 4 KiB (PCIe ECAM). */
    static const size_t bytes_per_level[] = { 0, 64, 256, 4096 };
    size_t bytes = bytes_per_level[opt_hex > 3 ? 3 : opt_hex];
    if (bytes == 0) {
        return;
    }
    /* axl_pci_dump caps internally at AXL_PCI_CONFIG_SPACE_MAX and
       fills with zeros past out_read on success. We tolerate a
       partial read (e.g. legacy device with no ECAM). */
    static uint8_t buf[4096];
    size_t ok = 0;
    if (axl_pci_dump(a, buf, bytes, &ok) != AXL_OK) {
        return;
    }
    /* lspci(8) -x format: "OO: BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB BB" */
    for (size_t off = 0; off < ok; off += 16) {
        axl_printf("%02x:", (unsigned)off);
        for (size_t i = 0; i < 16 && (off + i) < ok; i++) {
            axl_printf(" %02x", (unsigned)buf[off + i]);
        }
        axl_printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Per-function row
// ---------------------------------------------------------------------------

static void
list_function(
    AxlPciAddr  a,
    bool        diagnose_absent
    )
{
    uint16_t vid, did;
    if (axl_pci_get_vid_did(a, &vid, &did) != AXL_OK) {
        if (diagnose_absent) {
            print_bdf(a);
            axl_print(" no device\n");
        }
        return;
    }
    if (!match_filters(a, vid, did)) {
        return;
    }

    uint32_t class_code = 0;
    (void)axl_pci_get_class_code(a, &class_code);

    print_bdf(a);
    axl_print(" ");
    print_class(class_code);
    print_vendor_device(vid, did);

    if (opt_verbose >= 2) {
        print_subsystem(a);
    }
    if (opt_verbose >= 1) {
        print_caps(a);
    }
    if (opt_verbose >= 2) {
        print_ext_caps(a);
    }
    if (opt_hex >= 1) {
        print_hex_dump(a);
    }
}

// ---------------------------------------------------------------------------
// Tree view (SoftBMC style — every line is a real device, indented
// under its parent bridge by box-drawing connectors)
// ---------------------------------------------------------------------------

static int
tree_print_cb(
    AxlPciAddr  addr,
    unsigned    depth,
    bool        is_bridge,
    void       *ctx
    )
{
    (void)ctx; (void)is_bridge;

    uint16_t vid, did;
    if (axl_pci_get_vid_did(addr, &vid, &did) != AXL_OK) {
        return 0;
    }
    if (!match_filters(addr, vid, did)) {
        return 0;
    }

    /* Indent: two spaces per level, then a connector at the leaf
       depth (└─). SoftBMC's tree puts a single connector at the
       node row; for a UEFI text terminal we use ASCII fallbacks
       (the UEFI shell renders UCS-2 box-drawing reliably, but a
       console that pipes to a non-UCS-2 sink garbles the bytes). */
    for (unsigned i = 0; i + 1 < depth; i++) {
        axl_print("  ");
    }
    if (depth > 0) {
        axl_print("\\- ");
    }
    print_bdf(addr);
    axl_print(" ");
    /* Tree mode uses the subclass tier alone (Linux lspci convention)
       — full slash triplet blows out the right margin once a few
       layers of indent stack up. -n still wins (numeric class). */
    uint32_t class_code = 0;
    (void)axl_pci_get_class_code(addr, &class_code);
    if (opt_numeric) {
        axl_printf("%06x", (unsigned)class_code);
    } else {
        char buf[AXL_PCI_CLASS_NAME_MAX];
        if (axl_pci_class_string_fmt(class_code,
                                     AXL_PCI_CLASS_FMT_SUBCLASS,
                                     buf, sizeof(buf)) > 0) {
            axl_printf("%s", buf);
        }
    }
    print_vendor_device(vid, did);
    return 0;
}

static int
list_tree(
    void
    )
{
    return axl_pci_tree_for_each(tree_print_cb, NULL);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_lspci(
    AxlArgs  *a
    )
{
    opt_numeric     = axl_args_get_bool(a, "numeric");
    opt_show_domain = axl_args_get_bool(a, "domain");
    opt_tree        = axl_args_get_bool(a, "tree");

    /* -v / --vv / --vvv collapse to a 0..3 level. The pre-expand pass
       in main() rewrites clustered short forms (-vv, -vvv) into their
       long equivalents (--vv, --vvv) before AxlArgs sees them. */
    if (axl_args_get_bool(a, "vvv")) {
        opt_verbose = 3;
    } else if (axl_args_get_bool(a, "vv")) {
        opt_verbose = 2;
    } else if (axl_args_get_bool(a, "verbose")) {
        opt_verbose = 1;
    }
    if (axl_args_get_bool(a, "xxx")) {
        opt_hex = 3;
    } else if (axl_args_get_bool(a, "xx")) {
        opt_hex = 2;
    } else if (axl_args_get_bool(a, "hex")) {
        opt_hex = 1;
    }

    /* -t with -v/-vv/-vvv or -x[xx] would render the per-device
       extras inline with the tree connectors at the wrong indent
       level — the existing tab-prefixed formatters in print_caps /
       print_hex_dump are flat-list shaped. Reject the combo with
       a clear message rather than emitting a confusing layout. */
    if (opt_tree && (opt_verbose > 0 || opt_hex > 0)) {
        axl_printf("lspci: -t cannot be combined with -v / -x "
                   "(tree view shows the topology only; use the flat "
                   "list for per-device detail)\n");
        return 1;
    }

    /* Opportunistic vendor/device-name database load. -n suppresses
       the lookup, so don't bother loading when the user only wants
       numeric output. The class-name overlay is loaded under the
       same gate — it's optional, and missing/parse-fail is silent
       (the compiled-in tables are the bootstrap default). */
    if (!opt_numeric) {
        g_ids_loaded = (axl_pci_ids_load(
                            axl_args_get_string(a, "ids-file"))
                        == AXL_SIDECAR_OK);
        (void)axl_pci_class_load(NULL);
    }

    const char *addr_str = axl_args_get_string(a, "addr");
    if (addr_str != NULL && addr_str[0] != '\0') {
        if (axl_pci_addr_parse(addr_str, &filter_addr) != AXL_OK) {
            axl_printf("lspci: invalid -s value '%s' "
                       "(expected [SSSS:]BB:DD.F)\n", addr_str);
            return 1;
        }
        filter_addr_set = true;
    }
    const char *id_str = axl_args_get_string(a, "id");
    if (id_str != NULL && parse_id_filter(id_str) != 0) {
        axl_printf("lspci: invalid -d value '%s' "
                   "(expected VID[:DID] in hex)\n", id_str);
        return 1;
    }

    /* When -s narrows to a single function, probe it directly. This
       lets users introspect an absent BDF (returns "no device") rather
       than walking the whole bus. The tree-view flag is meaningless
       for a single function — fall through to flat list. */
    if (filter_addr_set) {
        list_function(filter_addr, true);
        return 0;
    }

    if (opt_tree) {
        int rc = list_tree();
        if (rc != 0) {
            axl_printf("lspci: tree walk failed (MCFG unavailable?)\n");
            return 1;
        }
    } else {
        AxlPciAddr  *p = NULL;
        size_t       n = 0;
        while ((p = axl_pci_next(p)) != NULL) {
            list_function(*p, false);
            if (++n > 4096) {
                axl_printf("lspci: <enumeration truncated at 4096 functions>\n");
                break;
            }
        }
    }

    /* Tip: only suggest the user grab pci-ids.json5 when names are
       wanted and the file isn't loaded. Tree view actively uses names,
       so include it as a trigger alongside -v. Stay polite — two
       lines max, suppressed when -n is set so casual users aren't
       pestered. */
    if (!opt_numeric && !g_ids_loaded
        && (opt_verbose >= 1 || opt_tree))
    {
        axl_printf("\n(pci-ids.json5 not loaded — vendor/device names unavailable.\n");
        axl_printf(" Stage axl-sdk's curated set or convert pci.ids via\n");
        axl_printf(" scripts/pci-ids-to-json5.py and place next to lspci.efi.)\n");
    }
    return 0;
}

/* Rewrite Linux-lspci-style clustered short flags into long-form
   equivalents that AxlArgs accepts. AxlArgs rejects compact short
   groups (-vv, -xxx, ...) by design; this lets users keep typing
   the muscle-memory form.

   Mapping: "-vv" → "--vv", "-vvv" → "--vvv", same for x. Anything
   else passes through. Returns a freshly allocated argv (caller
   frees the array; contents are either argv strings or static
   long-form tokens). */
static char **
expand_count_flags(
    int     argc,
    char  **argv,
    int    *out_argc
    )
{
    /* Per-cluster long-form tokens, one per (letter, length) pair we
       support. Static so the new_argv pointers stay valid until the
       program exits. */
    static char vv_buf[]  = "--vv";
    static char vvv_buf[] = "--vvv";
    static char xx_buf[]  = "--xx";
    static char xxx_buf[] = "--xxx";

    int    n = 0;

    char **new_argv = axl_malloc((size_t)(argc + 1) * sizeof(char *));
    if (new_argv == NULL) {
        /* OOM on a few-pointer alloc is implausible in UEFI, but a
           NULL-deref in the loop below is unsafe. Fall back to the
           caller's argv with no -vv/-xx expansion; main() detects the
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
        if (s[0] == '-' && (s[1] == 'v' || s[1] == 'x')
            && s[2] == s[1])
        {
            /* -vv / -vvv / -xx / -xxx (rejecting -vvvv and longer
               as user error; AxlArgs will reject the long-form
               expansion the same way for unknown flag --vvvv). */
            if (s[3] == '\0') {
                replacement = (s[1] == 'v') ? vv_buf : xx_buf;
            } else if (s[3] == s[1] && s[4] == '\0') {
                replacement = (s[1] == 'v') ? vvv_buf : xxx_buf;
            }
        }
        new_argv[n++] = (replacement != NULL) ? replacement : (char *)s;
    }
    new_argv[n] = NULL;
    *out_argc = n;
    return new_argv;
}

AXL_TOOL_MAIN(lspci)
{
    int    new_argc;
    char **new_argv = expand_count_flags(argc, argv, &new_argc);
    int rc = axl_args_run(new_argc, new_argv, &(AxlArgsNode){
        .name    = "lspci",
        .help    = "List PCI devices (UEFI lspci equivalent)",
        .flags   = flags,
        .handler = run_lspci,
    });
    /* On success new_argv is heap; its contents are either argv strings
       (caller owns) or pointers to static "-v" / "-x" buffers. Free
       only the array. On OOM, expand_count_flags returns argv unchanged
       — don't free in that case. */
    if (new_argv != argv) {
        axl_free(new_argv);
    }
    return rc;
}
