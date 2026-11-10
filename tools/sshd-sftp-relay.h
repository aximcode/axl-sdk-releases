/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-sftp-relay.h - sshd's side of the `sftp` subsystem: load the SFTP
 * helper as sshd starts and relay a channel's hooks to it over the narrow
 * interface in sshd-sftp-iface.h. Lives in the resident driver; nothing
 * SFTP is decoded here.
 *
 * The helper is loaded by sftp_relay_init, in the driver's entry point --
 * from the copy embedded in sshd-full.efi (built with SSHD_EMBEDDED_SFTP)
 * when the configured command is the default helper, otherwise from the
 * file the configuration names -- and unloaded by sftp_relay_shutdown.
 * Never from a session: a session runs in an event callback, where a
 * LoadImage can be handed pages the firmware is about to make
 * execute-never (sshd-sftp-relay.c says how). Every session is closed
 * before the image is unloaded, so no pointer into the helper outlives it.
 */

#ifndef AXL_TOOLS_SSHD_SFTP_RELAY_H
#define AXL_TOOLS_SSHD_SFTP_RELAY_H

#include <axl.h>

/// One channel's SFTP session, as sshd holds it.
typedef struct SftpRelayChan SftpRelayChan;

/// What the relay needs from sshd's options.
typedef struct {
    const char *path;      ///< the helper's file, absolute ("" = none on disk)
    bool        internal;  ///< the command is the default helper: an embedded
                           ///< copy is preferred when this image carries one
    bool        enabled;   ///< `Subsystem sftp` is configured
    const char *conf;      ///< the settings file's name, for the line saying
                           ///< SFTP is off (only a file can turn it off)
    bool        read_only; ///< sessions refuse every write op
    bool        read_only_forced; ///< @a read_only comes from AxlReadOnly (-R)
                           ///< alone, not AxlSftpReadOnly: init says so
    int         log_level; ///< the helper's AXL_LOG_* level
    size_t      write_max; ///< largest single channel write a session makes
    AxlLoop    *loop;      ///< the server's loop: where a session that
                           ///< yielded is resumed from
} SftpRelayConfig;

/**
 * Set the server and configuration. @a path is copied; @a conf is read
 * only during this call (for the line below) and not kept. Call before the
 * server listens, from the driver's entry point. With SFTP off it logs,
 * once, `off (<conf> has no Subsystem line)`: an sshd.conf written before
 * SFTP existed turns it off, as sshd_config does, and an operator upgrading
 * must be told why. With SFTP on and @a read_only_forced set it logs, once,
 * `read-only (AxlReadOnly)`: writes are refused although AxlSftpReadOnly did
 * not ask for it.
 *
 * With SFTP on it loads and starts the helper here, logging `SFTP helper
 * loaded from <where>`. A helper that cannot be loaded does not fail this
 * call: it logs `SFTP helper <path> <why>; sftp sessions are refused`, and
 * each sftp session is then refused, saying why (sftp_relay_open).
 *
 * @return AXL_OK; AXL_NO_RESOURCES when the strings cannot be copied.
 */
AXL_WARN_UNUSED int
sftp_relay_init(
    AxlSshServer          *srv,   ///< the server whose channels are relayed
    const SftpRelayConfig *cfg    ///< configuration
);

/**
 * The subsystem hook's work for @p ch. Returns AXL_OK to accept: either a
 * session was opened on the helper loaded at start (stored in @p *out) or
 * that load failed, in which case the channel was told why on stderr and
 * exits 127 (not found) or 126 (cannot start), as a shell that cannot run
 * sftp-server reports it. Never loads the helper. Any other value refuses:
 * a name other than `sftp`, or no `Subsystem sftp` configured.
 */
int
sftp_relay_open(
    AxlSshChannel   ch,     ///< the channel asking
    const char     *name,   ///< subsystem name
    size_t          len,    ///< its length
    SftpRelayChan **out     ///< [out] the session, or NULL
);

/// The channel's data, writable and EOF hooks for a relayed channel.
void sftp_relay_data(SftpRelayChan *rc, const uint8_t *buf, size_t len);
void sftp_relay_writable(SftpRelayChan *rc);
void sftp_relay_eof(SftpRelayChan *rc);

/**
 * The channel has closed: close its session and free @p rc. The helper
 * stays loaded until sftp_relay_shutdown. NULL-safe.
 */
void
sftp_relay_close(
    SftpRelayChan *rc   ///< the channel's session
);

/**
 * sshd is stopping: after every channel has closed, unload the helper
 * (logging `SFTP helper unloaded`) and release the configuration.
 */
void
sftp_relay_shutdown(void);

#endif /* AXL_TOOLS_SSHD_SFTP_RELAY_H */
