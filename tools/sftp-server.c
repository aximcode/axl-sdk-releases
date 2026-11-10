/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * sftp-server.c - SftpServer.efi, sshd's SFTP helper: a driver image that
 * does nothing on its own. Its entry point publishes the SftpIface table
 * (sshd-sftp-iface.h) on its own image handle; sshd, which loaded it, reads
 * the table from that handle and drives every session from its loop. Its
 * unload callback refuses while a session is open, so the image can never
 * leave from under a session that sshd still holds.
 */

#include <axl.h>

#include "sshd-sftp.h"
#include "sshd-sftp-iface.h"

AXL_LOG_DOMAIN("sftp");

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static void sftp_set_log(SftpLogFn fn, void *ud, int level);

static SftpIface m_iface = {
    .size             = sizeof(SftpIface),
    .version          = SFTP_IFACE_VERSION,
    .session_open     = sftp_session_open,
    .session_data     = sftp_session_data,
    .session_writable = sftp_session_writable,
    .session_resume   = sftp_session_resume,
    .session_eof      = sftp_session_eof,
    .session_close    = sftp_session_close,
    .set_log          = sftp_set_log,
    .set_resume       = sftp_set_resume,
};

static AxlHandle m_image;    ///< our image handle: where the table lives
static SftpLogFn m_log;      ///< where log lines go, or NULL for the console
static void     *m_log_ud;   ///< passed to m_log

// ---------------------------------------------------------------------------
// Log forwarding
// ---------------------------------------------------------------------------

/* Every line that passes this image's level goes to sshd's log. */
static void
log_forward(int level, const char *domain, const char *message,
            const AxlRealtime *stamp, void *data)
{
    (void)domain;
    (void)stamp;
    (void)data;
    if (m_log != NULL) {
        m_log(m_log_ud, level, message);
    }
}

static void
sftp_set_log(SftpLogFn fn, void *ud, int level)
{
    if (fn != NULL && m_log == NULL) {
        if (axl_log_add_handler(log_forward, NULL) != AXL_OK) {
            return;   /* the console keeps it */
        }
        axl_log_set_console_enabled(false);
    } else if (fn == NULL && m_log != NULL) {
        axl_log_remove_handler(log_forward);
        axl_log_set_console_enabled(true);
    }
    m_log    = fn;
    m_log_ud = ud;
    if (fn != NULL) {
        axl_log_set_level(level);
    }
}

// ---------------------------------------------------------------------------
// Entry and unload (external: AXL_DRIVER declares them)
// ---------------------------------------------------------------------------

int
sftp_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)st;
    m_image = image;
    if (axl_protocol_install(&SFTP_IFACE_GUID, &m_iface, &m_image) != AXL_OK) {
        axl_error("cannot publish the SFTP interface");
        return AXL_ERR;
    }
    return AXL_OK;
}

int
sftp_unload(AxlHandle image)
{
    (void)image;
    uint32_t n = sftp_sessions_open();

    if (n > 0) {
        axl_error("unload refused: %u SFTP session(s) still open", (unsigned)n);
        return AXL_ERR;
    }
    if (axl_protocol_uninstall(m_image, &SFTP_IFACE_GUID, &m_iface) != AXL_OK) {
        axl_error("unload refused: the SFTP interface is still in use");
        return AXL_ERR;
    }
    return AXL_OK;
}

AXL_DRIVER(sftp_entry, sftp_unload)
