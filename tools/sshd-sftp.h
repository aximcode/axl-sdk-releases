/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-sftp.h - the SFTP server's sessions (sshd-sftp.c), the functions
 * SftpServer.efi publishes through its SftpIface table. See
 * sshd-sftp-iface.h for what each one promises.
 */

#ifndef AXL_TOOLS_SSHD_SFTP_H
#define AXL_TOOLS_SSHD_SFTP_H

#include <axl.h>

#include "sshd-sftp-iface.h"

/**
 * Most bytes one request may grow a file by past its end: a SETSTAT or
 * FSETSTAT size, or the gap a WRITE leaves before its data. A larger growth
 * is FAILURE with the size unchanged -- a time in the same SETSTAT is still
 * set, as sftp-server applies each attribute in turn -- and a WRITE refused
 * this way writes nothing. A shrink is never limited.
 *
 * Why a ceiling at all: the EDK2-derived FAT driver writes every added byte
 * as zeros inside the one SetInfo or Write call, and sshd serves every
 * session from one event loop, so while it fills, every console session
 * and every other transfer stands still. Why 16 MiB: measured under QEMU on
 * x64 the fill runs at about 280 MB/s (256 MiB took 0.92 s), so 16 MiB is
 * about 60 ms there -- but USB flash takes writes at 10-30 MB/s, where
 * 16 MiB is 0.5-1.6 s, about the longest a typed key should wait. No
 * transfer by sftp(1), scp(1) or a resumed upload needs more: they write
 * in order, so their gaps are 0.
 */
#define SFTP_GROW_MAX  (16u * 1024u * 1024u)

/// SftpIface.session_open.
SftpSession *
sftp_session_open(
    SftpWriteFn    write,      ///< queues reply bytes on the channel
    SftpEndFn      end,        ///< ends the channel
    SftpConsumedFn consumed,   ///< reports each dispatched request's bytes
    void          *ctx,        ///< passed to all three
    uint32_t       flags,      ///< SFTP_SESSION_*
    size_t         write_max   ///< largest single write the channel accepts
);

/// SftpIface.session_data.
void
sftp_session_data(
    SftpSession   *s,    ///< session
    const uint8_t *buf,  ///< bytes from the client
    size_t         len   ///< how many
);

/// SftpIface.session_writable.
void
sftp_session_writable(
    SftpSession *s   ///< session
);

/// SftpIface.session_resume.
void
sftp_session_resume(
    SftpSession *s   ///< session
);

/// SftpIface.session_eof.
void
sftp_session_eof(
    SftpSession *s   ///< session
);

/// SftpIface.session_close.
void
sftp_session_close(
    SftpSession *s   ///< session
);

/**
 * SftpIface.set_resume: how sessions ask to be resumed after yielding.
 *
 * A session yields after a request that zero-filled part of a file (a size
 * or a WRITE gap past the end): it answers that request and stops, asking
 * @p fn to have session_resume() called from the loop, so that ~500
 * pipelined growths of up to SFTP_GROW_MAX each cannot hold the loop for
 * one dispatch. With @p fn NULL, or when @p fn returns false, it carries on
 * without yielding.
 */
void
sftp_set_resume(
    SftpResumeFn fn   ///< resume request, or NULL
);

/// Sessions open in this image: the helper refuses to unload while any is.
uint32_t
sftp_sessions_open(void);

/// How the dispatcher classes a request type for read-only mode
/// (SFTP_SESSION_READ_ONLY), which is enforced there and nowhere else.
typedef enum {
    SFTP_REQ_UNKNOWN = 0,  ///< not a request this server knows: logged, not answered
    SFTP_REQ_READS,        ///< changes nothing on a volume
    SFTP_REQ_WRITES,       ///< refused with PERMISSION_DENIED in read-only mode
    SFTP_REQ_OPEN,         ///< a write when its pflags carry any SFTP_OPEN_WRITES flag
    SFTP_REQ_EXTENDED,     ///< classed by its extension's name (sftp_extension_class())
} SftpRequestClass;

/**
 * The dispatcher's class for request @p type, from the same table it
 * dispatches by -- OpenSSH sftp-server's `does_write` column. Every type
 * sftp_decode() parses has one; AxlTestSshdSftp holds the two to each other.
 */
SftpRequestClass
sftp_request_class(
    uint8_t type   ///< SFTP_* request type
);

/**
 * The class of EXTENDED request @p name: SFTP_REQ_READS or SFTP_REQ_WRITES
 * for an extension this server serves, from the one table every served
 * extension is in; SFTP_REQ_UNKNOWN for any other, which is answered
 * OP_UNSUPPORTED in every mode, as sftp-server answers it.
 */
SftpRequestClass
sftp_extension_class(
    const uint8_t *name,   ///< extension name bytes (not NUL-terminated)
    size_t         len     ///< their count
);

/// The @p i-th served extension's name, or NULL past the last: the table
/// sftp_extension_class() reads, for the tests to enumerate.
const char *
sftp_extension_name(
    size_t i   ///< index from 0
);

#endif /* AXL_TOOLS_SSHD_SFTP_H */
