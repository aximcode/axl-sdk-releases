/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-idle.c
    Idle decisions; see axl-ssh-idle.h for the rules and their sshd sources. */

#include "axl-ssh-idle.h"

#define MS_PER_SEC  1000ull

/* The expiry a clock reading at @a now implies. uint64_t: 2^32 s * 1000 fits. */
static uint64_t
expiry_from(const AxlSshIdle *d, uint64_t now)
{
    return now + (uint64_t)d->unused_sec * MS_PER_SEC;
}

void
axl_ssh_idle_start(AxlSshIdle *d, uint32_t unused_sec, uint32_t alive_sec,
                   uint32_t alive_max, bool have_now, uint64_t now)
{
    d->unused_sec = unused_sec;
    d->alive_sec  = alive_sec;
    d->alive_max  = alive_max;
    d->unanswered = 0;
    d->armed      = have_now;
    d->heard_ms   = have_now ? now : 0;
    d->bytes_ms   = d->heard_ms;
    d->unused_expiry_ms = have_now ? expiry_from(d, now) : 0;
}

/* serverloop.c:239-240: any bytes restart the silence clock. */
void
axl_ssh_idle_heard(AxlSshIdle *d, bool have_now, uint64_t now)
{
    if (d->armed && have_now) {
        d->heard_ms = now;
        d->bytes_ms = now;
    }
}

/* packet.c:1814: every decoded packet zeroes keep_alive_timeouts. */
void
axl_ssh_idle_answered(AxlSshIdle *d)
{
    d->unanswered = 0;
}

void
axl_ssh_idle_channel_released(AxlSshIdle *d, bool have_now, uint64_t now)
{
    if (d->armed && have_now) {
        d->unused_expiry_ms = expiry_from(d, now);
    }
}

AxlSshIdleAct
axl_ssh_idle_tick(AxlSshIdle *d, const AxlSshIdleConn *conn, bool have_now, uint64_t now)
{
    bool probe_due = false;

    if (!d->armed || !have_now) {
        return AXL_SSH_IDLE_KEEP;
    }

    /* serverloop.c:233-241, client_alive_scheduled. */
    if (d->alive_sec != 0 && !conn->rx_gated
        && now >= d->heard_ms + (uint64_t)d->alive_sec * MS_PER_SEC) {
        /* serverloop.c:113-118: increment before the compare; count_max 0
           short-circuits before the increment. */
        if (d->alive_max > 0 && ++d->unanswered > d->alive_max) {
            return AXL_SSH_IDLE_DROP_SILENT;
        }
        d->heard_ms = now;
        probe_due = true;
    }

    /* serverloop.c:246-247: refreshed while in use; strictly past it, with
       nothing open, drops. Probed first, as sshd does, so DROP_SILENT above
       outranks this. */
    if (d->unused_sec != 0) {
        if (conn->in_use) {
            d->unused_expiry_ms = expiry_from(d, now);
        } else if (now > d->unused_expiry_ms) {
            return AXL_SSH_IDLE_DROP_UNUSED;
        }
    }

    return (probe_due && conn->can_send) ? AXL_SSH_IDLE_PROBE : AXL_SSH_IDLE_KEEP;
}

/* channels.c:895-934, channel_still_open: OPEN counts, LARVAL does not. */
bool
axl_ssh_idle_in_use(const AxlSshChanSlot *tab, unsigned cap,
                    unsigned chan_hooks)
{
    if (chan_hooks > 0) {
        return true;
    }
    for (unsigned i = 0; tab != NULL && i < cap; i++) {
        if (tab[i].id != 0 && tab[i].started) {
            return true;
        }
    }
    return false;
}

/* channels.c:955-993, channel_find_open: the first channel with a remote
   id, LARVAL included -- but never one closing on either side, which
   OpenSSH would not answer (clientloop.c:1998). */
int
axl_ssh_idle_probe_slot(const AxlSshChanSlot *tab, unsigned cap)
{
    for (unsigned i = 0; tab != NULL && i < cap; i++) {
        if (tab[i].id != 0 && !tab[i].close_sent && !tab[i].close_in) {
            return (int)i;
        }
    }
    return -1;
}

bool
axl_ssh_idle_scans(bool authenticated, bool drop_deferred, bool dead)
{
    return authenticated && !drop_deferred && !dead;
}
