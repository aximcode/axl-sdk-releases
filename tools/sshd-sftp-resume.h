/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * sshd-sftp-resume.h - the bookkeeping behind a yielded SFTP session's
 * resume (SftpResumeFn): at most one deferred call per session, and one
 * that is cancelled when the session's channel closes first, so it never
 * reaches a freed session. Header-only and free of sshd's state, so the
 * relay (sshd-sftp-relay.c) uses it and AxlTestSshdSftp drives it against
 * a real AxlLoop.
 */

#ifndef AXL_TOOLS_SSHD_SFTP_RESUME_H
#define AXL_TOOLS_SSHD_SFTP_RESUME_H

#include <axl.h>

/// One session's pending resume. Zero-initialise.
typedef struct {
    uint32_t handle;   ///< the pending axl_defer() call, or 0 for none
} SftpResume;

/**
 * Have @p fn(@p data) called from @p loop's next tick, unless a call is
 * already pending -- then that one serves.
 *
 * @return true when a call is pending once this returns; false when none
 *     could be scheduled (no loop, or its defer queue is full).
 */
static inline bool
sftp_resume_ask(SftpResume *r, AxlLoop *loop, AxlDeferCallback fn, void *data)
{
    if (r->handle != 0) {
        return true;
    }
    if (loop == NULL) {
        return false;
    }
    r->handle = axl_defer(loop, fn, data);
    return r->handle != 0;
}

/// The pending call is running: from now on none is pending.
static inline void
sftp_resume_fired(SftpResume *r)
{
    r->handle = 0;
}

/**
 * Cancel the pending call, if there is one, so it never runs.
 *
 * @return true when nothing is left pending; false when the loop no longer
 *     knew the call (a bookkeeping defect: the caller logs it).
 */
static inline bool
sftp_resume_cancel(SftpResume *r, AxlLoop *loop)
{
    if (r->handle == 0) {
        return true;
    }
    bool ok = axl_defer_cancel(loop, r->handle);
    r->handle = 0;
    return ok;
}

#endif /* AXL_TOOLS_SSHD_SFTP_RESUME_H */
