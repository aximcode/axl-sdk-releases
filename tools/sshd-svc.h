/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-svc.h - the `sshd` service descriptor, linked into BOTH binaries.
 *
 * sshd-svc.c defines SshdOpts, sshd_descs and sshd_service unconditionally;
 * the server and the console binding are gated on AXL_SERVICE_BUILD_DRIVER,
 * so the launcher carries only the descriptor it serializes into the
 * driver's LoadOptions. Same split as 9p-serve-svc.h, and the same ABI rule:
 * both images come from one source tree with identical flags but for that
 * toggle.
 */

#ifndef AXL_TOOLS_SSHD_SVC_H
#define AXL_TOOLS_SSHD_SVC_H

#include <axl.h>
#include <axl/axl-net-opts.h>

/// The default listen port, as a number and as the descriptor default.
#define SSHD_PORT_DEFAULT      22
#define SSHD_PORT_DEFAULT_STR  "22"

/// The whole network bring-up's default budget, seconds (AxlNetworkTimeout,
/// --net-timeout): the firmware's NICs, the sweep of the drivers beside
/// sshd.efi, every link wait and DHCP, as ONE deadline
/// (AxlNetAutoOpts.timeout_sec). Sized so a firmware NIC no DHCP
/// server answers still leaves the sweep room to lease: the firmware pass's
/// link wait and one DHCP attempt (5 + 15 s), the winning driver's link wait
/// and DHCP (5 + 15 s), and 20 s for loading and connecting drivers --
/// a USB NIC's connect is the slow one.
#define SSHD_NET_TIMEOUT_SEC   60u
#define SSHD_NET_TIMEOUT_STR   "60"   ///< the same, as the descriptor default

/// Most `-x` ranges one start takes: the server's own bound.
#define SSHD_EXEMPT_MAX        AXL_SSH_PENALTY_EXEMPT_MAX

/// Descriptor defaults for the server settings sshd.conf reaches. Each is the
/// AxlSsh default it names, spelled as the string AxlConfig parses; the unit
/// test AxlTestSshdConf holds every one to its macro.
#define SSHD_PUBKEY_DEFAULT_STR     "true"
#define SSHD_PWAUTH_DEFAULT_STR     "true"
#define SSHD_GRACE_DEFAULT_STR      "60"          ///< AXL_SSH_LOGIN_GRACE_DEFAULT_SEC
#define SSHD_MAX_AUTH_DEFAULT_STR   "3"           ///< AXL_SSH_PASSWORD_FAILURES_DEFAULT
#define SSHD_MAX_SESS_DEFAULT_STR   "10"          ///< AXL_SSH_MAX_SESSIONS_DEFAULT
#define SSHD_MAX_START_DEFAULT_STR  "10"          ///< AXL_SSH_MAX_STARTUPS_DEFAULT
#define SSHD_ALIVE_DEFAULT_STR      "60"          ///< AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC
#define SSHD_ALIVE_MAX_DEFAULT_STR  "3"           ///< AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX
#define SSHD_UNUSED_DEFAULT_STR     "300"         ///< AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC
#define SSHD_REKEY_DEFAULT_STR      "1073741824"  ///< AXL_SSH_REKEY_LIMIT_DEFAULT
#define SSHD_PEN_DEFAULT_STR        "true"
#define SSHD_PEN_AUTH_DEFAULT_STR   "5"           ///< AXL_SSH_PENALTY_AUTHFAIL_DEFAULT_SEC
#define SSHD_PEN_NOAUTH_DEFAULT_STR "1"           ///< AXL_SSH_PENALTY_NOAUTH_DEFAULT_SEC
#define SSHD_PEN_GRACE_DEFAULT_STR  "10"          ///< AXL_SSH_PENALTY_GRACE_EXCEEDED_DEFAULT_SEC
#define SSHD_PEN_MIN_DEFAULT_STR    "15"          ///< AXL_SSH_PENALTY_MIN_DEFAULT_SEC
#define SSHD_PEN_MAX_DEFAULT_STR    "600"         ///< AXL_SSH_PENALTY_MAX_DEFAULT_SEC

/// Configuration for the sshd service.
///
/// Every field crosses LoadOptions through @ref sshd_descs. Strings are
/// borrowed pointers, per AxlConfig's AXL_CFG_STRING contract.
///
/// @ref SshdOpts::exempt is ONE comma-joined string rather than a repeatable
/// option: the launcher-to-driver hand-off serializes the options struct
/// with axl_config_target_to_string(), which skips repeatable options, so a
/// list has to travel as a single value.
typedef struct {
    AxlNetOpts  net;       ///< nic_index + port (listen); local_ip unused
    const char *keys;      ///< absolute authorized_keys path, or "" for none
    const char *password;  ///< accepted password, or "" for none
    const char *exempt;    ///< comma-joined CIDR ranges exempt from penalties
    bool        ro;        ///< read-only: session input is dropped (and SFTP
                           ///< refuses every write, as with @ref sftp_ro)
    bool        sftp_ro;   ///< AxlSftpReadOnly: SFTP refuses every write op
    bool        geometry;  ///< report each console reshape on the first serial port
    bool        pubkey;        ///< PubkeyAuthentication
    bool        pwauth;        ///< PasswordAuthentication
    uint32_t    grace_sec;     ///< LoginGraceTime
    uint32_t    max_auth;      ///< MaxAuthTries (wrong passwords per connection)
    uint32_t    max_sessions;  ///< MaxSessions
    uint32_t    max_startups;  ///< MaxStartups (a single number)
    uint32_t    alive_sec;     ///< ClientAliveInterval; 0 = off
    uint32_t    alive_max;     ///< ClientAliveCountMax
    uint32_t    unused_sec;    ///< UnusedConnectionTimeout; 0 = none
    uint64_t    rekey_bytes;   ///< RekeyLimit, bytes per key; sshd.conf's 0 and default store AxlSsh's default
    const char *banner;        ///< Banner file path, or "" for none
    bool        penalties;     ///< PerSourcePenalties on
    uint32_t    pen_authfail;  ///< PerSourcePenalties authfail:
    uint32_t    pen_noauth;    ///< PerSourcePenalties noauth:
    uint32_t    pen_grace;     ///< PerSourcePenalties grace-exceeded:
    uint32_t    pen_min;       ///< PerSourcePenalties min:
    uint32_t    pen_max;       ///< PerSourcePenalties max:
    uint32_t    loglevel;      ///< LogLevel, -v, -d: an SshdLogLevel (sshd-bind.h); 0 = not given
    const char *drivers;       ///< the NIC-driver directory the launcher found beside
                               ///< sshd.efi (axl_net_driver_dir), or "" for none: the
                               ///< driver, started from memory, has no directory of its own
    uint32_t    net_timeout_sec; ///< AxlNetworkTimeout, --net-timeout: the whole network
                               ///< bring-up's deadline, seconds (1 to
                               ///< #AXL_NET_AUTO_TIMEOUT_MAX_SEC)
    const char *volume;        ///< the volume the launcher was run from ("FS0:\"),
                               ///< or "" for none: the device an embedded NIC driver
                               ///< is given when there is no drivers directory
    uint32_t    tcp_linger_us; ///< AxlTcpLinger, --tcp-linger: AxlTcpPacing.linger_us,
                               ///< microseconds; 0 = off (the default)
    uint64_t    log_port;      ///< AxlLogPort, --log-port: the serial port the log
                               ///< goes to while serving -- SSHD_LOG_PORT_AUTO,
                               ///< SSHD_LOG_PORT_NONE or an ordinal (sshd-bind.h)
    const char *sftp;          ///< `Subsystem sftp` command as configured
                               ///< (SFTP_HELPER_NAME, `internal-sftp` or a path),
                               ///< or "" when SFTP is off
    const char *conf;          ///< the settings file's name ("sshd.conf"), or ""
                               ///< when none was read: what the SFTP-off line names
    const char *sftp_path;     ///< that command's file made absolute beside
                               ///< sshd.efi by the launcher, or "" when it cannot
                               ///< be placed (an embedded helper still serves)
    const char *host_keys;     ///< HostKey / --host-key: their paths in order,
                               ///< SSHD_HOSTKEY_SEP-joined (sshd-bind.h), absolute
                               ///< once the launcher resolved them, `none` kept as
                               ///< an entry that loads nothing; "" = no HostKey:
                               ///< the key stored in the UEFI variable
} SshdOpts;

/// Published by the driver once it serves, withdrawn when it unloads: the
/// host key it serves with, for `sshd status`, `sshd hostkey` and the start's
/// own lines. PUBLIC MATERIAL ONLY -- never a byte of the private key crosses
/// this interface (nor LoadOptions, which carry only paths).
static const AxlGuid SSHD_HOSTKEY_GUID =
    AXL_GUID(0x3b9c6d2e, 0x7a41, 0x4f08, 0xb1, 0x5c, 0x6e, 0x92, 0x0d, 0x4a, 0x87, 0x3f);

/// SshdHostKeyIface::version this sshd publishes. A launcher reads only a
/// version it knows, and checks @c size before any field.
#define SSHD_HOSTKEY_IFACE_VERSION  1u

/// Where the served host key came from.
typedef enum {
    SSHD_HOSTKEY_LOADED    = 0,  ///< the stored key (UEFI variable)
    SSHD_HOSTKEY_GENERATED = 1,  ///< none was stored: made and stored now
    SSHD_HOSTKEY_EPHEMERAL = 2,  ///< made now and NOT stored (DEGRADED)
    SSHD_HOSTKEY_FILE      = 3,  ///< a HostKey file
} SshdHostKeyOrigin;

/// Room for the HostKey file a key came from.
#define SSHD_HOSTKEY_PATH_MAX  512u

/// SSHD_HOSTKEY_GUID's interface.
typedef struct {
    uint32_t size;          ///< sizeof(SshdHostKeyIface) as published
    uint32_t version;       ///< SSHD_HOSTKEY_IFACE_VERSION
    uint32_t origin;        ///< an SshdHostKeyOrigin
    char     fingerprint[AXL_SSH_FINGERPRINT_SIZE];  ///< `SHA256:...`
    char     public_line[AXL_SSH_PUBLIC_LINE_SIZE];  ///< `ssh-ed25519 AAAA...`
    char     path[SSHD_HOSTKEY_PATH_MAX];            ///< the file, for SSHD_HOSTKEY_FILE
    char     why[96];       ///< why it was not stored, for SSHD_HOSTKEY_EPHEMERAL
} SshdHostKeyIface;

/// Published by the driver, on a handle of its own, while it keeps its log in
/// memory -- from the moment it serves until it unloads, a refused unload
/// included: what `sshd log` and a failed `sshd stop` read.
static const AxlGuid SSHD_LOG_GUID =
    AXL_GUID(0x5d0e4a1c, 0x2b7f, 0x4c93, 0x9e, 0x61, 0x0a, 0x3f, 0x8d, 0x27, 0xc4, 0x15);

/// Bytes of the log kept in memory for `sshd log`: whole lines, the oldest
/// dropped first. In the driver's .bss, so it costs no image bytes.
#define SSHD_LOG_KEEP_BYTES 8192
/// Widest line kept, its `[LEVEL] domain: ` included; longer ones are cut.
#define SSHD_LOG_WIDTH      200

/// SSHD_LOG_GUID's interface: the driver's buffer of whole
/// `[LEVEL] domain: message` lines, oldest first. The driver appends at
/// TPL_NOTIFY, whatever TPL logged the line, so a reader copies it out at
/// that TPL and clamps the length to SSHD_LOG_KEEP_BYTES.
typedef struct {
    const char   *text;   ///< SSHD_LOG_KEEP_BYTES of room; not NUL-terminated
    const size_t *len;    ///< bytes of lines in @a text
    const uint64_t *gen;  ///< lines ever appended: a reader's "since" mark
} SshdLogIface;

/**
 * Every IPv4 address a NIC holds, read from the firmware's NIC registry
 * now (so an address another tool set counts too), as
 * sshd_bind_addr_list() joins them: where the server, listening on every
 * address as OpenSSH's default `0.0.0.0` does, can be reached. @p cap is
 * at least SSHD_ADDRS_MAX (sshd-bind.h).
 */
void
sshd_addresses(
    char  *out,   ///< [out] the addresses
    size_t cap    ///< sizeof @p out
);

/* Defined in sshd-svc.c, linked into both binaries. */
extern SshdOpts            g_sshd_opts;
extern const AxlConfigDesc sshd_descs[];
extern const AxlService    sshd_service;

/**
 * The driver's test seam: true makes setup act at @p point as if it had
 * failed there. The one point is "setup", right after the console take-over
 * and before the server is built -- the window where a failed setup must
 * give the console back, or hold it (AXL_SERVICE_SETUP_HELD).
 *
 * sshd-svc.c's definition is WEAK and always false, so no shipped sshd image
 * carries a test control; the test build (sshd-t.efi) links a strong one
 * from test/integration/sshd-test-hook.c.
 *
 * Keep the `*_test_point` name: test-efi-pack-release-qemu.sh finds the
 * seams in a shipped image by that suffix alone, and fails one that is not
 * the weak stub's size.
 */
bool
_sshd_test_point(
    const char *point);

#endif /* AXL_TOOLS_SSHD_SVC_H */
