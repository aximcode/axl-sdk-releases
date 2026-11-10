/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-txq.c
    The per-connection transmit queue axl-ssh-internal.h declares: pure
    bookkeeping over one pending run and one in-flight run.

    No socket, no loop and no firmware here, so the unit test drives it
    directly. The server (axl-ssh-server.c) owns the policy -- when a flush
    is scheduled, and what a completion does -- and this module owns the
    one invariant it must never break: frames leave in the order they were
    pushed, each exactly once.
**/

#include "axl-ssh-internal.h"
#include <axl/axl-mem.h>

int
axl_ssh_txq_init(
    AxlSshTxq *q  ///< queue to initialise
)
{
    if (q == NULL) {
        return AXL_ERR;
    }
    axl_memset(q, 0, sizeof *q);
    q->pending = axl_string_new("");
    return q->pending != NULL ? AXL_OK : AXL_ERR;
}

void
axl_ssh_txq_free(
    AxlSshTxq *q  ///< queue to release (NULL-safe)
)
{
    if (q == NULL) {
        return;
    }
    axl_string_free(q->pending);
    axl_memset(q, 0, sizeof *q);
}

int
axl_ssh_txq_push(
    AxlSshTxq  *q,      ///< queue
    const void *frame,  ///< one whole frame
    size_t      len     ///< its length; 0 is refused
)
{
    if (q == NULL || q->pending == NULL || frame == NULL || len == 0) {
        return AXL_ERR;
    }
    /* axl_string_append_len leaves the string as it was on failure, so a
       refused push neither counts a frame nor leaves half of one behind. */
    if (axl_string_append_len(q->pending, (const char *)frame, len)
        != AXL_OK) {
        return AXL_ERR;
    }
    q->pending_frames++;
    return AXL_OK;
}

bool
axl_ssh_txq_ready(
    const AxlSshTxq *q  ///< queue
)
{
    return q != NULL && q->inflight_len == 0 && q->pending_frames != 0;
}

int
axl_ssh_txq_take(
    AxlSshTxq *q,      ///< queue
    uint8_t  **buf,    ///< receives the run; the caller frees it
    size_t    *len,    ///< receives its length
    unsigned  *frames  ///< receives its frame count
)
{
    if (buf == NULL || len == NULL || frames == NULL
        || !axl_ssh_txq_ready(q)) {
        return AXL_ERR;
    }
    size_t   n = axl_string_len(q->pending);
    /* STEAL rather than copy: the string's own buffer becomes the send's,
       and the string is left empty and reusable (the next push reallocates
       it). The whole run, never part of one -- a split would be harmless
       for order but would count frames that straddle two sends. */
    uint8_t *run = (uint8_t *)axl_string_steal(q->pending);
    if (run == NULL) {
        return AXL_ERR;
    }
    *buf   = run;
    *len   = n;
    *frames = q->pending_frames;
    q->inflight_len    = n;
    q->inflight_frames = q->pending_frames;
    q->pending_frames  = 0;
    return AXL_OK;
}

void
axl_ssh_txq_sent(
    AxlSshTxq *q  ///< queue
)
{
    if (q == NULL) {
        return;
    }
    q->inflight_len    = 0;
    q->inflight_frames = 0;
}

size_t
axl_ssh_txq_bytes(
    const AxlSshTxq *q  ///< queue
)
{
    if (q == NULL || q->pending == NULL) {
        return 0;
    }
    return axl_string_len(q->pending) + q->inflight_len;
}
