/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * AxlTestSshdConf -- sshd.conf's keyword table (tools/sshd-conf.c), linked
 * from the very object sshd.efi ships.
 *
 * Every case goes through run(): parse the text into a fresh OpenSSH map,
 * apply it over the defaults, and return either the `sshd -T` dump joined
 * by '\n' or "<line>: <reason>" -- the exact text sshd prints after
 * "sshd.conf:".
 */

#include <axl.h>
#include "axl-test.h"
#include <axl/axl-str.h>
#include "../../tools/sshd-bind.h"
#include "../../tools/sshd-conf.h"
#include "../../tools/sshd-sftp-iface.h"

/* ---- harness ------------------------------------------------------------ */

typedef struct {
    char   buf[2048];
    size_t n;
} Dump;

static void
dump_emit(const char *line, void *ud)
{
    Dump  *d = ud;
    size_t l = axl_strlen(line);
    if (d->n + l + 2 < sizeof(d->buf)) {
        if (d->n > 0) {
            d->buf[d->n++] = '\n';
        }
        axl_memcpy(d->buf + d->n, line, l);
        d->n += l;
        d->buf[d->n] = '\0';
    }
}

static char m_out[2048];
static char m_warn[512];   /* warnings of the last run(): "<line>: <text>", '\n'-joined */

static void
warn_rec(size_t line, const char *text, void *ud)
{
    size_t n = axl_strlen(m_warn);
    (void)ud;
    axl_snprintf(m_warn + n, sizeof(m_warn) - n, "%s%zu: %s",
                 n > 0 ? "\n" : "", line, text);
}

/* The dump, or "<line>: <reason>", or "parse error" when the text does not
   tokenize (no case below means to hit that). */
static const char *
run(const char *text)
{
    AxlConfigFile *cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    SshdOpts       o;
    SshdConfError  err;
    Dump           d;

    m_out[0]  = '\0';
    m_warn[0] = '\0';
    if (cf == NULL) {
        axl_strlcpy(m_out, "no map", sizeof(m_out));
        return m_out;
    }
    if (axl_config_file_parse_string(cf, text) != AXL_OK) {
        axl_strlcpy(m_out, "parse error", sizeof(m_out));
        axl_config_file_free(cf);
        return m_out;
    }
    sshd_conf_defaults(&o);
    if (sshd_conf_apply(&o, cf, NULL, warn_rec, NULL, &err) != AXL_OK) {
        axl_snprintf(m_out, sizeof(m_out), "%zu: %s", err.line, err.reason);
    } else {
        d.n      = 0;
        d.buf[0] = '\0';
        sshd_conf_dump(&o, dump_emit, &d);
        axl_strlcpy(m_out, d.buf, sizeof(m_out));
    }
    axl_config_file_free(cf);
    return m_out;
}

/* run(), cut down to the one dump line whose keyword is @kw (a refusal is
   returned whole). */
static const char *
run_line(const char *text, const char *kw)
{
    const char *all = run(text);
    size_t      kl  = axl_strlen(kw);
    const char *p   = all;

    if (axl_strchr(all, ':') != NULL && axl_strchr(all, '\n') == NULL) {
        return all;   /* a refusal */
    }
    while (*p != '\0') {
        const char *eol = axl_strchr(p, '\n');
        size_t      len = eol != NULL ? (size_t)(eol - p) : axl_strlen(p);
        if (len > kl && axl_strncmp(p, kw, kl) == 0 && p[kl] == ' ') {
            static char line[256];
            size_t      n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
            axl_memcpy(line, p, n);
            line[n] = '\0';
            return line;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return all;
}

static void
expect(const char *got, const char *want, const char *name)
{
    bool ok = axl_strcmp(got, want) == 0;
    if (!ok) {
        axl_printf("    got:  [%s]\n    want: [%s]\n", got, want);
    }
    test_check(ok, name);
}

/* ---- the tests ------------------------------------------------------------ */

static const char DEFAULTS_DUMP[] =
    "port 22\n"
    "logingracetime 60\n"
    "maxauthtries 3\n"
    "maxsessions 10\n"
    "clientaliveinterval 60\n"
    "clientalivecountmax 3\n"
    "unusedconnectiontimeout 300\n"
    "pubkeyauthentication yes\n"
    "passwordauthentication yes\n"
    "banner none\n"
    "persourcepenaltyexemptlist none\n"
    "loglevel INFO\n"
    "authorizedkeysfile none\n"
    "maxstartups 10\n"
    "rekeylimit 1073741824 0\n"
    "persourcepenalties authfail:5 noauth:1 grace-exceeded:10 max:600 min:15\n"
    "subsystem none\n"
    "axlnic auto\n"
    "axlnetworktimeout 60\n"
    "axlpassword none\n"
    "axlreadonly no\n"
    "axlsftpreadonly no\n"
    "axlreportgeometry no\n"
    "axllogport auto\n"
    "axltcplinger 0\n"
    "hostkey uefi-variable:AxlSshHostKeyEd25519";

static void
test_defaults_dump(void)
{
    expect(run(""), DEFAULTS_DUMP, "sshd-conf: the defaults' dump, exactly");
    expect(run("# only a comment\n\n"), DEFAULTS_DUMP,
           "sshd-conf: comments and blank lines change nothing");
}

static void
test_defaults_track_axlssh(void)
{
    SshdOpts o;
    sshd_conf_defaults(&o);

    test_check(o.net.port == SSHD_PORT_DEFAULT
                   && o.net.nic_index == AXL_NET_NIC_AUTO,
               "sshd-conf defaults: port 22, NIC auto");
    test_check(o.grace_sec == AXL_SSH_LOGIN_GRACE_DEFAULT_SEC,
               "sshd-conf defaults: LoginGraceTime is AxlSsh's");
    test_check(o.max_auth == AXL_SSH_PASSWORD_FAILURES_DEFAULT,
               "sshd-conf defaults: MaxAuthTries is AxlSsh's");
    test_check(o.max_sessions == AXL_SSH_MAX_SESSIONS_DEFAULT,
               "sshd-conf defaults: MaxSessions is AxlSsh's");
    test_check(o.max_startups == AXL_SSH_MAX_STARTUPS_DEFAULT,
               "sshd-conf defaults: MaxStartups is AxlSsh's");
    test_check(o.alive_sec == AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC
                   && o.alive_max == AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX,
               "sshd-conf defaults: ClientAlive* are AxlSsh's");
    test_check(o.unused_sec == AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC,
               "sshd-conf defaults: UnusedConnectionTimeout is AxlSsh's");
    test_check(o.rekey_bytes == AXL_SSH_REKEY_LIMIT_DEFAULT,
               "sshd-conf defaults: RekeyLimit is AxlSsh's");
    test_check(o.penalties
                   && o.pen_authfail == AXL_SSH_PENALTY_AUTHFAIL_DEFAULT_SEC
                   && o.pen_noauth == AXL_SSH_PENALTY_NOAUTH_DEFAULT_SEC
                   && o.pen_grace == AXL_SSH_PENALTY_GRACE_EXCEEDED_DEFAULT_SEC
                   && o.pen_min == AXL_SSH_PENALTY_MIN_DEFAULT_SEC
                   && o.pen_max == AXL_SSH_PENALTY_MAX_DEFAULT_SEC,
               "sshd-conf defaults: PerSourcePenalties on, at AxlSsh's five");
    test_check(o.loglevel == SSHD_LOG_UNSET,
               "sshd-conf defaults: LogLevel not given (AxlLog's own level)");
    test_check(o.net_timeout_sec == SSHD_NET_TIMEOUT_SEC && SSHD_NET_TIMEOUT_SEC == 60u,
               "sshd-conf defaults: AxlNetworkTimeout is 60 s");
    test_check(o.log_port == SSHD_LOG_PORT_AUTO,
               "sshd-conf defaults: AxlLogPort auto");
    test_check(o.tcp_linger_us == AXL_TCP_PACING_LINGER_DEFAULT_US && o.tcp_linger_us == 0u,
               "sshd-conf defaults: AxlTcpLinger is AxlTcp's default, 0 (off)");
    test_check(o.sftp != NULL && axl_strcmp(o.sftp, "SftpServer.efi") == 0
                   && o.sftp_path != NULL && o.sftp_path[0] == '\0',
               "sshd-conf defaults: with no file, Subsystem sftp SftpServer.efi");
    test_check(o.pubkey && o.pwauth && !o.ro && !o.sftp_ro && !o.geometry,
               "sshd-conf defaults: both auth methods on; ro, sftp_ro, geometry off");
    test_check(o.keys != NULL && o.keys[0] == '\0'
                   && o.password != NULL && o.password[0] == '\0'
                   && o.exempt != NULL && o.exempt[0] == '\0'
                   && o.banner != NULL && o.banner[0] == '\0',
               "sshd-conf defaults: every string empty, never NULL");

    /* The descriptor defaults the launcher hands the driver: AxlConfig
       parses each default string into a struct of its own. */
    SshdOpts   c;
    axl_memset(&c, 0, sizeof c);
    AxlConfig *cfg = axl_config_new(sshd_descs, NULL, &c);
    test_check(cfg != NULL, "sshd-conf defaults: AxlConfig over sshd_descs");
    if (cfg == NULL) {
        test_skip_n(4, "no AxlConfig");
        return;
    }
    test_check(c.net.port == o.net.port && c.net.nic_index == o.net.nic_index
                   && c.ro == o.ro && c.sftp_ro == o.sftp_ro && c.geometry == o.geometry
                   && c.pubkey == o.pubkey && c.pwauth == o.pwauth
                   && c.loglevel == o.loglevel
                   && c.net_timeout_sec == o.net_timeout_sec
                   && c.log_port == o.log_port
                   && c.tcp_linger_us == o.tcp_linger_us,
               "sshd-conf defaults: descs agree (port, nic, the bools, loglevel, "
               "the network timeout, the log port, the TCP linger)");
    test_check(c.grace_sec == o.grace_sec && c.max_auth == o.max_auth
                   && c.max_sessions == o.max_sessions
                   && c.max_startups == o.max_startups
                   && c.alive_sec == o.alive_sec && c.alive_max == o.alive_max
                   && c.unused_sec == o.unused_sec
                   && c.rekey_bytes == o.rekey_bytes,
               "sshd-conf defaults: descs agree (the server limits)");
    test_check(c.penalties == o.penalties
                   && c.pen_authfail == o.pen_authfail
                   && c.pen_noauth == o.pen_noauth
                   && c.pen_grace == o.pen_grace
                   && c.pen_min == o.pen_min && c.pen_max == o.pen_max,
               "sshd-conf defaults: descs agree (the penalties)");
    test_check(c.keys != NULL && o.keys != NULL
                   && axl_strcmp(c.keys, o.keys) == 0
                   && c.password != NULL && o.password != NULL
                   && axl_strcmp(c.password, o.password) == 0
                   && c.exempt != NULL && o.exempt != NULL
                   && axl_strcmp(c.exempt, o.exempt) == 0
                   && c.banner != NULL && o.banner != NULL
                   && axl_strcmp(c.banner, o.banner) == 0
                   && c.sftp != NULL && axl_strcmp(c.sftp, o.sftp) == 0
                   && c.sftp_path != NULL && axl_strcmp(c.sftp_path, o.sftp_path) == 0
                   && c.conf != NULL && axl_strcmp(c.conf, o.conf) == 0,
               "sshd-conf defaults: descs agree (the strings)");
    axl_config_free(cfg);
}

static void
test_apply_all_keywords(void)
{
    expect(run("Port 2222\n"
               "LoginGraceTime 1m30s\n"
               "MaxAuthTries 2\n"
               "MaxSessions 4\n"
               "ClientAliveInterval 15\n"
               "ClientAliveCountMax 5\n"
               "UnusedConnectionTimeout none\n"
               "PubkeyAuthentication no\n"
               "PasswordAuthentication no\n"
               "Banner fs0:\\b.txt\n"
               "PerSourcePenaltyExemptList 10.0.2.2/32,192.168.0.0/16\n"
               "LogLevel DEBUG2\n"
               "AuthorizedKeysFile fs0:\\k\n"
               "MaxStartups 4\n"
               "RekeyLimit 512M none\n"
               "PerSourcePenalties authfail:7 max:20m\n"
               "Subsystem sftp internal-sftp\n"
               "AxlNic 1\n"
               "AxlNetworkTimeout 2m\n"
               "AxlPassword pw\n"
               "AxlReadOnly yes\n"
               "AxlSftpReadOnly yes\n"
               "AxlReportGeometry yes\n"
               "AxlLogPort 1\n"
               "AxlTcpLinger 9000\n"
               "HostKey fs0:\\h\n"),
           "port 2222\n"
           "logingracetime 90\n"
           "maxauthtries 2\n"
           "maxsessions 4\n"
           "clientaliveinterval 15\n"
           "clientalivecountmax 5\n"
           "unusedconnectiontimeout none\n"
           "pubkeyauthentication no\n"
           "passwordauthentication no\n"
           "banner fs0:\\b.txt\n"
           "persourcepenaltyexemptlist 10.0.2.2/32,192.168.0.0/16\n"
           "loglevel DEBUG2\n"
           "authorizedkeysfile fs0:\\k\n"
           "maxstartups 4\n"
           "rekeylimit 536870912 0\n"
           "persourcepenalties authfail:7 noauth:1 grace-exceeded:10 max:1200 min:15\n"
           "subsystem sftp internal-sftp\n"
           "axlnic 1\n"
           "axlnetworktimeout 120\n"
           "axlpassword set\n"
           "axlreadonly yes\n"
           "axlsftpreadonly yes\n"
           "axlreportgeometry yes\n"
           "axllogport 1\n"
           "axltcplinger 9000\n"
           "hostkey fs0:\\h",
           "sshd-conf: every keyword at a non-default, exactly");
}

static void
test_forms_accepted(void)
{
    expect(run_line("maxsessions=4\n", "maxsessions"), "maxsessions 4",
           "sshd-conf forms: lower-case keyword with '='");
    expect(run_line("MAXSESSIONS 4 # c\n", "maxsessions"), "maxsessions 4",
           "sshd-conf forms: upper-case keyword, trailing comment");
    expect(run_line("PubkeyAuthentication NO\n", "pubkeyauthentication"),
           "pubkeyauthentication no",
           "sshd-conf forms: yes/no in any case (OpenSSH's strcasecmp)");
    expect(run_line("Banner NONE\n", "banner"), "banner none",
           "sshd-conf forms: Banner's none in any case");
    expect(run_line("UnusedConnectionTimeout 0\n", "unusedconnectiontimeout"),
           "unusedconnectiontimeout none",
           "sshd-conf forms: an unused timeout of 0 prints none");
    expect(run_line("RekeyLimit default\n", "rekeylimit"),
           "rekeylimit 1073741824 0",
           "sshd-conf forms: RekeyLimit default is AxlSsh's default");
    expect(run_line("RekeyLimit 0\n", "rekeylimit"), "rekeylimit 1073741824 0",
           "sshd-conf forms: RekeyLimit 0 is OpenSSH's default, so AxlSsh's");
    expect(run_line("PerSourcePenalties no\n", "persourcepenalties"),
           "persourcepenalties no",
           "sshd-conf forms: PerSourcePenalties no");
    expect(run_line("PerSourcePenalties yes\n", "persourcepenalties"),
           "persourcepenalties authfail:5 noauth:1 grace-exceeded:10 max:600 min:15",
           "sshd-conf forms: PerSourcePenalties yes is the defaults");
    expect(run_line("PerSourcePenalties authfail:7 authfail:9\n",
                    "persourcepenalties"),
           "persourcepenalties authfail:7 noauth:1 grace-exceeded:10 max:600 min:15",
           "sshd-conf forms: a setting repeated on one line, first wins");
    expect(run_line("PerSourcePenaltyExemptList NONE\n",
                    "persourcepenaltyexemptlist"),
           "persourcepenaltyexemptlist none",
           "sshd-conf forms: the exempt list's none in any case");
    expect(run_line("AuthorizedKeysFile NONE\n", "authorizedkeysfile"),
           "authorizedkeysfile NONE",
           "sshd-conf forms: AuthorizedKeysFile's none is lower-case only (OpenSSH)");
    expect(run_line("AuthorizedKeysFile none\n", "authorizedkeysfile"),
           "authorizedkeysfile none",
           "sshd-conf forms: AuthorizedKeysFile none");
    expect(run_line("AxlNic auto\n", "axlnic"), "axlnic auto",
           "sshd-conf forms: AxlNic auto");
    expect(run_line("AxlLogPort NONE\n", "axllogport"), "axllogport none",
           "sshd-conf forms: AxlLogPort none, any case");
    expect(run_line("AxlLogPort 0\n", "axllogport"), "axllogport 0",
           "sshd-conf forms: AxlLogPort 0");

    /* The parser --log-port shares. */
    uint64_t v = 7;
    test_check(sshd_conf_log_port_parse("auto", &v) && v == SSHD_LOG_PORT_AUTO,
               "log port: auto");
    test_check(sshd_conf_log_port_parse("NONE", &v) && v == SSHD_LOG_PORT_NONE,
               "log port: none, any case");
    test_check(sshd_conf_log_port_parse("0", &v) && v == 0, "log port: 0");
    test_check(sshd_conf_log_port_parse("65535", &v) && v == 65535,
               "log port: the largest number");
    v = 7;
    test_check(!sshd_conf_log_port_parse("65536", &v) && v == 7,
               "log port: past the largest is refused, value untouched");
    test_check(!sshd_conf_log_port_parse("", &v) && !sshd_conf_log_port_parse("1x", &v)
               && !sshd_conf_log_port_parse("-1", &v) && !sshd_conf_log_port_parse("com2", &v)
               && !sshd_conf_log_port_parse("autox", &v) && v == 7,
               "log port: empty, junk, a sign and a name are refused");
    expect(run_line("AxlNetworkTimeout 1\n", "axlnetworktimeout"), "axlnetworktimeout 1",
           "sshd-conf forms: AxlNetworkTimeout's bottom is 1 s");
    expect(run_line("AxlTcpLinger 0\n", "axltcplinger"), "axltcplinger 0",
           "sshd-conf forms: AxlTcpLinger 0 (off)");
    expect(run_line("axltcplinger 20000\n", "axltcplinger"), "axltcplinger 20000",
           "sshd-conf forms: AxlTcpLinger's top is AxlTcp's 20000 us");
    expect(run_line("AxlNetworkTimeout 1d\n", "axlnetworktimeout"),
           "axlnetworktimeout 86400",
           "sshd-conf forms: AxlNetworkTimeout's top is the bring-up's one day");
    expect(run_line("ClientAliveCountMax 2147483647\n", "clientalivecountmax"),
           "clientalivecountmax 2147483647",
           "sshd-conf forms: ClientAliveCountMax's top is OpenSSH's INT_MAX");
    /* OpenSSH's names, any case; -T prints OpenSSH's own spelling (DEBUG1
       is DEBUG), as the host's sshd 9.9p1 does. */
    static const struct { const char *in; const char *want; } levels[] = {
        { "QUIET", "loglevel QUIET" },     { "fatal", "loglevel FATAL" },
        { "Error", "loglevel ERROR" },     { "info", "loglevel INFO" },
        { "VERBOSE", "loglevel VERBOSE" }, { "debug", "loglevel DEBUG" },
        { "DEBUG1", "loglevel DEBUG" },    { "DEBUG2", "loglevel DEBUG2" },
        { "debug3", "loglevel DEBUG3" },
    };
    for (size_t i = 0; i < AXL_ARRAY_SIZE(levels); i++) {
        char in[40];
        char name[80];
        axl_snprintf(in, sizeof(in), "LogLevel %s\n", levels[i].in);
        axl_snprintf(name, sizeof(name), "sshd-conf forms: LogLevel %s", levels[i].in);
        expect(run_line(in, "loglevel"), levels[i].want, name);
    }
    expect(run_line("AxlPassword \"two words\"\n", "axlpassword"),
           "axlpassword set",
           "sshd-conf forms: a quoted password is set, and never printed");
}

static void
test_first_wins(void)
{
    expect(run_line("MaxSessions 4\nMaxSessions 5\n", "maxsessions"),
           "maxsessions 4", "sshd-conf: the first value wins");
    expect(run_line("Banner a\nBanner b\n", "banner"), "banner a",
           "sshd-conf: the first value wins for a path too");
    expect(run_line("LogLevel DEBUG3\nLogLevel INFO\n", "loglevel"),
           "loglevel DEBUG3", "sshd-conf: the first LogLevel wins");
}

static void
test_dup_invalid_later_value_refused(void)
{
    expect(run("MaxAuthTries 2\nMaxAuthTries x\n"),
           "2: MaxAuthTries: \"x\" is not a number",
           "sshd-conf: a later occurrence is still validated");
    expect(run("PerSourcePenalties no\nPerSourcePenalties min:600 max:60\n"),
           "2: PerSourcePenalties: min must be below max",
           "sshd-conf: a later PerSourcePenalties is still validated");
    expect(run("LogLevel DEBUG3\nLogLevel bad\n"),
           "2: unsupported log level 'bad'",
           "sshd-conf: a later LogLevel is still validated");
}

typedef struct {
    const char *in;
    const char *want;
} Refusal;

static const Refusal REFUSALS[] = {
    { "Bogus 1\n",                     "1: Bad configuration option: Bogus" },
    { "ListenAddress 0.0.0.0\n",       "1: ListenAddress is not supported by this sshd" },
    { "Match User x\n",                "1: Match is not supported by this sshd" },
    { "Port\n",                        "1: no argument after keyword \"Port\"" },
    { "Port 22 23\n",                  "1: keyword Port extra arguments at end of line" },
    { "Port 0\n",                      "1: Port: 0 is out of range (1-65535)" },
    { "Port x\n",                      "1: Port: \"x\" is not a number" },
    { "Port 1\nPort 2\n",              "2: Port: only one Port is supported" },
    { "MaxSessions 11\n",              "1: MaxSessions: 11 is out of range (1-10)" },
    { "MaxStartups 10:30:100\n",       "1: MaxStartups: only a single number is supported" },
    { "LoginGraceTime 0\n",            "1: LoginGraceTime: 0 is out of range (1-3600)" },
    { "LoginGraceTime 5x\n",           "1: LoginGraceTime: \"5x\" is not a time (e.g. 90, 2m, 1h30m)" },
    { "PasswordAuthentication maybe\n", "1: PasswordAuthentication: \"maybe\" is not yes or no" },
    { "RekeyLimit 15\n",               "1: RekeyLimit: 15 is too small (minimum 16)" },
    { "RekeyLimit 1G 1h\n",            "1: RekeyLimit: a time limit is not supported" },
    { "RekeyLimit 1Q\n",               "1: RekeyLimit: \"1Q\" is not a size (e.g. 512M, 1G)" },
    { "UnusedConnectionTimeout 31d\n", "1: UnusedConnectionTimeout: 2678400 is out of range (0-2592000)" },
    { "PerSourcePenalties crash:90\n", "1: PerSourcePenalties: \"crash\" is not supported by this sshd" },
    { "PerSourcePenalties bogus:1\n",  "1: PerSourcePenalties: unknown setting \"bogus:1\"" },
    { "PerSourcePenalties min:600 max:60\n", "1: PerSourcePenalties: min must be below max" },
    { "PerSourcePenaltyExemptList 10.0.0.1/8\n",
      "1: PerSourcePenaltyExemptList: \"10.0.0.1/8\" is not an IPv4 address or CIDR range" },
    { "PerSourcePenaltyExemptList 10.0.0.1/32,10.0.0.2/32,10.0.0.3/32,"
      "10.0.0.4/32,10.0.0.5/32,10.0.0.6/32,10.0.0.7/32,10.0.0.8/32,"
      "10.0.0.9/32\n",
      "1: PerSourcePenaltyExemptList: at most 8 ranges" },
    { "AuthorizedKeysFile a b\n",      "1: AuthorizedKeysFile: only one file is supported" },
    { "AxlNic x\n",                    "1: AxlNic: \"x\" is not auto or a number" },
    { "AxlLogPort com2\n",             "1: AxlLogPort: \"com2\" is not auto, none or a port number" },
    { "AxlNetworkTimeout 0\n",         "1: AxlNetworkTimeout: 0 is out of range (1-86400)" },
    { "Port 22\nAxlNetworkTimeout 86401\n",
      "2: AxlNetworkTimeout: 86401 is out of range (1-86400)" },
    { "AxlNetworkTimeout soon\n",
      "1: AxlNetworkTimeout: \"soon\" is not a time (e.g. 90, 2m, 1h30m)" },
    { "AxlNetworkTimeout 30 60\n",     "1: keyword AxlNetworkTimeout extra arguments at end of line" },
    { "AxlTcpLinger 20001\n",          "1: AxlTcpLinger: 20001 is out of range (0-20000)" },
    { "AxlTcpLinger 9ms\n",            "1: AxlTcpLinger: \"9ms\" is not a number" },
    /* OpenSSH 9.9p1's own words. */
    { "LogLevel bogus\n",              "1: unsupported log level 'bogus'" },
    { "LogLevel DEBUG4\n",             "1: unsupported log level 'DEBUG4'" },
    { "LogLevel INFO extra\n",         "1: keyword LogLevel extra arguments at end of line" },
    /* Beyond the brief's table: OpenSSH's own words where it has them. */
    { "Banner \"\"\n",                 "1: keyword Banner empty argument" },
    { "port\n",                        "1: no argument after keyword \"port\"" },
    { "listenaddress 0.0.0.0\n",       "1: ListenAddress is not supported by this sshd" },
    { "Include other.conf\n",          "1: Include is not supported by this sshd" },
    { "UsePAM yes\n",                  "1: UsePAM is not supported by this sshd" },
    { "Port 65536\n",                  "1: Port: 65536 is out of range (1-65535)" },
    { "Port 99999999999999999999\n",   "1: Port integer value too large." },
    { "ClientAliveCountMax 2147483648\n", "1: ClientAliveCountMax integer value too large." },
    { "clientalivecountmax 4294967295\n", "1: clientalivecountmax integer value too large." },
    { "MaxSessions 99999999999\n",     "1: MaxSessions integer value too large." },
    /* OpenSSH's aliases follow their canonical keyword's rule. */
    { "ChallengeResponseAuthentication no\n",
      "1: KbdInteractiveAuthentication is not supported by this sshd" },
    { "PubkeyAcceptedKeyTypes ssh-ed25519\n",
      "1: PubkeyAcceptedAlgorithms is not supported by this sshd" },
    { "DSAAuthentication maybe\n",     "1: PubkeyAuthentication: \"maybe\" is not yes or no" },
    { "UseLogin\n",                    "1: no argument after keyword \"UseLogin\"" },
    { "Protocol\n",                    "1: no argument after keyword \"Protocol\"" },
    { "MaxAuthTries 0\n",              "1: MaxAuthTries: 0 is out of range (1-10)" },
    { "MaxStartups 17\n",              "1: MaxStartups: 17 is out of range (1-16)" },
    { "ClientAliveInterval 31d\n",     "1: ClientAliveInterval: 2678400 is out of range (0-2592000)" },
    { "LoginGraceTime 99999999999\n",  "1: LoginGraceTime: \"99999999999\" is not a time (e.g. 90, 2m, 1h30m)" },
    { "RekeyLimit 1G default\n",       "1: RekeyLimit: a time limit is not supported" },
    { "RekeyLimit DEFAULT\n",          "1: RekeyLimit: \"DEFAULT\" is not a size (e.g. 512M, 1G)" },
    { "PerSourcePenalties no authfail:7\n",
      "1: PerSourcePenalties: \"no\" must appear alone" },
    { "PerSourcePenalties authfail:x\n",
      "1: PerSourcePenalties: authfail: \"x\" is not a time (e.g. 90, 2m, 1h30m)" },
    { "PerSourcePenalties authfail:2d\n",
      "1: PerSourcePenalties: authfail: 172800 is out of range (0-86400)" },
    { "PerSourcePenalties min:700\n",  "1: PerSourcePenalties: min must be below max" },
    { "PerSourcePenalties authfail:0 noauth:0 grace-exceeded:0\n",
      "1: PerSourcePenalties: authfail, noauth and grace-exceeded cannot all be 0" },
    { "PerSourcePenalties refuseconnection:10\n",
      "1: PerSourcePenalties: \"refuseconnection\" is not supported by this sshd" },
    { "PerSourcePenalties authfail\n", "1: PerSourcePenalties: unknown setting \"authfail\"" },
    { "PerSourcePenaltyExemptList 10.0.0.0/8,\n",
      "1: PerSourcePenaltyExemptList: \"\" is not an IPv4 address or CIDR range" },
    { "PerSourcePenaltyExemptList 0.0.0.0/0\n",
      "1: PerSourcePenaltyExemptList: \"0.0.0.0/0\" is not an IPv4 address or CIDR range" },
    { "PerSourcePenaltyExemptList 10.0.0.0/8 192.168.0.0/16\n",
      "1: keyword PerSourcePenaltyExemptList extra arguments at end of line" },
    { "AxlReadOnly 1\n",               "1: AxlReadOnly: \"1\" is not yes or no" },
    { "AxlSftpReadOnly no no\n",
      "1: keyword AxlSftpReadOnly extra arguments at end of line" },
    { "AxlSftpReadOnly 1\n",           "1: AxlSftpReadOnly: \"1\" is not yes or no" },
    { "AxlPassword a b\n",             "1: keyword AxlPassword extra arguments at end of line" },
    { "MaxSessions 4\nBogus\n",        "2: Bad configuration option: Bogus" },
    { "Subsystem sftp\n",             "1: Missing subsystem command." },
    { "Subsystem foo /bin/foo\n",     "1: Subsystem foo: only sftp is supported" },
    { "Subsystem SFTP x\n",           "1: Subsystem SFTP: only sftp is supported" },
    { "Subsystem sftp x -l\n",
      "1: Subsystem sftp: arguments to the command are not supported" },
    { "Subsystem sftp a\nSubsystem sftp b\n",
      "2: Subsystem: only one Subsystem is supported" },
};

static void
test_refusals(void)
{
    for (size_t i = 0; i < AXL_ARRAY_SIZE(REFUSALS); i++) {
        char name[200];
        axl_snprintf(name, sizeof(name), "sshd-conf refusal: %s", REFUSALS[i].want);
        expect(run(REFUSALS[i].in), REFUSALS[i].want, name);
    }
}

/* OpenSSH 9.9p1's aliases and ignored keywords, as the host sshd treats
   them: an alias shares its canonical keyword's first-wins slot; an ignored
   keyword warns in OpenSSH's words (keyword as written) and changes
   nothing. */
static void
test_aliases_and_ignored(void)
{
    expect(run_line("DSAAuthentication no\n", "pubkeyauthentication"),
           "pubkeyauthentication no",
           "sshd-conf alias: DSAAuthentication sets PubkeyAuthentication");
    expect(run_line("DSAAuthentication no\nPubkeyAuthentication yes\n",
                    "pubkeyauthentication"),
           "pubkeyauthentication no",
           "sshd-conf alias: an alias and its keyword share one first value");

    expect(run("UseLogin yes\n"), DEFAULTS_DUMP,
           "sshd-conf ignored: UseLogin changes nothing");
    expect(m_warn, "1: Deprecated option UseLogin",
           "sshd-conf ignored: UseLogin warns in OpenSSH's words");
    expect(run_line("MaxSessions 4\nuselogin a b c\n", "maxsessions"),
           "maxsessions 4", "sshd-conf ignored: any arguments, read past");
    expect(m_warn, "2: Deprecated option uselogin",
           "sshd-conf ignored: the keyword as written, on its line");
    expect(run("AfsTokenPassing yes\nAuthorizedKeysFile2 k\n"), DEFAULTS_DUMP,
           "sshd-conf ignored: unsupported and deprecated together");
    expect(m_warn,
           "1: Unsupported option AfsTokenPassing\n"
           "2: Deprecated option AuthorizedKeysFile2",
           "sshd-conf ignored: OpenSSH's Unsupported wording, then Deprecated");
    expect(run("Protocol 2\n"), DEFAULTS_DUMP,
           "sshd-conf ignored: Protocol changes nothing");
    expect(m_warn, "", "sshd-conf ignored: Protocol is silent, as in OpenSSH");
}

/* argc == 0 is refused for EVERY supported keyword: the keywords are read
   back from the defaults' dump, so a new table row is covered without
   editing this test. */
static void
test_no_argument_every_keyword(void)
{
    char        dump[sizeof(m_out)];
    const char *p = dump;
    unsigned    rows = 0;

    axl_strlcpy(dump, run(""), sizeof(dump));
    while (*p != '\0') {
        const char *sp  = axl_strchr(p, ' ');
        const char *eol = axl_strchr(p, '\n');
        char        kw[64];
        char        in[80];
        char        want[128];
        size_t      kl  = sp != NULL ? (size_t)(sp - p) : 0;

        if (kl == 0 || kl >= sizeof(kw)) {
            break;
        }
        axl_memcpy(kw, p, kl);
        kw[kl] = '\0';
        axl_snprintf(in, sizeof(in), "%s\n", kw);
        if (axl_strcmp(kw, "hostkey") == 0) {
            /* servconf.c:1460's own words for HostKey. */
            axl_strlcpy(want, "1: HostKey: missing file name.", sizeof(want));
        } else {
            axl_snprintf(want, sizeof(want), "1: no argument after keyword \"%s\"", kw);
        }
        expect(run(in), want, want);
        rows++;
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    test_check(rows == 26, "sshd-conf no argument: all 26 keywords walked");
}

/* The origin lines Task 3 names a later read failure by, and the borrowed
   strings: they point into the map and live as long as it does. */
static void
test_origin(void)
{
    AxlConfigFile *cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    SshdOpts       o;
    SshdConfOrigin org = { 99, 99 };
    SshdConfError  err;

    test_check(cf != NULL
                   && axl_config_file_parse_string(cf,
                          "# x\nBanner b.txt\n\nAuthorizedKeysFile k\n"
                          "AuthorizedKeysFile k2\n") == AXL_OK,
               "sshd-conf origin: parse");
    sshd_conf_defaults(&o);
    test_check(sshd_conf_apply(&o, cf, &org, NULL, NULL, &err) == AXL_OK
                   && err.line == 0,
               "sshd-conf origin: applies, err line 0");
    test_check(org.banner_line == 2 && org.keys_line == 4,
               "sshd-conf origin: the FIRST occurrence's lines");
    test_check(axl_strcmp(o.banner, "b.txt") == 0
                   && axl_strcmp(o.keys, "k") == 0,
               "sshd-conf origin: the strings applied");
    axl_config_file_free(cf);

    cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    test_check(cf != NULL && axl_config_file_parse_string(cf, "Port 2\n") == AXL_OK,
               "sshd-conf origin: parse without paths");
    sshd_conf_defaults(&o);
    org.keys_line   = 99;
    org.banner_line = 99;
    test_check(sshd_conf_apply(&o, cf, &org, NULL, NULL, &err) == AXL_OK
                   && org.keys_line == 0 && org.banner_line == 0,
               "sshd-conf origin: not from the file is 0");
    axl_config_file_free(cf);
}

/* The read-only switches reach the driver: the launcher serialises SshdOpts
   through sshd_descs and the driver parses it back, so a field with no
   descriptor would arrive at its zero value whatever the file said. */
static void
test_read_only_crosses(void)
{
    static char buf[4096];
    SshdOpts    from;
    SshdOpts    to;

    sshd_conf_defaults(&from);
    from.ro      = true;
    from.sftp_ro = true;
    axl_memset(&to, 0, sizeof(to));
    AxlConfig *cfg = axl_config_new(sshd_descs, NULL, &to);
    test_check(cfg != NULL
                   && axl_config_target_to_string(sshd_descs, &from, buf, sizeof(buf)) == AXL_OK
                   && axl_config_from_string(cfg, buf) == AXL_OK
                   && to.ro && to.sftp_ro,
               "sshd-conf descs: AxlReadOnly and AxlSftpReadOnly cross to the driver");
    axl_config_free(cfg);
}

/* A setting the file does not name keeps what the caller put there: the
   layering defaults < file < flags depends on it. */
static void
test_unnamed_settings_kept(void)
{
    AxlConfigFile *cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    SshdOpts       o;
    SshdConfError  err;

    test_check(cf != NULL && axl_config_file_parse_string(cf, "MaxSessions 2\n") == AXL_OK,
               "sshd-conf kept: parse");
    sshd_conf_defaults(&o);
    o.net.port = 2022;
    o.ro       = true;
    test_check(sshd_conf_apply(&o, cf, NULL, NULL, NULL, &err) == AXL_OK
                   && o.max_sessions == 2 && o.net.port == 2022 && o.ro,
               "sshd-conf kept: an unnamed setting keeps the caller's value");
    axl_config_file_free(cf);
}

/* Subsystem: on with no file at all; a file serves sftp only if it lists
   it (sshd_config's rule); the command is taken as written; and which
   commands name the default helper. */
static void
test_subsystem(void)
{
    AxlConfigFile *cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    SshdOpts       o;
    SshdConfError  err;

    test_check(cf != NULL && axl_config_file_parse_string(cf, "Port 2\n") == AXL_OK,
               "sshd-conf subsystem: parse a file without the line");
    sshd_conf_defaults(&o);
    test_check(sshd_conf_apply(&o, cf, NULL, NULL, NULL, &err) == AXL_OK
                   && o.sftp != NULL && o.sftp[0] == '\0',
               "sshd-conf subsystem: a file without Subsystem turns SFTP off");
    axl_config_file_free(cf);

    expect(run_line("Subsystem sftp SftpServer.efi\n", "subsystem"),
           "subsystem sftp SftpServer.efi",
           "sshd-conf subsystem: the shipped line");
    expect(run_line("Subsystem sftp fs1:\\tools\\SftpServer.efi\n", "subsystem"),
           "subsystem sftp fs1:\\tools\\SftpServer.efi",
           "sshd-conf subsystem: a path, as written");
    expect(run_line("# Subsystem sftp SftpServer.efi\n", "subsystem"),
           "subsystem none",
           "sshd-conf subsystem: commented out is none");

    test_check(sftp_command_is_internal("internal-sftp")
                   && sftp_command_is_internal("Internal-SFTP")
                   && sftp_command_is_internal("SftpServer.efi")
                   && sftp_command_is_internal("sftpserver.EFI"),
               "sshd-conf subsystem: internal-sftp and SftpServer.efi, any case, are the default helper");
    test_check(!sftp_command_is_internal("fs0:\\SftpServer.efi")
                   && !sftp_command_is_internal("other.efi")
                   && !sftp_command_is_internal("")
                   && !sftp_command_is_internal(NULL),
               "sshd-conf subsystem: a path or another name is not");
}

static void
test_time_and_size(void)
{
    uint32_t t = 7;
    uint64_t z = 7;

    test_check(sshd_conf_parse_time("90", &t) == AXL_OK && t == 90, "time: 90");
    test_check(sshd_conf_parse_time("2m", &t) == AXL_OK && t == 120, "time: 2m");
    test_check(sshd_conf_parse_time("1h30m", &t) == AXL_OK && t == 5400, "time: 1h30m");
    test_check(sshd_conf_parse_time("1W", &t) == AXL_OK && t == 604800, "time: 1W");
    test_check(sshd_conf_parse_time("0", &t) == AXL_OK && t == 0, "time: 0");
    test_check(sshd_conf_parse_time("1h30", &t) == AXL_OK && t == 3630,
               "time: a unitless last group is seconds (OpenSSH: 3630)");
    test_check(sshd_conf_parse_time("2147483647", &t) == AXL_OK && t == 2147483647u,
               "time: INT_MAX is the top");
    t = 7;
    test_check(sshd_conf_parse_time("", &t) == AXL_INVALID && t == 7, "time: empty");
    test_check(sshd_conf_parse_time("m", &t) == AXL_INVALID, "time: a unit alone");
    test_check(sshd_conf_parse_time("5x", &t) == AXL_INVALID, "time: a bad unit");
    test_check(sshd_conf_parse_time("99999999999", &t) == AXL_INVALID,
               "time: overflow");
    test_check(sshd_conf_parse_time("2147483648", &t) == AXL_INVALID,
               "time: INT_MAX + 1 (OpenSSH refuses it too)");
    test_check(sshd_conf_parse_time("3550w3550w", &t) == AXL_INVALID,
               "time: a sum over INT_MAX, each group under it");
    test_check(sshd_conf_parse_time("3551w", &t) == AXL_INVALID,
               "time: one scaled group over INT_MAX");
    test_check(sshd_conf_parse_time("-1", &t) == AXL_INVALID, "time: a sign");

    test_check(sshd_conf_parse_size("16", &z) == AXL_OK && z == 16, "size: 16");
    test_check(sshd_conf_parse_size("512M", &z) == AXL_OK && z == 536870912ull,
               "size: 512M");
    test_check(sshd_conf_parse_size("1G", &z) == AXL_OK && z == 1073741824ull,
               "size: 1G");
    test_check(sshd_conf_parse_size("1k", &z) == AXL_OK && z == 1024, "size: 1k");
    z = 7;
    test_check(sshd_conf_parse_size("1Q", &z) == AXL_INVALID && z == 7, "size: 1Q");
    test_check(sshd_conf_parse_size("", &z) == AXL_INVALID, "size: empty");
    test_check(sshd_conf_parse_size("1kk", &z) == AXL_INVALID, "size: two suffixes");
    test_check(sshd_conf_parse_size("17179869184G", &z) == AXL_INVALID,
               "size: a scaled overflow");
}

static void
test_penalty_range_check(void)
{
    test_check(axl_ssh_penalty_range_check("10.0.2.2/32") == AXL_OK,
               "range check: 10.0.2.2/32");
    test_check(axl_ssh_penalty_range_check("10.0.0.0/8") == AXL_OK,
               "range check: 10.0.0.0/8");
    test_check(axl_ssh_penalty_range_check("10.0.2.2") == AXL_OK,
               "range check: a bare address");
    test_check(axl_ssh_penalty_range_check("0.0.0.0/0") == AXL_INVALID,
               "range check: /0 refused");
    test_check(axl_ssh_penalty_range_check("10.0.0.1/8") == AXL_INVALID,
               "range check: host bits below the prefix refused");
    test_check(axl_ssh_penalty_range_check("10.0.0.0/33") == AXL_INVALID,
               "range check: /33 refused");
    test_check(axl_ssh_penalty_range_check("x") == AXL_INVALID,
               "range check: not an address");
    test_check(axl_ssh_penalty_range_check(NULL) == AXL_INVALID,
               "range check: NULL");
}

/* HostKey (P5): repeatable and accumulating in order, as sshd's is --
   unlike every other keyword's first-wins -- `none` an entry that loads
   nothing, HostDSAKey its alias, at most SSHD_HOSTKEY_MAX, and `sshd -T`
   one line per entry. */
static void
test_hostkey(void)
{
    expect(run_line("HostKey a\nHostKey b\n", "hostkey"), "hostkey a",
           "HostKey: -T's first line is the first key");
    /* The defaults' dump up to its last line, which is the hostkey one. */
    char want[sizeof DEFAULTS_DUMP + 64];
    axl_strlcpy(want, DEFAULTS_DUMP, sizeof want);
    char *hk = axl_strstr(want, "\nhostkey ");
    if (hk != NULL) {
        *hk = '\0';
    }
    char want2[sizeof want];
    axl_snprintf(want2, sizeof want2, "%s\nhostkey a\nhostkey b", want);
    expect(run("HostKey a\nHostKey b\n"), want2,
           "HostKey: both kept, in file order, one -T line each");
    axl_snprintf(want2, sizeof want2, "%s\nhostkey x", want);
    expect(run("HostDSAKey x\n"), want2, "HostKey: HostDSAKey is its alias");
    expect(run("HostKey none\n"), want,
           "HostKey: none loads nothing and prints no -T line");
    expect(run("HostKey a b\n"), "1: keyword HostKey extra arguments at end of line",
           "HostKey: one path per line");
    expect(run("HostKey a|b\n"), "1: HostKey: a path may not contain '|'",
           "HostKey: the list separator is refused in a path");
    expect(run("HostKey 1\nHostKey 2\nHostKey 3\nHostKey 4\nHostKey 5\n"
               "HostKey 6\nHostKey 7\nHostKey 8\nHostKey 9\n"),
           "9: HostKey: at most 8 host keys", "HostKey: a ninth is refused");

    AxlConfigFile *cf = axl_config_file_new_syntax(AXL_CONFIG_FILE_OPENSSH);
    SshdOpts       o;
    SshdConfOrigin origin;
    sshd_conf_defaults(&o);
    test_check(cf != NULL
               && axl_config_file_parse_string(cf, "Port 2\nHostKey a\n\nHostKey none\n")
                      == AXL_OK
               && sshd_conf_apply(&o, cf, &origin, NULL, NULL, NULL) == AXL_OK
               && axl_strcmp(o.host_keys, "a|none") == 0
               && origin.hostkey_count == 2 && origin.hostkey_line[0] == 2
               && origin.hostkey_line[1] == 4,
               "HostKey: the list and each entry's line");
    axl_config_file_free(cf);
}

static int
test_sshd_conf_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    test_print_header("SshdConf");

    test_defaults_dump();
    test_defaults_track_axlssh();
    test_apply_all_keywords();
    test_forms_accepted();
    test_first_wins();
    test_dup_invalid_later_value_refused();
    test_refusals();
    test_aliases_and_ignored();
    test_no_argument_every_keyword();
    test_origin();
    test_read_only_crosses();
    test_unnamed_settings_kept();
    test_subsystem();
    test_time_and_size();
    test_penalty_range_check();
    test_hostkey();

    return test_print_results();
}

AXL_APP(test_sshd_conf_main)
