/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-admit.h
    The admission decision, factored out so the unit test can drive it.

    axl-ssh-server.c's on_accept() has to weigh a newly-accepted connection
    against two numbers at once -- the total connection cap and the pre-auth
    ("startups") cap -- and a boolean it can only get by walking the
    connection list first: does an unauthenticated connection exist that has
    not yet finished key exchange, the only kind eviction is allowed to
    take. axl_ssh_admit_decide() is that whole decision, pulled out as a
    pure function of plain values (no AxlSshServer, no connection list, no
    walk) so it can be pinned directly -- the same reason axl-ssh-reap.h
    pulls axl_ssh_reap_reason() out of on_reap(). on_accept()'s admission
    check calls this same function, so the unit test exercises the real
    decision rather than a restatement of it.
**/
#ifndef AXL_SSH_ADMIT_H
#define AXL_SSH_ADMIT_H

#include <stdbool.h>

/** What axl_ssh_admit_decide() says to do with a newly-accepted
    connection. */
typedef enum {
    AXL_SSH_ADMIT_ACCEPT = 0, /**< room: take the new connection */
    AXL_SSH_ADMIT_EVICT  = 1, /**< full: drop the victim, take the new one */
    AXL_SSH_ADMIT_REFUSE = 2  /**< full and no victim: abortive close */
} AxlSshAdmit;

/** Should a newly-accepted connection be admitted, and at whose expense?

    There is room, and the new connection is admitted outright, when
    @a nconns is below @a max_conns AND @a npreauth is below
    @a max_startups. Otherwise the server is full by one measure or the
    other, and @a have_victim decides what happens to the new connection:
    an unauthenticated connection that has not yet finished key exchange is
    evicted to make room for it, or, if there is no such connection, the
    new one is refused instead.

    @a have_victim is consulted ONLY when the server is full -- its
    presence must never cost anyone a connection while there is still
    room. That is what distinguishes an eviction from a refusal: both are
    "full" outcomes, and which one a caller gets depends solely on whether
    a victim exists, never on how full the server is.

    @return #AXL_SSH_ADMIT_ACCEPT, #AXL_SSH_ADMIT_EVICT or
        #AXL_SSH_ADMIT_REFUSE. */
AxlSshAdmit
axl_ssh_admit_decide(
    unsigned nconns,       ///< connections held now, of any state
    unsigned npreauth,     ///< ...of which not yet authenticated
    unsigned max_conns,    ///< MAX_CONNS
    unsigned max_startups, ///< the server's pre-auth cap
    bool     have_victim   ///< an unauthenticated connection exists that has
                           ///< not finished key exchange
);

#endif /* AXL_SSH_ADMIT_H */
