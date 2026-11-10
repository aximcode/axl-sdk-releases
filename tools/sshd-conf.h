/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-conf.h - sshd.conf: OpenSSH sshd_config(5) keywords onto SshdOpts.
 *
 * Pure: no file I/O, no server. The caller parses the file into an
 * AxlConfigFile created with AXL_CONFIG_FILE_OPENSSH (which reports syntax
 * errors itself), then sshd_conf_apply() walks its entries in file order and
 * applies each keyword the table knows, refusing anything else with the
 * reason sshd prints as `sshd.conf:<line>: <reason>`. sshd_conf_dump() is
 * `sshd -T`: the effective settings, one `keyword value` line each, in
 * OpenSSH's own `sshd -T` format.
 *
 * Keywords are OpenSSH's wherever OpenSSH has the setting; the settings it
 * does not have carry an `Axl` prefix so they can never clash with a future
 * OpenSSH keyword. Single-valued keywords follow OpenSSH: the FIRST value
 * wins and every occurrence is still validated.
 */

#ifndef AXL_TOOLS_SSHD_CONF_H
#define AXL_TOOLS_SSHD_CONF_H

#include <axl.h>

#include "sshd-bind.h"
#include "sshd-svc.h"

/// The settings file's name, beside sshd.efi.
#define SSHD_CONF_NAME        "sshd.conf"

/// The AxlPassword value the shipped sshd.conf (tools/sshd.conf) shows as a
/// placeholder. Refused as a password: uncommenting the line unchanged must
/// not leave sshd accepting a password printed in every release.
#define SSHD_CONF_PASSWORD_PLACEHOLDER "your-password-here"

/// Largest Banner file sshd sends: the room AxlSsh's banner leaves after the
/// console-geometry line sshd appends to it.
#define SSHD_BANNER_FILE_MAX  (AXL_SSH_BANNER_MAX - 128u)

/// Why sshd_conf_apply() refused, for `<file>:<line>: <reason>`.
typedef struct {
    size_t line;         ///< 1-based line of the refusal
    char   reason[160];  ///< text after "<file>:<line>: "
} SshdConfError;

/// Lines the file-path settings came from, so a later failure to READ the
/// file can still name its line. 0 = not set by the file.
typedef struct {
    size_t keys_line;    ///< line AuthorizedKeysFile came from; 0 = not from the file
    size_t banner_line;  ///< line Banner came from; 0 = not from the file
    /// Line of each HostKey, in SshdOpts::host_keys order.
    size_t hostkey_line[SSHD_HOSTKEY_MAX];
    size_t hostkey_count;  ///< HostKey lines read
} SshdConfOrigin;

/// One `sshd -T` line, without a line terminator.
typedef void (*SshdConfEmit)(const char *line, void *ud);

/// A warning about a keyword that was read and ignored (OpenSSH's deprecated
/// and unsupported keywords): print it as `<file>:<line>: <text>`.
typedef void (*SshdConfWarn)(size_t line, const char *text, void *ud);

/**
 * Fill @p o with sshd's defaults: every server setting at its AxlSsh
 * default, port 22, NIC auto, no keys, no password, no banner, no exempt
 * ranges, read-only and geometry reporting off.
 */
void
sshd_conf_defaults(
    SshdOpts *o   ///< [out] options
);

/**
 * Apply the entries of an OpenSSH-syntax map to @p o, in file order.
 *
 * Every occurrence of a keyword is validated; only the first is applied
 * (Port and Subsystem are the exceptions: a second line is refused, since
 * sshd listens on one port and serves one subsystem). OpenSSH's aliases
 * (`DSAAuthentication`, ...) are read as the keyword they name and share its
 * first value. Keywords OpenSSH reads and ignores (`UseLogin`, `Protocol`,
 * ...) are ignored here too, each reported through @p warn with OpenSSH's
 * own text (`Deprecated option UseLogin`) except Protocol, which OpenSSH
 * ignores silently. Settings the file does not name keep what @p o held --
 * except `Subsystem sftp`, which a file turns off unless it names it, as
 * sshd serves no subsystem its sshd_config does not list (the default,
 * SFTP on, is for running with no file at all). String fields BORROW the
 * entries' argv storage, so @p cf must outlive every use of @p o. A parse
 * error already in @p cf is the caller's to print; this reads only the
 * entries a successful parse recorded.
 *
 * @return AXL_OK; AXL_INVALID on the first refusal, with @p err naming its
 *     line and reason (@p o may then hold some of the file's settings).
 */
AXL_WARN_UNUSED int
sshd_conf_apply(
    SshdOpts       *o,        ///< [in,out] options, usually from sshd_conf_defaults()
    AxlConfigFile  *cf,       ///< map created with AXL_CONFIG_FILE_OPENSSH
    SshdConfOrigin *origin,   ///< [out] where file paths came from, or NULL
    SshdConfWarn    warn,     ///< ignored-keyword warnings, or NULL to drop them
    void           *warn_ud,  ///< passed to @p warn
    SshdConfError  *err       ///< [out] the refusal; line 0 on success
);

/**
 * `sshd -T`: emit every setting as `keyword value` (lower-case keyword, one
 * space; yes/no; seconds; `none`), one call per line, in table order.
 * AxlPassword is never printed: it shows as `set` or `none`.
 */
void
sshd_conf_dump(
    const SshdOpts *o,     ///< options to print
    SshdConfEmit    emit,  ///< called once per line
    void           *ud     ///< passed to @p emit
);

/**
 * OpenSSH's time format (convtime): one or more `<n>[sSmMhHdDwW]` groups,
 * summed; a group with no unit is seconds (`90`, `2m`, `1h30m`).
 *
 * @return AXL_OK; AXL_INVALID on an empty or malformed string, or a total
 *     over INT_MAX seconds (OpenSSH's own bound).
 */
AXL_WARN_UNUSED int
sshd_conf_parse_time(
    const char *s,    ///< text
    uint32_t   *out   ///< [out] seconds
);

/**
 * A byte count: `<n>` with an optional `k`, `m` or `g` suffix, either case,
 * 1024-based.
 *
 * @return AXL_OK; AXL_INVALID on an empty or malformed string, or a value
 *     that does not fit in 64 bits.
 */
AXL_WARN_UNUSED int
sshd_conf_parse_size(
    const char *s,    ///< text
    uint64_t   *out   ///< [out] bytes
);

/**
 * An AxlLogPort / `--log-port` value: `auto` or `none` in any case, or a
 * port number, the ordinal of a serial port in the firmware's order (0 for
 * the first, at most 65535).
 *
 * @return true with @p out set to SSHD_LOG_PORT_AUTO, SSHD_LOG_PORT_NONE
 *     or the number; false, @p out untouched, for anything else.
 */
bool
sshd_conf_log_port_parse(
    const char *s,    ///< text
    uint64_t   *out   ///< [out] the value
);

#endif /* AXL_TOOLS_SSHD_CONF_H */
