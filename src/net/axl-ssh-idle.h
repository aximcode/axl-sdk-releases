/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-idle.h
    Idle handling as pure decisions: sshd's UnusedConnectionTimeout and
    ClientAliveInterval / ClientAliveCountMax.

    Nothing here touches a socket, the loop, the clock or a connection.
    axl-ssh-server.c reports what it saw (bytes heard, a packet decoded, the
    last in-use channel freed), asks axl_ssh_idle_tick() once per reaper tick
    and does exactly what the answer says -- the shape axl-ssh-rekey.h set,
    so the unit test pins every decision with no fake clock.

    Every rule is OpenSSH 9.9p1's, read from its source:

    - Probe (serverloop.c:233-241): when now >= last_client_time + interval,
      client_alive_check() runs, then last_client_time = now. Any bytes
      received set last_client_time = now (:239-240).
    - Count (serverloop.c:113-118): with count_max > 0, the unanswered count
      is incremented BEFORE it is compared, so count_max probes go out and
      the next due check drops. With count_max 0 the && short-circuits and
      the count never moves, so the peer is probed forever and never
      dropped.
    - Answered (packet.c:1814): every packet decoded after authentication
      zeroes the count (keep_alive_timeouts), whatever its type. Bytes alone
      do not: they reset the clock, not the count.
    - Unused (serverloop.c:246-250): dropped when now > expiry (strictly)
      and no channel is open; the expiry is refreshed on every loop pass
      while one is.
    - Rank (serverloop.c:233-250): the probe check runs before the unused
      check, so an unresponsive peer is reported as that, not as unused.

    Deviations from sshd, each deliberate:

    - Output paused (the peer is not reading): a due probe is not queued,
      because a dead connection's queue would grow without bound under
      count_max 0. The count still advances and the clock is still set as
      if it had been sent; sshd queues regardless.
    - Input gated by this server's own hook: no probe and no count. The
      peer's answers sit unread because of the application, not the peer.
    - A failed clock reading decides KEEP and changes nothing; sshd's
      monotime() cannot fail.
    - A probe goes out on every tick it is due, even while the server is
      sending; sshd checks only when its ppoll() returned 0, with nothing
      to do (serverloop.c:235). A client that answers sees nothing
      different.
    - A channel closing on either side is never the probe's target
      (axl_ssh_idle_probe_slot): OpenSSH answers no request on a channel
      it has sent CLOSE on (clientloop.c:1998), so sshd's choice of one
      would draw no answer. With no other channel the probe is a
      GLOBAL_REQUEST, which is always answered.

    The count and the periods are two separate facts. alive_max takes any
    uint32_t: the unanswered count is a uint32_t compared against it after
    each increment, so UINT32_MAX can never be exceeded -- the count wraps
    to 0 first -- and that value probes forever, as 0 does. The periods are computed in uint64_t
    milliseconds, where no period up to AXL_SSH_IDLE_MAX_SEC can overflow.
**/
#ifndef AXL_SSH_IDLE_H
#define AXL_SSH_IDLE_H

#include "axl-ssh-chan.h"
#include <axl/axl-ssh-server.h>
#include <stdbool.h>
#include <stdint.h>

/** What the caller must do. It acts on this and nothing else. */
typedef enum {
    AXL_SSH_IDLE_KEEP,          ///< nothing to send or drop
    AXL_SSH_IDLE_PROBE,         ///< send keepalive@openssh.com now
    AXL_SSH_IDLE_DROP_SILENT,   ///< log "Timeout, client not responding", drop
    AXL_SSH_IDLE_DROP_UNUSED    ///< log "terminating inactive connection", drop
} AxlSshIdleAct;

/** One connection's idle state. Lives inside the connection; no allocation,
    nothing to free. Initialised by axl_ssh_idle_start(). */
typedef struct {
    uint32_t unused_sec;        ///< unused timeout, seconds; 0 = off
    uint32_t alive_sec;         ///< alive interval, seconds; 0 = off
    uint32_t alive_max;         ///< probes before dropping; 0 = probe, never drop
    bool     armed;             ///< started from a real clock reading
    uint64_t heard_ms;          ///< last bytes heard, or last probe due
    uint64_t bytes_ms;          ///< last bytes heard, only: what a drop
                                ///< reports as the silence it ended
    uint64_t unused_expiry_ms;  ///< refreshed while a channel is in use
    uint32_t unanswered;        ///< probes since the last decoded packet
} AxlSshIdle;

/** What the tick needs to know about the connection right now. */
typedef struct {
    bool in_use;     ///< a started channel exists (sshd channel_still_open)
    bool can_send;   ///< output is not paused
    bool rx_gated;   ///< receive parked by this server's own hook or parse gate
} AxlSshIdleConn;

/** Start the clocks at authentication.

    With @a have_now false the state is left unarmed and every later tick
    answers KEEP: an unreadable clock never manufactures an expiry. */
void
axl_ssh_idle_start(
    AxlSshIdle *d,           ///< state to initialise
    uint32_t    unused_sec,  ///< unused timeout, seconds; 0 = off
    uint32_t    alive_sec,   ///< alive interval, seconds; 0 = off
    uint32_t    alive_max,   ///< probes before dropping; 0 = never drop
    bool        have_now,    ///< @a now is a real reading
    uint64_t    now          ///< monotonic ms
);

/** Bytes arrived: the silence clock restarts, the count does not
    (serverloop.c:239-240). No effect unarmed or without a clock. */
void
axl_ssh_idle_heard(
    AxlSshIdle *d,         ///< state
    bool        have_now,  ///< @a now is a real reading
    uint64_t    now        ///< monotonic ms
);

/** A packet was decoded: the count restarts, the clock does not
    (packet.c:1814). */
void
axl_ssh_idle_answered(
    AxlSshIdle *d  ///< state
);

/** The last in-use channel was freed: the unused clock runs from now, as
    sshd's loop would have refreshed it on the CLOSE that woke it. No effect
    unarmed or without a clock. */
void
axl_ssh_idle_channel_released(
    AxlSshIdle *d,         ///< state
    bool        have_now,  ///< @a now is a real reading
    uint64_t    now        ///< monotonic ms
);

/** One reaper tick. Mutates @a d: refreshes the unused expiry while a
    channel is in use, and applies the count rule when a probe is due.

    Idempotent at one @a now: a second call at the same instant returns KEEP
    and leaves @a d byte-identical, because the reaper may scan the same
    connection twice after a drop elsewhere.

    PROBE is returned only when @a conn->can_send; otherwise the count and
    the clock advance as if it had been sent and KEEP is returned (or
    DROP_SILENT when the count is exceeded). With @a conn->rx_gated the alive
    half does nothing.

    @return the action; DROP_SILENT outranks DROP_UNUSED, which outranks
    PROBE. */
AxlSshIdleAct
axl_ssh_idle_tick(
    AxlSshIdle           *d,         ///< state
    const AxlSshIdleConn *conn,      ///< what the connection looks like now
    bool                  have_now,  ///< @a now is a real reading
    uint64_t              now        ///< monotonic ms
);

/** Is a channel IN USE, for the unused timeout? sshd's channel_still_open
    (channels.c:895-934) counts an OPEN channel and skips a LARVAL one;
    here that is an allocated slot (`id != 0`) whose shell or exec hook has
    accepted (`started`), until the slot is freed -- a half-closed or
    closing channel included, as sshd never makes a session channel
    CLOSED. A channel hook on the stack (@a chan_hooks > 0) counts too: the
    application is running on behalf of a channel, whatever its state.

    @return true when one is. */
bool
axl_ssh_idle_in_use(
    const AxlSshChanSlot *tab,        ///< the connection's channel table
    unsigned              cap,        ///< slots in @a tab
    unsigned              chan_hooks  ///< channel hooks on the stack now
);

/** The slot a keepalive goes to, as a CHANNEL_REQUEST. sshd's
    channel_find_open (channels.c:955-993) takes the first LARVAL or OPEN
    channel with a remote id; here every allocated slot has the peer's id
    (it is set from the peer's CHANNEL_OPEN), and the first one where
    neither side has sent CLOSE is taken. A closing one never is (see the
    deviations above).

    @return the slot index, or -1 for none: the probe is then a
        GLOBAL_REQUEST. */
int
axl_ssh_idle_probe_slot(
    const AxlSshChanSlot *tab,  ///< the connection's channel table
    unsigned              cap   ///< slots in @a tab
);

/** Does the reaper's idle pass tick this connection? Only an authenticated
    one that is still live: not a drop conn_drop deferred, and not one
    already marked to end (disconnect() ahead of a scheduled drop), which
    stays listed and authenticated until the reply timer frees it -- a
    probe or an idle line for it would be about a connection the server
    has already ended.

    @return true when it does. */
bool
axl_ssh_idle_scans(
    bool authenticated,  ///< past USERAUTH_SUCCESS
    bool drop_deferred,  ///< conn_drop deferred its drop
    bool dead            ///< already marked to end
);

/** The idle configuration a live server will start its authenticated
    connections with (axl_ssh_server_set_unused_timeout,
    axl_ssh_server_set_client_alive), for the unit test only. Defined in
    axl-ssh-server.c, which owns the struct.

    @return #AXL_OK with every out parameter written; #AXL_ERR when @a s is
        not live or any out parameter is NULL. */
int
axl_ssh_server_idle_config(
    const AxlSshServer *s,          ///< server from axl_ssh_server_new()
    uint32_t           *unused_sec, ///< receives the unused timeout
    uint32_t           *alive_sec,  ///< receives the alive interval
    uint32_t           *alive_max   ///< receives the alive count
);

#endif /* AXL_SSH_IDLE_H */
