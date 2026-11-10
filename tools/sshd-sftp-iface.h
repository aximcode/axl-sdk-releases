/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-sftp-iface.h - the narrow interface between sshd and its SFTP
 * helper, SftpServer.efi.
 *
 * OpenSSH's split: sshd owns the transport and the channel; a separate
 * program (sftp-server) owns the files. Here the program is a driver image
 * that sshd loads as it starts -- from the file beside sshd.efi, or from a
 * copy embedded in sshd-full.efi -- and unloads as it stops (never per
 * session: see sshd-sftp-relay.c). sshd keeps only the
 * subsystem hook and a relay (sshd-sftp-relay.c); everything SFTP is in the
 * helper (sshd-sftp.c over sshd-sftp-codec.c and AxlFs).
 *
 * **How the table crosses.** The helper's entry point installs ONE
 * protocol, SFTP_IFACE_GUID, whose interface is an SftpIface, on its OWN
 * image handle -- the repo's shared-driver convention
 * (axl_shared_driver_publish) without the name-derived GUID. sshd reads it
 * back from the image handle it loaded (HandleProtocol on that handle, not
 * LocateProtocol), so it can only ever hold the table of the image it owns,
 * never a stale or foreign one. The helper uninstalls it in its unload
 * callback.
 *
 * **Lifetime.** Every pointer in the table, and every SftpSession, lives in
 * the helper's image. sshd therefore:
 *   - calls session_close() for every session it opened before unloading
 *     the image, and never touches a session after closing it;
 *   - unloads the image only when no session is open;
 *   - checks @a size and @a version before the first call.
 * The helper's unload callback REFUSES while any session is open, so an
 * unload from elsewhere (the Shell's `unload`) cannot pull the code out from
 * under sshd.
 *
 * **Threading.** Every call, both ways, is made from sshd's loop: the
 * helper never runs on its own, so it needs no timers, events or locks.
 * The callbacks sshd hands in (write, end) may be called from inside any
 * session_* call and never re-enter the helper.
 *
 * **ABI.** Both images come from this tree; the struct is versioned
 * anyway, because the helper is a separate file a user can mix with another
 * sshd build. A grown table appends fields and keeps @a version; a changed
 * meaning bumps it.
 */

#ifndef AXL_TOOLS_SSHD_SFTP_IFACE_H
#define AXL_TOOLS_SSHD_SFTP_IFACE_H

#include <axl.h>

/// The protocol SftpServer.efi installs on its own image handle.
static const AxlGuid SFTP_IFACE_GUID =
    AXL_GUID(0x3c9b7e41, 0x6a2d, 0x4f18, 0x9b, 0x57, 0x2e, 0x81, 0x0d, 0xc4, 0x63, 0xaf);

/// The interface's version: what a matching sshd expects. 2 added
/// session_open()'s @a consumed: a version-1 helper never reports what it
/// has dispatched, and sshd would hold its client's window shut, so sshd
/// refuses it rather than mixing the two.
#define SFTP_IFACE_VERSION  2u

/// The helper's file name, beside sshd.efi; also the default `Subsystem sftp`
/// command, and the name an embedded copy is loaded under.
#define SFTP_HELPER_NAME    "SftpServer.efi"

/**
 * Whether a `Subsystem sftp` command names the default helper --
 * `internal-sftp` (OpenSSH's built-in server) or SFTP_HELPER_NAME, either
 * case -- which an image carrying an embedded helper serves from memory, and
 * the launcher otherwise finds beside sshd.efi.
 */
static inline bool
sftp_command_is_internal(const char *cmd)
{
    return cmd != NULL
           && (axl_strcasecmp(cmd, "internal-sftp") == 0
               || axl_strcasecmp(cmd, SFTP_HELPER_NAME) == 0);
}

/**
 * A log-safe copy of @p n peer bytes: every byte below 0x20, 0x7f and above
 * is written as `?`, so a name from the wire cannot put escape sequences or
 * line breaks into a log line. Truncated to fit @p cap (NUL included).
 */
static inline void
sftp_printable(char *out, size_t cap, const void *p, size_t n)
{
    const unsigned char *b = p;
    size_t               k = 0;

    if (cap == 0) {
        return;
    }
    for (; k < n && k + 1 < cap; k++) {
        out[k] = b[k] >= 0x20 && b[k] < 0x7f ? (char)b[k] : '?';
    }
    out[k] = '\0';
}

/// session_open() flags.
#define SFTP_SESSION_READ_ONLY  0x1u   ///< refuse every write op (AxlSftpReadOnly)

/// One SFTP session: the helper's, opaque to sshd.
typedef struct SftpSession SftpSession;

/**
 * Queue @p len reply bytes on the session's channel (sshd's side of
 * axl_ssh_channel_write()). All or nothing.
 *
 * @return AXL_OK when queued; AXL_BUSY when the channel is behind -- nothing
 *     was queued, and session_writable() follows once a retry of the same
 *     length would fit; anything else when it can never be written (the
 *     session should stop).
 */
typedef int  (*SftpWriteFn)(void *ctx, const void *buf, size_t len);

/**
 * Report that the session is done with @p n of the bytes session_data()
 * handed it: sshd passes it to axl_ssh_channel_consumed(), and the client
 * may send that many more. The session reports each request's whole packet
 * (its 4-byte length included) as it DISPATCHES it -- not as it arrives --
 * so a session that is yielded, or behind on BUSY output, stops its client
 * after one SSH window instead of buffering everything the client sends.
 * Never more than was handed over. Every request is far smaller than the
 * window (SFTP_PACKET_MAX against AXL_SSH_CHANNEL_WINDOW_SIZE), so a whole
 * request always fits behind the bytes already held.
 */
typedef void (*SftpConsumedFn)(void *ctx, size_t n);

/**
 * End the session's channel: what has been written drains, then the client
 * sees exit status @p status and the channel closes. session_close() follows
 * later, from sshd; no session_* call but session_close() is made after this.
 */
typedef void (*SftpEndFn)(void *ctx, uint32_t status);

/**
 * Receive one of the helper's log lines: @p level is an AXL_LOG_* level and
 * @p message the formatted text, valid only during the call. sshd logs it
 * as its own, so it reaches `sshd log` and sshd's log port rather than the
 * console the sessions share. It must not call back into the helper.
 */
typedef void (*SftpLogFn)(void *ud, int level, const char *message);

/**
 * Ask sshd to call session_resume() for the session whose callbacks carry
 * @p ctx, from its loop, after the current session_* call has returned. A
 * session uses it to YIELD: having zero-filled a file it stops answering
 * requests, so the loop serves every other session before it goes on, and
 * nothing else would wake it while its input is already held. Only that
 * call ends the yield -- a session_writable() from the channel does not,
 * since it can arrive in the same drain. One request is outstanding at a
 * time: the session asks again only after the session_resume() it asked
 * for. It must not call back into the helper.
 *
 * @return true when that session_resume() will follow; false when it
 *     cannot be arranged, and the session then goes on without yielding.
 */
typedef bool (*SftpResumeFn)(void *ctx);

/// The table SftpServer.efi publishes (see the file comment).
typedef struct {
    uint32_t size;      ///< sizeof(SftpIface) as the helper was built
    uint32_t version;   ///< SFTP_IFACE_VERSION

    /**
     * Start a session for one channel. @p write, @p end and @p consumed are
     * how the session talks back; @p ctx is passed to all three. @p flags
     * are SFTP_SESSION_*. @p write_max is the largest single write the
     * channel can ever accept (its per-channel queue limit): the session
     * writes a reply in pieces no larger. NULL when the session cannot be
     * created, or when any callback is NULL.
     */
    SftpSession *(*session_open)(SftpWriteFn write, SftpEndFn end,
                                 SftpConsumedFn consumed, void *ctx,
                                 uint32_t flags, size_t write_max);
    /// Bytes from the client, in order, in chunks with no packet boundaries.
    /// @p buf is valid only during the call.
    void (*session_data)(SftpSession *s, const uint8_t *buf, size_t len);
    /// A write refused with AXL_BUSY may be retried. It does not end a
    /// yield (see SftpResumeFn).
    void (*session_writable)(SftpSession *s);
    /// The resume a yield asked for (SftpResumeFn): requests may be
    /// answered again.
    void (*session_resume)(SftpSession *s);
    /// The client will send nothing more.
    void (*session_eof)(SftpSession *s);
    /// The channel has closed: free everything. @p s is gone when this
    /// returns, and no callback follows.
    void (*session_close)(SftpSession *s);
    /**
     * Route the helper's log through @p fn at @p level (an AXL_LOG_* level)
     * and off the console; NULL @p fn puts it back on the console. sshd
     * calls it once after loading the helper, and with NULL before
     * unloading it.
     */
    void (*set_log)(SftpLogFn fn, void *ud, int level);
    /**
     * How a session asks to be resumed (SftpResumeFn); NULL -- the state
     * before the first call -- means it never yields. Part of version 1,
     * which never shipped without it: sshd requires the whole table
     * (@a size at least sizeof(SftpIface)), calls this once after loading
     * the helper, and with NULL before unloading it.
     */
    void (*set_resume)(SftpResumeFn fn);
} SftpIface;

#endif /* AXL_TOOLS_SSHD_SFTP_IFACE_H */
