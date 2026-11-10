/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-penalty.h
    Per-source penalties (axl_ssh_server_set_penalties), as pure functions.

    Everything that DECIDES lives here -- which accrual a finished connection
    earns, how a source's penalty grows, activates, burns down and expires,
    what the 256-slot table does when it is full, and how an exempt range is
    parsed and matched. None of it reads a clock, a socket or a connection:
    time arrives as an argument, an address as four octets. axl-ssh-server.c
    only gathers those values and acts on the answers, so the unit test pins
    the real decisions directly -- the shape axl-ssh-reap.h set for the login
    grace, and for the same reason.

    The model is OpenSSH 9.9p1's PerSourcePenalties, MEASURED against a
    throwaway sshd rather than recalled: a penalty is deferred until it
    exceeds min_sec, activates, and is then enforced until it has burned down
    to nothing -- min_sec decides when refusing starts, not how long it
    lasts. The header's AxlSshPenalties comment carries the measurement.

    Addresses stay as octets end to end -- axl_tcp_get_remote_addr's string
    through axl_ipv4_parse at accept, axl_ipv4_parse_cidr for an exempt entry,
    axl_ipv4_equals and axl_ipv4_in_subnet to compare -- so no byte order is
    chosen anywhere in this module and none can disagree.
**/
#ifndef AXL_SSH_PENALTY_H
#define AXL_SSH_PENALTY_H

#include <axl/axl-ssh-server.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Sources one server remembers (ruling P5). The header's AxlSshPenalties
    comment says why 256: a full table is a BYPASS, not a slowdown -- a
    source that cannot be recorded is not throttled at all -- so the number
    is sized against how many addresses a guesser can field, not against
    MAX_CONNS, which bounds live connections. It was 32, argued from
    MAX_CONNS, and a flat management LAN fields 33 addresses trivially. */
#define AXL_SSH_PENALTY_SOURCES     256u

/** Which accrual a finished connection earns -- the header's rules 1 to 4,
    in the header's order. */
typedef enum {
    AXL_SSH_PENALTY_NONE           = 0, /**< rule 1: it authenticated */
    AXL_SSH_PENALTY_GRACE_EXCEEDED = 1, /**< rule 2: the reaper dropped it */
    AXL_SSH_PENALTY_AUTHFAIL       = 2, /**< rule 3: a real attempt refused */
    AXL_SSH_PENALTY_NOAUTH         = 3  /**< rule 4: anything else */
} AxlSshPenaltyEvent;

/** What the server observed about one connection by the time it ended.
    Named after the observations, not after the rules, so each field has one
    place in axl-ssh-server.c that sets it. */
typedef struct {
    bool authenticated; /**< reached USERAUTH_SUCCESS */
    bool grace_expired; /**< dropped by the reaper with an ARMED deadline past
                             -- not an unenforceable one */
    bool refused;       /**< a real attempt was answered with USERAUTH_FAILURE
                             (schedule_failure) -- never the `none` probe */
    bool forged;        /**< a signature under an AUTHORIZED key failed to
                             verify: DISCONNECT, no failure reply */
    bool server_fault;  /**< the SERVER failed it -- a resource failure while
                             setting it up, or a login grace it could not
                             time -- so the peer is charged nothing (ruling
                             P6) */
} AxlSshConnOutcome;

/** One tracked source. `used` is its own flag rather than a zero expiry:
    this server has already shipped the bug of one field meaning two things
    (see axl-ssh-reap.h). */
typedef struct {
    uint64_t expiry_ms; /**< monotonic ms at which remaining reaches zero */
    uint8_t  addr[4];   /**< the source, as octets */
    bool     used;      /**< this slot holds a source */
    bool     active;    /**< exceeded min_sec; enforced until expiry */
    uint8_t  cause;     /**< the AxlSshPenaltyEvent that last accrued */
} AxlSshPenaltySource;

/** One exempt range: a network and its mask, host bits clear. */
typedef struct {
    uint8_t addr[4];
    uint8_t mask[4];
} AxlSshPenaltyRange;

/** Everything one server keeps for penalties. Lives inside the server; no
    allocation, nothing to free. */
typedef struct {
    AxlSshPenalties     cfg;      /**< in force while `enabled` */
    uint64_t            drop_noted_ms; /**< when a dropped accrual was last
                                            reported at INFO */
    bool                enabled;  /**< false after set_penalties(NULL) */
    bool                drop_noted; /**< drop_noted_ms is set */
    unsigned            nexempt;  /**< ranges in use */
    AxlSshPenaltyRange  exempt[AXL_SSH_PENALTY_EXEMPT_MAX];
    AxlSshPenaltySource src[AXL_SSH_PENALTY_SOURCES];
} AxlSshPenaltyTable;

/** What axl_ssh_penalty_record() did with one accrual. */
typedef enum {
    AXL_SSH_PENALTY_SKIPPED  = 0, /**< disabled, exempt, or a zero accrual:
                                       nothing recorded, no slot used */
    AXL_SSH_PENALTY_UPDATED  = 1, /**< the source was tracked; grown in place */
    AXL_SSH_PENALTY_INSERTED = 2, /**< an unused or expired slot took it */
    AXL_SSH_PENALTY_EVICTED  = 3, /**< the table was full; the entry with the
                                       least time left made room */
    AXL_SSH_PENALTY_DROPPED  = 4  /**< the table was full and every entry
                                       outranked it; nothing changed */
} AxlSshPenaltyRecord;

/** The ladder: rule 1 first and unconditional, then 2, 3, 4.

    @return the event @a o earns; AXL_SSH_PENALTY_NOAUTH for a NULL @a o,
        the same answer as a connection nothing was observed about. */
AxlSshPenaltyEvent
axl_ssh_penalty_classify(
    const AxlSshConnOutcome *o  ///< what the server observed
);

/** @return the seconds @a ev accrues under @a cfg; 0 for
        AXL_SSH_PENALTY_NONE, an unknown event, or a NULL @a cfg. */
uint32_t
axl_ssh_penalty_accrual_sec(
    const AxlSshPenalties *cfg,  ///< the configuration in force
    AxlSshPenaltyEvent     ev    ///< what the connection earned
);

/** @return milliseconds until @a expiry_ms; 0 at or past it. */
uint64_t
axl_ssh_penalty_remaining_ms(
    uint64_t expiry_ms,  ///< the entry's deadline
    uint64_t now         ///< monotonic ms, from a reading that succeeded
);

/** Is @a e refusing right now? Active AND not yet expired: an active
    penalty is refused 1 ms before its expiry and allowed at it.

    @return false for a NULL or unused @a e. */
bool
axl_ssh_penalty_refuses(
    const AxlSshPenaltySource *e,   ///< the entry
    uint64_t                   now  ///< monotonic ms
);

/** Reset @a t to the shipped state: enabled, OpenSSH's defaults, no exempt
    range, no source. */
void
axl_ssh_penalty_init(
    AxlSshPenaltyTable *t  ///< the table
);

/** @return #AXL_OK when axl_ssh_server_set_penalties() would accept @a p;
        #AXL_INVALID for a NULL @a p, a field above
        #AXL_SSH_PENALTY_FIELD_MAX_SEC, @a min_sec not below @a max_sec, or
        three zero accruals. */
int
axl_ssh_penalty_validate(
    const AxlSshPenalties *p  ///< candidate settings
);

/** Install validated settings, or disable with NULL. Touches neither the
    exempt list nor the sources -- the independence the header promises. */
void
axl_ssh_penalty_configure(
    AxlSshPenaltyTable    *t,  ///< the table
    const AxlSshPenalties *p   ///< validated settings, or NULL to disable
);

/** Parse one exempt entry: axl_ipv4_parse_cidr, a bare address as /32, then
    AxlSsh's own two refusals -- host bits set below the prefix, and /0 as
    POLICY (it parses; it is refused because it exempts everything).

    @return #AXL_OK with @a out filled; #AXL_INVALID otherwise. */
int
axl_ssh_penalty_parse_range(
    const char         *s,   ///< "a.b.c.d" or "a.b.c.d/n"
    AxlSshPenaltyRange *out  ///< receives the range
);

/** Parse every entry into @a out without touching anything else, so the
    caller can install all of them or none.

    @return #AXL_OK with @a nout set; #AXL_INVALID for a NULL @a ranges with a
        non-zero @a count, a NULL entry or a malformed one; #AXL_NO_RESOURCES
        for @a count above #AXL_SSH_PENALTY_EXEMPT_MAX. */
int
axl_ssh_penalty_parse_ranges(
    const char *const  *ranges,  ///< entries, or NULL with count 0
    size_t              count,   ///< entries in @a ranges
    AxlSshPenaltyRange *out,     ///< AXL_SSH_PENALTY_EXEMPT_MAX slots
    unsigned           *nout     ///< receives how many were parsed
);

/** Install @a n staged ranges, replacing the list. */
void
axl_ssh_penalty_set_exempt(
    AxlSshPenaltyTable       *t,       ///< the table
    const AxlSshPenaltyRange *ranges,  ///< from axl_ssh_penalty_parse_ranges
    unsigned                  n        ///< how many
);

/** @return true when @a addr falls in any exempt range of @a t. */
bool
axl_ssh_penalty_is_exempt(
    const AxlSshPenaltyTable *t,    ///< the table
    const uint8_t             addr[4] ///< the source
);

/** @return the entry tracking @a addr, or NULL. */
const AxlSshPenaltySource *
axl_ssh_penalty_find(
    const AxlSshPenaltyTable *t,    ///< the table
    const uint8_t             addr[4] ///< the source
);

/** Should a connection from @a addr be refused at accept? False when
    penalties are disabled or @a addr is exempt -- both checked before the
    table -- and otherwise axl_ssh_penalty_refuses() on its entry.

    @return true to refuse; @a hit, when not NULL, receives the entry. */
bool
axl_ssh_penalty_check(
    const AxlSshPenaltyTable   *t,       ///< the table
    const uint8_t               addr[4], ///< the peer
    uint64_t                    now,     ///< monotonic ms
    const AxlSshPenaltySource **hit      ///< receives the entry, or NULL
);

/** Apply one connection's accrual to its source.

    Tracked: remaining becomes `min(remaining + accrual, max_sec)`, in place,
    and an entry that had expired starts deferred again. New: an unused slot
    takes it; otherwise the lowest-RANKED entry is evicted if -- and only if
    -- the newcomer strictly outranks it, and the accrual is dropped if not.
    The rank is (active, time left), compared active first: expired entries
    rank lowest, then deferred ones by time left, then active ones by time
    left, and a newcomer is active on arrival when its first accrual already
    exceeds min_sec (ruling P4 -- ranked by time left alone, an active entry
    near the end of its refusal would lose its slot to a deferred one that
    has never refused anything). Either way the entry ACTIVATES when its
    remaining exceeds min_sec and it was not already active.

    @return what happened; @a activated (optional) is true only for the call
        that activated the entry; @a remaining_ms (optional) receives the
        entry's remaining after the call, 0 when nothing was recorded. */
AxlSshPenaltyRecord
axl_ssh_penalty_record(
    AxlSshPenaltyTable *t,            ///< the table
    const uint8_t       addr[4],      ///< the source
    uint64_t            now,          ///< monotonic ms
    AxlSshPenaltyEvent  ev,           ///< what the connection earned
    bool               *activated,    ///< receives whether it activated, or NULL
    uint64_t           *remaining_ms  ///< receives the new remaining, or NULL
);

/** Should THIS dropped accrual be reported at INFO? True for the first
    drop, then false until max_sec has passed since the last one reported --
    one line per penalty lifetime, however hard a crowded table is pushed, so
    a full table is visible at the default level without a peer being able
    to turn its own drops into a console flood. Every drop still logs at
    DEBUG. Records the report when it returns true.

    @return true to log this drop at INFO. */
bool
axl_ssh_penalty_drop_notice(
    AxlSshPenaltyTable *t,   ///< the table
    uint64_t            now  ///< monotonic ms
);

/** sshd's wording for a cause, for the log lines. The first two are sshd's
    own, from its log; the third is ours (not measured against sshd). */
const char *
axl_ssh_penalty_cause(
    uint8_t ev  ///< an AxlSshPenaltyEvent
);

/** The penalty state of a live server, for the unit test only; NULL when
    @a s is not live. Defined in axl-ssh-server.c, which owns the struct. */
const AxlSshPenaltyTable *
axl_ssh_server_penalty_table(
    const AxlSshServer *s  ///< server from axl_ssh_server_new()
);

#endif /* AXL_SSH_PENALTY_H */
