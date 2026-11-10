/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd.c - serve the live UEFI Shell over SSH: the launcher.
 *
 * `sshd` resolves its options -- sshd's defaults, then sshd.conf beside
 * sshd.efi (or `-f`), then the command line's flags -- refuses what cannot
 * work (a bad settings file, no way to log in, another console take-over),
 * and deploys the embedded sshd-dxe.efi as a resident driver with the
 * options in its LoadOptions, then returns to the Shell. `sshd -T` prints
 * the options it would deploy, in OpenSSH's `sshd -T` format, and stops. The driver (sshd-svc.c) takes over the console and serves it.
 * `sshd stop` unloads it; `sshd status` says whether it is running.
 *
 * The 9p tool's shape (9p.c + 9p-cmd-serve.c + 9p-serve-svc.c), with one
 * resident service and the start as the root node's default handler.
 */

#include <axl.h>

#include "sshd-bind.h"
#include "sshd-conf.h"
#include "sshd-sftp-iface.h"
#include "sshd-svc.h"

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/** Room for a resolved `fsN:\...` path. */
#define SSHD_PATH_MAX   512

/** Room for the line saying where the settings came from. */
#define SSHD_NOTE_MAX   (SSHD_PATH_MAX + 48)

/** Room for SSHD_EXEMPT_MAX comma-joined ranges ("255.255.255.255/32"). */
#define SSHD_EXEMPT_CAP (SSHD_EXEMPT_MAX * 20)

/** What `sshd status` and `sshd hostkey` say when the running server
    publishes no host-key interface: an older sshd.efi started it, or its
    install failed -- they cannot tell which (final review F12). */
#define SSHD_NO_HOSTKEY_IFACE  "sshd: the running server does not report its host key\n"

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static AxlServiceDeploy sshd_deploy(void);
static bool is_file(const char *path);
static const char *base_name(const char *path);
static void conf_warn(size_t line, const char *text, void *ud);
static void dump_line(const char *line, void *ud);
static int load_conf(AxlArgs *a, SshdOpts *o, AxlConfigFile *cf,
                     SshdConfOrigin *origin, const char **name, char *note,
                     size_t note_cap);
static int resolve_keys(SshdOpts *o, size_t line, const char *name, bool flag);
static int resolve_banner(SshdOpts *o, size_t line, const char *name);
static void resolve_sftp(SshdOpts *o);
static int join_exempt(AxlArgs *a, char *out, size_t cap);
static int resolve_opts(AxlArgs *a, SshdOpts *o, AxlConfigFile *cf, char *note,
                        size_t note_cap);
static int verb_start(AxlArgs *a);
static int verb_stop(AxlArgs *a);
static int verb_status(AxlArgs *a);
static int verb_log(AxlArgs *a);
static int verb_hostkey(AxlArgs *a);
static int resolve_host_keys(AxlArgs *a, SshdOpts *o);
static int host_key_precheck(const SshdOpts *o);
static bool host_key_running(SshdHostKeyIface *out);
static bool sshd_orphaned(const AxlServiceDeploy *d);
static char *log_text(size_t *len, uint64_t *gen);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/* Embedded sshd-dxe.efi, spliced in by the build's axl_embed_blob(sshd_dxe). */
AXL_EMBED_DECLARE(sshd_dxe);

static const AxlArgDesc sshd_flags[] = {
    { .name = "port", .short_name = 'p', .type = AXL_ARG_U64,
      .min = 1, .max = 65535,
      .help = "TCP port (1-65535; default " SSHD_PORT_DEFAULT_STR ")" },
    { .name = "nic", .short_name = 'n', .type = AXL_ARG_U64,
      .help = "NIC ordinal to bring up (default: first usable)" },
    { .name = "keys", .short_name = 'k', .type = AXL_ARG_STRING,
      .help = "authorized_keys file (ssh-ed25519 lines); turns "
              "PubkeyAuthentication on. A path with no volume is beside "
              "sshd.efi (\\ starts at its volume's root); -k \"\" means no "
              "file. Default: authorized_keys beside sshd.efi, if present" },
    { .name = "password", .short_name = 'P', .type = AXL_ARG_STRING,
      .help = "Also accept this password, for any user name; turns "
              "PasswordAuthentication on. -P \"\" means no password, "
              "clearing the settings file's AxlPassword. A demo "
              "convenience: it is visible on the command line" },
    { .name = "read-only", .short_name = 'R', .type = AXL_ARG_BOOL,
      .help = "Sessions watch the console; their input is dropped, and SFTP "
              "refuses every write" },
    { .name = "sftp-read-only", .type = AXL_ARG_BOOL,
      .help = "SFTP (and scp) refuse every write: files can be read, not "
              "changed (AxlSftpReadOnly)" },
    { .name = "report-geometry", .type = AXL_ARG_BOOL,
      .help = "Write each console reshape (a `mode`) to the first serial "
              "port, outside the console, for a test harness's serial "
              "terminal: an ESC _ axl-console-size=CxR ESC \\ string" },
    { .name = "log-port", .type = AXL_ARG_STRING,
      .help = "Serial port for the log while serving: auto (one the "
              "console is not on), none, or N (0 = first; AxlLogPort)" },
    { .name = "net-timeout", .type = AXL_ARG_U64,
      .min = 1, .max = AXL_NET_AUTO_TIMEOUT_MAX_SEC,
      .help = "Seconds the network bring-up may take before the start "
              "gives up (1-86400; default " SSHD_NET_TIMEOUT_STR "; "
              "replaces the settings file's AxlNetworkTimeout)" },
    { .name = "tcp-linger", .type = AXL_ARG_U64,
      .min = 0, .max = AXL_TCP_PACING_LINGER_MAX_US,
      .help = "Microseconds a connection keeps being polled after a reply, "
              "for the client's next request (0-20000; default 0, off; "
              "replaces the settings file's AxlTcpLinger)" },
    { .name = "exempt", .short_name = 'x', .type = AXL_ARG_MULTI,
      .help = "Never slow down or block failed logins from this range, "
              "e.g. 10.0.2.2/32 (repeatable, up to 8; replaces the "
              "settings file's PerSourcePenaltyExemptList)" },
    { .name = "host-key", .type = AXL_ARG_MULTI,
      .help = "Host key file (an unencrypted ssh-ed25519 key as ssh-keygen "
              "writes it), beside sshd.efi like -k; repeatable, the first "
              "that loads is used. Replaces the settings file's HostKey; "
              "--host-key \"\" means the key stored on this machine" },
    { .name = "config-file", .short_name = 'f', .type = AXL_ARG_STRING,
      .help = "sshd_config-syntax file (default: sshd.conf beside sshd.efi)" },
    { .name = "dump-config", .short_name = 'T', .type = AXL_ARG_BOOL,
      .help = "Print the effective settings (sshd -T format) and exit" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Log at DEBUG (LogLevel DEBUG), in this command and in the "
              "server: each step of the start is named before it runs" },
    { .name = "debug", .short_name = 'd', .type = AXL_ARG_BOOL,
      .help = "The same as -v (OpenSSH sshd's -d; the server still "
              "returns to the Shell)" },
    { 0 }
};

static const AxlArgDesc hostkey_flags[] = {
    { .name = "regenerate", .type = AXL_ARG_BOOL,
      .help = "Delete the stored host key; the next start makes a new one "
              "(every client will see a changed key)" },
    { .name = "import", .type = AXL_ARG_STRING,
      .help = "Store this key file (as ssh-keygen -t ed25519 -N '' writes it) "
              "in the UEFI variable, so it can leave the medium" },
    { .name = "force", .type = AXL_ARG_BOOL,
      .help = "With --import: replace a key already stored" },
    { 0 }
};

static const AxlArgsNode verbs[] = {
    { .name = "stop",   .help = "Unload the resident server",
      .handler = verb_stop },
    { .name = "status", .help = "Report whether the server is running",
      .handler = verb_status },
    { .name = "log",    .help = "Print the server log",
      .handler = verb_log },
    { .name = "hostkey", .help = "Print the host key, or --regenerate / --import it",
      .flags = hostkey_flags, .handler = verb_hostkey },
    { 0 }
};

// ---------------------------------------------------------------------------
// Implementations
// ---------------------------------------------------------------------------

static AxlServiceDeploy
sshd_deploy(void)
{
    AxlServiceDeploy d = {
        .service         = &sshd_service,
        .driver_blob     = AXL_EMBED_DATA(sshd_dxe),
        .driver_blob_len = AXL_EMBED_SIZE(sshd_dxe),
        .driver_name     = "sshd-dxe.efi",
        /* Never a stale loose sshd-dxe.efi from some other build. */
        .embedded_only   = true,
    };
    return d;
}

static bool
is_file(const char *path)
{
    AxlFsEntry e;

    return axl_file_info(path, &e) == AXL_OK && !axl_fs_entry_is_dir(&e);
}

/* The file name a refusal starts with: `sshd.conf:3: ...`. */
static const char *
base_name(const char *path)
{
    const char *b = path;

    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') {
            b = p + 1;
        }
    }
    return b;
}

/* An OpenSSH keyword read and ignored: said as OpenSSH says it. */
static void
conf_warn(size_t line, const char *text, void *ud)
{
    axl_printerr("%s:%u: %s\n", (const char *)ud, (unsigned)line, text);
}

static void
dump_line(const char *line, void *ud)
{
    (void)ud;
    axl_printf("%s\n", line);
}

/* Defaults, then the settings file into `o`. The default file may be
   missing -- sshd then runs on its defaults -- but nothing else: a file
   that is there and cannot be read, or that says something sshd refuses,
   stops it, so an operator never gets a server configured other than they
   wrote. `name` receives the file's base name for later messages, and
   `note` the line a start prints to say where the settings came from ("" if
   that was already said). Returns 0, or 1 having said why.

   Every path sshd reads (here, -k, Banner) is made absolute HERE, beside the
   launcher's image (axl_app_image_dir_path): the resident driver is buffer-loaded
   and has no volume of its own. */
static int
load_conf(AxlArgs *a, SshdOpts *o, AxlConfigFile *cf, SshdConfOrigin *origin,
          const char **name, char *note, size_t note_cap)
{
    static char        path[SSHD_PATH_MAX];
    const char        *f        = axl_args_get_string(a, "config-file");
    bool               explicit = axl_args_is_set(a, "config-file");
    AxlConfigFileError cerr;
    SshdConfError      err;

    sshd_conf_defaults(o);
    origin->keys_line   = 0;
    origin->banner_line = 0;
    *name               = SSHD_CONF_NAME;
    note[0]             = '\0';
    if (explicit) {
        if (f == NULL || f[0] == '\0' || axl_app_image_dir_path(f, path, sizeof(path)) != AXL_OK) {
            axl_printerr("sshd: cannot place %s beside sshd.efi\n",
                         f != NULL ? f : "");
            return 1;
        }
    } else if (axl_app_image_dir_path(SSHD_CONF_NAME, path, sizeof(path)) != AXL_OK) {
        /* Said, so running on the defaults is never a surprise. */
        axl_printerr("sshd: cannot locate sshd.efi's volume; not reading %s\n",
                     SSHD_CONF_NAME);
        return 0;
    }
    *name = base_name(path);

    AxlStatus rc = axl_config_file_read(cf, path, &cerr);
    if (rc == AXL_NOT_FOUND && !explicit) {
        /* Named, so a file put somewhere else is seen not to be read. */
        axl_snprintf(note, note_cap, "sshd: no %s; using defaults", path);
        return 0;
    }
    if (rc == AXL_INVALID && cerr.line != 0) {
        axl_printerr("%s:%u: %s\n", *name, (unsigned)cerr.line, cerr.reason);
        return 1;
    }
    if (rc == AXL_NO_RESOURCES) {
        axl_printerr("sshd: out of memory reading %s\n", path);
        return 1;
    }
    if (rc != AXL_OK) {
        axl_printerr("sshd: cannot read %s\n", path);
        return 1;
    }
    if (sshd_conf_apply(o, cf, origin, conf_warn, (void *)*name, &err) != AXL_OK) {
        axl_printerr("%s:%u: %s\n", *name, (unsigned)err.line, err.reason);
        return 1;
    }
    axl_snprintf(note, note_cap, "sshd: settings from %s", path);
    o->conf = *name;
    return 0;
}

/* The keys file the driver will read, made absolute in place ("" for
   none). From -k (`flag`), a failure is said as -k always said it; from the
   file, at its line. With neither, authorized_keys beside sshd.efi, only if
   it is there. Returns 0, or 1 having said why. */
static int
resolve_keys(SshdOpts *o, size_t line, const char *name, bool flag)
{
    static char keys[SSHD_PATH_MAX];

    if (!flag && line == 0) {
        /* Neither named one: the default file, only if it is there. */
        if (axl_app_image_dir_path("authorized_keys", keys, sizeof(keys)) == AXL_OK
            && is_file(keys)) {
            o->keys = keys;
        }
        return 0;
    }
    if (o->keys[0] == '\0') {
        return 0;   /* `AuthorizedKeysFile none` */
    }
    if (axl_app_image_dir_path(o->keys, keys, sizeof(keys)) != AXL_OK) {
        axl_printerr("sshd: cannot place %s beside sshd.efi\n", o->keys);
        return 1;
    }
    if (!is_file(keys)) {
        if (flag) {
            axl_printerr("sshd: cannot read %s\n", keys);
        } else {
            axl_printerr("%s:%u: AuthorizedKeysFile: cannot read %s\n", name,
                         (unsigned)line, keys);
        }
        return 1;
    }
    o->keys = keys;
    return 0;
}

/* The Banner file, made absolute in place and checked now -- there, text
   (UTF-8, or UTF-16LE + BOM as the Shell writes it), not too large -- so a
   bad one stops `sshd` at its line rather than the driver's setup. The
   driver reads it again when it starts. */
static int
resolve_banner(SshdOpts *o, size_t line, const char *name)
{
    static char banner[SSHD_PATH_MAX];
    void       *buf = NULL;
    size_t      len = 0;
    AxlFsEntry  e;

    if (o->banner[0] == '\0') {
        return 0;
    }
    if (axl_app_image_dir_path(o->banner, banner, sizeof(banner)) != AXL_OK) {
        axl_printerr("sshd: cannot place %s beside sshd.efi\n", o->banner);
        return 1;
    }
    if (axl_file_info(banner, &e) != AXL_OK || axl_fs_entry_is_dir(&e)) {
        axl_printerr("%s:%u: Banner: cannot read %s\n", name, (unsigned)line, banner);
        return 1;
    }
    /* The limit is on the text. A file over twice it (plus a BOM) holds
       more text than that whichever way it is written, so it is refused
       unread. */
    SshdBannerRead r = SSHD_BANNER_TOO_LARGE;
    if (e.size <= 2u * SSHD_BANNER_FILE_MAX + 2u) {
        if (axl_file_get_contents(banner, &buf, &len) != AXL_OK) {
            axl_printerr("%s:%u: Banner: cannot read %s\n", name, (unsigned)line, banner);
            return 1;
        }
        char  *text     = NULL;
        size_t text_len = 0;
        r = sshd_bind_banner_text(buf, len, SSHD_BANNER_FILE_MAX, &text, &text_len);
        axl_free(text);
        axl_free(buf);
    }
    switch (r) {
    case SSHD_BANNER_OK:
        o->banner = banner;
        return 0;
    case SSHD_BANNER_TOO_LARGE:
        axl_printerr("%s:%u: Banner: %s is larger than %u bytes\n", name,
                     (unsigned)line, banner, (unsigned)SSHD_BANNER_FILE_MAX);
        return 1;
    case SSHD_BANNER_NOT_TEXT:
        axl_printerr("%s:%u: Banner: %s is not UTF-8 text\n", name,
                     (unsigned)line, banner);
        return 1;
    case SSHD_BANNER_NO_MEMORY:
    default:
        axl_printerr("sshd: out of memory reading %s\n", banner);
        return 1;
    }
}

/* The -x ranges, each checked, joined with ',' into `out`. Returns 0, or 1
   having said why. */
static int
join_exempt(AxlArgs *a, char *out, size_t cap)
{
    int n = axl_args_get_multi_count(a, "exempt");

    out[0] = '\0';
    if (n > (int)SSHD_EXEMPT_MAX) {
        axl_printerr("sshd: at most %u -x ranges\n", (unsigned)SSHD_EXEMPT_MAX);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        const char *r = axl_args_get_multi(a, "exempt", i);
        /* The server's own rule, so a typo stops `sshd` here rather than
           the driver's setup. */
        if (axl_ssh_penalty_range_check(r) != AXL_OK) {
            axl_printerr("sshd: -x %s is not an IPv4 address or CIDR range\n", r);
            return 1;
        }
        if ((i > 0 && axl_strlcat(out, ",", cap) >= cap)
            || axl_strlcat(out, r, cap) >= cap) {
            axl_printerr("sshd: -x ranges too long\n");
            return 1;
        }
    }
    return 0;
}

/* The driver is loaded but serves nothing: the Shell it took over exited,
   which released the console and freed the server, and the driver stays
   resident until something unloads it. Its take-over is the one console
   device there can be, so loaded-without-one means exactly that -- unless
   another take-over (fbcon) has installed one since that Shell exited: then
   this reads false and `sshd status` says "running" for a driver serving
   nothing. Rare (it needs fbcon started in the Shell that follows), and
   `sshd stop` still unloads it. */
static bool
sshd_orphaned(const AxlServiceDeploy *d)
{
    return axl_service_is_running(d) && !axl_console_device_present();
}

/* The driver could not give the console back (an uninstall failed for good):
   it stays resident with its take-over in place and no server. The service
   state says so -- STOPPING when the failure was in setup or a teardown,
   DEGRADED, with sshd's own reason, when it was the release after the Shell
   exited. A degraded state with any OTHER reason is not this. */
static bool
sshd_stuck(void)
{
    char why[AXL_SERVICE_REASON_MAX + 1];

    switch (axl_service_state(&sshd_service)) {
    case AXL_SERVICE_STOPPING:
        return true;
    case AXL_SERVICE_DEGRADED:
        return axl_service_state_reason(&sshd_service, why, sizeof(why)) == AXL_OK
               && axl_strcmp(why, SSHD_STUCK_REASON) == 0;
    default:
        return false;
    }
}

/* The SFTP helper's file, beside sshd.efi like every other path sshd reads:
   `internal-sftp` names the default helper's file. Not checked for here --
   the file is loaded when a client first asks for sftp, and may be added
   after the start -- and "" when it cannot be placed, which leaves only an
   embedded helper (sshd-full) to serve. */
static void
resolve_sftp(SshdOpts *o)
{
    static char sftp[SSHD_PATH_MAX];
    const char *file = o->sftp;

    o->sftp_path = "";
    if (file == NULL || file[0] == '\0') {
        return;
    }
    if (sftp_command_is_internal(file)) {
        file = SFTP_HELPER_NAME;
    }
    if (axl_app_image_dir_path(file, sftp, sizeof(sftp)) == AXL_OK) {
        o->sftp_path = sftp;
    }
}

/* Every option the driver will get, in `o`: defaults < settings file <
   flags, every path made absolute and checked. Strings borrow from `cf` and
   `a`. Returns 0, or 1 having said why. */
static int
resolve_opts(AxlArgs *a, SshdOpts *o, AxlConfigFile *cf, char *note, size_t note_cap)
{
    static char    exempt[SSHD_EXEMPT_CAP];
    static char    drivers[SSHD_PATH_MAX];
    static char    volume[SSHD_PATH_MAX];
    SshdConfOrigin origin;
    const char    *name = SSHD_CONF_NAME;

    if (load_conf(a, o, cf, &origin, &name, note, note_cap) != 0) {
        return 1;
    }
    /* The command line wins over the file, flag by flag. Safe to narrow
       the port: its flag bounds it to 1-65535. */
    if (axl_args_is_set(a, "port")) {
        o->net.port = (uint16_t)axl_args_get_uint(a, "port");
    }
    if (axl_args_is_set(a, "nic")) {
        o->net.nic_index = axl_args_get_uint(a, "nic");
    }
    /* A key file or password given here is meant to be used: it turns its
       method on for this run, whatever the file says (`-k ""` is no file
       and `-P ""` no password: each clears the file's and turns nothing
       on). */
    if (axl_args_is_set(a, "keys")) {
        o->keys = axl_args_get_string(a, "keys");
        if (o->keys != NULL && o->keys[0] != '\0') {
            o->pubkey = true;
        }
    }
    if (axl_args_is_set(a, "password")) {
        o->password = axl_args_get_string(a, "password");
        if (o->password != NULL && o->password[0] != '\0') {
            o->pwauth = true;
        }
    }
    if (axl_args_is_set(a, "read-only")) {
        o->ro = axl_args_get_bool(a, "read-only");
    }
    if (axl_args_is_set(a, "sftp-read-only")) {
        o->sftp_ro = axl_args_get_bool(a, "sftp-read-only");
    }
    if (axl_args_is_set(a, "report-geometry")) {
        o->geometry = axl_args_get_bool(a, "report-geometry");
    }
    if (axl_args_is_set(a, "log-port")) {
        const char *lp = axl_args_get_string(a, "log-port");
        if (lp == NULL || !sshd_conf_log_port_parse(lp, &o->log_port)) {
            axl_printerr("sshd: --log-port %s is not auto, none or a port number\n",
                         lp != NULL ? lp : "");
            return 1;
        }
    }
    /* Safe to narrow: the flag bounds it to 1-86400. */
    if (axl_args_is_set(a, "net-timeout")) {
        o->net_timeout_sec = (uint32_t)axl_args_get_uint(a, "net-timeout");
    }
    /* Safe to narrow: the flag bounds it to 0-20000. */
    if (axl_args_is_set(a, "tcp-linger")) {
        o->tcp_linger_us = (uint32_t)axl_args_get_uint(a, "tcp-linger");
    }
    if (axl_args_get_bool(a, "verbose") || axl_args_get_bool(a, "debug")) {
        o->loglevel = SSHD_LOG_DEBUG1;
    }
    if (axl_args_is_set(a, "exempt")) {
        if (join_exempt(a, exempt, sizeof(exempt)) != 0) {
            return 1;
        }
        o->exempt = exempt;
    }
    if (o->keys == NULL) {
        o->keys = "";
    }
    if (o->password == NULL) {
        o->password = "";
    }
    if (resolve_keys(o, origin.keys_line, name, axl_args_is_set(a, "keys")) != 0
        || resolve_banner(o, origin.banner_line, name) != 0
        || resolve_host_keys(a, o) != 0) {
        return 1;
    }
    /* The drivers to sweep: drivers\<arch> beside sshd.efi, as netload finds
       them. Resolved here because the driver, started from memory, has no
       directory of its own; "" (none found) sweeps every volume's. */
    o->drivers = axl_net_driver_dir(drivers, sizeof(drivers)) == AXL_OK ? drivers : "";
    /* And the volume sshd.efi is on: sshd-rtk's embedded NIC driver is given
       it as its device when there is no drivers directory. */
    o->volume = axl_app_boot_path("", volume, sizeof(volume)) == AXL_OK ? volume : "";
    resolve_sftp(o);
    return 0;
}

/* --host-key, or the file's HostKey lines, made absolute beside sshd.efi into
   o->host_keys, in order; `none` kept as is. --host-key REPLACES the file's
   lines -- a flag wins over the file, as -x does -- where sshd's -h is added
   to them (sshd.c:1312-1315), a recorded deviation; `--host-key ""` alone
   means "the stored key", whatever the file says. Returns 0, or 1 having
   said why. */
static int
resolve_host_keys(AxlArgs *a, SshdOpts *o)
{
    static char list[SSHD_HOSTKEY_MAX * (SSHD_PATH_MAX + 1)];
    char        path[SSHD_PATH_MAX];
    const char *src = o->host_keys != NULL ? o->host_keys : "";
    char        flags[SSHD_HOSTKEY_MAX * (SSHD_PATH_MAX + 1)];

    if (axl_args_is_set(a, "host-key")) {
        int n = axl_args_get_multi_count(a, "host-key");
        if (n > (int)SSHD_HOSTKEY_MAX) {
            axl_printerr("sshd: at most %u --host-key files\n", (unsigned)SSHD_HOSTKEY_MAX);
            return 1;
        }
        flags[0] = '\0';
        for (int i = 0; i < n; i++) {
            const char *f = axl_args_get_multi(a, "host-key", i);
            if (f == NULL || f[0] == '\0') {
                if (n > 1) {
                    axl_printerr("sshd: --host-key \"\" (the stored key) cannot be"
                                 " combined with files\n");
                    return 1;
                }
                continue;
            }
            if (axl_strchr(f, SSHD_HOSTKEY_SEP) != NULL) {
                axl_printerr("sshd: --host-key %s: a path may not contain '%c'\n", f,
                             SSHD_HOSTKEY_SEP);
                return 1;
            }
            char sep[2] = { SSHD_HOSTKEY_SEP, '\0' };
            if ((flags[0] != '\0' && axl_strlcat(flags, sep, sizeof flags) >= sizeof flags)
                || axl_strlcat(flags, f, sizeof flags) >= sizeof flags) {
                axl_printerr("sshd: --host-key paths too long\n");
                return 1;
            }
        }
        src = flags;
    }
    list[0] = '\0';
    const char *cur = src;
    while (sshd_bind_hostkey_next(&cur, path, sizeof path)) {
        char abs[SSHD_PATH_MAX];
        if (axl_strcmp(path, SSHD_HOSTKEY_NONE) == 0) {
            axl_strlcpy(abs, path, sizeof abs);
        } else if (axl_app_image_dir_path(path, abs, sizeof abs) != AXL_OK) {
            axl_printerr("sshd: cannot place %s beside sshd.efi\n", path);
            return 1;
        }
        char sep[2] = { SSHD_HOSTKEY_SEP, '\0' };
        if ((list[0] != '\0' && axl_strlcat(list, sep, sizeof list) >= sizeof list)
            || axl_strlcat(list, abs, sizeof list) >= sizeof list) {
            axl_printerr("sshd: HostKey paths too long\n");
            return 1;
        }
    }
    o->host_keys = list;
    return 0;
}

/* The two lines a key is shown with: ssh(1)'s first-contact wording for the
   fingerprint, character for character, and the public line for
   known_hosts. */
static void
print_key(const char *fingerprint, const char *public_line)
{
    axl_printf("ED25519 key fingerprint is %s.\n%s\n", fingerprint, public_line);
}

/* The first HostKey entry the launcher can read, into @p rec and @p used --
   provider-free, so ADVISORY: the driver's full load decides, and may skip
   an entry this accepted (a seed that does not derive its public key). Each
   entry that cannot be read is said in sshd's two lines (sshd.c:1484,
   1534). Returns 0, or 1 having said "no hostkeys available". */
static int
host_key_first_file(const char *list, AxlSshHostKeyRecord *rec, char *used, size_t cap)
{
    const char *cur = list;
    char        path[SSHD_PATH_MAX];

    while (sshd_bind_hostkey_next(&cur, path, sizeof path)) {
        if (axl_strcmp(path, SSHD_HOSTKEY_NONE) == 0) {
            continue;
        }
        const char *why = NULL;
        int         rc  = axl_ssh_host_key_read_file(path, rec, &why);
        if (rc == AXL_OK) {
            axl_strlcpy(used, path, cap);
            return 0;
        }
        if (rc != AXL_NOT_FOUND) {
            axl_printerr("sshd: Unable to load host key \"%s\": %s\n", path, why);
        }
        axl_printerr("sshd: Unable to load host key: %s\n", path);
    }
    axl_printerr("sshd: no hostkeys available -- exiting.\n");
    return 1;
}

/* Before a start: the key the server will use must be there and usable, so
   a bad one stops `sshd` here rather than in the driver's setup. Read-only:
   a missing stored key is fine (the first start makes it). Returns 0, or 1
   having said why. */
static int
host_key_precheck(const SshdOpts *o)
{
    AxlSshHostKeyRecord rec;
    const char         *why = NULL;
    char                used[SSHD_PATH_MAX];
    int                 bad = 0;

    if (o->host_keys != NULL && o->host_keys[0] != '\0') {
        bad = host_key_first_file(o->host_keys, &rec, used, sizeof used);
    } else {
        int rc = axl_ssh_host_key_nv_read(&rec, &why);
        if (rc == AXL_ERR) {
            axl_printerr("sshd: the stored host key is unusable (%s); not starting. Run"
                         " 'sshd hostkey --regenerate' to replace it\n", why);
            bad = 1;
        } else if (rc != AXL_OK && rc != AXL_NOT_FOUND) {
            axl_printerr("sshd: the stored host key cannot be read (%s); not starting."
                         " Run 'sshd hostkey --regenerate' to replace it\n",
                         why != NULL ? why : "?");
            bad = 1;
        }
    }
    axl_ssh_host_key_record_clear(&rec);
    return bad;
}

/* The running driver's host-key interface, copied out, or false when it
   publishes none of a version this sshd reads. */
static bool
host_key_running(SshdHostKeyIface *out)
{
    const SshdHostKeyIface *hi = NULL;

    if (axl_protocol_find_guid(&SSHD_HOSTKEY_GUID, (void **)&hi) != AXL_OK || hi == NULL
        || hi->size < sizeof *hi || hi->version != SSHD_HOSTKEY_IFACE_VERSION) {
        return false;
    }
    *out = *hi;
    out->fingerprint[sizeof out->fingerprint - 1] = '\0';
    out->public_line[sizeof out->public_line - 1] = '\0';
    out->path[sizeof out->path - 1]               = '\0';
    out->why[sizeof out->why - 1]                 = '\0';
    return true;
}

/* `sshd hostkey --regenerate`: delete the stored key; the next start makes
   a new one. Deletes, never creates -- the launcher has no Ed25519
   provider, and creating is what a start does (ruling 1). */
static int
hostkey_regenerate(const SshdOpts *o)
{
    AxlSshHostKeyRecord rec;
    char                fp[AXL_SSH_FINGERPRINT_SIZE];
    int                 rc = axl_ssh_host_key_nv_read(&rec, NULL);

    if (o->host_keys != NULL && o->host_keys[0] != '\0') {
        axl_printf("sshd: note: HostKey is set, so the server uses its file, not the"
                   " stored key\n");
    }
    /* The old fingerprint, so an operator can match a client's report
       against what is being thrown away (review L4). Taken before the
       delete; said only after it, so a refused delete warns of nothing. */
    bool have_fp = rc == AXL_OK
                   && axl_ssh_host_key_fingerprint(rec.public_key, sizeof rec.public_key, fp,
                                                   sizeof fp) == AXL_OK;
    axl_ssh_host_key_record_clear(&rec);
    if (rc == AXL_NOT_FOUND) {
        axl_printf("sshd: no host key is stored; the next start generates one\n");
    }
    int drc = axl_ssh_host_key_nv_delete();
    if (drc == AXL_DENIED) {
        axl_printerr("sshd: the firmware refuses to delete the stored host key (an"
                     " authenticated variable: only its signer can). Use a HostKey file"
                     " instead, or clear NVRAM in firmware setup\n");
        return 1;
    }
    if (drc != AXL_OK) {
        axl_printerr("sshd: cannot delete the stored host key\n");
        return 1;
    }
    if (rc != AXL_NOT_FOUND) {
        axl_printf("sshd: WARNING: replacing the host key. Every client that has connected"
                   " before\n"
                   "sshd: will refuse this server (\"REMOTE HOST IDENTIFICATION HAS"
                   " CHANGED\") until\n"
                   "sshd: its known_hosts entry is removed: ssh-keygen -R '[host]:port'\n");
        if (have_fp) {
            axl_printf("sshd: deleted host key ED25519 %s\n", fp);
        }
        axl_printf("sshd: the host key was deleted; the next start generates a new one and"
                   " prints it\n");
    }
    return 0;
}

/* `sshd hostkey --import FILE [--force]`: store a provisioned key file in
   the UEFI variable, so the file can leave the medium (review M9). */
static int
hostkey_import(const char *file, bool force)
{
    char                path[SSHD_PATH_MAX];
    AxlSshHostKeyRecord rec;
    AxlSshHostKeyRecord old;
    const char         *why = NULL;
    char                fp[AXL_SSH_FINGERPRINT_SIZE];
    char                line[AXL_SSH_PUBLIC_LINE_SIZE];

    if (axl_app_image_dir_path(file, path, sizeof path) != AXL_OK) {
        axl_printerr("sshd: cannot place %s beside sshd.efi\n", file);
        return 1;
    }
    int rc = axl_ssh_host_key_read_file(path, &rec, &why);
    if (rc != AXL_OK) {
        axl_printerr("sshd: cannot import %s: %s\n", path, why);
        return 1;
    }
    /* The key being replaced is KEPT until the new one is stored (final
       review F3): the variable has to be deleted first (the store refuses
       to overwrite), so a write that then fails puts this one back. */
    int orc = axl_ssh_host_key_nv_read(&old, NULL);
    if (orc != AXL_NOT_FOUND && !force) {
        axl_ssh_host_key_record_clear(&old);
        axl_ssh_host_key_record_clear(&rec);
        axl_printerr("sshd: a host key is already stored; run 'sshd hostkey --regenerate'"
                     " first, or pass --force\n");
        return 1;
    }
    if (orc != AXL_NOT_FOUND) {
        int drc = axl_ssh_host_key_nv_delete();
        if (drc != AXL_OK) {
            axl_ssh_host_key_record_clear(&old);
            axl_ssh_host_key_record_clear(&rec);
            axl_printerr("sshd: cannot delete the stored host key%s\n",
                         drc == AXL_DENIED ? " (an authenticated variable: only its"
                                             " signer can)" : "");
            return 1;
        }
    }
    /* Success is the write and its read-back, nothing else (re-review N2):
       a fingerprint that cannot be formatted (out of memory) must not send
       a key that IS stored down the failure path. */
    rc = axl_ssh_host_key_nv_write(&rec, &why);
    if (rc == AXL_OK) {
        bool shown = axl_ssh_host_key_fingerprint(rec.public_key, sizeof rec.public_key, fp,
                                                  sizeof fp) == AXL_OK
                     && axl_ssh_host_key_public_line(rec.public_key, sizeof rec.public_key,
                                                     line, sizeof line) == AXL_OK;
        axl_ssh_host_key_record_clear(&old);
        axl_ssh_host_key_record_clear(&rec);
        axl_printf("sshd: imported %s into UEFI variable %s\n", path, AXL_SSH_HOST_KEY_VAR);
        if (shown) {
            print_key(fp, line);
        } else {
            axl_printerr("sshd: its fingerprint cannot be shown now (out of memory); 'sshd"
                         " hostkey' prints it\n");
        }
        /* Not "the file can be removed": the launcher cannot derive the
           public key from the seed (no Ed25519 provider), so a corrupted
           seed passes every check here and only the server's load refuses
           it (final review F6). */
        axl_printf("sshd: start sshd and confirm this fingerprint before removing the key"
                   " file; the server checks the key fully when it loads it\n");
        return 0;
    }
    axl_ssh_host_key_record_clear(&rec);
    const char *what = why != NULL ? why : "?";
    if (orc == AXL_OK && axl_ssh_host_key_nv_write(&old, NULL) == AXL_OK) {
        if (axl_ssh_host_key_fingerprint(old.public_key, sizeof old.public_key, fp,
                                         sizeof fp) != AXL_OK) {
            axl_strlcpy(fp, "?", sizeof fp);
        }
        axl_ssh_host_key_record_clear(&old);
        axl_printerr("sshd: cannot store the host key (%s); the previous host key, ED25519"
                     " %s, is stored again\n", what, fp);
        return 1;
    }
    /* What the variable holds NOW is read back before it is reported, on
       every failure path (re-review N1): a write that did not read back and
       whose clean-up delete was refused stays there, and is not "nothing". */
    bool left = axl_ssh_host_key_nv_read(&old, NULL) != AXL_NOT_FOUND;
    axl_ssh_host_key_record_clear(&old);
    const char *lost = orc == AXL_NOT_FOUND ? ""
                       : orc == AXL_OK      ? ", and the previous one could not be put back"
                                            : ", and the unusable one it replaced was deleted";
    if (left) {
        axl_printerr("sshd: cannot store the host key (%s)%s; the variable holds a write that"
                     " failed: run 'sshd hostkey --regenerate'\n", what, lost);
    } else if (orc == AXL_NOT_FOUND) {
        axl_printerr("sshd: cannot store the host key (%s)\n", what);
    } else {
        axl_printerr("sshd: cannot store the host key (%s)%s: no host key is stored now; the"
                     " next start generates a new one\n", what, lost);
    }
    return 1;
}

/* `sshd hostkey`: print the key -- the running server's, or the one a start
   would use. Read-only, never creates (diagnostics must be read-only). */
static int
verb_hostkey(AxlArgs *a)
{
    const AxlServiceDeploy d = sshd_deploy();
    bool                   regen  = axl_args_get_bool(a, "regenerate");
    const char            *import = axl_args_get_string(a, "import");
    bool                   want_import = axl_args_is_set(a, "import");
    SshdOpts               o;
    char                   note[SSHD_NOTE_MAX];
    AXL_AUTOPTR(AxlConfigFile) cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);

    if (cf == NULL) {
        axl_printerr("sshd: out of memory\n");
        return 1;
    }
    if (regen && want_import) {
        axl_printerr("sshd: --regenerate and --import cannot be combined\n");
        return 1;
    }
    if (axl_args_get_bool(a, "force") && !want_import) {
        axl_printerr("sshd: --force goes with --import\n");
        return 1;
    }
    bool running = axl_service_is_running(&d) && !sshd_orphaned(&d);
    if (running && (regen || want_import)) {
        axl_printerr("sshd: stop the server first ('sshd stop'); a running server keeps"
                     " the host key it started with\n");
        return 1;
    }
    if (running) {
        SshdHostKeyIface hi;
        if (!host_key_running(&hi)) {
            axl_printf("%s", SSHD_NO_HOSTKEY_IFACE);
            return 0;
        }
        axl_printf("sshd: the running server's host key:\n");
        print_key(hi.fingerprint, hi.public_line);
        return 0;
    }
    if (want_import) {
        if (import == NULL || import[0] == '\0') {
            axl_printerr("sshd: --import needs a key file\n");
            return 1;
        }
        return hostkey_import(import, axl_args_get_bool(a, "force"));
    }
    if (resolve_opts(a, &o, cf, note, sizeof note) != 0) {
        return 1;
    }
    if (regen) {
        return hostkey_regenerate(&o);
    }

    AxlSshHostKeyRecord rec;
    const char         *why = NULL;
    char                used[SSHD_PATH_MAX];
    char                fp[AXL_SSH_FINGERPRINT_SIZE];
    char                line[AXL_SSH_PUBLIC_LINE_SIZE];
    if (o.host_keys[0] != '\0') {
        if (host_key_first_file(o.host_keys, &rec, used, sizeof used) != 0) {
            return 1;
        }
        /* Read provider-free: a start may still skip it, for a seed that
           does not derive its public key (final review F7). */
        axl_printf("sshd: the first readable host key file, %s (the server checks it fully"
                   " when it loads it):\n", used);
    } else {
        int rc = axl_ssh_host_key_nv_read(&rec, &why);
        if (rc == AXL_NOT_FOUND) {
            axl_printf("sshd: no host key is stored yet; the first start generates one\n");
            return 0;
        }
        if (rc != AXL_OK) {
            axl_printerr("sshd: the stored host key is %s (%s); run 'sshd hostkey"
                         " --regenerate' to replace it\n",
                         rc == AXL_ERR ? "unusable" : "unreadable", why != NULL ? why : "?");
            return 1;
        }
        axl_printf("sshd: the stored host key (UEFI variable %s):\n", AXL_SSH_HOST_KEY_VAR);
    }
    int frc = axl_ssh_host_key_fingerprint(rec.public_key, sizeof rec.public_key, fp,
                                           sizeof fp);
    int lrc = axl_ssh_host_key_public_line(rec.public_key, sizeof rec.public_key, line,
                                           sizeof line);
    axl_ssh_host_key_record_clear(&rec);
    if (frc != AXL_OK || lrc != AXL_OK) {
        axl_printerr("sshd: out of memory\n");
        return 1;
    }
    print_key(fp, line);
    return 0;
}

static int
verb_start(AxlArgs *a)
{
    const AxlServiceDeploy d = sshd_deploy();
    SshdOpts o;
    char     note[SSHD_NOTE_MAX];
    AXL_AUTOPTR(AxlConfigFile) cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);

    if (cf == NULL) {
        axl_printerr("sshd: out of memory\n");
        return 1;
    }
    /* Read and validate everything before looking at a running server, so
       an operator who edited sshd.conf learns it is broken either way. */
    if (resolve_opts(a, &o, cf, note, sizeof(note)) != 0) {
        return 1;
    }
    /* This image's own log; the driver sets its own from the options. */
    int level = sshd_bind_log_level(o.loglevel);
    if (level >= 0) {
        axl_log_set_level(level);
    }
    if (axl_args_get_bool(a, "dump-config")) {
        sshd_conf_dump(&o, dump_line, NULL);
        return 0;
    }

    /* A driver left behind by an exited Shell is stopped so a fresh one can
       take this Shell over. */
    if (axl_service_is_running(&d) && sshd_stuck()) {
        axl_printerr("sshd: %s\n", sshd_bind_status(true, true, true));
        return 1;
    }
    if (sshd_orphaned(&d) && axl_service_stop(&d) != AXL_OK) {
        axl_printerr("sshd: could not unload the server left by an exited Shell\n");
        return 1;
    }
    if (axl_service_is_running(&d)) {
        axl_printf("sshd: already running - the running server was not "
                   "reconfigured; run 'sshd stop' then 'sshd' again to change "
                   "its options\n");
        return 0;
    }
    if (!sshd_bind_can_log_in(o.pubkey, o.keys[0] != '\0', o.pwauth,
                              o.password[0] != '\0')) {
        char def[SSHD_PATH_MAX];
        if (axl_app_image_dir_path("authorized_keys", def, sizeof(def)) != AXL_OK) {
            axl_strlcpy(def, "authorized_keys", sizeof(def));
        }
        axl_printerr("sshd: no way to log in - give -k with an authorized_keys "
                     "file, put one at %s, or give -P\n", def);
        return 1;
    }
    if (axl_console_device_present()) {
        axl_printerr("sshd: another console take-over is active "
                     "(fbcon or a BMC application)\n");
        return 1;
    }
    if (host_key_precheck(&o) != 0) {
        return 1;
    }

    /* Filled BEFORE start_embedded serializes it; the strings it borrows
       (from AxlArgs and `cf`) stay valid until this handler returns. The
       line goes out first: the start brings the network up before it says
       anything else, and a stall there must not look like a hang. */
    axl_printf("sshd: starting -- bringing up the network (up to %u s)\n",
               (unsigned)o.net_timeout_sec);
    g_sshd_opts = o;
    AxlStatus src = axl_service_start_embedded(&d);
    /* A setup that failed after the take-over, and could not undo it, stays
       resident and STOPPING, and the start reports AXL_BUSY: say what status
       says. */
    if (src == AXL_BUSY || sshd_stuck()) {
        axl_printerr("sshd: %s\n", sshd_bind_status(true, true, true));
        return 1;
    }
    if (src != AXL_OK) {
        axl_printerr("sshd: could not start the server\n");
        return 1;
    }
    if (note[0] != '\0') {
        axl_printf("%s\n", note);
    }
    char addrs[SSHD_ADDRS_MAX];
    sshd_addresses(addrs, sizeof(addrs));
    axl_printf("sshd: serving the UEFI Shell on %s port %u%s\n", addrs,
               (unsigned)g_sshd_opts.net.port,
               g_sshd_opts.ro ? " (read-only)" : "");
    SshdHostKeyIface hi;
    if (host_key_running(&hi)) {
        if (hi.origin == SSHD_HOSTKEY_GENERATED) {
            axl_printf("sshd: generated a new host key\n");
        }
        axl_printf("sshd: host key ED25519 key fingerprint is %s.\n", hi.fingerprint);
        if (hi.origin == SSHD_HOSTKEY_EPHEMERAL) {
            axl_printf("sshd: WARNING: the host key could not be saved (%s); it changes"
                       " at every start\n", hi.why);
        }
    }
    return 0;
}

static int
verb_stop(AxlArgs *a)
{
    const AxlServiceDeploy d = sshd_deploy();

    (void)a;
    if (!axl_service_is_running(&d)) {
        axl_printf("sshd: not running\n");
        return 0;
    }
    /* The log's line count before the stop: only an error written after
       it is this stop's reason. */
    uint64_t before = 0;
    axl_free(log_text(NULL, &before));
    if (axl_service_stop(&d) != AXL_OK) {
        size_t      len   = 0;
        uint64_t    after = before;
        char       *text  = log_text(&len, &after);
        const char *why   = NULL;
        size_t      wlen  = 0;
        axl_printerr("sshd: could not stop the server\n");
        if (text != NULL
            && sshd_bind_newest_error(text, len, (size_t)(after - before), &why, &wlen)) {
            axl_printerr("%.*s\n", (int)wlen, why);
        }
        axl_free(text);
        return 1;
    }
    axl_printf("sshd: stopped\n");
    return 0;
}

static int
verb_status(AxlArgs *a)
{
    const AxlServiceDeploy d = sshd_deploy();

    (void)a;
    bool running = axl_service_is_running(&d);
    bool taken   = axl_console_device_present();
    bool stuck   = sshd_stuck();
    char addrs[SSHD_ADDRS_MAX];

    /* Serving: where, as the start said it. The port is the running
       server's, which this command cannot see. */
    if (running && taken && !stuck) {
        char why[AXL_SERVICE_REASON_MAX + 1];
        sshd_addresses(addrs, sizeof(addrs));
        if (axl_service_state(&sshd_service) == AXL_SERVICE_DEGRADED
            && axl_service_state_reason(&sshd_service, why, sizeof(why)) == AXL_OK) {
            axl_printf("sshd: running on %s (degraded: %s)\n", addrs, why);
        } else {
            axl_printf("sshd: running on %s\n", addrs);
        }
        SshdHostKeyIface hi;
        if (host_key_running(&hi)) {
            axl_printf("sshd: host key ED25519 key fingerprint is %s.\n", hi.fingerprint);
        } else {
            axl_printf("%s", SSHD_NO_HOSTKEY_IFACE);
        }
        return 0;
    }
    axl_printf("sshd: %s\n", sshd_bind_status(running, taken, stuck));
    /* Half-stopped has the framework's own status code; degraded is up. */
    return axl_service_state(&sshd_service) == AXL_SERVICE_STOPPING
           ? AXL_SERVICE_EXIT_STOPPING : 0;
}

/* The running server's kept log, NUL-terminated, or NULL when none is
   published; axl_free() it. @p len (may be NULL) receives its length, and
   @p gen (may be NULL) the count of lines ever appended, read together. */
static char *
log_text(size_t *len, uint64_t *gen)
{
    SshdLogIface *li = NULL;

    if (axl_protocol_find_guid(&SSHD_LOG_GUID, (void **)&li) != AXL_OK || li == NULL) {
        return NULL;
    }
    char *buf = axl_malloc(SSHD_LOG_KEEP_BYTES + 1);
    if (buf == NULL) {
        return NULL;
    }
    unsigned tpl = axl_tpl_raise(AXL_TPL_NOTIFY);
    size_t   n   = *li->len < SSHD_LOG_KEEP_BYTES ? *li->len : SSHD_LOG_KEEP_BYTES;
    axl_memcpy(buf, li->text, n);
    if (gen != NULL) {
        *gen = *li->gen;
    }
    axl_tpl_restore(tpl);
    buf[n] = '\0';
    if (len != NULL) {
        *len = n;
    }
    return buf;
}

static int
verb_log(AxlArgs *a)
{
    char *text = log_text(NULL, NULL);

    (void)a;
    if (text == NULL) {
        axl_printf("sshd: not running\n");
        return 0;
    }
    axl_printf("%s", text);
    axl_free(text);
    return 0;
}

AXL_TOOL_MAIN(sshd)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "sshd",
        .help        = "Serve the live UEFI Shell over SSH (resident)",
        .help_prolog =
            "Serves the live UEFI Shell over SSH. `sshd` deploys a resident "
            "driver that takes over the console (the local display keeps "
            "painting) and returns to the Shell at once; every SSH shell "
            "session then sees and types into the same console, as if at the "
            "keyboard. A session joining late is repainted with the current "
            "screen. A session's EOF does not end it: it keeps watching until "
            "its channel closes, as a pty does. `exit` ends the Shell for "
            "everyone, as at the local console. Only interactive sessions are served (ssh -t); a "
            "command (`ssh host cmd`) is refused, because there is no private "
            "shell to run it in. The host key is kept in a UEFI variable on this "
            "machine, made at the first start (or read from a HostKey file); "
            "`sshd hostkey` prints it.",
        .help_epilog =
            "Settings come from sshd.conf beside sshd.efi (OpenSSH sshd_config "
            "keywords), if it is there; a flag given here wins over the "
            "file.\n"
            "\n"
            "Examples:\n"
            "  sshd -P demo -x 10.0.2.2/32\n"
            "  sshd -T\n"
            "  sshd -f fs0:\\my.conf\n"
            "  sshd -k fs0:\\keys\\authorized_keys -p 2222\n"
            "  sshd status\n"
            "  sshd hostkey\n"
            "  sshd hostkey --import fs0:\\ssh_host_ed25519_key\n"
            "  sshd stop\n"
            "\n"
            "Connect with: ssh -t -p <port> <any-user>@<host>",
        .flags       = sshd_flags,
        .verbs       = verbs,
        .handler     = verb_start,
    });
}
