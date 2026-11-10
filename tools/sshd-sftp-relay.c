/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sshd-sftp-relay.c - sshd's side of the `sftp` subsystem (see
 * sshd-sftp-relay.h): it loads SftpServer.efi as sshd starts, hands each
 * channel's bytes to the helper through the SftpIface table, and unloads
 * the helper as sshd stops.
 *
 * WHEN the helper is loaded is the first rule here. sftp_relay_init runs in
 * the driver's entry point, at TPL_APPLICATION; a session runs in an event
 * callback, at TPL_CALLBACK, where UEFI (2.11 Table 7.3) forbids LoadImage
 * and StartImage. EDK2 shows why: the callback can be dispatched from
 * inside CoreStartImage's unload of an application that just returned,
 * after CoreFreePages has given the pages back to the memory map and
 * released its lock but BEFORE it applies the NX policy for free memory to
 * them. A LoadImage there is handed those pages, and the policy applied
 * after the callback returns makes the helper's code RW + execute-never: an
 * instruction abort at the first call into it (measured on aarch64, where
 * ArmVirtQemu sets PcdDxeNxMemoryProtectionPolicy for free memory; x64
 * firmware that does the same has the same window). Loaded at start, the
 * helper is never the allocation inside that window.
 *
 * The lifetime rule is the second: every pointer into the helper -- the
 * table and each SftpSession -- is dropped BEFORE the image is unloaded.
 * Every channel has closed (session_close() has returned for each) before
 * sftp_relay_shutdown unloads it. The helper refuses to unload while it
 * holds a session, so an unload from anywhere else cannot strand us either.
 */

#include <axl.h>

#include "sshd-sftp-iface.h"
#include "sshd-sftp-relay.h"
#include "sshd-sftp-resume.h"

AXL_LOG_DOMAIN("sftp");

/** sshd-full.efi's driver (-DSSHD_EMBEDDED_SFTP) carries SftpServer.efi,
    the `sshd-full` goal (tools/CMakeLists.txt) splices it in
    as the embed blob sftp_server_raw (the unpacked image: the launcher
    around it is packed already). */
#ifdef SSHD_EMBEDDED_SFTP
AXL_EMBED_DECLARE(sftp_server_raw);
#endif

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/// The name sshd's protocol registry reaches SFTP_IFACE_GUID by.
#define SFTP_IFACE_NAME      "axl-sftp-iface"

/// Exit statuses a shell gives a command it cannot run, which is how a
/// client of OpenSSH's sshd sees a missing or broken sftp-server.
#define SFTP_EXIT_NOT_FOUND  127u
#define SFTP_EXIT_NO_START   126u

/// Most bytes of a refused subsystem name the log shows (sshd's %.100s).
#define SFTP_RELAY_NAME_MAX  101u

/// Room for the stderr line a failed start writes: a path and a reason.
#define SFTP_RELAY_MSG_MAX   600u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct SftpRelayChan {
    AxlSshChannel ch;
    SftpSession  *sess;    ///< the helper's session, or NULL
    bool          ended;   ///< the session asked for the channel to end
    SftpResume    resume;  ///< the resume a yield asked for, if pending
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static AxlSshServer    *m_srv;
static SftpRelayConfig  m_cfg;
static char            *m_path;      ///< m_cfg.path, owned
static AxlDriverHandle  m_image;     ///< the loaded helper, or NULL
static const SftpIface *m_iface;     ///< its table, valid while m_image is
static uint32_t         m_sessions;  ///< sessions open on m_iface
static int              m_load_rc;   ///< the start's load result, AXL_OK if loaded
static const char      *m_load_why;  ///< what a session is told when it failed

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static int  helper_load(const char **why);
static void helper_unload(void);
static void helper_log(void *ud, int level, const char *message);
static int  relay_write(void *ctx, const void *buf, size_t len);
static void relay_consumed(void *ctx, size_t n);
static void relay_end(void *ctx, uint32_t status);
static bool relay_resume(void *ctx);
static void resume_cb(void *data);
static void refuse_on_stderr(AxlSshChannel ch, const char *why, uint32_t status);
static const char *helper_name(void);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

int
sftp_relay_init(AxlSshServer *srv, const SftpRelayConfig *cfg)
{
    axl_free(m_path);
    m_path = axl_strdup(cfg->path != NULL ? cfg->path : "");
    if (m_path == NULL) {
        return AXL_NO_RESOURCES;
    }
    m_srv      = srv;
    m_cfg      = *cfg;
    m_cfg.path = m_path;
    /* The table is read back by name from the image handle; the name is
       this image's own alias for the GUID. */
    if (axl_protocol_register_name(SFTP_IFACE_NAME, &SFTP_IFACE_GUID) != AXL_OK) {
        return AXL_ERR;
    }
    if (!m_cfg.enabled) {
        /* Only a settings file can turn SFTP off: with none it is on. */
        axl_info("off (%s has no Subsystem line)",
                 cfg->conf != NULL && cfg->conf[0] != '\0' ? cfg->conf : "sshd.conf");
    } else if (m_cfg.read_only_forced) {
        /* Nobody asked for read-only SFTP by name: say what did. */
        axl_info("read-only (AxlReadOnly)");
    }
    if (m_cfg.enabled) {
        /* Now, in the driver's entry point, and never from a session (the
           file comment says why). A helper that cannot be loaded does not
           stop sshd: as OpenSSH's sshd with no sftp-server, each sftp
           session is refused, saying why. */
        m_load_why = NULL;
        m_load_rc  = helper_load(&m_load_why);
        if (m_load_rc != AXL_OK) {
            /* Known now, so said now, at the default level: the operator
               hears of a missing helper before the first client does. */
            axl_info("SFTP helper %s %s; sftp sessions are refused",
                     helper_name(), m_load_why);
        }
    }
    return AXL_OK;
}

void
sftp_relay_shutdown(void)
{
    if (m_iface != NULL) {
        /* sshd is going: whatever happens to the helper below, it must not
           keep a pointer to helper_log in an image about to unload. */
        m_iface->set_log(NULL, NULL, 0);
        m_iface->set_resume(NULL);
    }
    if (m_sessions != 0) {
        /* Every channel closes before this: a count left over is a defect
           here, and unloading under it would strand those sessions. */
        axl_error("%u SFTP session(s) still open at shutdown; helper left loaded",
                  (unsigned)m_sessions);
    } else {
        helper_unload();
    }
    axl_free(m_path);
    m_path = NULL;
    m_srv  = NULL;
}

// ---------------------------------------------------------------------------
// The helper image
// ---------------------------------------------------------------------------

static void
helper_log(void *ud, int level, const char *message)
{
    (void)ud;
    axl_log(level, "sftp", "%s", message);
}

/* Load and start the helper and read its table from its own image handle.
   On failure *why says what the client is told. */
static int
helper_load(const char **why)
{
    AxlDriverHandle h    = NULL;
    const char     *from = m_cfg.path;
    void           *t    = NULL;

    if (m_iface != NULL) {
        return AXL_OK;   /* loaded already: one helper per sshd */
    }
#ifdef SSHD_EMBEDDED_SFTP
    if (m_cfg.internal) {
        AxlEmbeddedImageInfo info = { .file_name = SFTP_HELPER_NAME };
        from = "(embedded)";
        if (axl_driver_load_buffer_with_image_info(AXL_EMBED_DATA(sftp_server_raw),
                                                   AXL_EMBED_SIZE(sftp_server_raw),
                                                   &info, &h) != AXL_OK) {
            *why = "cannot load";
            return AXL_ERR;
        }
    } else
#endif
    {
        AxlFsEntry e;
        if (m_cfg.path[0] == '\0' || axl_file_info(m_cfg.path, &e) != AXL_OK
            || axl_fs_entry_is_dir(&e)) {
            *why = "not found";
            return AXL_NOT_FOUND;
        }
        if (axl_driver_load(m_cfg.path, &h) != AXL_OK) {
            *why = "cannot load";
            return AXL_ERR;
        }
    }
    if (axl_driver_start(h) != AXL_OK) {
        /* The firmware unloads an image whose entry failed; this frees
           only what AXL still tracks. */
        if (axl_driver_unload(h) != AXL_OK) {
            axl_error("SFTP helper %s failed to start and could not be unloaded", from);
        }
        *why = "failed to start";
        return AXL_ERR;
    }
    if (axl_handle_get_protocol(h, SFTP_IFACE_NAME, &t) != AXL_OK
        || ((const SftpIface *)t)->size < sizeof(SftpIface)
        || ((const SftpIface *)t)->version != SFTP_IFACE_VERSION) {
        axl_error("SFTP helper %s published no interface this sshd can use", from);
        if (axl_driver_unload(h) != AXL_OK) {
            axl_error("SFTP helper %s could not be unloaded", from);
        }
        *why = "is not an SFTP helper for this sshd";
        return AXL_ERR;
    }
    m_image = h;
    m_iface = t;
    m_iface->set_log(helper_log, NULL, m_cfg.log_level);
    m_iface->set_resume(relay_resume);
    axl_info("SFTP helper loaded from %s", from);
    return AXL_OK;
}

/* As sshd stops, with no session open: take the log back and unload the
   image. A refused unload leaves it loaded, and it outlives sshd. */
static void
helper_unload(void)
{
    if (m_iface == NULL) {
        return;
    }
    m_iface->set_log(NULL, NULL, 0);
    m_iface->set_resume(NULL);
    if (axl_driver_unload(m_image) != AXL_OK) {
        /* Left loaded, it logs to the console again rather than into this
           image: never re-armed, so a helper that outlives sshd holds no
           pointer into it. */
        axl_error("SFTP helper could not be unloaded; it stays loaded");
        return;
    }
    m_iface = NULL;
    m_image = NULL;
    axl_info("SFTP helper unloaded");
}

// ---------------------------------------------------------------------------
// Callbacks the helper calls
// ---------------------------------------------------------------------------

/* A session yielded: have its session_resume() called from the loop. The
   pending call is cancelled if the channel closes first (sftp_relay_close),
   so it never reaches a freed session. */
static bool
relay_resume(void *ctx)
{
    SftpRelayChan *rc = ctx;

    return !rc->ended && sftp_resume_ask(&rc->resume, m_cfg.loop, resume_cb, rc);
}

static void
resume_cb(void *data)
{
    SftpRelayChan *rc = data;

    sftp_resume_fired(&rc->resume);
    if (rc->sess != NULL && !rc->ended) {
        axl_debug("resuming a yielded session");
        m_iface->session_resume(rc->sess);
    }
}

static int
relay_write(void *ctx, const void *buf, size_t len)
{
    SftpRelayChan *rc = ctx;

    if (rc->ended) {
        return AXL_ERR;
    }
    return axl_ssh_channel_write(m_srv, rc->ch, buf, len);
}

/* The session dispatched @a n more bytes: reopen that much of the
   client's window. */
static void
relay_consumed(void *ctx, size_t n)
{
    SftpRelayChan *rc = ctx;

    if (rc->ended) {
        return;
    }
    int r = axl_ssh_channel_consumed(m_srv, rc->ch, n);
    if (r == AXL_NOT_FOUND) {
        /* The channel is gone -- its connection is being dropped, and this
           is a deferred resume's pump running first. Nothing is owed to a
           peer that is no longer there, and the session's next write ends
           it. */
        axl_debug("SFTP: a report of %zu consumed bytes found no channel", n);
    } else if (r != AXL_OK) {
        /* The session reported more than it was handed: a defect in the
           helper, which would otherwise leave the window short for ever. */
        axl_error("SFTP: a report of %zu consumed bytes was refused (%d)", n, r);
    }
}

static void
relay_end(void *ctx, uint32_t status)
{
    SftpRelayChan *rc = ctx;

    if (!rc->ended) {
        rc->ended = true;
        /* Nothing more is dispatched, so nothing more is reported: credit
           what the session still held, and whatever the client sends from
           now on as it arrives, as sftp_relay_data drops it unread. */
        if (axl_ssh_channel_set_window_mode(m_srv, rc->ch,
                                            AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL)
            != AXL_OK) {
            axl_debug("the ended session's channel is already gone");
        }
        if (axl_ssh_channel_exit(m_srv, rc->ch, status) != AXL_OK) {
            axl_ssh_channel_close(m_srv, rc->ch);
        }
    }
}

// ---------------------------------------------------------------------------
// Channel hooks
// ---------------------------------------------------------------------------

/* The helper as messages name it: its file, or its name when it has none. */
static const char *
helper_name(void)
{
    return m_cfg.path[0] != '\0' ? m_cfg.path : SFTP_HELPER_NAME;
}

/* Tell the client why on stderr, then exit as a shell would. */
static void
refuse_on_stderr(AxlSshChannel ch, const char *why, uint32_t status)
{
    char msg[SFTP_RELAY_MSG_MAX];
    int  n = axl_snprintf(msg, sizeof(msg), "sshd: subsystem sftp: %s %s\r\n",
                          helper_name(), why);

    if (n > 0 && axl_ssh_channel_write_stderr(m_srv, ch, msg, (size_t)n) != AXL_OK) {
        axl_debug("the refusal did not fit the channel");
    }
    if (axl_ssh_channel_exit(m_srv, ch, status) != AXL_OK) {
        axl_ssh_channel_close(m_srv, ch);
    }
}

int
sftp_relay_open(AxlSshChannel ch, const char *name, size_t len, SftpRelayChan **out)
{
    *out = NULL;
    if (m_srv == NULL) {
        return AXL_ERR;
    }
    if (len != 4 || axl_strcmp(name, "sftp") != 0 || !m_cfg.enabled) {
        /* sshd's own line for a subsystem it does not serve. */
        char        shown[SFTP_RELAY_NAME_MAX];
        const char *user = axl_ssh_channel_user(m_srv, ch);
        sftp_printable(shown, sizeof(shown), name, len);
        axl_info("subsystem request for %s by user %s failed, subsystem not found",
                 shown, user != NULL ? user : "?");
        return AXL_ERR;
    }
    if (m_iface == NULL) {
        /* The start's load failed; a load here could land in pages the
           firmware is about to make execute-never (the file comment). */
        const char *why = m_load_why != NULL ? m_load_why : "cannot load";
        axl_error("subsystem sftp: %s %s", helper_name(), why);
        refuse_on_stderr(ch, why, m_load_rc == AXL_NOT_FOUND ? SFTP_EXIT_NOT_FOUND
                                                             : SFTP_EXIT_NO_START);
        return AXL_OK;
    }
    SftpRelayChan *c = axl_calloc(1, sizeof(*c));
    if (c == NULL) {
        return AXL_NO_RESOURCES;
    }
    c->ch   = ch;
    c->sess = m_iface->session_open(relay_write, relay_end, relay_consumed, c,
                                    m_cfg.read_only ? SFTP_SESSION_READ_ONLY : 0u,
                                    m_cfg.write_max);
    if (c->sess == NULL) {
        axl_free(c);
        return AXL_NO_RESOURCES;
    }
    m_sessions++;
    /* The session reports what it dispatches, so the client's window follows
       the session, not the network: set here, in the subsystem hook, before
       the window is first granted, so no byte is delivered in the other
       mode. */
    if (axl_ssh_channel_set_window_mode(m_srv, ch, AXL_SSH_CHANNEL_WINDOW_ON_CONSUME)
        != AXL_OK) {
        axl_error("SFTP: the channel's window cannot follow the session");
    }
    axl_info("subsystem request for sftp by user %s",
             axl_ssh_channel_user(m_srv, ch) != NULL ? axl_ssh_channel_user(m_srv, ch) : "?");
    *out = c;
    return AXL_OK;
}

void
sftp_relay_data(SftpRelayChan *rc, const uint8_t *buf, size_t len)
{
    if (rc != NULL && rc->sess != NULL && !rc->ended) {
        m_iface->session_data(rc->sess, buf, len);
    }
}

void
sftp_relay_writable(SftpRelayChan *rc)
{
    if (rc != NULL && rc->sess != NULL && !rc->ended) {
        m_iface->session_writable(rc->sess);
    }
}

void
sftp_relay_eof(SftpRelayChan *rc)
{
    if (rc != NULL && rc->sess != NULL && !rc->ended) {
        m_iface->session_eof(rc->sess);
    }
}

void
sftp_relay_close(SftpRelayChan *rc)
{
    if (rc == NULL) {
        return;
    }
    if (rc->resume.handle != 0) {
        /* Closed between a yield and its resume (the same tick): the
           resume must never reach the session freed below. */
        axl_debug("cancelling a pending resume: its channel closed");
    }
    if (!sftp_resume_cancel(&rc->resume, m_cfg.loop)) {
        axl_error("a pending SFTP resume could not be cancelled");
    }
    if (rc->sess != NULL) {
        m_iface->session_close(rc->sess);
        rc->sess = NULL;
        m_sessions--;
    }
    axl_free(rc);
}
