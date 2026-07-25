/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file netload.c
    Interactive NIC-driver loader + link/DHCP probe with crash-culprit NVRAM
    breadcrumb. See docs/AXL-Netload-Design.md.
**/

#include <axl.h>
#include <uefi/axl-uefi.h>   /* EFI_SIMPLE_NETWORK_PROTOCOL for the --_hmap root-cause repro seam */

AXL_LOG_DOMAIN("netload");

#if defined(__aarch64__)
#  define NETLOAD_ARCH "aa64"
#elif defined(__x86_64__)
#  define NETLOAD_ARCH "x64"
#else
#  error "unsupported arch"
#endif

#define NETLOAD_NS       "netload"
#define NETLOAD_NAME_MAX 64        /* driver basename bound */
#define NETLOAD_Q_MAX    1024      /* quarantine var bound (bytes) */
#define NETLOAD_LOG_MAX  2048      /* result-log var bound (bytes) */
#define NETLOAD_CFG_MAX  256       /* saved Config var bound (bytes) */
#define NETLOAD_NV_FLAGS (AXL_NV_PERSISTENT | AXL_NV_BOOT)

#define NETLOAD_MAX_TRIED  16      /* distinct dependencies attempted per session */
#define NETLOAD_DEPS_FILE  "netload-drivers.json5"  /* optional per-dir sidecar */

/* Vendor GUID for netload's NVRAM namespace (generated once, keep stable). */
static const AxlGuid NETLOAD_GUID =
    AXL_GUID(0x6e65746c, 0x6f61, 0x64ff, 0x9a, 0x21, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55);

/* A validated static-IP configuration (--ip/--mask/--gw/--dns). */
typedef struct {
    bool    have;
    uint8_t ip[4];
    uint8_t mask[4];
    bool    have_gw;
    uint8_t gw[4];
    uint8_t dns[2][4];
    size_t  ndns;
} NetloadStatic;

/* Every config-bearing flag, gathered and validated by netload_cfg_parse
   before any bring-up starts. Not yet threaded into bring-up (this lands
   in a later commit); this commit only parses/validates and rejects bad
   combinations early. */
typedef struct {
    NetloadStatic st;
    bool          have_sel;
    bool          sel_by_mac;
    uint8_t       sel_mac[6];
    size_t        sel_nic;
    uint32_t      dhcp_timeout;   /* seconds; 0 -> tool default 15 */
    uint32_t      retries;        /* >=1 */
    const char   *ping;           /* NULL = none */
    bool          ping_gw;
    const char   *resolve;        /* NULL = none */
    bool          want_json;
    bool          want_save;      /* --save: persist a real win to the Config NVRAM var */
} NetloadCfg;

static AxlDriverDeps g_deps_data;               /* parsed dep sidecar (empty if absent) */
static bool       g_load_deps = true;           /* cleared by --no-deps */
static char       g_tried[NETLOAD_MAX_TRIED][NETLOAD_NAME_MAX];  /* dependencies attempted this session */
static bool       g_tried_ok[NETLOAD_MAX_TRIED];                 /* ...and whether each load succeeded */
static size_t     g_ntried;

/* --out FILE: tee netload's own output to a file, and (for the --diag/--dump
   shell dumps) the redirect target the shell appends to. g_tee owns the open
   stream; g_outpath is the raw path for building "drivers >>a <path>" lines. */
static AxlStream  *g_tee = NULL;
static const char *g_outpath = NULL;

/* User-facing flags first, then the headless test/diagnostic seams last.
   The seams are marked .hidden so axl_args_run omits them from --help
   entirely (they are still parsed and accepted normally); grouping them
   last keeps the source readable too. Their .help text is dev-facing only
   now — never rendered — so it drops the old "TEST SEAM:" reader warning. */
static const AxlArgDesc flags[] = {
    { .name = "auto",  .short_name = 'a', .type = AXL_ARG_BOOL,
      .help = "Try every driver until one gets a DHCP lease" },
    { .name = "list",  .short_name = 'l', .type = AXL_ARG_BOOL,
      .help = "List discovered drivers (tags quarantined ones [crashed]) and exit" },
    { .name = "probe", .short_name = 'p', .type = AXL_ARG_STRING,
      .help = "Run the full load/link/DHCP probe on one named driver and exit" },
    { .name = "dump",  .short_name = 'u', .type = AXL_ARG_BOOL,
      .help = "Print the NVRAM quarantine + result log and exit" },
    { .name = "clear", .short_name = 'c', .type = AXL_ARG_BOOL,
      .help = "Clear all netload NVRAM state and exit" },
    { .name = "dir",   .short_name = 'D', .type = AXL_ARG_STRING,
      .help = "Override the driver directory (default: <boot-vol>:\\drivers\\<arch>)" },
    { .name = "no-deps", .short_name = 'N', .type = AXL_ARG_BOOL,
      .help = "Probe each driver standalone; do not auto-load dependency drivers" },
    { .name = "connect", .short_name = 'f', .type = AXL_ARG_BOOL,
      .help = "Connect the firmware's own NIC drivers and try DHCP; no staging" },
    { .name = "debug", .short_name = 'v', .type = AXL_ARG_BOOL, .help = "Verbose (DEBUG) logging" },
    { .name = "out",  .short_name = 'o', .type = AXL_ARG_STRING,
      .help = "Tee all output to FILE (e.g. FS0:\\netload.txt); also the redirect target for --diag/--dump" },
    { .name = "diag", .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "Diagnostic dump: network landscape + shell 'drivers' (pair with -o to save it)" },
    { .name = "dh",   .type = AXL_ARG_BOOL,
      .help = "With --diag: also run 'dh -v' (verbose handle dump) to the SCREEN only -- not saved to --out (redirecting its full output crashes some firmware)" },
    /* --- IP configuration --- */
    { .name = "ip",   .short_name = 'i', .type = AXL_ARG_STRING,
      .help = "Static IPv4, dotted-decimal, optional /N CIDR (needs --mac/--nic or --probe)" },
    { .name = "mask", .short_name = 'm', .type = AXL_ARG_STRING,
      .help = "Netmask for --ip when no /N given (default 255.255.255.0)" },
    { .name = "gw",   .short_name = 'g', .type = AXL_ARG_STRING,
      .help = "Default gateway for the static path" },
    { .name = "dns",  .short_name = 'e', .type = AXL_ARG_STRING,
      .help = "DNS server(s): S or S,S2" },
    { .name = "dhcp", .short_name = 'H', .type = AXL_ARG_BOOL,
      .help = "Force DHCP (the default; overrides a saved static config)" },
    { .name = "dhcp-timeout", .short_name = 't', .type = AXL_ARG_U32, .base = 10, .min = 1,
      .help = "DHCP wait in seconds (default 15)" },
    /* --- NIC selection --- */
    { .name = "mac", .short_name = 'M', .type = AXL_ARG_STRING,
      .help = "Target one NIC by MAC (xx:xx:xx:xx:xx:xx)" },
    { .name = "nic", .short_name = 'n', .type = AXL_ARG_U32, .base = 10,
      .help = "Target one NIC by enumeration index" },
    /* --- Verification (gates success) --- */
    { .name = "ping", .short_name = 'P', .type = AXL_ARG_STRING,
      .help = "After bring-up, ICMP a target; failure means not up" },
    { .name = "ping-gw", .short_name = 'G', .type = AXL_ARG_BOOL,
      .help = "After bring-up, ping the gateway (learned or --gw)" },
    { .name = "resolve", .short_name = 'r', .type = AXL_ARG_STRING,
      .help = "After bring-up, resolve a DNS name; failure means not up" },
    /* --- Tier 2 (present now, wired in later commits) --- */
    { .name = "save",  .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Persist the winning config to NVRAM for --apply" },
    { .name = "apply", .short_name = 'y', .type = AXL_ARG_BOOL,
      .help = "Re-apply the saved config, skipping the sweep" },
    { .name = "json",  .short_name = 'j', .type = AXL_ARG_BOOL,
      .help = "Append a machine-readable JSON result object" },
    { .name = "retries", .short_name = 'R', .type = AXL_ARG_U32, .base = 10, .min = 1,
      .help = "Retry a link-up-no-lease NIC N times (default 1)" },
    { .name = "_mark", .type = AXL_ARG_STRING, .hidden = true,
      .help = "set the crash breadcrumb to <name> and exit" },
    { .name = "_log",  .type = AXL_ARG_STRING, .hidden = true,
      .help = "append '<token> <_logname|X.efi>' to the NVRAM result log and exit" },
    { .name = "_logname", .type = AXL_ARG_STRING, .hidden = true,
      .help = "driver name paired with --_log (default X.efi)" },
    { .name = "_saveconf", .type = AXL_ARG_STRING, .hidden = true,
      .help = "append <line> to the Config NVRAM value (newline-joined) and exit" },
    { .name = "_applydry", .type = AXL_ARG_BOOL, .hidden = true,
      .help = "parse the saved Config via config_load and print its fields" },
    { .name = "_hmap", .type = AXL_ARG_BOOL, .hidden = true,
      .help = "dump SNP handle order vs IP4Config2 handle order (DHCP index diag)" },
    { .name = "_fwrow", .type = AXL_ARG_STRING, .hidden = true,
      .help = "print the firmware summary-row detail for N link-down NICs and exit" },
    { .name = "_drvresolve", .type = AXL_ARG_BOOL, .hidden = true,
      .help = "trace how each bound NIC's owning driver resolves (run after -a)" },
    {0}
};

/* netload's crash-safety state. The driver quarantine (Trying/Quarantine/Log)
   now lives in the SHARED axl-net driver-quarantine namespace, bound via
   axl_net_driver_quarantine_init: a driver netload quarantines is one
   axl_net_auto_init_opts also skips, and `netload --clear` clears the one every
   consumer of the engine reads. netload's own replay bookkeeping (the Config
   var, --save/--apply) stays in NETLOAD_NS -- that is netload's, not shared
   driver state. */
static AxlAttempt g_attempt;

static void
nv_init(void)
{
    axl_net_driver_quarantine_init(&g_attempt);
    /* Register netload's own namespace too, for the Config var below
       (axl_nvstore_set refuses an unregistered namespace). A failure
       here means every saved-config read and write is a no-op, which
       is worth saying out loud rather than looking like an empty
       config. */
    if (axl_nvstore_register_namespace(NETLOAD_NS, &NETLOAD_GUID) != AXL_OK) {
        axl_warning("netload: '%s' namespace unavailable - "
                    "saved config will not load or persist", NETLOAD_NS);
    }
}

/* Best-effort write of the saved-config @line to the Config NVRAM var.
   Refuses (with a warning) a line whose length+1 exceeds NETLOAD_CFG_MAX --
   config_load reads into a fixed NETLOAD_CFG_MAX buffer and axl_nvstore_get
   returns AXL_ERR (not a truncated read) for an over-long value, so a value
   config_load could never read back is rejected up front, exactly as
   axl_attempt_begin guards the breadcrumb. Returns true on a committed write. */
static bool
nv_set_config(const char *line)
{
    if (axl_strlen(line) + 1 > NETLOAD_CFG_MAX) {
        axl_warning("saved config too long (>= %d bytes); skipping", NETLOAD_CFG_MAX);
        return false;
    }
    if (axl_nvstore_set(NETLOAD_NS, "Config", line, axl_strlen(line) + 1,
                        NETLOAD_NV_FLAGS) != AXL_OK) {
        axl_warning("could not persist Config (read-only/full NVRAM?)");
        return false;
    }
    return true;
}

/* Is @name in the NVRAM quarantine list? Drives the `[crashed]` tag in
   `--list`, and the driver skip-logic in the interactive menu / auto mode
   (see docs/AXL-Netload-Design.md, "Modes"). */
static bool
is_quarantined(const char *name)
{
    return axl_attempt_is_quarantined(&g_attempt, name);
}

static void
log_append(const char *line)
{
    axl_attempt_log(&g_attempt, line);
}

static void
log_append_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
log_append_fmt(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    axl_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_append(line);
}

/* Heal a prior crash. Composed from the AxlAttempt primitives rather than
   built on axl_attempt_recover so netload keeps ownership of the ordering:
   the CRASH log line is written while the breadcrumb is still outstanding, so
   a box that dies again mid-recovery re-quarantines and re-logs the same
   culprit on the next boot (both dedup) instead of losing the log line to a
   breadcrumb that was already cleared. */
static int
recover_crash(void)
{
    char name[NETLOAD_NAME_MAX] = {0};
    if (!axl_attempt_pending(&g_attempt, name, sizeof name)) {
        return 0;
    }
    axl_printf("!! last run CRASHED while loading %s -- quarantining it\n", name);
    axl_attempt_quarantine(&g_attempt, name);
    log_append_fmt("CRASH %s", name);
    axl_attempt_end(&g_attempt);
    return 1;
}

/* Map a NVRAM result-log token to a rendered label + color. Returns NULL for
   an unknown token (SWEEP summary lines are handled separately by the caller). */
static const char *
log_token_label(const char *token, AxlConsoleFg *color)
{
    *color = AXL_CONSOLE_FG_DEFAULT;
    if (axl_strcmp(token, "OK") == 0)           { *color = AXL_CONSOLE_FG_GREEN;  return "LEASED"; }
    if (axl_strcmp(token, "LINK_NO_DHCP") == 0) { *color = AXL_CONSOLE_FG_YELLOW; return "LINK, no lease"; }
    if (axl_strcmp(token, "NOREACH") == 0)      { *color = AXL_CONSOLE_FG_YELLOW; return "up, no reach"; }
    if (axl_strcmp(token, "NONIC") == 0)        { *color = AXL_CONSOLE_FG_GRAY;   return "no NIC"; }
    if (axl_strcmp(token, "LOADFAIL") == 0)     { *color = AXL_CONSOLE_FG_RED;    return "load failed"; }
    if (axl_strcmp(token, "CRASH") == 0)        { *color = AXL_CONSOLE_FG_RED;    return "CRASHED"; }
    if (axl_strcmp(token, "DEP") == 0)         { *color = AXL_CONSOLE_FG_CYAN;   return "dependency"; }
    if (axl_strcmp(token, "DEPFAIL") == 0)     { *color = AXL_CONSOLE_FG_RED;    return "dependency (failed)"; }
    return NULL;
}

/* Render the '\n'-separated NVRAM result log @log as a readable findings table:
   each "TOKEN driver" line becomes a colored row, SWEEP lines become a summary
   line. This is how a sweep that CRASHED or HUNG before its on-screen table
   printed still yields a legible summary via `--dump` after the reboot. */
static void
dump_log_table(const char *log)
{
    axl_printf("=== netload findings (reconstructed from the log) ===\n");
    bool any = false;
    const char *p = log;
    while (*p != '\0') {
        const char *nl = axl_strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : axl_strlen(p);
        if (len > 0) {
            char line[128];
            size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
            axl_memcpy(line, p, n);
            line[n] = '\0';
            char *spc = axl_strchr(line, ' ');
            const char *rest = spc ? spc + 1 : "";
            if (spc != NULL) { *spc = '\0'; }
            if (axl_strcmp(line, "SWEEP") == 0) {
                axl_printf("  -- sweep: %s\n", rest);
            } else {
                AxlConsoleFg color;
                const char *label = log_token_label(line, &color);
                if (label != NULL) {
                    axl_printf("  %-24s ", rest);
                    axl_console_set_color(color);
                    axl_printf("%s", label);
                    axl_console_reset_color();
                    axl_printf("\n");
                } else {
                    axl_printf("  %s %s\n", line, rest);   /* unknown token -> raw */
                }
            }
            any = true;
        }
        p = nl ? nl + 1 : p + len;
    }
    if (!any) { axl_printf("  (no findings recorded)\n"); }
}

static bool mac_in(uint8_t macs[][6], size_t n, const uint8_t *m); /* defined below */

/* Close the --out tee. Idempotent: registered via axl_atexit AND called
   explicitly before a shell redirect so the file handle is released for the
   shell's own append. Signature matches AxlAtexitFn. */
static void
close_tee(void *data)
{
    (void)data;
    if (g_tee != NULL) {
        /* Detach FIRST so the notice below cannot recurse into the stream it
           is reporting on, then flush and check. axl_fclose drains the
           AXL-side buffer but never calls the stream's flush, and the
           firmware close under it cannot report anything
           (EFI_FILE_PROTOCOL.Close is specified to return only
           EFI_SUCCESS), so a truncated --out log would otherwise be silent.
           A NOTICE is all this site can honestly do: it is an AxlAtexitFn
           and also runs mid-session before a shell redirect, so there is no
           exit status left to set and nothing to retry -- but the operator
           is about to go READ that file, and needs to know it is short. */
        axl_stream_set_stdout_tee(NULL);
        if (axl_fflush(g_tee) != AXL_OK) {
            axl_printerr("netload: warning: --out log '%s' could not be "
                         "flushed; it may be truncated\n",
                         (g_outpath != NULL) ? g_outpath : "?");
        }
        axl_fclose(g_tee);
        g_tee = NULL;
    }
}

/* Run one shell dump ('drivers' / 'dh -v'), appending to the --out file when set
   (else to the console). Prints a labeled header on-screen so a reader can find
   each section, mirrors that header into the file, and prints a clear notice
   when no UEFI shell protocol is available (run outside a shell). The caller
   must have released the tee (close_tee) first when g_outpath is set. */
static void
run_shell_dump(const char *cmd)
{
    axl_printf("=== shell: %s ===\n", cmd);
    char line[320];
    if (g_outpath != NULL && g_outpath[0] != '\0') {
        axl_snprintf(line, sizeof line, "echo === shell: %s === >>a %s", cmd, g_outpath);
        axl_shell_execute(line);                      /* label the section in the file */
        axl_snprintf(line, sizeof line, "%s >>a %s", cmd, g_outpath);
    } else {
        axl_strlcpy(line, cmd, sizeof line);
    }
    if (axl_shell_execute(line) != AXL_OK) {
        axl_printf("  (shell command unavailable -- netload was not launched from a "
                   "UEFI shell)\n");
    }
}

/* Passive network landscape: every present interface with MAC / link / binding
   layer / bound-driver name (read-only -- no connect, no DHCP). Reuses netinfo's
   accessors, the same picture the firmware-first probe reports. */
static void
print_net_landscape(void)
{
    axl_printf("=== network interfaces ===\n");
    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    axl_net_list_interfaces_alloc(&ifs, &count);
    if (count == 0) {
        axl_printf("  (no network interfaces present)\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        AxlNetLinkStats ls = {0};
        axl_net_get_link_stats(i, &ls);
        AxlNetDriverInfo di = {0};
        bool haved = (axl_net_get_driver_info(ifs[i].mac, &di) == AXL_OK);
        char macbuf[18];
        axl_mac_format(ifs[i].mac, macbuf, sizeof macbuf);
        axl_printf("  NIC %s  link=%s  layer=%s  driver=%s\n",
                   macbuf, ls.link_up ? "UP" : "DOWN",
                   haved ? di.layer : "-",
                   (haved && di.driver[0]) ? di.driver : "-");
    }
    axl_free(ifs);
}

static int
cmd_dump(void)
{
    char buf[NETLOAD_LOG_MAX] = {0};
    size_t sz = sizeof buf;
    axl_printf("=== netload quarantine ===\n");
    if (axl_attempt_quarantine_read(&g_attempt, buf, sizeof buf)) {
        axl_printf("%s", buf);
    } else {
        axl_printf("(empty)\n");
    }
    if (axl_attempt_log_read(&g_attempt, buf, sizeof buf)) {
        dump_log_table(buf);                      /* readable reconstructed table */
        axl_printf("=== netload raw result log ===\n%s", buf);
    } else {
        axl_printf("=== netload result log ===\n(empty)\n");
    }
    axl_printf("=== netload saved config ===\n");
    sz = sizeof buf; buf[0] = '\0';
    if (axl_nvstore_get(NETLOAD_NS, "Config", buf, &sz) == AXL_OK && buf[0]) {
        buf[sizeof buf - 1] = '\0';
        axl_printf("%s\n", buf);
    } else {
        axl_printf("(none)\n");
    }
    /* Full driver landscape (the shell's own 'drivers' command) after the
       netload findings -- what actually bound on the box, for network triage.
       Release the --out tee first so the shell can append to that file. */
    close_tee(NULL);
    run_shell_dump("drivers");
    return 0;
}

/* --diag: the diagnostic dump. netload's own passive network landscape, then the
   shell's 'drivers' list (safe to redirect). With @want_dh (--dh) it also runs a
   full 'dh -v' -- but to the SCREEN only, never through --out's redirect: a
   verbose handle walk piped through the shell's file-redirect faults the shell
   on some real firmware (observed as an RSOD on a Dell PowerEdge Shell.efi;
   fine under OVMF and when 'dh -v' is typed manually). Pair with --out to save
   the landscape + 'drivers' to a file on a writable volume. */
static void
run_diag_report(bool want_dh)
{
    axl_printf("=== netload diagnostic dump ===\n");
    print_net_landscape();
    /* netload's own sections above are tee'd to --out; release the file so the
       shell can append 'drivers' to the same file. */
    close_tee(NULL);
    run_shell_dump("drivers");
    if (want_dh) {
        axl_printf("=== shell: dh -v (screen only -- not saved to --out) ===\n");
        axl_printf("  a full verbose handle walk; redirecting it via the shell crashes\n"
                   "  some firmware, so it is NOT tee'd to --out. Scroll back / screenshot,\n"
                   "  or run 'dh -v >FILE' manually if your firmware tolerates it.\n");
        if (axl_shell_execute("dh -v") != AXL_OK) {
            axl_printf("  (shell command unavailable -- netload was not launched from a "
                       "UEFI shell)\n");
        }
    }
}

static int
cmd_clear(void)
{
    axl_net_clear_driver_quarantine();          /* shared Trying + Quarantine + Log */
    axl_nvstore_delete(NETLOAD_NS, "Config");    /* netload's own, not the engine's */
    axl_printf("netload: NVRAM state cleared\n");
    return 0;
}

/* Resolve the driver directory: --dir override, else
   <boot-vol>:\drivers\<arch>, else fallback <boot-vol>:\drivers. */
static int
resolve_driver_dir(AxlArgs *a, char *out, size_t cap)
{
    const char *override = axl_args_get_string(a, "dir");
    if (override != NULL && override[0] != '\0') {
        axl_strlcpy(out, override, cap);
        return AXL_OK;
    }
    if (axl_app_boot_path("\\drivers\\" NETLOAD_ARCH, out, cap) == AXL_OK) {
        return AXL_OK;
    }
    return axl_app_boot_path("\\drivers", out, cap);
}

/* Alphabetical, EXCEPT any candidate axl_net_driver_is_ipxe recognizes
   sorts after every non-iPXE candidate (iPXE among iPXE, and everything
   else among itself, both still alphabetical -- deterministic either
   way). iPXE's LoadImage hook breaks every subsequent .efi load in the
   session (see axl_net_driver_is_ipxe's header doc), so a driver-load
   sweep that just sorts by name can hand it out first -- exactly the
   ordering that poisons the rest of the sweep. */
static int
cmp_name(const void *x, const void *y)
{
    const char *a = (const char *)x;
    const char *b = (const char *)y;
    bool a_ipxe = axl_net_driver_is_ipxe(a);
    bool b_ipxe = axl_net_driver_is_ipxe(b);
    if (a_ipxe != b_ipxe) {
        return a_ipxe ? 1 : -1;
    }
    return axl_strcmp(a, b);
}

/* Scan @dir for *.efi basenames (case-insensitive), sorted. Returns the
   count written into @names (capped at @max). */
static size_t
scan_drivers(const char *dir, char names[][NETLOAD_NAME_MAX], size_t max)
{
    AxlDir *d = axl_dir_open(dir);
    if (d == NULL) {
        return 0;
    }
    size_t n = 0;
    AxlFsEntry e;
    while (n < max && axl_dir_read(d, &e)) {
        /* ".efi" (case-insensitive); a leading dot (".", "..", or a dotfile
           literally named ".efi") is rejected first regardless of extension. */
        const char *ext = axl_path_extension(e.name);
        if (e.name[0] == '.' || ext == NULL || axl_strcasecmp(ext, "efi") != 0) {
            continue;
        }
        /* A truncated name wouldn't resolve to the real file once callers
           (auto sweep, menu) load it back by "<dir>\<name>" -- skip it
           loudly instead of silently handing out a name that doesn't exist. */
        if (axl_strlen(e.name) >= NETLOAD_NAME_MAX) {
            axl_warning("driver name too long (>= %d bytes), skipping: %s",
                        NETLOAD_NAME_MAX, e.name);
            continue;
        }
        axl_strlcpy(names[n], e.name, NETLOAD_NAME_MAX);
        n++;
    }
    axl_dir_close(d);
    axl_qsort(names, n, NETLOAD_NAME_MAX, cmp_name);
    return n;
}

static void deps_load(const char *dir);   /* defined below, before probe_driver */

/* Shared discovery step for every mode that needs a driver list: resolve
   the driver directory (--dir override or the default), then scan it.
   Prints the same "could not resolve" diagnostic every caller used to
   print individually and returns AXL_ERR; on AXL_OK, @dir/@names/@out_nd
   are populated. */
static int
discover_or_report(AxlArgs *a, char *dir, size_t dircap,
                    char names[][NETLOAD_NAME_MAX], size_t maxn, size_t *out_nd)
{
    if (resolve_driver_dir(a, dir, dircap) != AXL_OK) {
        axl_printf("netload: could not resolve a driver directory\n");
        return AXL_ERR;
    }
    deps_load(dir);                       /* optional dep sidecar (empty if absent) */
    *out_nd = scan_drivers(dir, names, maxn);
    return AXL_OK;
}

/* Gather and validate all config-bearing flags. Prints a specific error and
   returns AXL_ERR on any conflict / malformed value; AXL_OK fills @c. */
static int
netload_cfg_parse(AxlArgs *a, NetloadCfg *c)
{
    axl_memset(c, 0, sizeof *c);
    c->retries = 1;

    const char *mac = axl_args_get_string(a, "mac");
    const char *nic = axl_args_get_string(a, "nic");
    if (mac && mac[0] && nic && nic[0]) {
        axl_printf("netload: --mac and --nic are mutually exclusive\n");
        return AXL_ERR;
    }
    if (mac && mac[0]) {
        if (axl_mac_parse(mac, c->sel_mac) != AXL_OK) {
            axl_printf("netload: invalid --mac '%s' (want xx:xx:xx:xx:xx:xx)\n", mac);
            return AXL_ERR;
        }
        c->have_sel = true;
        c->sel_by_mac = true;
    } else if (nic && nic[0]) {
        /* --nic is AXL_ARG_U32 -- axl_args_run already rejected a non-numeric
           or overflowing value (unrepresentable at the type level) before this
           handler ever ran, so the parsed value here is always in range. */
        c->have_sel = true;
        c->sel_nic = (size_t)axl_args_get_uint(a, "nic");
    }

    const char *ip = axl_args_get_string(a, "ip");
    if (ip && ip[0]) {
        bool hp = false;
        axl_ipv4_parse("255.255.255.0", c->st.mask);   /* default mask */
        if (axl_ipv4_parse_cidr(ip, c->st.ip, c->st.mask, &hp) != AXL_OK) {
            axl_printf("netload: invalid --ip '%s'\n", ip);
            return AXL_ERR;
        }
        const char *mask = axl_args_get_string(a, "mask");
        if (mask && mask[0]) {
            if (axl_ipv4_parse(mask, c->st.mask) != AXL_OK) {
                axl_printf("netload: invalid --mask '%s'\n", mask);
                return AXL_ERR;
            }
        }
        const char *gw = axl_args_get_string(a, "gw");
        if (gw && gw[0]) {
            if (axl_ipv4_parse(gw, c->st.gw) != AXL_OK) {
                axl_printf("netload: invalid --gw '%s'\n", gw);
                return AXL_ERR;
            }
            c->st.have_gw = true;
        }
        c->st.have = true;
    }

    /* DNS (usable on both static and DHCP paths). */
    const char *dns = axl_args_get_string(a, "dns");
    if (dns && dns[0]) {
        char tmp[64];
        axl_strlcpy(tmp, dns, sizeof tmp);
        char *comma = axl_strchr(tmp, ',');
        if (comma) { *comma = '\0'; }
        if (axl_ipv4_parse(tmp, c->st.dns[0]) != AXL_OK) {
            axl_printf("netload: invalid --dns '%s'\n", dns);
            return AXL_ERR;
        }
        c->st.ndns = 1;
        if (comma && comma[1]) {
            if (axl_ipv4_parse(comma + 1, c->st.dns[1]) != AXL_OK) {
                axl_printf("netload: invalid secondary --dns '%s'\n", comma + 1);
                return AXL_ERR;
            }
            c->st.ndns = 2;
        }
    }

    /* --dhcp-timeout / --retries are AXL_ARG_U32 with .min = 1 -- the framework
       rejects a non-numeric value, an overflow, or an explicit 0 before this
       handler ever runs, so 0 here can only mean "flag not given" (the
       dhcp_timeout sentinel every reader of it relies on -- see the struct
       comment -- and the retries default set above). */
    const char *dt = axl_args_get_string(a, "dhcp-timeout");
    if (dt && dt[0]) {
        c->dhcp_timeout = (uint32_t)axl_args_get_uint(a, "dhcp-timeout");
    }
    const char *rt = axl_args_get_string(a, "retries");
    if (rt && rt[0]) {
        c->retries = (uint32_t)axl_args_get_uint(a, "retries");
    }

    c->ping     = axl_args_get_string(a, "ping");
    c->ping_gw  = axl_args_get_bool(a, "ping-gw");
    c->resolve  = axl_args_get_string(a, "resolve");
    c->want_json = axl_args_get_bool(a, "json");
    c->want_save = axl_args_get_bool(a, "save");

    /* Static IP needs a target NIC; forbid it under a blind -a sweep. */
    if (c->st.have && !c->have_sel && axl_args_get_bool(a, "auto")) {
        axl_printf("netload: static IP needs a target NIC: add --mac or --nic, "
                   "or use --probe\n");
        return AXL_ERR;
    }
    return AXL_OK;
}

/* Outcome of one probe_driver() call. */
typedef enum {
    PR_OK,             /* a new interface got a DHCP lease -- winner, left bound */
    PR_LINK_NO_DHCP,   /* a new interface linked up but never leased */
    PR_NO_REACH,       /* configured (static/DHCP) but --ping/--ping-gw/--resolve failed */
    PR_NO_NIC,         /* driver loaded/started but produced no new interface */
    PR_LOAD_FAIL,      /* load/start/connect did not survive to AXL_OK */
} ProbeResult;

/* One row of the end-of-sweep findings report. probe_driver() fills the
   outcome + the representative NIC it saw (if any); cmd_auto also synthesizes
   `skipped` rows for quarantined drivers it didn't probe. */
typedef struct {
    char        name[NETLOAD_NAME_MAX];
    ProbeResult result;
    bool        skipped;        /* quarantined -> not probed (result unused) */
    bool        is_firmware;    /* the firmware-first probe's row, not a staged driver */
    bool        have_nic;       /* a new interface appeared for this driver */
    uint8_t     mac[6];
    bool        link_up;
    char        layer[12];      /* binding layer, e.g. "NII3.1"/"SNP" ("-" if unknown) */
    bool        have_ip;        /* a DHCP lease was acquired */
    uint8_t     ip[4];
    bool        did_ping;       /* an explicit --ping TARGET was attempted */
    bool        ping_ok;        /* ...and it succeeded (valid only if did_ping) */
    size_t      ping_rtt_ms;    /* round-trip time on success */
    size_t      fw_down;        /* firmware row only: NICs enumerated but link-down
                                   (result==PR_NO_NIC). 0 = none present at all. */
} DriverReport;

/* Fill @macs (capacity @max) with the MACs of every interface currently
   enumerated by axl_net_list_interfaces. Returns the count written. */
static size_t
snapshot_macs(uint8_t macs[][6], size_t max)
{
    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    axl_net_list_interfaces_alloc(&ifs, &count);
    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; i++) {
        axl_memcpy(macs[i], ifs[i].mac, 6);
    }
    axl_free(ifs);
    return n;
}

/* Pure: is @m one of the @n MACs in @macs? */
static bool
mac_in(uint8_t macs[][6], size_t n, const uint8_t *m)
{
    for (size_t i = 0; i < n; i++) {
        if (axl_memcmp(macs[i], m, 6) == 0) { return true; }
    }
    return false;
}

/* Load the optional dependency sidecar <dir>\netload-drivers.json5 into
   g_deps_data via the library resolver. Absent -> empty table (standalone
   behavior, unchanged). Parse error / bad schema -> warn and treat as absent.
   Called once per resolved driver directory. The schema tag + filename are
   passed in, so netload's on-disk format is unchanged. */
static void
deps_load(const char *dir)
{
    AxlSidecarStatus rc = axl_driver_deps_load(dir, NETLOAD_DEPS_FILE, "netload",
                                               &g_deps_data);
    if (rc == AXL_SIDECAR_PARSE_ERROR) {
        axl_warning("ignoring malformed %s -- probing every driver standalone",
                    NETLOAD_DEPS_FILE);
    }
}

/* Mark an already-tried dependency's load as successful (its "tried" row was
   added before the load so a dependency cycle terminates; this flips its
   ok flag once the load actually succeeds). */
static void
dep_set_ok(const char *name)
{
    for (size_t i = 0; i < g_ntried; i++) {
        if (axl_strcmp(g_tried[i], name) == 0) {
            g_tried_ok[i] = true;
            return;
        }
    }
}

/* Pure: has dependency @name already been attempted this session? */
static bool
dep_tried(const char *name)
{
    for (size_t i = 0; i < g_ntried; i++) {
        if (axl_strcmp(g_tried[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void
dep_mark_tried(const char *name, bool ok)
{
    if (dep_tried(name) || g_ntried >= NETLOAD_MAX_TRIED) {
        return;
    }
    axl_strlcpy(g_tried[g_ntried], name, NETLOAD_NAME_MAX);
    g_tried_ok[g_ntried] = ok;
    g_ntried++;
}

/* Load + start one driver at @p path -- and, if @p path is recognized as
   an iPXE driver, disarm the boot-services watchdog it just armed. iPXE
   arms a 5-minute UEFI watchdog and never disarms it outside an OS-chain
   exit (see axl_net_driver_is_ipxe's header doc), so an unattended tool
   that starts iPXE and keeps running gets reset from under it. This is
   the single choke point for every place netload loads a driver directly
   -- a co-loaded dependency, a NIC candidate in the sweep, or a saved
   --apply config -- so the disarm lives in exactly one place instead of
   three that could drift out of sync. */
static int
driver_load_start(const char *path, AxlDriverHandle *out)
{
    int rc = axl_driver_load(path, out);
    if (rc == AXL_OK) {
        rc = axl_driver_start(*out);
    }
    if (rc == AXL_OK && axl_net_driver_is_ipxe(path)) {
        axl_watchdog_disarm();
    }
    return rc;
}

/* AxlDriverDepVisitor::enter -- decide whether to bring dependency @dep
   (required by @parent) resident. An already-attempted dependency is reused
   (loaded once); a quarantined one is skipped with a precise warning naming the
   NIC. Marking tried here (BEFORE the walk descends) is what dedups a dependency
   across the sweep; the library walk breaks cycles on its own. @ctx is the
   driver directory (unused here). */
static bool
dep_enter(const char *dep, const char *parent, void *ctx)
{
    (void)ctx;
    if (dep_tried(dep)) {
        return false;                            /* already resident/attempted */
    }
    if (is_quarantined(dep)) {
        axl_printf("  dependency %s is quarantined -- %s may not come up; "
                   "skipping it\n", dep, parent);
        dep_mark_tried(dep, false);            /* don't re-warn on every probe */
        return false;
    }
    dep_mark_tried(dep, false);
    return true;
}

/* AxlDriverDepVisitor::load -- bring dependency @dep (required by @parent)
   resident: its OWN breadcrumbed load/start step, so a hang still pins exactly
   one driver. The library walk has already brought @dep's own dependencies up
   first (post-order). A failed load is warned but non-fatal (the NIC may still
   be self-contained on this box). Dependencies are never unloaded on success --
   a winning NIC needs its dependency to stay bound, and the interface diff is
   unaffected because a dependency driver makes no NIC of its own. @ctx is the
   driver directory. */
static void
dep_load(const char *dep, const char *parent, void *ctx)
{
    const char *dir = (const char *)ctx;
    char path[300];
    axl_snprintf(path, sizeof path, "%s\\%s", dir, dep);
    axl_printf("> loading dependency %s (needed by %s) ...\n", dep, parent);
    axl_attempt_begin(&g_attempt, dep);         /* breadcrumb the DEPENDENCY */
    AxlDriverHandle h = NULL;
    int rc = driver_load_start(path, &h);
    axl_attempt_end(&g_attempt);
    if (rc != AXL_OK) {
        axl_printf("  [dependency %s load failed rc=%d -- %s may not come up]\n",
                   dep, rc, parent);
        log_append_fmt("DEPFAIL %s", dep);
        if (h) { axl_driver_unload(h); }
    } else {
        dep_set_ok(dep);                        /* flip the pre-marked row to ok */
        axl_printf("  [ok] dependency %s resident\n", dep);
        log_append_fmt("DEP %s", dep);
    }
}

/* Before probing @nic, bring its declared dependency subtree resident via the
   library's transitive walk, dependencies-first. No-op when --no-deps is set or
   @nic declares no dependencies. The subsequent NIC probe's broad
   axl_driver_connect(NULL) drives each dependency's binding onto the NIC's
   freshly-published protocol, so no separate connect is needed here. */
static void
ensure_deps(const char *dir, const char *nic)
{
    if (!g_load_deps) {
        return;
    }
    AxlDriverDepVisitor v = { dep_enter, dep_load, (void *)dir };
    axl_driver_deps_walk(&g_deps_data, nic, &v);
}

/* Copy the probeable NIC candidates from @names (count @nd) into @out, dropping
   dependency drivers (auto-loaded on demand, never picked directly). Returns the
   filtered count. @out must hold at least @nd rows. */
static size_t
filter_candidates(char names[][NETLOAD_NAME_MAX], size_t nd,
                  char out[][NETLOAD_NAME_MAX])
{
    size_t n = 0;
    for (size_t i = 0; i < nd; i++) {
        if (axl_driver_deps_is_required(&g_deps_data, names[i])) {
            continue;
        }
        axl_strlcpy(out[n], names[i], NETLOAD_NAME_MAX);
        n++;
    }
    return n;
}

/* Map --mac / --nic to a concrete list_interfaces index. AXL_ERR if the
   selector matches no present interface. */
static int
resolve_selected_index(const NetloadCfg *c, size_t *out_idx)
{
    if (!c->have_sel) { return AXL_ERR; }
    if (!c->sel_by_mac) {
        *out_idx = c->sel_nic;   /* by index: trust it; bring_up validates range */
        return AXL_OK;
    }
    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    axl_net_list_interfaces_alloc(&ifs, &count);
    int rc = AXL_ERR;
    for (size_t i = 0; i < count; i++) {
        if (axl_memcmp(ifs[i].mac, c->sel_mac, 6) == 0) { *out_idx = i; rc = AXL_OK; break; }
    }
    axl_free(ifs);
    return rc;
}

/* Configure @nic_index (static if cfg.st.have, else DHCP with the configured
   timeout, retried cfg.retries times), program DNS, then run any requested
   verification. Verification failure downgrades a configured NIC to
   PR_NO_REACH. Fills @rep's ip/have_ip on a successful configure (even on a
   later PR_NO_REACH -- the address DID come up, just wasn't reachable). Logs
   the NVRAM "NOREACH <driver>" token itself on that path; PR_OK / PR_LINK_NO_
   DHCP logging remains the caller's job (it knows the OK/LINK_NO_DHCP wording
   and whether this is a staged-driver or firmware-stack probe). */
static ProbeResult
bring_up_and_verify(size_t nic_index, const NetloadCfg *c, const char *drv_name,
                    DriverReport *rep)
{
    uint32_t timeout = c->dhcp_timeout ? c->dhcp_timeout : 15;
    const uint8_t *ip   = c->st.have ? c->st.ip   : NULL;
    const uint8_t *mask = c->st.have ? c->st.mask : NULL;
    const uint8_t *gw   = (c->st.have && c->st.have_gw) ? c->st.gw : NULL;

    AxlIPv4Address got = {0};
    int rc = AXL_ERR;
    for (uint32_t attempt = 0; attempt < c->retries && rc != AXL_OK; attempt++) {
        if (attempt > 0) {
            axl_printf("  retry %u/%u ...\n", attempt + 1, c->retries);
        }
        rc = axl_net_bring_up(nic_index, ip, mask, gw, timeout, &got);
    }
    if (rc != AXL_OK) {
        return PR_LINK_NO_DHCP;   /* configured link but no address */
    }
    if (rep != NULL) {
        rep->have_ip = true;
        axl_memcpy(rep->ip, got.addr, 4);
    }
    if (c->st.ndns > 0) {
        axl_net_set_dns(nic_index, c->st.dns[0], c->st.ndns > 1 ? c->st.dns[1] : NULL);
    }

    /* Verification: any requested check that fails => not really up. */
    bool verify_requested = (c->ping && c->ping[0]) || c->ping_gw || (c->resolve && c->resolve[0]);
    if (!verify_requested) {
        axl_printf("  [ok] address up: %u.%u.%u.%u\n",
                   got.addr[0], got.addr[1], got.addr[2], got.addr[3]);
        return PR_OK;
    }
    bool reach_ok = true;
    if (c->ping && c->ping[0]) {
        AxlIPv4Address tgt = {0};
        size_t rtt = 0;
        if (rep != NULL) { rep->did_ping = true; }
        if (axl_ipv4_parse(c->ping, tgt.addr) != AXL_OK
            || axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
            axl_printf("  ping %s FAILED -- configured but unreachable\n", c->ping);
            reach_ok = false;
        } else {
            axl_printf("  [ok] ping %s: %zu ms\n", c->ping, rtt);
            if (rep != NULL) { rep->ping_ok = true; rep->ping_rtt_ms = rtt; }
        }
    }
    if (c->ping_gw) {
        uint8_t gwip[4] = {0};
        bool have = false;
        if (gw != NULL) { axl_memcpy(gwip, gw, 4); have = true; }
        else {
            /* look up the gateway from the lease by MAC */
            AxlNetInterface *ifs = NULL;
            size_t count = 0;
            axl_net_list_interfaces_alloc(&ifs, &count);
            if (nic_index < count) {
                AxlDhcpLease lease = {0};
                if (axl_net_get_dhcp_lease_by_mac(ifs[nic_index].mac, &lease) == AXL_OK) {
                    axl_memcpy(gwip, lease.router, 4); have = true;
                }
            }
            axl_free(ifs);
        }
        if (!have) {
            axl_printf("  --ping-gw: no gateway known -- skipping\n");
        } else {
            AxlIPv4Address tgt = {0};
            size_t rtt = 0;
            axl_memcpy(tgt.addr, gwip, 4);
            if (axl_net_ping(&tgt, 2000, &rtt) != AXL_OK) {
                axl_printf("  ping gateway %u.%u.%u.%u FAILED\n",
                           gwip[0], gwip[1], gwip[2], gwip[3]);
                reach_ok = false;
            } else {
                axl_printf("  [ok] ping gateway: %zu ms\n", rtt);
            }
        }
    }
    if (c->resolve && c->resolve[0]) {
        AxlIPv4Address ra = {0};
        if (axl_net_resolve(c->resolve, &ra) != AXL_OK) {
            axl_printf("  resolve %s FAILED\n", c->resolve);
            reach_ok = false;
        } else {
            axl_printf("  [ok] resolve %s -> %u.%u.%u.%u\n", c->resolve,
                       ra.addr[0], ra.addr[1], ra.addr[2], ra.addr[3]);
        }
    }
    if (!reach_ok) {
        log_append_fmt("NOREACH %s", drv_name);
        return PR_NO_REACH;
    }
    return PR_OK;
}

/* Try the firmware's OWN network drivers before staging any of ours: a broad
   `connect -r` (which binds firmware NIC drivers that are loaded but not yet
   connected), then enumerate EVERY present interface, report it (the network
   "landscape"), and DHCP each link-up one. A lease here means networking is up
   with zero staging -- exactly the case netload's staged-driver-only sweep used
   to miss, because it only DHCPs interfaces its own drivers newly produce and
   treats firmware-provided ones as "pre-existing". Uses only public axl-net /
   axl-driver APIs. Returns PR_OK on a lease, else the best non-winning result.
   @rep (may be NULL) is filled on EVERY path -- unlike a staged probe_driver()
   row, there is no ".efi" name to start from, so name defaults to the literal
   "(firmware)" and is upgraded to the resolved bound-driver name (from
   axl_net_get_driver_info) the moment a link-up candidate is examined. This is
   how a firmware win becomes a real summary row instead of the sweep just
   returning silently. */
static ProbeResult
probe_firmware_stack(const NetloadCfg *c, DriverReport *rep)
{
    if (rep != NULL) {
        axl_memset(rep, 0, sizeof *rep);
        axl_strlcpy(rep->name, "(firmware)", NETLOAD_NAME_MAX);
        axl_strlcpy(rep->layer, "-", sizeof rep->layer);
        rep->is_firmware = true;
        rep->result = PR_NO_NIC;   /* placeholder until a link-up NIC is examined */
    }

    axl_printf("=== netload: trying the firmware's own network drivers "
               "(connect -r, no staging) ===\n");
    axl_driver_connect(NULL);   /* bind firmware NIC drivers loaded-but-unconnected */

    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    axl_net_list_interfaces_alloc(&ifs, &count);
    if (count == 0) {
        axl_printf("  no network interfaces present -- no firmware NIC driver is bound\n"
                   "  (a NIC may be present but unmanaged; stage a driver below)\n");
        return PR_NO_NIC;   /* rep already carries "(firmware)" / PR_NO_NIC */
    }

    /* Resolve --mac/--nic once against this (stable-for-the-call) interface
       list; every candidate below is filtered against the same index. */
    size_t sel_idx = 0;
    bool sel_ok = !c->have_sel || resolve_selected_index(c, &sel_idx) == AXL_OK;

    /* Best-outcome-wins representative selection. Each link-up candidate is
       configured into its OWN scratch DriverReport, so bring_up_and_verify's
       have_ip/ip/ping fields can never cross-wire with a different candidate's
       identity (the multi-NIC no-win hazard: NIC0 leases-but-unreachable, NIC1
       links-no-lease -- the row must describe ONE NIC, not a mix). The scratch
       replaces *rep wholesale the moment its outcome ranks >= the best seen,
       so mac/name/layer/ip/result always describe the same NIC. Ranks:
       PR_OK(3) > PR_NO_REACH(2) > PR_LINK_NO_DHCP(1). */
    ProbeResult best_result = PR_NO_NIC;
    int best_rank = 0;
    size_t ndown = 0;   /* firmware NICs enumerated but link-down */
    for (size_t i = 0; i < count; i++) {
        AxlNetLinkStats ls = {0};
        axl_net_get_link_stats(i, &ls);
        AxlNetDriverInfo di = {0};
        bool haved = (axl_net_get_driver_info(ifs[i].mac, &di) == AXL_OK);
        const char *drv = (haved && di.driver[0]) ? di.driver : "(firmware)";
        char macbuf[18];
        axl_mac_format(ifs[i].mac, macbuf, sizeof macbuf);
        axl_printf("  NIC %s  link=%s  layer=%s driver=%s\n",
                   macbuf, ls.link_up ? "UP" : "DOWN",
                   haved ? di.layer : "-", drv);

        if (!ls.link_up) {
            ndown++;
            continue;
        }
        if (c->have_sel && (!sel_ok || i != sel_idx)) {
            continue;   /* not the NIC the operator selected */
        }
        /* Configure/verify THIS NIC into a fresh scratch report (have_ip/ip/
           ping start clean each candidate), then finish its identity fields. */
        DriverReport cand = {0};
        cand.is_firmware = true;
        ProbeResult br = bring_up_and_verify(i, c, drv, &cand);
        cand.have_nic = true;
        axl_memcpy(cand.mac, ifs[i].mac, 6);
        cand.link_up = true;
        axl_strlcpy(cand.layer, haved ? di.layer : "-", sizeof cand.layer);
        axl_strlcpy(cand.name, drv, NETLOAD_NAME_MAX);
        cand.result = br;

        int rank = (br == PR_OK) ? 3 : (br == PR_NO_REACH) ? 2 : 1;
        if (rank >= best_rank) {
            best_rank = rank;
            best_result = br;
            if (rep != NULL) { *rep = cand; }   /* atomic: whole row = this NIC */
        }

        if (br == PR_OK) {
            axl_printf("  -- no staged driver needed (firmware driver %s)\n", drv);
            log_append_fmt("OK %s", drv);
            axl_free(ifs);
            return PR_OK;                 /* WIN: firmware brought networking up */
        }
        if (br == PR_NO_REACH) {
            continue;   /* already logged (NOREACH) inside bring_up_and_verify */
        }
        axl_printf("  link is up but no DHCP lease in %us (data plane may be stalled;\n"
                   "  try 'rndisfix' for an RNDIS NIC)\n",
                   c->dhcp_timeout ? c->dhcp_timeout : 15);
        log_append_fmt("LINK_NO_DHCP %s", drv);
    }
    /* No usable firmware NIC. If any were enumerated they were all link-down
       (or selection-filtered) -- record the count so the summary row says
       "N present, all link-down" rather than the untrue "no firmware NIC
       bound". rep was left at the preamble PR_NO_NIC placeholder. */
    if (rep != NULL && best_rank == 0) {
        rep->fw_down = ndown;
    }
    axl_free(ifs);
    return best_result;
}

/* Pure: does @name look like an RNDIS driver? rndisfix sets the RNDIS
   packet filter (an RNDIS-specific control message), so it only helps
   RNDIS NICs -- suggesting it for a Realtek/ASIX UNDI or iPXE NIC that
   links-but-won't-lease sends the operator down the wrong path. Matches a
   case-insensitive "rndis" substring in the driver basename. */
static bool
is_rndis_like(const char *name)
{
    size_t n = axl_strlen(name);
    for (size_t i = 0; i + 5 <= n; i++) {
        if ((name[i]     | 0x20) == 'r' && (name[i + 1] | 0x20) == 'n' &&
            (name[i + 2] | 0x20) == 'd' && (name[i + 3] | 0x20) == 'i' &&
            (name[i + 4] | 0x20) == 's') {
            return true;
        }
    }
    return false;
}

/* The crash-safe per-driver probe: breadcrumb -> load -> start -> connect
   -> interface diff -> per-new-NIC link + DHCP -> outcome. Verbose to the
   screen (the FAT staging volume is read-only, so there is no log file);
   the NVRAM breadcrumb/log are the crash-surviving record. On PR_OK the
   driver is deliberately left loaded and bound (the winning NIC/lease
   must survive); every other outcome unloads it before returning so the
   next probe starts from a clean slate. */
static ProbeResult
probe_driver(const char *dir, const char *name, const NetloadCfg *c, DriverReport *rep)
{
    if (rep != NULL) {
        axl_memset(rep, 0, sizeof *rep);
        axl_strlcpy(rep->name, name, NETLOAD_NAME_MAX);
        axl_strlcpy(rep->layer, "-", sizeof rep->layer);
    }

    /* Co-load any declared dependency driver(s) first (resident, breadcrumbed);
       no-op with no sidecar / no dependencies / --no-deps. Runs BEFORE the
       before-snapshot so the diff still attributes the NIC to @name. */
    ensure_deps(dir, name);

    char path[300];
    axl_snprintf(path, sizeof path, "%s\\%s", dir, name);

    /* Snapshot interfaces BEFORE, so the diff after connect is this driver's.
       netload keeps its OWN per-interface diff (not axl_net_try_driver's SNP
       delta) because bring_up_and_verify / axl_net_get_link_stats key off the
       axl_net_list_interfaces ordinal, which the SNP-handle delta does not
       carry. try_driver owns the load/connect/unload mechanics; this diff is
       netload's per-NIC policy layer on top. */
    uint8_t before[16][6];
    size_t nbefore = snapshot_macs(before, 16);

    axl_printf("> loading %s ...\n", name);
    /* A dependency-dependent USB-NIC (RNDIS/CDC) makes the connect query the
       device during bind; on a real RNDIS device that firmware step can stall
       ~30-60s before it completes. Warn so the operator waits it out instead of
       mistaking a bounded stall for a hang and power-cycling. axl_net_try_driver
       runs load+connect as one call, so this is the last point we can post the
       notice. Gated on "declares a dependency" (not on the dependency having
       actually loaded): over-warning is the safe direction here -- a wasted
       notice when connect turns out fast beats a silent stall an operator
       power-cycles through. */
    if (g_load_deps && axl_driver_deps_lookup(&g_deps_data, name) != NULL) {
        axl_printf("  connecting %s -- a USB-NIC/RNDIS device can make this\n"
                   "  step take up to ~60s; that is normal, do NOT reset the box\n",
                   name);
    }
    axl_attempt_begin(&g_attempt, name);   /* durable breadcrumb BEFORE the risky load */

    /* Load + start + connect the stack + attribute SNP handles via the library's
       selective-retry primitive: it disarms an iPXE watchdog, and unloads a
       driver that bound nothing. On success it hands back the resident handle in
       tr.driver so a non-winning driver can be dropped below. The breadcrumb
       wraps the whole call so a hang inside the load/connect still pins @name. */
    AxlNetTryResult tr;
    axl_net_try_driver(path, &tr);
    axl_attempt_end(&g_attempt);           /* survived -> breadcrumb no longer needed */
    axl_free(tr.bound_nic_macs);           /* netload re-derives NICs from its own diff */
    AxlDriverHandle drv = (AxlDriverHandle)tr.driver;   /* resident handle, NULL if unloaded */

    if (!tr.loaded) {
        axl_printf("  [load failed]\n");
        log_append_fmt("LOADFAIL %s", name);
        if (rep != NULL) { rep->result = PR_LOAD_FAIL; }
        return PR_LOAD_FAIL;   /* try_driver already unloaded on a load/start failure */
    }
    axl_printf("  [ok] loaded\n");

    /* Interface diff -> the NIC(s) this driver produced. */
    AxlNetInterface *ifs = NULL;
    size_t count = 0;
    axl_net_list_interfaces_alloc(&ifs, &count);

    /* Resolve --mac/--nic once against this (stable-for-the-call) interface
       list; every new-NIC candidate below is filtered against the same index. */
    size_t sel_idx = 0;
    bool sel_ok = !c->have_sel || resolve_selected_index(c, &sel_idx) == AXL_OK;

    ProbeResult result = PR_NO_NIC;
    for (size_t i = 0; i < count; i++) {
        if (mac_in(before, nbefore, ifs[i].mac)) { continue; }   /* pre-existing */
        if (c->have_sel && (!sel_ok || i != sel_idx)) {
            continue;   /* this driver's NIC isn't the one the operator selected */
        }

        AxlNetLinkStats ls = {0};
        axl_net_get_link_stats(i, &ls);
        AxlNetDriverInfo di = {0};
        bool haved = (axl_net_get_driver_info(ifs[i].mac, &di) == AXL_OK);
        char macbuf[18];
        axl_mac_format(ifs[i].mac, macbuf, sizeof macbuf);
        axl_printf("  NIC %s  link=%s  layer=%s driver=%s\n",
                   macbuf, ls.link_up ? "UP" : "DOWN",
                   haved ? di.layer : "-", haved ? di.driver : "-");

        /* Record the representative NIC for the report: the first new one,
           but prefer a link-up NIC over a down one. */
        if (rep != NULL && (!rep->have_nic || (ls.link_up && !rep->link_up))) {
            rep->have_nic = true;
            axl_memcpy(rep->mac, ifs[i].mac, 6);
            rep->link_up = ls.link_up;
            axl_strlcpy(rep->layer, haved ? di.layer : "-", sizeof rep->layer);
        }

        if (!ls.link_up) {
            continue;
        }
        ProbeResult br = bring_up_and_verify(i, c, name, rep);
        if (br == PR_OK) {
            log_append_fmt("OK %s", name);
            if (rep != NULL) {
                rep->result = PR_OK;
                rep->have_nic = true;
                axl_memcpy(rep->mac, ifs[i].mac, 6);
                rep->link_up = true;
                axl_strlcpy(rep->layer, haved ? di.layer : "-", sizeof rep->layer);
                /* have_ip/ip already filled by bring_up_and_verify */
            }
            axl_free(ifs);
            return PR_OK;                 /* WIN: stop; leave driver bound */
        }
        if (br == PR_NO_REACH) {
            result = PR_NO_REACH;   /* already logged (NOREACH) inside bring_up_and_verify */
            continue;
        }
        uint32_t tmo = c->dhcp_timeout ? c->dhcp_timeout : 15;
        if (is_rndis_like(name)) {
            axl_printf("  link is up but no DHCP lease in %us -- the data plane may be\n"
                       "  stalled (the EDK2 UsbRndis packet-filter bug). Try 'rndisfix',\n"
                       "  then re-probe.\n", tmo);
        } else {
            axl_printf("  link is up but no DHCP lease in %us -- no DHCP server responding\n"
                       "  on this segment? (check cabling / VLAN / that a DHCP server is\n"
                       "  reachable, or configure a static IP)\n", tmo);
        }
        result = PR_LINK_NO_DHCP;
    }
    axl_free(ifs);

    if (result == PR_NO_NIC) {
        /* Only blame a missing dependency when auto-load is OFF -- in the
           default mode any declared dependency was already co-loaded, so a
           NO_NIC means no matching hardware, not a missing dependency. */
        if (g_load_deps) {
            axl_printf("  loaded, but no NIC came up (no matching hardware)\n");
        } else {
            axl_printf("  loaded, but no NIC came up (--no-deps: a dependency was "
                       "not auto-loaded, or no matching hardware)\n");
        }
        log_append_fmt("NONIC %s", name);
    } else if (result == PR_LINK_NO_DHCP) {
        log_append_fmt("LINK_NO_DHCP %s", name);
    }   /* PR_NO_REACH already logged (NOREACH) inside bring_up_and_verify */
    if (rep != NULL) { rep->result = result; }
    if (drv) { axl_driver_unload(drv); }   /* not a winner -> free it for the next */
    return result;
}

/* Pure: short label for one ProbeResult (skipped/firmware are
   outcome_label's job, not this). */
static const char *
result_label(ProbeResult r)
{
    switch (r) {
    case PR_OK:           return "LEASED";
    case PR_LINK_NO_DHCP: return "LINK, no lease";
    case PR_NO_REACH:     return "up, no reach";
    case PR_NO_NIC:       return "no NIC";
    case PR_LOAD_FAIL:    return "load failed";
    }
    return "?";
}

/* Pure: short outcome label for a report row, written into @buf (>= 32
   bytes). A firmware row (r->is_firmware) gets a "firmware:" prefix so it
   reads distinctly from a staged-driver row at a glance (and so a log/summary
   grep can tell the two apart). */
static void
outcome_label(const DriverReport *r, char *buf, size_t cap)
{
    const char *base = r->skipped ? "skipped" : result_label(r->result);
    if (r->is_firmware) {
        axl_snprintf(buf, cap, "firmware:%s", base);
    } else {
        axl_strlcpy(buf, base, cap);
    }
}

/* Pure: console color for a report row's outcome. */
static AxlConsoleFg
outcome_color(const DriverReport *r)
{
    if (r->skipped) { return AXL_CONSOLE_FG_RED; }
    switch (r->result) {
    case PR_OK:           return AXL_CONSOLE_FG_GREEN;
    case PR_LINK_NO_DHCP:
    case PR_NO_REACH:     return AXL_CONSOLE_FG_YELLOW;
    case PR_NO_NIC:       return AXL_CONSOLE_FG_GRAY;
    case PR_LOAD_FAIL:    return AXL_CONSOLE_FG_RED;
    }
    return AXL_CONSOLE_FG_DEFAULT;
}

/* Print the detail column for a report row (MAC / lease / reason). A
   firmware row (r->is_firmware) gets wording that fits "nothing was staged"
   instead of probe_driver's load/dependency language, which does not apply
   to it: "linked, no lease" (not "up, no DHCP in 15s") and "no firmware NIC
   bound" (not "loaded, no interface ..."). */
static void
print_row_detail(const DriverReport *r)
{
    char macbuf[18] = "";
    if (r->have_nic) { axl_mac_format(r->mac, macbuf, sizeof macbuf); }
    if (r->skipped) {
        axl_printf("quarantined (crashed a prior run)");
    } else if (r->result == PR_OK && r->have_ip) {
        axl_printf("%s  leased %u.%u.%u.%u  layer=%s", macbuf,
                   r->ip[0], r->ip[1], r->ip[2], r->ip[3], r->layer);
    } else if (r->result == PR_LINK_NO_DHCP) {
        axl_printf(r->is_firmware ? "%s  linked, no lease  layer=%s"
                                  : "%s  up, no DHCP lease  layer=%s",
                   macbuf, r->layer);
    } else if (r->result == PR_NO_REACH) {
        axl_printf("%s  up, no reach  layer=%s", macbuf, r->layer);
    } else if (r->result == PR_NO_NIC) {
        if (r->is_firmware) {
            if (r->fw_down > 0) {
                /* NICs WERE bound by firmware, they just have no link (the real
                   Dell R6725 case: link-down Broadcoms alongside the link-up
                   USB NIC). "no firmware NIC bound" would be a plain lie here. */
                axl_printf("%zu firmware NIC%s present, all link-down",
                           r->fw_down, r->fw_down == 1 ? "" : "s");
            } else {
                axl_printf("no firmware NIC bound");
            }
        } else if (g_load_deps) {
            axl_printf("loaded, no interface (no matching HW)");
        } else {
            axl_printf("loaded, no interface (--no-deps: needs a dependency, or no matching HW)");
        }
    } else {
        axl_printf("load/start did not survive");
    }
}

/* Print the "dependencies (auto-loaded on demand): ..." style rows for every
   dependency driver attempted this session, listing which NICs each served. */
static void
print_dep_rows(void)
{
    for (size_t t = 0; t < g_ntried; t++) {
        axl_printf("  %-24s ", g_tried[t]);
        axl_console_set_color(AXL_CONSOLE_FG_CYAN);
        axl_printf("%-15s", "dependency");
        axl_console_reset_color();
        axl_printf(" %s", g_tried_ok[t] ? "co-loaded for" : "LOAD FAILED for");
        /* the NICs that declare this dependency */
        const char *sep = " ";
        for (size_t i = 0; i < g_deps_data.n_rows; i++) {
            for (size_t j = 0; j < g_deps_data.rows[i].n_needs; j++) {
                if (axl_strcmp(g_deps_data.rows[i].needs[j], g_tried[t]) == 0) {
                    axl_printf("%s%s", sep, g_deps_data.rows[i].name);
                    sep = ", ";
                    break;
                }
            }
        }
        axl_printf("\n");
    }
}

/* Compute + persist the bounded "SWEEP ..." bottom-line to the NVRAM log
   (auto sweeps only) -- factored out of print_summary so --json mode can
   still log the same token while suppressing the decorative tables that
   are print_summary's other (and only other) job. */
static void
persist_sweep_summary(DriverReport *reports, size_t nr)
{
    size_t link = 0, noreach = 0, nonic = 0, fail = 0, skip = 0;
    const DriverReport *winner = NULL;
    for (size_t i = 0; i < nr; i++) {
        if (reports[i].skipped) { skip++; continue; }
        /* winner considers every row (a firmware NIC win IS a win, and the
           breadcrumb should record it honestly). The firmware-first row is
           excluded only from the staged-driver counts below. */
        if (reports[i].result == PR_OK) { winner = &reports[i]; }
        if (reports[i].is_firmware) { continue; }
        switch (reports[i].result) {
        case PR_OK:           break;   /* winner already captured above */
        case PR_LINK_NO_DHCP: link++;    break;
        case PR_NO_REACH:     noreach++; break;
        case PR_NO_NIC:       nonic++;   break;
        case PR_LOAD_FAIL:    fail++;    break;
        }
    }
    log_append_fmt("SWEEP %s lease=%s link=%zu noreach=%zu nonic=%zu fail=%zu skip=%zu",
                   winner ? "up" : "no-lease", winner ? winner->name : "-",
                   link, noreach, nonic, fail, skip);
}

/* Print the end-of-run findings report: a per-driver outcome table, a count
   line, the bottom-line outcome, and actionable next-step recommendations.
   @persist appends a single bounded SWEEP line to the NVRAM log (auto sweeps
   only) so `--dump` after a reboot still shows the last result. */
static void
print_summary(const char *dir, DriverReport *reports, size_t nr, bool persist)
{
    size_t leased = 0, link = 0, noreach = 0, nonic = 0, fail = 0, skip = 0, staged = 0;
    size_t ndeps = g_ntried;   /* every attempted dependency gets a dependency row */
    const DriverReport *winner = NULL;
    bool any_linked = false;   /* any interface (incl. the firmware NIC) came up link-up */
    for (size_t i = 0; i < nr; i++) {
        if (reports[i].skipped) { staged++; skip++; continue; }   /* firmware rows are never skipped */
        /* winner + any_linked consider EVERY row, including the firmware-first
           row: a firmware NIC bringing networking up is a real win, and its
           link state must gate the "nothing linked" hint. */
        if (reports[i].result == PR_OK) { winner = &reports[i]; }
        if (reports[i].result == PR_OK || reports[i].result == PR_LINK_NO_DHCP
            || reports[i].result == PR_NO_REACH) {
            any_linked = true;
        }
        /* The firmware-first row (row 0) is SHOWN but is not a staged .efi
           driver, so it is excluded from the staged-driver counts + the
           N-drivers total + the staging-oriented next-step hints below. */
        if (reports[i].is_firmware) { continue; }
        staged++;
        switch (reports[i].result) {
        case PR_OK:           leased++;  break;
        case PR_LINK_NO_DHCP: link++;    break;
        case PR_NO_REACH:     noreach++; break;
        case PR_NO_NIC:       nonic++;   break;
        case PR_LOAD_FAIL:    fail++;    break;
        }
    }

    axl_printf("\n=== netload summary: %s  (%zu driver%s) ===\n",
               dir, nr, nr == 1 ? "" : "s");
    for (size_t i = 0; i < nr; i++) {
        char label[32];
        outcome_label(&reports[i], label, sizeof label);
        axl_printf("  %-24s ", reports[i].name);
        axl_console_set_color(outcome_color(&reports[i]));
        axl_printf("%-15s", label);
        axl_console_reset_color();
        axl_printf(" ");
        print_row_detail(&reports[i]);
        axl_printf("\n");
    }
    print_dep_rows();

    axl_printf("  %zu driver%s: %zu leased, %zu linked-no-lease, %zu unreachable, "
               "%zu no-NIC, %zu load-fail, %zu skipped",
               staged, staged == 1 ? "" : "s", leased, link, noreach, nonic, fail, skip);
    if (ndeps > 0) { axl_printf("  (+%zu deps)", ndeps); }
    axl_printf("\n");

    if (winner != NULL) {
        axl_printf("  outcome: ");
        axl_console_set_color(AXL_CONSOLE_FG_GREEN);
        axl_printf("NETWORKING IS UP via %s  (%u.%u.%u.%u)",
                   winner->name, winner->ip[0], winner->ip[1],
                   winner->ip[2], winner->ip[3]);
        axl_console_reset_color();
        axl_printf("\n");
    } else {
        axl_printf("  outcome: ");
        axl_console_set_color(AXL_CONSOLE_FG_YELLOW);
        axl_printf(noreach > 0 ? "CONFIGURED BUT UNREACHABLE" : "NO DHCP LEASE");
        axl_console_reset_color();
        axl_printf("\n");
        /* Actionable next steps, most useful first. The firmware-first row is
           skipped here: its hints ("run rndisfix, then netload --probe X")
           are staged-driver actions -- "(firmware)" is not a stageable name. */
        for (size_t i = 0; i < nr; i++) {
            if (reports[i].is_firmware) { continue; }
            if (reports[i].result == PR_LINK_NO_DHCP) {
                char macbuf[18];
                axl_mac_format(reports[i].mac, macbuf, sizeof macbuf);
                if (is_rndis_like(reports[i].name)) {
                    axl_printf("  > %s (%s) linked up but never leased -- may be the "
                               "EDK2 UsbRndis\n    packet-filter stall. run 'rndisfix', "
                               "then: netload --probe %s\n",
                               reports[i].name, macbuf, reports[i].name);
                } else {
                    axl_printf("  > %s (%s) linked up but never leased -- no DHCP server\n"
                               "    responding on this segment? (cabling / VLAN, or set a "
                               "static IP)\n", reports[i].name, macbuf);
                }
            } else if (reports[i].result == PR_NO_REACH) {
                char macbuf[18];
                axl_mac_format(reports[i].mac, macbuf, sizeof macbuf);
                axl_printf("  > %s (%s) got an address but failed the reachability check\n"
                           "    (--ping/--ping-gw/--resolve) -- check routing/firewall, or "
                           "drop the\n    verification flag(s) to accept it as up.\n",
                           reports[i].name, macbuf);
            }
        }
        if (nonic > 0) {
            if (g_load_deps) {
                axl_printf("  > %zu driver(s) produced no NIC (no matching hardware).\n",
                           nonic);
            } else {
                axl_printf("  > %zu driver(s) produced no NIC (--no-deps is set -- they "
                           "may need a dependency -- or no matching hardware).\n", nonic);
            }
        }
        if (skip > 0) {
            axl_printf("  > skipped %zu quarantined driver(s):", skip);
            const char *sep = " ";
            for (size_t i = 0; i < nr; i++) {
                if (reports[i].skipped) {
                    axl_printf("%s%s", sep, reports[i].name);
                    sep = ", ";
                }
            }
            axl_printf("\n    clear the quarantine to retry: netload --clear\n");
        }
        if (!any_linked) {
            axl_printf("  > no interface linked up; check cabling and that a "
                       "matching NIC driver is staged.\n");
        }
    }

    if (persist) {
        persist_sweep_summary(reports, nr);
    }
}

/* Pure: map a probe outcome to the --json "result" field. "up" (not "leased")
   because the static-IP path never leases anything -- "up" is accurate for
   both DHCP and static wins. PR_NO_NIC and PR_LOAD_FAIL share "none": from
   an automation caller's standpoint neither produced an interface, and the
   distinction is already in the human progress log if needed. */
static const char *
json_result_label(ProbeResult pr)
{
    switch (pr) {
    case PR_OK:           return "up";
    case PR_NO_REACH:     return "noreach";
    case PR_LINK_NO_DHCP: return "no-lease";
    case PR_NO_NIC:
    case PR_LOAD_FAIL:    return "none";
    }
    return "none";
}

/* Round-trip the JSON line print_json_result just built through AxlJsonReader
   before it ever reaches a consumer. A machine-readable interface is exactly
   where a silent escaping regression does the most damage -- a hand-rolled
   predecessor of print_json_result interpolated CLI-supplied strings (driver
   names, --ping targets) into the object with no escaping at all, so a quote
   or backslash in either produced invalid JSON no consumer could parse.
   Silent on success; the line is printed either way (a caller scripting
   $lasterror still needs SOME output), but a parse/round-trip failure here
   means the writer itself regressed. */
static void
json_result_selfcheck(const char *json, size_t len, const char *want_driver)
{
    AxlJsonReader check;
    if (!axl_json_parse(json, len, &check)) {
        axl_printf("netload: INTERNAL ERROR -- --json emitted unparsable JSON: %s\n", json);
        return;
    }
    char got[NETLOAD_NAME_MAX] = "";
    if (!axl_json_get_string(&check, "driver", got, sizeof got)
        || axl_strcmp(got, want_driver) != 0) {
        axl_printf("netload: INTERNAL ERROR -- --json 'driver' field did not round-trip\n");
    }
    axl_json_free(&check);
}

/* Append one machine-readable JSON result line for automation callers.
   @r is the representative driver report (the winner on a win, otherwise the
   most recently attempted driver); @c supplies the method/retries context
   that isn't tracked per-report. ip/mask/gw:
     - ip: the address @r actually got (r->have_ip), else "" -- never
       fabricated.
     - mask/gw: only known for the static path (c->st); DriverReport does not
       capture the DHCP lease's subnet/gateway (bring_up_and_verify never
       stores it), so the DHCP method emits "" for both rather than a value
       this object can't actually vouch for.
   "retries" reports the CONFIGURED --retries budget (retries allowed, not
   the number actually consumed on this NIC). "result" is forced to "none"
   for a skipped (quarantined) representative row regardless of its result
   enum -- a skipped row is zero-initialized, so result==0==PR_OK, and this
   guard stops any caller from ever emitting a false "up" for one.
   A "ping" sub-object is appended only when @r->did_ping (an explicit
   `--ping TARGET` ran) -- --ping-gw/--resolve are not folded in (the task
   scoped the sub-object to `--ping` only). Built via AxlJsonWriter, so driver
   names, dotted IPs, and the ping target are all properly escaped -- none of
   them are library-generated, and a driver name or --ping target containing
   a quote or backslash is real CLI-supplied input, not something this
   function can assume is already JSON-safe. */
static void
print_json_result(const DriverReport *r, const NetloadCfg *c)
{
    char ip[16] = "", mask[16] = "", gw[16] = "";
    if (r->have_ip) {
        axl_ipv4_format(r->ip, ip, sizeof ip);
    }
    if (c->st.have) {
        axl_ipv4_format(c->st.mask, mask, sizeof mask);
        if (c->st.have_gw) {
            axl_ipv4_format(c->st.gw, gw, sizeof gw);
        }
    }
    const char *result = r->skipped ? "none" : json_result_label(r->result);

    AXL_AUTOPTR(AxlString) json = axl_string_new(NULL);
    AxlJsonWriter w;
    axl_json_writer_init(&w, json, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "driver", r->name);
    axl_json_kv_str(&w, "method", c->st.have ? "static" : "dhcp");
    axl_json_kv_str(&w, "ip", ip);
    axl_json_kv_str(&w, "mask", mask);
    axl_json_kv_str(&w, "gw", gw);
    axl_json_kv_bool(&w, "link", r->link_up);
    axl_json_kv_uint(&w, "retries", c->retries);
    axl_json_kv_str(&w, "result", result);
    if (r->did_ping) {
        axl_json_key(&w, "ping");
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "target", c->ping ? c->ping : "");
        axl_json_kv_uint(&w, "rtt_ms", r->ping_rtt_ms);
        axl_json_kv_bool(&w, "ok", r->ping_ok);
        axl_json_obj_end(&w);
    }
    axl_json_obj_end(&w);
    size_t len = axl_json_writer_finish(&w);
    const char *buf = axl_string_str(json);

    json_result_selfcheck(buf, len, r->name);
    axl_printf("%s\n", buf);
}

/* Serialize a winning driver + its working config into the bounded Config
   NVRAM var as `key=value` text (driver/method/ip/mask/gw/dns1/dns2/mac;
   method "dhcp"/"static"; the static fields and dns1/dns2 are present only
   when applicable -- AxlConfigFile's own grammar, no positional format or
   comma-joining to hand-roll). Best-effort, mirroring the AxlAttempt
   writes: warns and continues -- on a write failure (read-only/
   full NVRAM) or on the serialized text not fitting NETLOAD_CFG_MAX --
   rather than failing the run that just won. */
static void
config_save(const char *driver, const NetloadCfg *c, const uint8_t nic_mac[6])
{
    AXL_AUTOPTR(AxlConfigFile) cf = axl_config_file_new();
    if (cf == NULL) {
        axl_warning("config_save: out of memory");
        return;
    }

    axl_config_file_set(cf, "driver", driver);
    axl_config_file_set(cf, "method", c->st.have ? "static" : "dhcp");
    if (c->st.have) {
        char ip[16], mask[16];
        axl_ipv4_format(c->st.ip, ip, sizeof ip);
        axl_ipv4_format(c->st.mask, mask, sizeof mask);
        axl_config_file_set(cf, "ip", ip);
        axl_config_file_set(cf, "mask", mask);
        if (c->st.have_gw) {
            char gw[16];
            axl_ipv4_format(c->st.gw, gw, sizeof gw);
            axl_config_file_set(cf, "gw", gw);
        }
    }
    if (c->st.ndns > 0) {
        char dns1[16];
        axl_ipv4_format(c->st.dns[0], dns1, sizeof dns1);
        axl_config_file_set(cf, "dns1", dns1);
        if (c->st.ndns > 1) {
            char dns2[16];
            axl_ipv4_format(c->st.dns[1], dns2, sizeof dns2);
            axl_config_file_set(cf, "dns2", dns2);
        }
    }
    char mac[18];
    axl_mac_format(nic_mac, mac, sizeof mac);
    axl_config_file_set(cf, "mac", mac);

    char line[NETLOAD_CFG_MAX];
    if (axl_config_file_to_string(cf, line, sizeof line) == AXL_OK) {
        nv_set_config(line);   /* best-effort; warns + continues on failure */
    } else {
        axl_warning("saved config too long (>= %d bytes); skipping", NETLOAD_CFG_MAX);
    }
}

/* Parse the saved Config NVRAM var (key=value text: driver/method/ip/mask/
   gw/dns1/dns2/mac) back into @driver (a NETLOAD_NAME_MAX-bounded buffer)/
   @c/@nic_mac. Populates @c->st (static fields) and a by-MAC selection so a
   subsequent bring_up_and_verify targets the right NIC. Returns false --
   leaving @driver/@c/@nic_mac unspecified -- if the var is absent or
   malformed (missing required key, bad method/address/MAC); callers must
   not act on a false return. An old positional-format Config (pre-
   AxlConfigFile) has no '=' anywhere, so it parses to an empty map and is
   rejected here exactly like an absent var -- a one-time no-op on upgrade,
   not a crash or a garbage parse. */
static bool
config_load(char *driver, size_t dcap, NetloadCfg *c, uint8_t nic_mac[6])
{
    char buf[NETLOAD_CFG_MAX];
    size_t sz = sizeof buf;
    if (axl_nvstore_get(NETLOAD_NS, "Config", buf, &sz) != AXL_OK || sz == 0) {
        return false;
    }
    buf[sizeof buf - 1] = '\0';

    AXL_AUTOPTR(AxlConfigFile) cf = axl_config_file_new();
    if (cf == NULL || axl_config_file_parse_string(cf, buf) != AXL_OK) {
        return false;
    }

    const char *f_driver = axl_config_file_get(cf, "driver", "");
    const char *f_method = axl_config_file_get(cf, "method", "");
    const char *f_ip     = axl_config_file_get(cf, "ip", "");
    const char *f_mask   = axl_config_file_get(cf, "mask", "");
    const char *f_gw     = axl_config_file_get(cf, "gw", "");
    const char *f_dns1   = axl_config_file_get(cf, "dns1", "");
    const char *f_dns2   = axl_config_file_get(cf, "dns2", "");
    const char *f_mac    = axl_config_file_get(cf, "mac", "");

    if (f_driver[0] == '\0' || f_mac[0] == '\0') {
        return false;
    }
    bool is_static;
    if (axl_strcmp(f_method, "static") == 0) {
        is_static = true;
    } else if (axl_strcmp(f_method, "dhcp") == 0) {
        is_static = false;
    } else {
        return false;
    }
    uint8_t mac[6];
    if (axl_mac_parse(f_mac, mac) != AXL_OK) {
        return false;
    }

    axl_memset(c, 0, sizeof *c);
    c->retries = 1;
    if (is_static) {
        if (f_ip[0] == '\0' || f_mask[0] == '\0'
            || axl_ipv4_parse(f_ip, c->st.ip) != AXL_OK
            || axl_ipv4_parse(f_mask, c->st.mask) != AXL_OK) {
            return false;
        }
        c->st.have = true;
        if (f_gw[0] != '\0') {
            if (axl_ipv4_parse(f_gw, c->st.gw) != AXL_OK) {
                return false;
            }
            c->st.have_gw = true;
        }
    }
    if (f_dns1[0] != '\0') {
        if (axl_ipv4_parse(f_dns1, c->st.dns[0]) != AXL_OK) {
            return false;
        }
        c->st.ndns = 1;
        if (f_dns2[0] != '\0') {
            if (axl_ipv4_parse(f_dns2, c->st.dns[1]) != AXL_OK) {
                return false;
            }
            c->st.ndns = 2;
        }
    }
    c->have_sel   = true;
    c->sel_by_mac = true;
    axl_memcpy(c->sel_mac, mac, 6);
    axl_memcpy(nic_mac, mac, 6);
    axl_strlcpy(driver, f_driver, dcap);
    return true;
}

/* Re-apply the saved Config: load its driver (breadcrumbed, like a normal
   probe) + declared dependencies, resolve the saved MAC to a live interface,
   then bring it up via the same bring_up_and_verify every other probe path
   uses. --apply restores the last known-WORKING network config, so the whole
   saved static configuration -- address, mask, gateway, AND DNS (everything
   in NetloadCfg.st, which config_load fills from the Config line) -- comes
   from the saved var, NOT from @base. Only the tuning/verification knobs that
   live OUTSIDE .st -- dhcp_timeout, retries, ping, ping_gw, resolve -- carry
   over from @base (this run's CLI flags), so e.g. `--apply --ping 8.8.8.8`
   re-applies the saved addressing but gates success on a fresh reachability
   check. Skips the whole discovery/sweep -- this is the point of --apply.
   Returns 0 on a reachable win, 1 otherwise (no saved config, load failure,
   the saved NIC isn't present, or verification/DHCP failure) -- leaving the
   driver bound only on a win, exactly like probe_driver. Callers must have
   already resolved @dir's dependency sidecar via deps_load (every caller
   either just discovered @dir via discover_or_report, or does so itself --
   see run_netload's --apply branch); re-parsing it here on every call would
   be wasted disk I/O on the -a saved-config-first path, which already
   primed it. */
static int
cmd_apply(const char *dir, const NetloadCfg *base)
{
    char driver[NETLOAD_NAME_MAX];
    NetloadCfg saved;
    uint8_t mac[6];
    if (!config_load(driver, sizeof driver, &saved, mac)) {
        axl_printf("netload: no saved config -- run with --save after a working setup\n");
        return 1;
    }

    NetloadCfg c = *base;
    c.st          = saved.st;
    c.have_sel    = true;
    c.sel_by_mac  = true;
    axl_memcpy(c.sel_mac, mac, 6);

    char macbuf[18];
    axl_mac_format(mac, macbuf, sizeof macbuf);
    axl_printf("=== netload: applying saved config -- %s on %s (%s) ===\n",
               driver, macbuf, c.st.have ? "static" : "dhcp");

    ensure_deps(dir, driver);

    char path[300];
    axl_snprintf(path, sizeof path, "%s\\%s", dir, driver);
    axl_printf("> loading %s ...\n", driver);
    axl_attempt_begin(&g_attempt, driver);
    AxlDriverHandle drv = NULL;
    int rc = driver_load_start(path, &drv);
    if (rc == AXL_OK) {
        /* Same RNDIS/CDC stall notice as probe_driver: a re-applied
           dependency-dependent USB-NIC connects through the same slow
           firmware query, and --apply is exactly the iDRAC/RNDIS
           re-bring-up path this warning exists for. */
        if (g_load_deps && axl_driver_deps_lookup(&g_deps_data, driver) != NULL) {
            axl_printf("  connecting %s -- a USB-NIC/RNDIS device can make this\n"
                       "  step take up to ~60s; that is normal, do NOT reset the box\n",
                       driver);
        }
        axl_driver_connect(NULL);
    }
    axl_attempt_end(&g_attempt);
    if (rc != AXL_OK) {
        axl_printf("  [load failed rc=%d]\n", rc);
        log_append_fmt("LOADFAIL %s", driver);
        if (drv) { axl_driver_unload(drv); }
        return 1;
    }
    axl_printf("  [ok] loaded\n");

    size_t idx;
    if (resolve_selected_index(&c, &idx) != AXL_OK) {
        axl_printf("  saved NIC %s not present -- cannot apply\n", macbuf);
        axl_driver_unload(drv);
        return 1;
    }
    ProbeResult pr = bring_up_and_verify(idx, &c, driver, NULL);
    if (pr == PR_OK) {
        axl_printf("  [ok] saved config applied\n");
        log_append_fmt("OK %s", driver);
        return 0;                 /* leave the driver bound, like a probe win */
    }
    if (pr == PR_LINK_NO_DHCP) {
        axl_printf("  saved config: link up but no DHCP lease\n");
        log_append_fmt("LINK_NO_DHCP %s", driver);
    } else {
        axl_printf("  saved config: configured but unreachable\n");
    }   /* PR_NO_REACH already logged (NOREACH) inside bring_up_and_verify */
    axl_driver_unload(drv);
    return 1;
}

/* Sweep every discovered driver, skipping quarantined ones, stopping (and
   leaving the driver bound) the moment one wins a DHCP lease. Records every
   driver's outcome and prints an end-of-sweep findings report. Returns 0 on
   a win, 1 if the whole sweep finishes without one. */
/* Map an engine per-driver outcome to netload's ProbeResult. */
static ProbeResult
engine_outcome_to_pr(AxlNetDriverOutcome o)
{
    switch (o) {
        case AXL_NET_DRV_EV_UP:            return PR_OK;
        case AXL_NET_DRV_EV_LINK_NO_LEASE: return PR_LINK_NO_DHCP;
        case AXL_NET_DRV_EV_NO_REACH:      return PR_NO_REACH;
        case AXL_NET_DRV_EV_LOAD_FAIL:     return PR_LOAD_FAIL;
        case AXL_NET_DRV_EV_NO_NIC:
        case AXL_NET_DRV_EV_SKIPPED_QUAR:
        case AXL_NET_DRV_EV_TRYING:         break;
    }
    return PR_NO_NIC;
}

/* cmd_auto's engine callback: reproduce the staged-sweep progress output and
   fill the findings reports[] the summary/--json renderers consume, so netload's
   -a rides axl_net_auto_init_opts while keeping its own UI. */
typedef struct {
    DriverReport *reports;
    size_t       *nr;
    size_t        cap;
} AutoCbCtx;

static void
auto_sweep_cb(const AxlNetDriverEvent *ev, void *ctx)
{
    AutoCbCtx *c = (AutoCbCtx *)ctx;

    if (ev->is_dependency) {
        /* A co-loaded dependency (not a candidate row): print + track it so the
           summary's [dep] rows still render. */
        if (ev->outcome == AXL_NET_DRV_EV_UP) {
            axl_printf("> loading dependency %s ...\n", ev->driver);
        } else {
            axl_printf("  [dependency %s load failed]\n", ev->driver);
        }
        dep_mark_tried(ev->driver, ev->outcome == AXL_NET_DRV_EV_UP);
        return;
    }

    /* TRYING fires BEFORE the load/connect: print progress and, for a
       dependency-declaring (USB-RNDIS-ish) driver, the connect-stall warning so
       an operator doesn't power-cycle a bounded ~60s connect. The result event
       that follows fills the report row. */
    if (ev->outcome == AXL_NET_DRV_EV_TRYING) {
        axl_printf("> loading %s ...\n", ev->driver);
        if (g_load_deps && axl_driver_deps_lookup(&g_deps_data, ev->driver) != NULL) {
            axl_printf("  connecting %s -- a USB-NIC/RNDIS device can make this\n"
                       "  step take up to ~60s; that is normal, do NOT reset the box\n",
                       ev->driver);
        }
        return;
    }

    if (*c->nr >= c->cap) {
        return;
    }
    DriverReport *r = &c->reports[*c->nr];
    axl_memset(r, 0, sizeof *r);
    axl_strlcpy(r->name, ev->driver, NETLOAD_NAME_MAX);
    axl_strlcpy(r->layer, "-", sizeof r->layer);

    if (ev->outcome == AXL_NET_DRV_EV_SKIPPED_QUAR) {
        axl_printf("-- skipping %s [crashed a prior run]\n", ev->driver);
        r->skipped = true;
        (*c->nr)++;
        return;
    }

    r->result = engine_outcome_to_pr(ev->outcome);
    if (ev->have_nic) {
        r->have_nic = true;
        axl_memcpy(r->mac, ev->mac, 6);
        r->link_up = ev->link_up;
        AxlNetDriverInfo di = { 0 };
        if (axl_net_get_driver_info(ev->mac, &di) == AXL_OK && di.layer[0] != '\0') {
            axl_strlcpy(r->layer, di.layer, sizeof r->layer);
        }
    }
    if (ev->have_ip) {
        r->have_ip = true;
        axl_memcpy(r->ip, ev->ipv4, 4);
    }
    (*c->nr)++;
}

static int
cmd_auto(const char *dir, char names[][NETLOAD_NAME_MAX], size_t nd, const NetloadCfg *cfg)
{
    (void)names;   /* the engine enumerates the sweep dir itself; nd is just the count */
    /* 65 = the firmware-first row (index 0) + up to 64 staged candidates
       (the discover/filter path caps candidates at 64). Sized so a full
       64-candidate sweep drops nothing -- reports[64]/nr<64 would have
       silently skipped the 64th once row 0 took a slot. */
    DriverReport reports[65];
    size_t nr = 1;   /* reports[0] is always the firmware-first probe's row */
    axl_printf("=== netload auto sweep: %zu driver(s) in %s ===\n", nd, dir);
    /* Firmware-first: maybe the firmware's own NIC drivers already bring up
       networking (or just needed connecting) -- try that before staging ours,
       so a working firmware NIC isn't missed by the staged-only diff. Always
       becomes reports[0] -- even on a win -- so the summary (and --json)
       output below always shows whether the built-in path was tried and what
       it found, instead of returning silently and leaving the operator to
       wonder. */
    ProbeResult fw = probe_firmware_stack(cfg, &reports[0]);
    if (fw == PR_OK) {
        axl_printf("=== netload: networking came up via a firmware driver -- "
                   "no staged driver needed ===\n");
        if (cfg->want_json) {
            persist_sweep_summary(reports, nr);
            print_json_result(&reports[0], cfg);
        } else {
            print_summary(dir, reports, nr, /*persist=*/true);
        }
        return 0;
    }
    /* Saved-config-first, best-effort: a prior --save run may already know a
       working driver+config for this box -- try it before the full staged
       sweep. Peek via config_load itself (cheap) so a box with nothing saved
       (the common case) stays silent and falls straight to the sweep.
       cmd_apply() below re-reads Config a second time; that double read is
       intentional (an NVRAM peek, not a defect) -- it keeps cmd_apply a
       self-contained entry point callable on its own (see --apply). */
    {
        char sdriver[NETLOAD_NAME_MAX];
        NetloadCfg saved;
        uint8_t smac[6];
        if (config_load(sdriver, sizeof sdriver, &saved, smac)) {
            axl_printf("=== netload: trying the saved config before the staged sweep ===\n");
            if (cmd_apply(dir, cfg) == 0) {
                return 0;
            }
            axl_printf("=== netload: saved config did not come up -- "
                       "falling back to the staged sweep ===\n");
        }
    }
    /* Staged sweep -- ride the shared engine (firmware-first already ran above,
       so skip_firmware_first). The callback prints the per-driver progress and
       fills reports[1..] for the renderers below. */
    AutoCbCtx cbc = { .reports = reports, .nr = &nr, .cap = 65 };
    AxlNetAutoOpts opts = {
        .driver_strategy     = AXL_NET_DRV_SWEEP_DIR,
        .skip_firmware_first = true,
        .sweep_dir           = dir,
        .load_deps           = g_load_deps,
        .dhcp_timeout_sec    = cfg->dhcp_timeout,
        .dhcp_retries        = cfg->retries,
        .on_driver           = auto_sweep_cb,
        .on_driver_ctx       = &cbc,
    };
    if (cfg->have_sel && cfg->sel_by_mac) {
        opts.nic_select = AXL_NET_NIC_SEL_MAC;
        axl_memcpy(opts.nic_mac, cfg->sel_mac, 6);
    } else if (cfg->have_sel) {
        opts.nic_select = AXL_NET_NIC_SEL_INDEX;
        opts.nic_index  = cfg->sel_nic;
    }
    if (cfg->st.have) {
        opts.ip_mode     = AXL_NET_IP_STATIC;
        opts.static_ipv4 = cfg->st.ip;
        opts.static_mask = cfg->st.mask;
        opts.static_gw   = cfg->st.have_gw ? cfg->st.gw : NULL;
        opts.dns1        = cfg->st.ndns > 0 ? cfg->st.dns[0] : NULL;
        opts.dns2        = cfg->st.ndns > 1 ? cfg->st.dns[1] : NULL;
    }
    uint8_t ping_ip[4];
    if ((cfg->ping && cfg->ping[0]) || cfg->ping_gw || (cfg->resolve && cfg->resolve[0])) {
        opts.verify = AXL_NET_VERIFY_REACHABLE;
        if (cfg->ping && cfg->ping[0] && axl_ipv4_parse(cfg->ping, ping_ip) == AXL_OK) {
            opts.ping_ipv4 = ping_ip;
        }
        opts.ping_gateway = cfg->ping_gw;
        opts.resolve_host = (cfg->resolve && cfg->resolve[0]) ? cfg->resolve : NULL;
    }

    AxlNetBringUpResult res;
    if (axl_net_auto_init_opts(&opts, &res) == AXL_OK && res.online) {
        axl_printf("=== netload: %s brought networking UP ===\n", res.via);
        if (cfg->want_save) {
            config_save(res.via, cfg, res.mac);
        }
        if (cfg->want_json) {
            persist_sweep_summary(reports, nr);
            print_json_result(&reports[nr - 1], cfg);
        } else {
            print_summary(dir, reports, nr, /*persist=*/true);
        }
        return 0;                         /* WIN */
    }
    axl_printf("=== netload: no driver acquired a DHCP lease ===\n");
    if (cfg->want_json) {
        persist_sweep_summary(reports, nr);
        /* Representative = the last *attempted* driver, skipping trailing
           quarantined (skipped) rows -- a skipped row has result==0==PR_OK
           and would otherwise emit a false "result":"up". reports[0] (the
           firmware probe) is never skipped, so this always finds a row --
           even a sweep with zero/all-quarantined staged drivers still has
           the firmware attempt to fall back to -- keeping -a --json
           deterministic for scripters. */
        size_t last = nr;
        while (last > 0 && reports[last - 1].skipped) { last--; }
        print_json_result(&reports[last - 1], cfg);
    } else {
        print_summary(dir, reports, nr, /*persist=*/true);
    }
    return 1;
}


/* TEST SEAM (root-cause repro): dump the SNP handle order (what a NIC index
   from axl_net_list_interfaces means) next to the IP4Config2 handle order (what
   axl_net_auto_init/set_static_ip index with that same nic_index). If the two
   MAC sequences differ, indexing IP4Config2 by the SNP-derived nic_index applies
   DHCP/static to the WRONG NIC -- the real-HW "link up, no lease" bug. */
static void
dump_hmap(void)
{
    void  **h = NULL;
    size_t  c = 0;
    axl_printf("=== simple-network handles (nic_index order) ===\n");
    if (axl_protocol_enumerate("simple-network", &h, &c) == AXL_OK) {
        for (size_t i = 0; i < c; i++) {
            EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
            axl_handle_get_protocol(h[i], "simple-network", (void **)&snp);
            char m[18] = "??";
            if (snp != NULL && snp->Mode != NULL) {
                axl_mac_format((const uint8_t *)&snp->Mode->CurrentAddress, m, sizeof m);
            }
            axl_printf("  snp[%zu]    mac=%s\n", i, m);
        }
        axl_free(h);
    }
    h = NULL; c = 0;
    axl_printf("=== ip4-config2 handles (auto_init/set_static_ip idx order) ===\n");
    if (axl_protocol_enumerate("ip4-config2", &h, &c) == AXL_OK) {
        for (size_t i = 0; i < c; i++) {
            EFI_SIMPLE_NETWORK_PROTOCOL *snp = NULL;
            axl_handle_get_protocol(h[i], "simple-network", (void **)&snp);
            char m[24] = "(no SNP on this handle)";
            if (snp != NULL && snp->Mode != NULL) {
                axl_mac_format((const uint8_t *)&snp->Mode->CurrentAddress, m, sizeof m);
            }
            axl_printf("  ip4cfg[%zu] mac=%s\n", i, m);
        }
        axl_free(h);
    }
}

static int
run_netload(AxlArgs *a)
{
    if (axl_args_get_bool(a, "_hmap")) {   /* root-cause repro seam */
        dump_hmap();
        return 0;
    }
    if (axl_args_get_bool(a, "debug")) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }
    if (axl_args_get_bool(a, "no-deps")) {
        g_load_deps = false;   /* probe every driver standalone (no dependency co-load) */
    }
    nv_init();

    NetloadCfg cfg;
    if (netload_cfg_parse(a, &cfg) != AXL_OK) {
        return 1;
    }

    const char *mark = axl_args_get_string(a, "_mark");
    if (mark != NULL && mark[0] != '\0') {   /* test seam: simulate a crash */
        axl_attempt_begin(&g_attempt, mark);
        axl_printf("netload: breadcrumb set to %s\n", mark);
        return 0;
    }
    const char *lg = axl_args_get_string(a, "_log");
    if (lg != NULL && lg[0] != '\0') {           /* test seam: seed one log line */
        const char *nm = axl_args_get_string(a, "_logname");
        char line[128];
        axl_snprintf(line, sizeof line, "%s %s", lg, nm && nm[0] ? nm : "X.efi");
        log_append(line);
        return 0;
    }
    const char *sc = axl_args_get_string(a, "_saveconf");
    if (sc != NULL && sc[0] != '\0') {
        /* test seam: append one line to the Config NVRAM var (newline-joined;
           --clear resets first). The UEFI shell has no way to embed a literal
           newline in a single command-line argument, so a multi-line
           key=value Config is built up with one --_saveconf call per line
           rather than one call carrying the whole blob. */
        char cfgbuf[NETLOAD_CFG_MAX] = "";
        size_t cfgsz = sizeof cfgbuf;
        axl_nvstore_get(NETLOAD_NS, "Config", cfgbuf, &cfgsz);   /* empty if absent */
        cfgbuf[sizeof cfgbuf - 1] = '\0';
        size_t cfglen = axl_strlen(cfgbuf);
        if (cfglen > 0 && cfglen + 1 < sizeof cfgbuf) {
            cfgbuf[cfglen++] = '\n';
            cfgbuf[cfglen] = '\0';
        }
        axl_strlcat(cfgbuf, sc, sizeof cfgbuf);
        nv_set_config(cfgbuf);
        axl_printf("netload: Config appended: %s\n", sc);
        return 0;
    }
    if (axl_args_get_bool(a, "_drvresolve")) {
        /* test/diagnostic seam: for every SNP handle currently bound, print how
           axl_net_get_driver_info resolved its owning driver, with the library's
           DEBUG resolution trace (which layer/source, and WHY it fell to a
           placeholder) surfaced. Run AFTER `netload -a` so the winner's driver
           is resident and its NIC present. This is how a real-hardware
           "driver=<firmware volume>" is diagnosed without a debugger. */
        axl_log_set_level(AXL_LOG_DEBUG);
        axl_driver_connect(NULL);   /* ensure firmware NICs are bound */
        AxlNetInterface *ifs = NULL;
        size_t count = 0;
        axl_net_list_interfaces_alloc(&ifs, &count);
        axl_printf("=== driver resolution for %zu NIC(s) ===\n", count);
        for (size_t i = 0; i < count; i++) {
            char macbuf[18];
            axl_mac_format(ifs[i].mac, macbuf, sizeof macbuf);
            AxlNetDriverInfo di = {0};
            bool ok = (axl_net_get_driver_info(ifs[i].mac, &di) == AXL_OK);
            axl_printf("NIC %s -> layer=%s driver=%s\n", macbuf,
                       ok ? di.layer : "-", ok ? di.driver : "-");
        }
        axl_free(ifs);
        return 0;
    }
    const char *fwr = axl_args_get_string(a, "_fwrow");
    if (fwr != NULL && fwr[0] != '\0') {
        /* test seam: exercise print_row_detail's firmware-row wording without
           needing link-down hardware (QEMU's OVMF reports every NIC link-up, so
           the "firmware NICs present but all link-down" path is otherwise
           real-HW-only). fwr = the number of enumerated-but-down firmware NICs. */
        uint32_t n = 0;
        axl_str_to_u32(fwr, 10, &n, NULL);
        DriverReport r = {0};
        r.is_firmware = true;
        r.result = PR_NO_NIC;
        r.fw_down = n;
        print_row_detail(&r);
        axl_printf("\n");
        return 0;
    }
    if (axl_args_get_bool(a, "_applydry")) {   /* test seam: exercise config_load alone */
        char driver[NETLOAD_NAME_MAX];
        NetloadCfg c;
        uint8_t mac[6];
        if (!config_load(driver, sizeof driver, &c, mac)) {
            axl_printf("applydry: malformed\n");
            return 0;
        }
        char macbuf[18];
        axl_mac_format(mac, macbuf, sizeof macbuf);
        char ip[16] = "-", mask[16] = "-", gw[16] = "-";
        if (c.st.have) {
            axl_ipv4_format(c.st.ip, ip, sizeof ip);
            axl_ipv4_format(c.st.mask, mask, sizeof mask);
            if (c.st.have_gw) { axl_ipv4_format(c.st.gw, gw, sizeof gw); }
        }
        axl_printf("applydry: driver=%s method=%s ip=%s mask=%s gw=%s dns=%zu mac=%s\n",
                   driver, c.st.have ? "static" : "dhcp", ip, mask, gw, c.st.ndns, macbuf);
        return 0;
    }
    if (axl_args_get_bool(a, "clear")) {
        return cmd_clear();
    }

    recover_crash();   /* every real run first heals a prior crash */

    /* --out FILE: tee all of netload's output to a file on a writable volume
       (the bounce-console operator reads the file instead of the scrolling
       screen). Best-effort: a read-only/bad path warns and continues screen
       only. axl_atexit closes it on every exit path; --diag/--dump close it
       early so the shell can append their 'drivers'/'dh -v' dumps. */
    g_outpath = axl_args_get_string(a, "out");
    if (g_outpath != NULL && g_outpath[0] != '\0') {
        g_tee = axl_fopen(g_outpath, "w");
        if (g_tee == NULL) {
            axl_warning("could not open --out '%s' (read-only volume?); screen only",
                        g_outpath);
            g_outpath = NULL;
        } else {
            axl_stream_set_stdout_tee(g_tee);
            axl_atexit(close_tee, NULL);
        }
    }

    /* --diag is a *report* that runs AFTER the action (sweep / probe / connect),
       not a mode that preempts it: `netload -a -d` runs the DHCP sweep and THEN
       dumps diagnostics (so 'drivers' reflects what the sweep just bound). With
       no action flag it runs standalone (handled below, before interactive). */
    bool want_diag = axl_args_get_bool(a, "diag");
    bool want_dh   = axl_args_get_bool(a, "dh");

    if (axl_args_get_bool(a, "apply")) {
        char dir[256];
        if (resolve_driver_dir(a, dir, sizeof dir) != AXL_OK) {
            axl_printf("netload: could not resolve a driver directory\n");
            return 1;
        }
        deps_load(dir);        /* so an explicit --apply of a dependency-dependent NIC co-loads */
        int rc = cmd_apply(dir, &cfg);
        if (want_diag) { run_diag_report(want_dh); }
        return rc;
    }

    if (axl_args_get_bool(a, "list")) {
        char dir[256];
        char names[64][NETLOAD_NAME_MAX];
        size_t nd;
        if (discover_or_report(a, dir, sizeof dir, names, 64, &nd) != AXL_OK) {
            return 1;
        }
        axl_printf("=== drivers in %s ===\n", dir);
        for (size_t i = 0; i < nd; i++) {
            axl_printf("  %2zu) %s%s%s\n", i + 1, names[i],
                       axl_driver_deps_is_required(&g_deps_data, names[i]) ? "  [dep]" : "",
                       is_quarantined(names[i]) ? "  [crashed]" : "");
        }
        if (nd == 0) { axl_printf("  (none)\n"); }
        return 0;
    }

    if (axl_args_get_bool(a, "auto")) {
        char dir[256];
        char names[64][NETLOAD_NAME_MAX];
        size_t nd;
        if (discover_or_report(a, dir, sizeof dir, names, 64, &nd) != AXL_OK) {
            return 1;
        }
        char cand[64][NETLOAD_NAME_MAX];   /* NIC candidates only (dependency filtered) */
        size_t nc = filter_candidates(names, nd, cand);
        int rc = cmd_auto(dir, cand, nc, &cfg);
        if (want_diag) { run_diag_report(want_dh); }
        return rc;
    }

    if (axl_args_get_bool(a, "connect")) {
        ProbeResult r = probe_firmware_stack(&cfg, NULL);
        axl_printf(r == PR_OK
                   ? "=== netload: networking is UP via a firmware driver ===\n"
                   : "=== netload: the firmware's drivers did not bring up networking ===\n");
        if (want_diag) { run_diag_report(want_dh); }
        return r == PR_OK ? 0 : 1;
    }

    if (axl_args_get_bool(a, "dump")) {
        return cmd_dump();
    }

    const char *one = axl_args_get_string(a, "probe");
    if (one != NULL && one[0] != '\0') {
        char dir[256];
        if (resolve_driver_dir(a, dir, sizeof dir) != AXL_OK) {
            axl_printf("netload: could not resolve a driver directory\n");
            return 1;
        }
        deps_load(dir);        /* so an explicit --probe of a dependency-dependent NIC co-loads */
        /* single driver -> the live output is the report; non-PR_OK -> non-zero exit
           (a caller scripting --probe needs to see "not really up" via $lasterror). */
        DriverReport rep = {0};
        ProbeResult pr = probe_driver(dir, one, &cfg, &rep);
        if (pr == PR_OK && cfg.want_save) {
            config_save(one, &cfg, rep.mac);
        }
        if (cfg.want_json) {
            print_json_result(&rep, &cfg);
        }
        if (want_diag) { run_diag_report(want_dh); }
        return pr == PR_OK ? 0 : 1;
    }

    /* Standalone `--diag` (no action flag above matched): just the report. */
    if (want_diag) {
        run_diag_report(want_dh);
        return 0;
    }

    char dir[256];
    char names[64][NETLOAD_NAME_MAX];
    size_t nd;
    if (discover_or_report(a, dir, sizeof dir, names, 64, &nd) != AXL_OK) {
        return 1;
    }
    char cand[64][NETLOAD_NAME_MAX];   /* NIC candidates only (dependency filtered) */
    size_t nc = filter_candidates(names, nd, cand);

    /* No action flag: run the auto sweep. netload is used as `netload -a`; the
       former interactive driver-picker menu was removed (unused, and its live
       read-loop was untested). */
    return cmd_auto(dir, cand, nc, &cfg);
}

AXL_TOOL_MAIN(netload)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name    = "netload",
        .help    = "Load a staged NIC driver, check link, and try DHCP; "
                   "records a crash-surviving NVRAM breadcrumb to find a driver "
                   "that hangs the box.",
        .flags   = flags,
        .handler = run_netload,
    });
}
