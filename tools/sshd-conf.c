/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-conf.c - sshd.conf's keyword table: OpenSSH sshd_config(5) keywords
 * onto SshdOpts, the refusals sshd prints, and the `sshd -T` dump.
 *
 * One row per keyword. A row's KIND gives the generic parse and print (an
 * integer, a time, yes/no, a path or `none`, a hidden string); the few
 * keywords whose grammar is their own (RekeyLimit, PerSourcePenalties, the
 * exempt list, MaxStartups, AxlNic) add an apply and/or show handler. The
 * checks every keyword shares -- an argument at all, no empty argument, not
 * too many -- run before any row's handler, in OpenSSH's own words.
 *
 * Validation of a later occurrence runs the SAME handler against a scratch
 * copy of the options, so "validated" and "applied" cannot drift apart.
 *
 * Where OpenSSH's grammar is wider than what AxlSsh can do (a time-based
 * RekeyLimit, MaxStartups' start:rate:full, several AuthorizedKeysFiles),
 * the value is refused with a reason that says so, rather than accepted and
 * ignored: a setting the operator wrote must either work or stop sshd.
 */

#include <axl.h>
#include <axl/axl-str.h>

#include "sshd-bind.h"
#include "sshd-conf.h"
#include "sshd-sftp-iface.h"

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/** OpenSSH's convtime() bound: a time must fit in an int. */
#define SC_TIME_MAX          2147483647u

/** OpenSSH's integer bound (an int): above it, "integer value too large." */
#define SC_INT_MAX           2147483647u

/** One `sshd -T` line: a keyword plus a value, the longest of which is a
    path from one config line. */
#define SC_DUMP_LINE_MAX     (AXL_CONFIG_FILE_LINE_MAX + 64u)

/** The example every time refusal ends with. */
#define SC_TIME_EG           "(e.g. 90, 2m, 1h30m)"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** The generic parse and print a row gets unless it brings its own. */
typedef enum {
    SC_INT,        ///< decimal integer in [min, max]
    SC_TIME,       ///< OpenSSH time in [min, max] seconds
    SC_TIME_NONE,  ///< `none` (any case; stored as 0) or a time in [min, max]
    SC_BOOL,       ///< yes or no, any case
    SC_PATH,       ///< `none` (stored as "") or one path
    SC_SECRET,     ///< one string; printed only as `set` or `none`
    SC_OWN,        ///< apply and show are the row's own
} ScKind;

typedef struct ScRow ScRow;

/** Parse @a e into @a o, or fill @a err. */
typedef int (*ScApply)(const ScRow *row, const AxlConfigFileEntry *e,
                       SshdOpts *o, SshdConfError *err);

/** Print @a o's value for @a row (no keyword) into @a out. */
typedef void (*ScShow)(const ScRow *row, const SshdOpts *o, char *out,
                       size_t cap);

struct ScRow {
    const char *keyword;   ///< OpenSSH's spelling; -T prints it lower-cased
    ScKind      kind;
    size_t      max_args;  ///< arguments allowed; 0 = the handler counts
    const char *too_many;  ///< reason past max_args, or NULL for OpenSSH's own
    bool        once;      ///< a second occurrence is refused
    bool        none_cs;   ///< SC_PATH: only lower-case `none` means none
    uint64_t    min;       ///< SC_INT / SC_TIME*: inclusive range
    uint64_t    max;
    size_t      offset;    ///< offsetof(SshdOpts, field)
    size_t      width;     ///< sizeof the field, for SC_INT / SC_TIME*
    ScApply     apply;     ///< non-NULL: replaces the kind's parse
    ScShow      show;      ///< non-NULL: replaces the kind's print
};

/** One PerSourcePenalties setting AxlSsh has. */
typedef struct {
    const char *name;
    size_t      offset;    ///< offsetof(SshdOpts, pen_*)
} ScPenalty;

/** An old OpenSSH spelling that OpenSSH still reads as another keyword. */
typedef struct {
    const char *alias;
    const char *canonical;
} ScAlias;

/** A keyword OpenSSH reads and IGNORES, with the warning it prints. */
typedef struct {
    const char *keyword;
    const char *why;       ///< "Deprecated" or "Unsupported"; NULL = silent
} ScIgnored;

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static int  refuse(SshdConfError *err, size_t line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static bool parse_dec(const char *s, uint64_t *out, bool *big);
static void put_uint(SshdOpts *o, const ScRow *row, uint64_t v);
static uint64_t get_uint(const SshdOpts *o, const ScRow *row);
static int  check_range(const ScRow *row, const AxlConfigFileEntry *e,
                        uint64_t v, SshdConfError *err);
static int  apply_generic(const ScRow *row, const AxlConfigFileEntry *e,
                          SshdOpts *o, SshdConfError *err);
static void show_generic(const ScRow *row, const SshdOpts *o, char *out,
                         size_t cap);
static int  apply_max_startups(const ScRow *row, const AxlConfigFileEntry *e,
                               SshdOpts *o, SshdConfError *err);
static int  apply_rekey(const ScRow *row, const AxlConfigFileEntry *e,
                        SshdOpts *o, SshdConfError *err);
static void show_rekey(const ScRow *row, const SshdOpts *o, char *out,
                       size_t cap);
static int  apply_penalties(const ScRow *row, const AxlConfigFileEntry *e,
                            SshdOpts *o, SshdConfError *err);
static void show_penalties(const ScRow *row, const SshdOpts *o, char *out,
                           size_t cap);
static int  apply_exempt(const ScRow *row, const AxlConfigFileEntry *e,
                         SshdOpts *o, SshdConfError *err);
static int  apply_nic(const ScRow *row, const AxlConfigFileEntry *e,
                      SshdOpts *o, SshdConfError *err);
static void show_nic(const ScRow *row, const SshdOpts *o, char *out,
                     size_t cap);
static int  apply_log_port(const ScRow *row, const AxlConfigFileEntry *e,
                            SshdOpts *o, SshdConfError *err);
static void show_log_port(const ScRow *row, const SshdOpts *o, char *out,
                          size_t cap);
static int  apply_loglevel(const ScRow *row, const AxlConfigFileEntry *e,
                           SshdOpts *o, SshdConfError *err);
static int  apply_subsystem(const ScRow *row, const AxlConfigFileEntry *e,
                            SshdOpts *o, SshdConfError *err);
static void show_subsystem(const ScRow *row, const SshdOpts *o, char *out,
                           size_t cap);
static void show_loglevel(const ScRow *row, const SshdOpts *o, char *out,
                          size_t cap);
static int  apply_hostkey(const ScRow *row, const AxlConfigFileEntry *e,
                          SshdOpts *o, SshdConfError *err);
static const ScRow *find_row(const char *keyword);
static const char  *find_unsupported(const char *keyword);
static const char  *find_alias(const char *keyword);
static const ScIgnored *find_ignored(const char *keyword);
static int  apply_entry(const AxlConfigFileEntry *e, SshdOpts *o,
                        bool *seen, SshdConfOrigin *origin,
                        SshdConfWarn warn, void *warn_ud,
                        SshdConfError *err);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/** The HostKey paths as written, SSHD_HOSTKEY_SEP-joined: what
    SshdOpts::host_keys points at once a HostKey is read. Reset by each
    sshd_conf_apply(). */
static char m_hostkeys[SSHD_HOSTKEY_MAX * (AXL_CONFIG_FILE_LINE_MAX + 1u)];

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

#define SC_FIELD(f)  offsetof(SshdOpts, f), sizeof(((SshdOpts *)0)->f)

/** The keyword table, in `sshd -T` order. */
static const ScRow m_rows[] = {
    { "Port",                       SC_INT,       1, NULL, true,  false,
      1, 65535,                     SC_FIELD(net.port),     NULL, NULL },
    { "LoginGraceTime",             SC_TIME,      1, NULL, false, false,
      1, AXL_SSH_LOGIN_GRACE_MAX_SEC, SC_FIELD(grace_sec),  NULL, NULL },
    { "MaxAuthTries",               SC_INT,       1, NULL, false, false,
      1, AXL_SSH_PASSWORD_FAILURES_MAX, SC_FIELD(max_auth), NULL, NULL },
    { "MaxSessions",                SC_INT,       1, NULL, false, false,
      1, AXL_SSH_MAX_SESSIONS_MAX,  SC_FIELD(max_sessions), NULL, NULL },
    { "ClientAliveInterval",        SC_TIME,      1, NULL, false, false,
      0, AXL_SSH_IDLE_MAX_SEC,      SC_FIELD(alive_sec),    NULL, NULL },
    { "ClientAliveCountMax",        SC_INT,       1, NULL, false, false,
      0, SC_INT_MAX,                SC_FIELD(alive_max),    NULL, NULL },
    { "UnusedConnectionTimeout",    SC_TIME_NONE, 1, NULL, false, false,
      0, AXL_SSH_IDLE_MAX_SEC,      SC_FIELD(unused_sec),   NULL, NULL },
    { "PubkeyAuthentication",       SC_BOOL,      1, NULL, false, false,
      0, 0,                         SC_FIELD(pubkey),       NULL, NULL },
    { "PasswordAuthentication",     SC_BOOL,      1, NULL, false, false,
      0, 0,                         SC_FIELD(pwauth),       NULL, NULL },
    { "Banner",                     SC_PATH,      1, NULL, false, false,
      0, 0,                         SC_FIELD(banner),       NULL, NULL },
    { "PerSourcePenaltyExemptList", SC_PATH,      1, NULL, false, false,
      0, 0,                         SC_FIELD(exempt),       apply_exempt, NULL },
    { "LogLevel",                   SC_OWN,       1, NULL, false, false,
      0, 0,                         SC_FIELD(loglevel),
      apply_loglevel, show_loglevel },
    { "AuthorizedKeysFile",         SC_PATH,      1,
      "only one file is supported",            false, true,
      0, 0,                         SC_FIELD(keys),         NULL, NULL },
    { "MaxStartups",                SC_INT,       1, NULL, false, false,
      1, AXL_SSH_MAX_STARTUPS_MAX,  SC_FIELD(max_startups),
      apply_max_startups, NULL },
    { "RekeyLimit",                 SC_OWN,       2, NULL, false, false,
      0, 0,                         SC_FIELD(rekey_bytes),
      apply_rekey, show_rekey },
    { "PerSourcePenalties",         SC_OWN,       0, NULL, false, false,
      0, 0,                         SC_FIELD(penalties),
      apply_penalties, show_penalties },
    { "Subsystem",                  SC_OWN,       0, NULL, true,  false,
      0, 0,                         SC_FIELD(sftp),
      apply_subsystem, show_subsystem },
    { "AxlNic",                     SC_OWN,       1, NULL, false, false,
      0, 0,                         SC_FIELD(net.nic_index),
      apply_nic, show_nic },
    { "AxlNetworkTimeout",          SC_TIME,      1, NULL, false, false,
      1, AXL_NET_AUTO_TIMEOUT_MAX_SEC, SC_FIELD(net_timeout_sec), NULL, NULL },
    { "AxlPassword",                SC_SECRET,    1, NULL, false, false,
      0, 0,                         SC_FIELD(password),     NULL, NULL },
    { "AxlReadOnly",                SC_BOOL,      1, NULL, false, false,
      0, 0,                         SC_FIELD(ro),           NULL, NULL },
    { "AxlSftpReadOnly",            SC_BOOL,      1, NULL, false, false,
      0, 0,                         SC_FIELD(sftp_ro),      NULL, NULL },
    { "AxlReportGeometry",          SC_BOOL,      1, NULL, false, false,
      0, 0,                         SC_FIELD(geometry),     NULL, NULL },
    { "AxlLogPort",                 SC_OWN,       1, NULL, false, false,
      0, 0,                         SC_FIELD(log_port),
      apply_log_port, show_log_port },
    { "AxlTcpLinger",               SC_INT,       1, NULL, false, false,
      0, AXL_TCP_PACING_LINGER_MAX_US, SC_FIELD(tcp_linger_us), NULL, NULL },
    /* Repeatable and accumulating, in order (servconf.c:1457-1466), unlike
       every other row's first-wins: apply_entry gives it every occurrence.
       sshd_conf_dump prints it one line per entry. */
    { "HostKey",                    SC_OWN,       1, NULL, false, false,
      0, 0,                         SC_FIELD(host_keys),    apply_hostkey, NULL },
};

#define SC_ROWS  AXL_ARRAY_SIZE(m_rows)

/**
 * Every other keyword OpenSSH 9.9p1's sshd_config(5) documents (the build
 * this was taken from carries its distribution's GSSAPI/Kerberos keywords
 * too), sorted. sshd refuses each by name as not supported rather than as
 * unknown: the operator wrote a real sshd keyword, and should be told it is
 * this sshd that lacks it, not their spelling.
 */
static const char *const m_unsupported[] = {
    "AcceptEnv", "AddressFamily", "AllowAgentForwarding", "AllowGroups",
    "AllowStreamLocalForwarding", "AllowTcpForwarding", "AllowUsers",
    "AuthenticationMethods", "AuthorizedKeysCommand",
    "AuthorizedKeysCommandUser", "AuthorizedPrincipalsCommand",
    "AuthorizedPrincipalsCommandUser", "AuthorizedPrincipalsFile",
    "CanonicalMatchUser", "CASignatureAlgorithms", "ChannelTimeout",
    "ChrootDirectory", "Ciphers", "Compression", "DenyGroups", "DenyUsers",
    "DisableForwarding", "ExposeAuthInfo", "FingerprintHash", "ForceCommand",
    "GatewayPorts", "GSSAPIAuthentication", "GSSAPICleanupCredentials",
    "GSSAPIDelegateCredentials", "GSSAPIEnablek5users", "GSSAPIIndicators",
    "GSSAPIKexAlgorithms", "GSSAPIKeyExchange",
    "GSSAPIStoreCredentialsOnRekey", "GSSAPIStrictAcceptorCheck",
    "HostbasedAcceptedAlgorithms", "HostbasedAuthentication",
    "HostbasedUsesNameFromPacketOnly", "HostCertificate",
    "HostKeyAgent", "HostKeyAlgorithms", "IgnoreRhosts",
    "IgnoreUserKnownHosts", "Include", "IPQoS",
    "KbdInteractiveAuthentication", "KerberosAuthentication",
    "KerberosGetAFSToken", "KerberosOrLocalPasswd", "KerberosTicketCleanup",
    "KerberosUniqueCCache", "KerberosUseKuserok", "KexAlgorithms",
    "ListenAddress", "LogVerbose", "MACs", "Match", "ModuliFile",
    "PAMServiceName", "PermitEmptyPasswords", "PermitListen", "PermitOpen",
    "PermitRootLogin", "PermitTTY", "PermitTunnel", "PermitUserEnvironment",
    "PermitUserRC", "PerSourceMaxStartups", "PerSourceNetBlockSize",
    "PidFile", "PrintLastLog", "PrintMotd", "PubkeyAcceptedAlgorithms",
    "PubkeyAuthOptions", "RDomain", "RefuseConnection", "RequiredRSASize",
    "RevokedKeys", "SecurityKeyProvider", "SetEnv", "SshdSessionPath",
    "StreamLocalBindMask", "StreamLocalBindUnlink", "StrictModes",
    "SyslogFacility", "TCPKeepAlive", "TrustedUserCAKeys",
    "UseDNS", "UsePAM", "VersionAddendum", "X11DisplayOffset",
    "X11Forwarding", "X11MaxDisplays", "X11UseLocalhost", "XAuthLocation",
};

/** PerSourcePenalties settings AxlSsh has. */
static const ScPenalty m_penalties[] = {
    { "authfail",       offsetof(SshdOpts, pen_authfail) },
    { "noauth",         offsetof(SshdOpts, pen_noauth) },
    { "grace-exceeded", offsetof(SshdOpts, pen_grace) },
    { "max",            offsetof(SshdOpts, pen_max) },
    { "min",            offsetof(SshdOpts, pen_min) },
};

/**
 * OpenSSH 9.9p1's aliases: the old name is read as the new keyword, sharing
 * its first-wins slot, so it follows that keyword's rule here too (applied,
 * or "<canonical> is not supported"). Taken from the host sshd binary's
 * behaviour, not its man page, which documents none of them.
 */
static const ScAlias m_aliases[] = {
    { "ChallengeResponseAuthentication", "KbdInteractiveAuthentication" },
    { "DSAAuthentication",               "PubkeyAuthentication" },
    { "GSSAPICleanupCreds",              "GSSAPICleanupCredentials" },
    { "HostbasedAcceptedKeyTypes",       "HostbasedAcceptedAlgorithms" },
    { "HostDSAKey",                      "HostKey" },
    { "KeepAlive",                       "TCPKeepAlive" },
    { "PubkeyAcceptedKeyTypes",          "PubkeyAcceptedAlgorithms" },
    { "RSAMinSize",                      "RequiredRSASize" },
    { "SkeyAuthentication",              "KbdInteractiveAuthentication" },
};

/**
 * Keywords OpenSSH 9.9p1 accepts and ignores, warning "<why> option
 * <keyword as written>" (Protocol silently). A file written for an older
 * or differently-built sshd keeps working, as it does there. Same source
 * as m_aliases.
 */
static const ScIgnored m_ignored[] = {
    { "AFSTokenPassing",            "Unsupported" },
    { "AuthorizedKeysFile2",        "Deprecated" },
    { "CheckMail",                  "Deprecated" },
    { "GSSAPIUseSessionCredCache",  "Unsupported" },
    { "GSSUseSessionCCache",        "Unsupported" },
    { "KerberosTgtPassing",         "Unsupported" },
    { "KeyRegenerationInterval",    "Deprecated" },
    { "PAMAuthenticationViaKbdInt", "Deprecated" },
    { "Protocol",                   NULL },
    { "ReverseMappingCheck",        "Deprecated" },
    { "RhostsAuthentication",       "Deprecated" },
    { "RhostsRSAAuthentication",    "Deprecated" },
    { "RSAAuthentication",          "Deprecated" },
    { "ServerKeyBits",              "Deprecated" },
    { "UseLogin",                   "Deprecated" },
    { "UsePrivilegeSeparation",     "Deprecated" },
    { "VerifyReverseMapping",       "Deprecated" },
};

/** LogLevel's names, indexed by SshdLogLevel, as `sshd -T` prints them:
    OpenSSH's log_level_name() gives DEBUG1 as DEBUG. */
static const char *const m_log_names[] = {
    [SSHD_LOG_QUIET]   = "QUIET",
    [SSHD_LOG_FATAL]   = "FATAL",
    [SSHD_LOG_ERROR]   = "ERROR",
    [SSHD_LOG_INFO]    = "INFO",
    [SSHD_LOG_VERBOSE] = "VERBOSE",
    [SSHD_LOG_DEBUG1]  = "DEBUG",
    [SSHD_LOG_DEBUG2]  = "DEBUG2",
    [SSHD_LOG_DEBUG3]  = "DEBUG3",
};

/** PerSourcePenalties settings OpenSSH 9.9p1 has and AxlSsh does not. */
static const char *const m_penalties_unsupported[] = {
    "crash", "refuseconnection", "max-sources4", "max-sources6",
    "overflow", "overflow6",
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int
refuse(SshdConfError *err, size_t line, const char *fmt, ...)
{
    va_list ap;
    err->line = line;
    va_start(ap, fmt);
    axl_vsnprintf(err->reason, sizeof err->reason, fmt, ap);
    va_end(ap);
    return AXL_INVALID;
}

/* Decimal digits only (no sign, no space, no hex). A value past 64 bits is
   still "a number", reported through @big so the caller can say it is out
   of range rather than malformed. */
static bool
parse_dec(const char *s, uint64_t *out, bool *big)
{
    uint64_t v = 0;

    *big = false;
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!axl_isdigit((unsigned char)*s)) {
            return false;
        }
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            *big = true;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

static void
put_uint(SshdOpts *o, const ScRow *row, uint64_t v)
{
    char *p = (char *)o + row->offset;
    if (row->width == sizeof(uint16_t)) {
        uint16_t x = (uint16_t)v;
        axl_memcpy(p, &x, sizeof x);
    } else if (row->width == sizeof(uint32_t)) {
        uint32_t x = (uint32_t)v;
        axl_memcpy(p, &x, sizeof x);
    } else {
        axl_memcpy(p, &v, sizeof v);
    }
}

static uint64_t
get_uint(const SshdOpts *o, const ScRow *row)
{
    const char *p = (const char *)o + row->offset;
    if (row->width == sizeof(uint16_t)) {
        uint16_t x;
        axl_memcpy(&x, p, sizeof x);
        return x;
    }
    if (row->width == sizeof(uint32_t)) {
        uint32_t x;
        axl_memcpy(&x, p, sizeof x);
        return x;
    }
    uint64_t x;
    axl_memcpy(&x, p, sizeof x);
    return x;
}

static int
check_range(const ScRow *row, const AxlConfigFileEntry *e, uint64_t v,
            SshdConfError *err)
{
    if (v < row->min || v > row->max) {
        return refuse(err, e->line, "%s: %llu is out of range (%llu-%llu)",
                      row->keyword, (unsigned long long)v,
                      (unsigned long long)row->min,
                      (unsigned long long)row->max);
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Generic kinds
// ---------------------------------------------------------------------------

static int
apply_generic(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
              SshdConfError *err)
{
    const char *a = e->argv[0];
    char       *f = (char *)o + row->offset;

    switch (row->kind) {
    case SC_INT: {
        uint64_t v   = 0;
        bool     big = false;
        if (!parse_dec(a, &v, &big)) {
            return refuse(err, e->line, "%s: \"%s\" is not a number",
                          row->keyword, a);
        }
        /* OpenSSH parses every integer keyword as an int, in these words
           and naming the keyword as written. */
        if (big || v > SC_INT_MAX) {
            return refuse(err, e->line, "%s integer value too large.",
                          e->keyword);
        }
        if (check_range(row, e, v, err) != AXL_OK) {
            return AXL_INVALID;
        }
        put_uint(o, row, v);
        return AXL_OK;
    }
    case SC_TIME:
    case SC_TIME_NONE: {
        uint32_t t = 0;
        if (row->kind == SC_TIME_NONE && axl_strcasecmp(a, "none") == 0) {
            put_uint(o, row, 0);
            return AXL_OK;
        }
        if (sshd_conf_parse_time(a, &t) != AXL_OK) {
            return refuse(err, e->line, "%s: \"%s\" is not a time " SC_TIME_EG,
                          row->keyword, a);
        }
        if (check_range(row, e, t, err) != AXL_OK) {
            return AXL_INVALID;
        }
        put_uint(o, row, t);
        return AXL_OK;
    }
    case SC_BOOL: {
        bool b;
        if (axl_strcasecmp(a, "yes") == 0) {
            b = true;
        } else if (axl_strcasecmp(a, "no") == 0) {
            b = false;
        } else {
            return refuse(err, e->line, "%s: \"%s\" is not yes or no",
                          row->keyword, a);
        }
        axl_memcpy(f, &b, sizeof b);
        return AXL_OK;
    }
    case SC_PATH: {
        bool        none = row->none_cs ? axl_strcmp(a, "none") == 0
                                        : axl_strcasecmp(a, "none") == 0;
        const char *v    = none ? "" : a;
        axl_memcpy(f, &v, sizeof v);
        return AXL_OK;
    }
    case SC_SECRET:
        if (axl_strcmp(a, SSHD_CONF_PASSWORD_PLACEHOLDER) == 0) {
            return refuse(err, e->line,
                          "%s: replace the placeholder with a password",
                          row->keyword);
        }
        axl_memcpy(f, &a, sizeof a);
        return AXL_OK;
    case SC_OWN:
        break;
    }
    return refuse(err, e->line, "%s: no parser", row->keyword);
}

static void
show_generic(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    const char *f = (const char *)o + row->offset;

    switch (row->kind) {
    case SC_INT:
    case SC_TIME:
        axl_snprintf(out, cap, "%llu", (unsigned long long)get_uint(o, row));
        return;
    case SC_TIME_NONE: {
        uint64_t v = get_uint(o, row);
        if (v == 0) {
            axl_strlcpy(out, "none", cap);
        } else {
            axl_snprintf(out, cap, "%llu", (unsigned long long)v);
        }
        return;
    }
    case SC_BOOL: {
        bool b;
        axl_memcpy(&b, f, sizeof b);
        axl_strlcpy(out, b ? "yes" : "no", cap);
        return;
    }
    case SC_PATH:
    case SC_SECRET: {
        const char *s;
        axl_memcpy(&s, f, sizeof s);
        bool empty = s == NULL || s[0] == '\0';
        if (row->kind == SC_SECRET) {
            axl_strlcpy(out, empty ? "none" : "set", cap);
        } else {
            axl_strlcpy(out, empty ? "none" : s, cap);
        }
        return;
    }
    case SC_OWN:
        break;
    }
    out[0] = '\0';
}

// ---------------------------------------------------------------------------
// Keywords with a grammar of their own
// ---------------------------------------------------------------------------

/* AxlSsh's cap is one number; OpenSSH's start:rate:full random early drop is
   not implemented, so it is refused rather than read as its first field. */
static int
apply_max_startups(const ScRow *row, const AxlConfigFileEntry *e,
                   SshdOpts *o, SshdConfError *err)
{
    if (axl_strchr(e->argv[0], ':') != NULL) {
        return refuse(err, e->line, "%s: only a single number is supported",
                      row->keyword);
    }
    return apply_generic(row, e, o, err);
}

/* `default` or a size, then optionally `none` -- OpenSSH's own spellings,
   case-sensitive as OpenSSH's are. 0 is OpenSSH's "default" too, so both
   become AxlSsh's default: -T prints the value in force, exactly as it does
   when the file has no RekeyLimit (OpenSSH prints its `0` sentinel for all
   three). sshd.conf cannot turn byte rekeying off; OpenSSH cannot either. */
static int
apply_rekey(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
            SshdConfError *err)
{
    const char *a = e->argv[0];
    uint64_t    v = 0;

    if (axl_strcmp(a, "default") == 0) {
        v = 0;
    } else if (sshd_conf_parse_size(a, &v) != AXL_OK) {
        return refuse(err, e->line, "%s: \"%s\" is not a size (e.g. 512M, 1G)",
                      row->keyword, a);
    } else if (v != 0 && v < AXL_SSH_REKEY_LIMIT_MIN) {
        return refuse(err, e->line, "%s: %llu is too small (minimum %u)",
                      row->keyword, (unsigned long long)v,
                      (unsigned)AXL_SSH_REKEY_LIMIT_MIN);
    }
    if (e->argc == 2 && axl_strcmp(e->argv[1], "none") != 0) {
        return refuse(err, e->line, "%s: a time limit is not supported",
                      row->keyword);
    }
    o->rekey_bytes = v != 0 ? v : AXL_SSH_REKEY_LIMIT_DEFAULT;
    return AXL_OK;
}

static void
show_rekey(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    axl_snprintf(out, cap, "%llu 0", (unsigned long long)o->rekey_bytes);
}

/* `yes`, `no` (alone), or name:time settings. Each line is a whole
   specification, as in OpenSSH: a setting it does not name takes its
   default, not the value an earlier line gave. Within a line a repeated
   setting keeps its first value, as OpenSSH's does. */
static int
apply_penalties(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
                SshdConfError *err)
{
    uint32_t vals[AXL_ARRAY_SIZE(m_penalties)];
    bool     set[AXL_ARRAY_SIZE(m_penalties)];
    SshdOpts d;

    sshd_conf_defaults(&d);
    for (size_t k = 0; k < AXL_ARRAY_SIZE(m_penalties); k++) {
        axl_memcpy(&vals[k], (const char *)&d + m_penalties[k].offset,
                   sizeof vals[k]);
        set[k] = false;
    }

    for (size_t i = 0; i < e->argc; i++) {
        const char *a     = e->argv[i];
        const char *colon = axl_strchr(a, ':');

        if (axl_strcasecmp(a, "no") == 0) {
            if (e->argc != 1) {
                return refuse(err, e->line, "%s: \"no\" must appear alone",
                              row->keyword);
            }
            o->penalties = false;
            return AXL_OK;
        }
        if (axl_strcasecmp(a, "yes") == 0) {
            continue;
        }
        if (colon == NULL) {
            return refuse(err, e->line, "%s: unknown setting \"%s\"",
                          row->keyword, a);
        }
        size_t nlen = (size_t)(colon - a);
        size_t k    = 0;
        while (k < AXL_ARRAY_SIZE(m_penalties)
               && !(axl_strlen(m_penalties[k].name) == nlen
                    && axl_strncmp(m_penalties[k].name, a, nlen) == 0)) {
            k++;
        }
        if (k == AXL_ARRAY_SIZE(m_penalties)) {
            for (size_t u = 0; u < AXL_ARRAY_SIZE(m_penalties_unsupported); u++) {
                const char *n = m_penalties_unsupported[u];
                if (axl_strlen(n) == nlen && axl_strncmp(n, a, nlen) == 0) {
                    return refuse(err, e->line,
                                  "%s: \"%s\" is not supported by this sshd",
                                  row->keyword, n);
                }
            }
            return refuse(err, e->line, "%s: unknown setting \"%s\"",
                          row->keyword, a);
        }
        uint32_t t = 0;
        if (sshd_conf_parse_time(colon + 1, &t) != AXL_OK) {
            return refuse(err, e->line,
                          "%s: %s: \"%s\" is not a time " SC_TIME_EG,
                          row->keyword, m_penalties[k].name, colon + 1);
        }
        if (t > AXL_SSH_PENALTY_FIELD_MAX_SEC) {
            return refuse(err, e->line, "%s: %s: %u is out of range (0-%u)",
                          row->keyword, m_penalties[k].name, (unsigned)t,
                          (unsigned)AXL_SSH_PENALTY_FIELD_MAX_SEC);
        }
        if (!set[k]) {
            vals[k] = t;
            set[k]  = true;
        }
    }

    for (size_t k = 0; k < AXL_ARRAY_SIZE(m_penalties); k++) {
        axl_memcpy((char *)o + m_penalties[k].offset, &vals[k], sizeof vals[k]);
    }
    /* axl_ssh_server_set_penalties()'s two silently-inert combinations,
       refused here at their line instead of by the server at start. */
    if (o->pen_min >= o->pen_max) {
        return refuse(err, e->line, "%s: min must be below max", row->keyword);
    }
    if (o->pen_authfail == 0 && o->pen_noauth == 0 && o->pen_grace == 0) {
        return refuse(err, e->line,
                      "%s: authfail, noauth and grace-exceeded cannot all be 0",
                      row->keyword);
    }
    o->penalties = true;
    return AXL_OK;
}

static void
show_penalties(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    if (!o->penalties) {
        axl_strlcpy(out, "no", cap);
        return;
    }
    axl_snprintf(out, cap, "authfail:%u noauth:%u grace-exceeded:%u max:%u min:%u",
                 (unsigned)o->pen_authfail, (unsigned)o->pen_noauth,
                 (unsigned)o->pen_grace, (unsigned)o->pen_max,
                 (unsigned)o->pen_min);
}

/* `none`, or ONE comma-joined argument (OpenSSH's shape), each range judged
   by axl_ssh_penalty_range_check() -- the rule the server applies. */
static int
apply_exempt(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
             SshdConfError *err)
{
    const char *a = e->argv[0];
    size_t      n = 1;

    if (axl_strcasecmp(a, "none") == 0) {
        o->exempt = "";
        return AXL_OK;
    }
    for (const char *p = a; *p != '\0'; p++) {
        n += *p == ',' ? 1u : 0u;
    }
    if (n > AXL_SSH_PENALTY_EXEMPT_MAX) {
        return refuse(err, e->line, "%s: at most %u ranges", row->keyword,
                      (unsigned)AXL_SSH_PENALTY_EXEMPT_MAX);
    }
    for (const char *p = a;;) {
        const char *comma = axl_strchr(p, ',');
        size_t      len   = comma != NULL ? (size_t)(comma - p) : axl_strlen(p);
        /* Sized from the line cap, which bounds every argument, so the
           range check alone judges the entry: nothing is refused for its
           length here. The guard only fails safe if that ever changes. */
        char        one[AXL_CONFIG_FILE_LINE_MAX];
        bool        ok    = len < sizeof one;

        if (ok) {
            axl_memcpy(one, p, len);
            one[len] = '\0';
            ok       = axl_ssh_penalty_range_check(one) == AXL_OK;
        }
        if (!ok) {
            return refuse(err, e->line,
                          "%s: \"%.*s\" is not an IPv4 address or CIDR range",
                          row->keyword, (int)len, p);
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    o->exempt = a;
    return AXL_OK;
}

static int
apply_nic(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
          SshdConfError *err)
{
    const char *a   = e->argv[0];
    uint64_t    v   = 0;
    bool        big = false;

    if (axl_strcasecmp(a, "auto") == 0) {
        o->net.nic_index = AXL_NET_NIC_AUTO;
        return AXL_OK;
    }
    if (!parse_dec(a, &v, &big) || big) {
        return refuse(err, e->line, "%s: \"%s\" is not auto or a number",
                      row->keyword, a);
    }
    o->net.nic_index = v;
    return AXL_OK;
}

static void
show_nic(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    if (o->net.nic_index == AXL_NET_NIC_AUTO) {
        axl_strlcpy(out, "auto", cap);
    } else {
        axl_snprintf(out, cap, "%llu", (unsigned long long)o->net.nic_index);
    }
}

static int
apply_log_port(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
               SshdConfError *err)
{
    if (!sshd_conf_log_port_parse(e->argv[0], &o->log_port)) {
        return refuse(err, e->line, "%s: \"%s\" is not auto, none or a port number",
                      row->keyword, e->argv[0]);
    }
    return AXL_OK;
}

static void
show_log_port(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    if (o->log_port == SSHD_LOG_PORT_AUTO || o->log_port == SSHD_LOG_PORT_NONE) {
        axl_strlcpy(out, o->log_port == SSHD_LOG_PORT_AUTO ? "auto" : "none", cap);
    } else {
        axl_snprintf(out, cap, "%llu", (unsigned long long)o->log_port);
    }
}

/* OpenSSH's names in any case, and DEBUG1 for DEBUG; refused in OpenSSH's
   words. */
static int
apply_loglevel(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
               SshdConfError *err)
{
    const char *a = e->argv[0];

    (void)row;
    if (axl_strcasecmp(a, "DEBUG1") == 0) {
        o->loglevel = SSHD_LOG_DEBUG1;
        return AXL_OK;
    }
    for (uint32_t l = SSHD_LOG_QUIET; l < AXL_ARRAY_SIZE(m_log_names); l++) {
        if (axl_strcasecmp(a, m_log_names[l]) == 0) {
            o->loglevel = l;
            return AXL_OK;
        }
    }
    return refuse(err, e->line, "unsupported log level '%s'", a);
}

/* Not given prints OpenSSH's default, INFO: what AxlLog runs at unless
   AXL_LOG_LEVEL says otherwise. */
static void
show_loglevel(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    bool known = o->loglevel > SSHD_LOG_UNSET && o->loglevel < AXL_ARRAY_SIZE(m_log_names);
    axl_strlcpy(out, known ? m_log_names[o->loglevel] : "INFO", cap);
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

static const ScRow *
find_row(const char *keyword)
{
    for (size_t i = 0; i < SC_ROWS; i++) {
        if (axl_strcasecmp(m_rows[i].keyword, keyword) == 0) {
            return &m_rows[i];
        }
    }
    return NULL;
}

static const char *
find_unsupported(const char *keyword)
{
    for (size_t i = 0; i < AXL_ARRAY_SIZE(m_unsupported); i++) {
        if (axl_strcasecmp(m_unsupported[i], keyword) == 0) {
            return m_unsupported[i];
        }
    }
    return NULL;
}

static const char *
find_alias(const char *keyword)
{
    for (size_t i = 0; i < AXL_ARRAY_SIZE(m_aliases); i++) {
        if (axl_strcasecmp(m_aliases[i].alias, keyword) == 0) {
            return m_aliases[i].canonical;
        }
    }
    return keyword;
}

static const ScIgnored *
find_ignored(const char *keyword)
{
    for (size_t i = 0; i < AXL_ARRAY_SIZE(m_ignored); i++) {
        if (axl_strcasecmp(m_ignored[i].keyword, keyword) == 0) {
            return &m_ignored[i];
        }
    }
    return NULL;
}

/* `Subsystem sftp <command>`: sftp is the one subsystem this sshd has, and
   its command takes no arguments here. The words for a missing name or
   command are OpenSSH's. */
static int
apply_subsystem(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
                SshdConfError *err)
{
    if (axl_strcmp(e->argv[0], "sftp") != 0) {
        return refuse(err, e->line, "%s %s: only sftp is supported",
                      row->keyword, e->argv[0]);
    }
    if (e->argc < 2) {
        return refuse(err, e->line, "Missing subsystem command.");
    }
    if (e->argc > 2) {
        return refuse(err, e->line,
                      "%s sftp: arguments to the command are not supported",
                      row->keyword);
    }
    o->sftp = e->argv[1];
    return AXL_OK;
}

static void
show_subsystem(const ScRow *row, const SshdOpts *o, char *out, size_t cap)
{
    (void)row;
    if (o->sftp == NULL || o->sftp[0] == '\0') {
        axl_strlcpy(out, "none", cap);
    } else {
        axl_snprintf(out, cap, "sftp %s", o->sftp);
    }
}

/* One directive. The shared checks use the keyword as WRITTEN, in OpenSSH's
   words; a row's own reasons name it as the table spells it. An alias is
   looked up as its canonical keyword and shares that keyword's first-wins
   slot, as in OpenSSH. */
static int
apply_entry(const AxlConfigFileEntry *e, SshdOpts *o, bool *seen,
            SshdConfOrigin *origin, SshdConfWarn warn, void *warn_ud,
            SshdConfError *err)
{
    const char      *name    = find_alias(e->keyword);
    const ScRow     *row     = find_row(name);
    const ScIgnored *ignored = row == NULL ? find_ignored(name) : NULL;

    if (ignored != NULL) {
        if (e->argc == 0) {
            return refuse(err, e->line, "no argument after keyword \"%s\"",
                          e->keyword);
        }
        if (ignored->why != NULL && warn != NULL) {
            char text[sizeof err->reason];
            axl_snprintf(text, sizeof text, "%s option %s", ignored->why,
                         e->keyword);
            warn(e->line, text, warn_ud);
        }
        return AXL_OK;
    }
    if (row != NULL && row->apply == apply_hostkey) {
        /* servconf.c:1460's words, and every occurrence applied: HostKey
           accumulates, in order. */
        if (e->argc == 0 || e->argv[0][0] == '\0') {
            return refuse(err, e->line, "%s: missing file name.", row->keyword);
        }
        if (e->argc > 1) {
            return refuse(err, e->line, "keyword %s extra arguments at end of line",
                          e->keyword);
        }
        if (apply_hostkey(row, e, o, err) != AXL_OK) {
            return AXL_INVALID;
        }
        if (origin != NULL && origin->hostkey_count < SSHD_HOSTKEY_MAX) {
            origin->hostkey_line[origin->hostkey_count++] = e->line;
        }
        return AXL_OK;
    }
    if (row == NULL) {
        const char *known = find_unsupported(name);
        if (known != NULL) {
            return refuse(err, e->line, "%s is not supported by this sshd",
                          known);
        }
        return refuse(err, e->line, "Bad configuration option: %s",
                      e->keyword);
    }
    if (e->argc == 0) {
        return refuse(err, e->line, "no argument after keyword \"%s\"",
                      e->keyword);
    }
    for (size_t i = 0; i < e->argc; i++) {
        if (e->argv[i][0] == '\0') {
            return refuse(err, e->line, "keyword %s empty argument",
                          e->keyword);
        }
    }
    if (row->max_args != 0 && e->argc > row->max_args) {
        if (row->too_many != NULL) {
            return refuse(err, e->line, "%s: %s", row->keyword, row->too_many);
        }
        return refuse(err, e->line, "keyword %s extra arguments at end of line",
                      e->keyword);
    }

    size_t idx   = (size_t)(row - m_rows);
    bool   first = !seen[idx];
    if (row->once && !first) {
        return refuse(err, e->line, "%s: only one %s is supported",
                      row->keyword, row->keyword);
    }

    /* A later occurrence runs the same parse into a copy, and is dropped. */
    SshdOpts  scratch = *o;
    SshdOpts *dst     = first ? o : &scratch;
    ScApply   apply   = row->apply != NULL ? row->apply : apply_generic;
    if (apply(row, e, dst, err) != AXL_OK) {
        return AXL_INVALID;
    }
    if (first) {
        seen[idx] = true;
        if (origin != NULL && row->offset == offsetof(SshdOpts, keys)) {
            origin->keys_line = e->line;
        }
        if (origin != NULL && row->offset == offsetof(SshdOpts, banner)) {
            origin->banner_line = e->line;
        }
    }
    return AXL_OK;
}

/* HostKey PATH: appended to the list. `none` stays an entry that loads
   nothing (servconf.c:531-532). At most SSHD_HOSTKEY_MAX -- a deviation:
   sshd has no limit, but the list travels in one LoadOptions string. */
static int
apply_hostkey(const ScRow *row, const AxlConfigFileEntry *e, SshdOpts *o,
              SshdConfError *err)
{
    const char *path = e->argv[0];
    size_t      n    = 1;

    for (const char *p = m_hostkeys; *p != '\0'; p++) {
        n += *p == SSHD_HOSTKEY_SEP;
    }
    if (m_hostkeys[0] != '\0' && n >= SSHD_HOSTKEY_MAX) {
        return refuse(err, e->line, "%s: at most %u host keys", row->keyword,
                      (unsigned)SSHD_HOSTKEY_MAX);
    }
    if (axl_strchr(path, SSHD_HOSTKEY_SEP) != NULL) {
        return refuse(err, e->line, "%s: a path may not contain '%c'", row->keyword,
                      SSHD_HOSTKEY_SEP);
    }
    if (axl_strcasecmp(path, SSHD_HOSTKEY_NONE) == 0) {
        path = SSHD_HOSTKEY_NONE;
    }
    char sep[2] = { SSHD_HOSTKEY_SEP, '\0' };
    if ((m_hostkeys[0] != '\0' && axl_strlcat(m_hostkeys, sep, sizeof m_hostkeys)
                                   >= sizeof m_hostkeys)
        || axl_strlcat(m_hostkeys, path, sizeof m_hostkeys) >= sizeof m_hostkeys) {
        return refuse(err, e->line, "%s: path too long", row->keyword);
    }
    o->host_keys = m_hostkeys;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool
sshd_conf_log_port_parse(const char *s, uint64_t *out)
{
    uint64_t v   = 0;
    bool     big = false;

    if (axl_strcasecmp(s, "auto") == 0 || axl_strcasecmp(s, "none") == 0) {
        *out = (s[0] | 0x20) == 'a' ? SSHD_LOG_PORT_AUTO : SSHD_LOG_PORT_NONE;
        return true;
    }
    if (!parse_dec(s, &v, &big) || big || v > 65535u) {
        return false;
    }
    *out = v;
    return true;
}

void
sshd_conf_defaults(SshdOpts *o)
{
    axl_memset(o, 0, sizeof *o);
    o->net.nic_index = AXL_NET_NIC_AUTO;
    o->net.port      = SSHD_PORT_DEFAULT;
    o->keys          = "";
    o->password      = "";
    o->exempt        = "";
    o->banner        = "";
    o->drivers       = "";
    o->volume        = "";
    o->net_timeout_sec = SSHD_NET_TIMEOUT_SEC;
    o->ro            = false;
    o->sftp_ro       = false;
    o->geometry      = false;
    o->log_port      = SSHD_LOG_PORT_AUTO;
    o->tcp_linger_us = AXL_TCP_PACING_LINGER_DEFAULT_US;
    o->sftp          = SFTP_HELPER_NAME;
    o->sftp_path     = "";
    o->conf          = "";
    o->host_keys     = "";
    o->pubkey        = true;
    o->pwauth        = true;
    o->grace_sec     = AXL_SSH_LOGIN_GRACE_DEFAULT_SEC;
    o->max_auth      = AXL_SSH_PASSWORD_FAILURES_DEFAULT;
    o->max_sessions  = AXL_SSH_MAX_SESSIONS_DEFAULT;
    o->max_startups  = AXL_SSH_MAX_STARTUPS_DEFAULT;
    o->alive_sec     = AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC;
    o->alive_max     = AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX;
    o->unused_sec    = AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC;
    o->rekey_bytes   = AXL_SSH_REKEY_LIMIT_DEFAULT;
    o->penalties     = true;
    o->pen_authfail  = AXL_SSH_PENALTY_AUTHFAIL_DEFAULT_SEC;
    o->pen_noauth    = AXL_SSH_PENALTY_NOAUTH_DEFAULT_SEC;
    o->pen_grace     = AXL_SSH_PENALTY_GRACE_EXCEEDED_DEFAULT_SEC;
    o->pen_min       = AXL_SSH_PENALTY_MIN_DEFAULT_SEC;
    o->pen_max       = AXL_SSH_PENALTY_MAX_DEFAULT_SEC;
}

int
sshd_conf_apply(SshdOpts *o, AxlConfigFile *cf, SshdConfOrigin *origin,
                SshdConfWarn warn, void *warn_ud, SshdConfError *err)
{
    SshdConfError scratch;
    bool          seen[SC_ROWS];
    size_t        n = axl_config_file_entry_count(cf);

    if (err == NULL) {
        err = &scratch;
    }
    err->line      = 0;
    err->reason[0] = '\0';
    if (origin != NULL) {
        origin->keys_line     = 0;
        origin->banner_line   = 0;
        origin->hostkey_count = 0;
    }
    m_hostkeys[0] = '\0';
    axl_memset(seen, 0, sizeof seen);
    /* A file serves only the subsystems it lists, as sshd_config does. */
    o->sftp = "";
    for (size_t i = 0; i < n; i++) {
        const AxlConfigFileEntry *e = axl_config_file_entry_at(cf, i);
        if (e == NULL || apply_entry(e, o, seen, origin, warn, warn_ud, err) != AXL_OK) {
            return AXL_INVALID;
        }
    }
    return AXL_OK;
}

void
sshd_conf_dump(const SshdOpts *o, SshdConfEmit emit, void *ud)
{
    for (size_t i = 0; i < SC_ROWS; i++) {
        const ScRow *row = &m_rows[i];
        char         line[SC_DUMP_LINE_MAX];
        size_t       k = 0;

        for (; row->keyword[k] != '\0' && k < sizeof line - 2; k++) {
            line[k] = (char)axl_tolower((unsigned char)row->keyword[k]);
        }
        line[k++] = ' ';
        if (row->apply == apply_hostkey) {
            /* One `hostkey PATH` per entry (servconf.c:3084-3090), `none`
               ones skipped as sshd skips them; with none at all, the
               stored key, by the variable that holds it. */
            const char *cur  = o->host_keys;
            bool        any  = false;
            char        path[SC_DUMP_LINE_MAX];
            while (sshd_bind_hostkey_next(&cur, path, sizeof path)) {
                if (axl_strcmp(path, SSHD_HOSTKEY_NONE) != 0) {
                    axl_snprintf(line + k, sizeof line - k, "%s", path);
                    emit(line, ud);
                    any = true;
                }
            }
            if (!any && (o->host_keys == NULL || o->host_keys[0] == '\0')) {
                axl_snprintf(line + k, sizeof line - k, "%s", SSHD_HOSTKEY_STORED_NAME);
                emit(line, ud);
            }
            continue;
        }
        if (row->show != NULL) {
            row->show(row, o, line + k, sizeof line - k);
        } else {
            show_generic(row, o, line + k, sizeof line - k);
        }
        emit(line, ud);
    }
}

int
sshd_conf_parse_time(const char *s, uint32_t *out)
{
    uint64_t total = 0;

    if (s == NULL || out == NULL || *s == '\0') {
        return AXL_INVALID;
    }
    while (*s != '\0') {
        uint64_t n = 0;
        if (!axl_isdigit((unsigned char)*s)) {
            return AXL_INVALID;
        }
        for (; axl_isdigit((unsigned char)*s); s++) {
            n = n * 10u + (uint64_t)(*s - '0');
            if (n > SC_TIME_MAX) {
                return AXL_INVALID;
            }
        }
        uint64_t mult = 1;
        switch (*s) {
        case '\0':           break;
        case 's': case 'S':  s++; break;
        case 'm': case 'M':  mult = 60u;     s++; break;
        case 'h': case 'H':  mult = 3600u;   s++; break;
        case 'd': case 'D':  mult = 86400u;  s++; break;
        case 'w': case 'W':  mult = 604800u; s++; break;
        default:             return AXL_INVALID;
        }
        total += n * mult;
        if (total > SC_TIME_MAX) {
            return AXL_INVALID;
        }
    }
    *out = (uint32_t)total;
    return AXL_OK;
}

int
sshd_conf_parse_size(const char *s, uint64_t *out)
{
    uint64_t v     = 0;
    uint64_t scale = 1;
    bool     big   = false;
    char     num[24];
    size_t   len;

    if (s == NULL || out == NULL) {
        return AXL_INVALID;
    }
    len = axl_strlen(s);
    if (len > 0) {
        switch (s[len - 1]) {
        case 'k': case 'K': scale = 1024ull;               len--; break;
        case 'm': case 'M': scale = 1024ull * 1024u;        len--; break;
        case 'g': case 'G': scale = 1024ull * 1024u * 1024u; len--; break;
        default: break;
        }
    }
    if (len == 0 || len >= sizeof num) {
        return AXL_INVALID;
    }
    axl_memcpy(num, s, len);
    num[len] = '\0';
    if (!parse_dec(num, &v, &big) || big || v > UINT64_MAX / scale) {
        return AXL_INVALID;
    }
    *out = v * scale;
    return AXL_OK;
}
