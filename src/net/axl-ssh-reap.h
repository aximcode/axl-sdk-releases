/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-reap.h
    The login-grace reap decision, factored out so the unit test can drive it.

    axl-ssh-server.c's connection struct used to keep the login grace in one
    field, born_ms: zero meant "authenticated, never reap", and zero was also
    what a failed monotonic clock reading produced, so an unreadable clock
    made every connection look authenticated and the grace silently stopped
    existing. The fix replaced it with three independent fields -- see the
    SshConn comment in axl-ssh-server.c -- and axl_ssh_reap_reason() is the
    whole decision that keeps them from colliding again, pulled out as a pure
    function of plain values (no SshConn, no clock call) so it can be pinned
    directly: no fake clock, no live socket, no event loop. axl-ssh-server.c's
    on_reap() calls this same function, so the unit test exercises the real
    decision rather than a restatement of it.
**/
#ifndef AXL_SSH_REAP_H
#define AXL_SSH_REAP_H

#include <stdbool.h>
#include <stdint.h>

/** Should the reaper drop this connection, and why?

    @a authenticated exempts the connection UNCONDITIONALLY -- regardless of
    @a deadline_armed, @a have_now or how far past @a deadline_ms @a now is.
    That exemption is the entire point of splitting the old born_ms sentinel,
    and re-merging it (making the exemption depend on the deadline fields
    again) is exactly the regression this function exists to catch.

    Otherwise: an unarmed @a deadline_armed (no readable clock at accept)
    drops unconditionally -- a login grace that cannot be enforced is not
    granted. An armed deadline drops only when @a have_now is true and
    @a now is past @a deadline_ms; a failed reading at reap time
    (@a have_now false) must never manufacture an expiry by comparing
    against a meaningless @a now.

    @return a non-NULL reason string if the connection should be dropped,
    NULL to keep it. */
const char *
axl_ssh_reap_reason(
    bool     authenticated,  ///< reached USERAUTH_SUCCESS
    bool     deadline_armed, ///< deadline_ms came from a real clock reading
    bool     have_now,       ///< this reap tick's own clock reading succeeded
    uint64_t now,            ///< this reap tick's monotonic ms, if have_now
    uint64_t deadline_ms     ///< monotonic ms after which the grace runs out
);

/** How often the reaper should tick, in ms, for a server whose login grace
    is @a grace_sec (axl_ssh_server_set_login_grace), whose unused
    timeout is @a unused_sec and whose client-alive interval is @a alive_sec
    (axl-ssh-idle.h).

    The library's own reap cadence is 5000 ms (REAP_INTERVAL_MS,
    axl-ssh-server.c) -- coarse on purpose, since a pre-auth deadline does
    not need precision. This returns that ceiling, or half of the finest of
    the three periods in ms, whichever is FINER (smaller). A period of 0 is
    "off" and ignored. For the default grace (60 s) and no idle periods the
    ceiling wins and this is unchanged from the fixed cadence the reaper
    always had; a 1 s grace ticks at 500 ms rather than being silently
    rounded up to a lag longer than the grace itself. All arithmetic is in
    64 bits, so the 30 day maximum of each period cannot overflow.

    Pulled out as a pure function of plain values, like axl_ssh_reap_reason
    above, so the sub-ceiling branch -- exercised by no integration
    scenario, since every one of them uses a grace of 10 s or more -- has
    unit coverage a regression that made this always return the ceiling
    would still fail.

    @return the tick, in milliseconds; never 0 for any period >= 1,
        since half of 1000 ms is 500. Assumes @a grace_sec has already been
        validated by axl_ssh_server_set_login_grace (1 <=
        @a grace_sec <= AXL_SSH_LOGIN_GRACE_MAX_SEC); it is not itself a
        validating function. */
uint32_t
axl_ssh_reap_tick_ms(
    uint32_t grace_sec,   ///< the login grace this tick is derived from, in seconds
    uint32_t unused_sec,  ///< UnusedConnectionTimeout in seconds; 0 = off, ignored
    uint32_t alive_sec    ///< ClientAliveInterval in seconds; 0 = off, ignored
);

/** Does conn_drop START the linger (P5) rather than reset the connection?
    Only for a connection whose DISCONNECT went out on a healthy stream
    (@a is_linger: ST_LINGER) and whose linger has not started, while the
    server runs -- and never once the stream has lost a run (@a tx_broken):
    a run in flight ahead of the DISCONNECT can fail between disconnect()
    and the drop that follows it (a schedule_drop, or a drop deferred under
    a hook), and a peer that missed bytes cannot decrypt what comes after
    them (contract review M7, midpoint review MP1). A pure function, like
    axl_ssh_reap_reason, so the unit test pins every combination.
    @return true to linger. */
bool
axl_ssh_linger_due(
    bool is_linger,  ///< the connection is ST_LINGER
    bool lingering,  ///< its linger has already started
    bool running,    ///< the server is not being freed
    bool tx_broken   ///< a run was lost on the stream
);

#endif /* AXL_SSH_REAP_H */
