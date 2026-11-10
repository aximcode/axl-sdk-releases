/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-penalty.c
    Per-source penalties: the pure decisions axl-ssh-penalty.h declares.

    The model is OpenSSH 9.9p1's PerSourcePenalties as MEASURED against a
    throwaway sshd on the build host, not as recalled: an accrual is deferred
    until the source's remaining penalty exceeds min_sec, then the penalty
    ACTIVATES and is enforced until it has burned down to nothing. The
    measurement, and why an earlier reading of min_sec as a floor was wrong,
    are in include/axl/axl-ssh-server.h's AxlSshPenalties comment.
**/

#include "axl-ssh-penalty.h"
#include <axl/axl-net.h>
#include <axl/axl-str.h>

/* ------------------------------------------------------------------------ */
/* The ladder                                                                */
/* ------------------------------------------------------------------------ */

AxlSshPenaltyEvent
axl_ssh_penalty_classify(const AxlSshConnOutcome *o)
{
    if (o == NULL) {
        return AXL_SSH_PENALTY_NOAUTH;
    }
    /* The server's own failure is never the peer's accrual (ruling P6): a
       resource failure while setting the connection up, or a login grace
       the server could not time. Charging it would let pressure a peer may
       have induced -- a full source table, say -- penalise legitimate
       operators. */
    if (o->server_fault) {
        return AXL_SSH_PENALTY_NONE;
    }
    /* Rule 1 FIRST, and depending on nothing below it -- the shape
       axl_ssh_reap_reason() uses for the same exemption, and for the same
       reason: a success that had a wrong password before it is the common
       typo, and it must fall out of this line, not out of the ordering of
       the ones after it. */
    if (o->authenticated) {
        return AXL_SSH_PENALTY_NONE;
    }
    if (o->grace_expired) {
        return AXL_SSH_PENALTY_GRACE_EXCEEDED;
    }
    /* A forged signature sends no USERAUTH_FAILURE (the key WAS authorized,
       and the signature ends the connection by DISCONNECT), so it needs its
       own term here: read off the failure replies alone it would fall
       through to noauth, the mildest charge, for the case that most deserves
       the harshest. */
    if (o->refused || o->forged) {
        return AXL_SSH_PENALTY_AUTHFAIL;
    }
    return AXL_SSH_PENALTY_NOAUTH;
}

uint32_t
axl_ssh_penalty_accrual_sec(const AxlSshPenalties *cfg, AxlSshPenaltyEvent ev)
{
    if (cfg == NULL) {
        return 0;
    }
    switch (ev) {
    case AXL_SSH_PENALTY_GRACE_EXCEEDED:
        return cfg->grace_exceeded_sec;
    case AXL_SSH_PENALTY_AUTHFAIL:
        return cfg->authfail_sec;
    case AXL_SSH_PENALTY_NOAUTH:
        return cfg->noauth_sec;
    case AXL_SSH_PENALTY_NONE:
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

uint64_t
axl_ssh_penalty_remaining_ms(uint64_t expiry_ms, uint64_t now)
{
    return expiry_ms > now ? expiry_ms - now : 0;
}

bool
axl_ssh_penalty_refuses(const AxlSshPenaltySource *e, uint64_t now)
{
    /* Active AND unexpired. NOT "remaining > min": min_sec decides when a
       penalty starts refusing, not how long it refuses -- sshd refused for
       the whole 19 s it activated with, not the 4 s above its threshold. */
    return e != NULL && e->used && e->active
           && axl_ssh_penalty_remaining_ms(e->expiry_ms, now) > 0;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

/* OpenSSH 9.9p1's defaults -- the one place they are written down in code. */
static void
fill_defaults(AxlSshPenalties *out)
{
    out->authfail_sec       = AXL_SSH_PENALTY_AUTHFAIL_DEFAULT_SEC;
    out->noauth_sec         = AXL_SSH_PENALTY_NOAUTH_DEFAULT_SEC;
    out->grace_exceeded_sec = AXL_SSH_PENALTY_GRACE_EXCEEDED_DEFAULT_SEC;
    out->min_sec            = AXL_SSH_PENALTY_MIN_DEFAULT_SEC;
    out->max_sec            = AXL_SSH_PENALTY_MAX_DEFAULT_SEC;
}

int
axl_ssh_penalties_default(AxlSshPenalties *out)
{
    if (out == NULL) {
        return AXL_INVALID;
    }
    fill_defaults(out);
    return AXL_OK;
}

void
axl_ssh_penalty_init(AxlSshPenaltyTable *t)
{
    axl_memset(t, 0, sizeof *t);
    fill_defaults(&t->cfg);
    t->enabled = true;   /* on by default, as sshd ships it */
}

int
axl_ssh_penalty_validate(const AxlSshPenalties *p)
{
    if (p == NULL) {
        return AXL_INVALID;
    }
    if (p->authfail_sec > AXL_SSH_PENALTY_FIELD_MAX_SEC
        || p->noauth_sec > AXL_SSH_PENALTY_FIELD_MAX_SEC
        || p->grace_exceeded_sec > AXL_SSH_PENALTY_FIELD_MAX_SEC
        || p->min_sec > AXL_SSH_PENALTY_FIELD_MAX_SEC
        || p->max_sec > AXL_SSH_PENALTY_FIELD_MAX_SEC) {
        return AXL_INVALID;
    }
    /* Both of these configure penalties that can never refuse anything --
       the accrual is capped at or under the threshold it has to exceed, or
       nothing ever accrues -- which is penalties OFF dressed as tuning. */
    if (p->min_sec >= p->max_sec) {
        return AXL_INVALID;
    }
    if (p->authfail_sec == 0 && p->noauth_sec == 0
        && p->grace_exceeded_sec == 0) {
        return AXL_INVALID;
    }
    return AXL_OK;
}

void
axl_ssh_penalty_configure(AxlSshPenaltyTable *t, const AxlSshPenalties *p)
{
    if (p == NULL) {
        t->enabled = false;
        return;
    }
    t->cfg     = *p;
    t->enabled = true;
}

/* ------------------------------------------------------------------------ */
/* The exempt list                                                           */
/* ------------------------------------------------------------------------ */

int
axl_ssh_penalty_parse_range(const char *s, AxlSshPenaltyRange *out)
{
    /* A bare address keeps this /32 mask: axl_ipv4_parse_cidr leaves the
       mask untouched when there is no suffix, so the narrowest reading is
       the default and a dropped suffix cannot widen a range. */
    uint8_t addr[4] = { 0, 0, 0, 0 };
    uint8_t mask[4] = { 255, 255, 255, 255 };

    if (s == NULL || out == NULL
        || axl_ipv4_parse_cidr(s, addr, mask, NULL) != AXL_OK) {
        return AXL_INVALID;
    }
    /* /0 parses -- it is a passing case of axl_ipv4_parse_cidr's own tests
       -- and is refused as POLICY: exempting everything is turning
       penalties off, which set_penalties(NULL) says at the call site. */
    if ((mask[0] | mask[1] | mask[2] | mask[3]) == 0) {
        return AXL_INVALID;
    }
    /* Bits set below the prefix are sshd's error too: "10.1.2.3/8" was
       almost certainly written for "10.0.0.0/8". */
    for (unsigned i = 0; i < 4; i++) {
        if ((addr[i] & (uint8_t)~mask[i]) != 0) {
            return AXL_INVALID;
        }
    }
    axl_memcpy(out->addr, addr, sizeof out->addr);
    axl_memcpy(out->mask, mask, sizeof out->mask);
    return AXL_OK;
}

/* The public, server-less form of the rule above: the same function judges
   the entry, and the parsed range is thrown away. */
int
axl_ssh_penalty_range_check(const char *range)
{
    AxlSshPenaltyRange discard;
    return axl_ssh_penalty_parse_range(range, &discard);
}

int
axl_ssh_penalty_parse_ranges(const char *const *ranges, size_t count,
                             AxlSshPenaltyRange *out, unsigned *nout)
{
    if (out == NULL || nout == NULL || (count > 0 && ranges == NULL)) {
        return AXL_INVALID;
    }
    if (count > AXL_SSH_PENALTY_EXEMPT_MAX) {
        return AXL_NO_RESOURCES;
    }
    for (size_t i = 0; i < count; i++) {
        if (axl_ssh_penalty_parse_range(ranges[i], &out[i]) != AXL_OK) {
            return AXL_INVALID;
        }
    }
    *nout = (unsigned)count;
    return AXL_OK;
}

void
axl_ssh_penalty_set_exempt(AxlSshPenaltyTable *t,
                           const AxlSshPenaltyRange *ranges, unsigned n)
{
    if (n > AXL_SSH_PENALTY_EXEMPT_MAX) {
        n = AXL_SSH_PENALTY_EXEMPT_MAX;
    }
    for (unsigned i = 0; i < n; i++) {
        t->exempt[i] = ranges[i];
    }
    t->nexempt = n;
}

bool
axl_ssh_penalty_is_exempt(const AxlSshPenaltyTable *t, const uint8_t addr[4])
{
    for (unsigned i = 0; i < t->nexempt; i++) {
        /* A zero mask never matches in axl_ipv4_in_subnet -- and never
           reaches it, since /0 is refused at parse. */
        if (axl_ipv4_in_subnet(addr, t->exempt[i].addr, t->exempt[i].mask)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

const AxlSshPenaltySource *
axl_ssh_penalty_find(const AxlSshPenaltyTable *t, const uint8_t addr[4])
{
    for (unsigned i = 0; i < AXL_SSH_PENALTY_SOURCES; i++) {
        if (t->src[i].used && axl_ipv4_equals(t->src[i].addr, addr)) {
            return &t->src[i];
        }
    }
    return NULL;
}

bool
axl_ssh_penalty_check(const AxlSshPenaltyTable *t, const uint8_t addr[4],
                      uint64_t now, const AxlSshPenaltySource **hit)
{
    /* Disabled and exempt are both decided BEFORE the table is read: an
       exemption lifts a refusal the table would otherwise make. */
    if (!t->enabled || axl_ssh_penalty_is_exempt(t, addr)) {
        return false;
    }
    const AxlSshPenaltySource *e = axl_ssh_penalty_find(t, addr);
    if (hit != NULL) {
        *hit = e;
    }
    return axl_ssh_penalty_refuses(e, now);
}

/* An entry's retention rank (ruling P4), as one number compared
   lexicographically: ACTIVE first, then time left. An expired entry ranks as
   deferred with nothing left -- the lowest rank there is -- whatever its
   stale active flag says. Active needs its own term because an active
   penalty spends its whole enforced decay passing back down through the
   range a deferred one lives in: ranked by time left alone, a refusal with
   3 s to run loses its slot to a deferred entry at 14 s that has never
   refused anything. Every value fits: remaining is at most FIELD_MAX in ms,
   well under 2^63, so the active bit sits above all of it. */
#define PENALTY_RANK_ACTIVE  (1ull << 63)

static uint64_t
penalty_rank(bool active, uint64_t remaining_ms)
{
    if (remaining_ms == 0) {
        return 0;
    }
    return (active ? PENALTY_RANK_ACTIVE : 0) | remaining_ms;
}

AxlSshPenaltyRecord
axl_ssh_penalty_record(AxlSshPenaltyTable *t, const uint8_t addr[4],
                       uint64_t now, AxlSshPenaltyEvent ev, bool *activated,
                       uint64_t *remaining_ms)
{
    if (activated != NULL) {
        *activated = false;
    }
    if (remaining_ms != NULL) {
        *remaining_ms = 0;
    }
    if (!t->enabled || axl_ssh_penalty_is_exempt(t, addr)) {
        return AXL_SSH_PENALTY_SKIPPED;
    }
    uint64_t accrual = (uint64_t)axl_ssh_penalty_accrual_sec(&t->cfg, ev)
                       * 1000u;
    if (accrual == 0) {
        return AXL_SSH_PENALTY_SKIPPED;
    }
    uint64_t min_ms = (uint64_t)t->cfg.min_sec * 1000u;
    uint64_t max_ms = (uint64_t)t->cfg.max_sec * 1000u;

    AxlSshPenaltySource *e      = NULL;
    AxlSshPenaltyRecord  result = AXL_SSH_PENALTY_UPDATED;
    uint64_t             rem    = 0;

    for (unsigned i = 0; i < AXL_SSH_PENALTY_SOURCES; i++) {
        if (t->src[i].used && axl_ipv4_equals(t->src[i].addr, addr)) {
            e = &t->src[i];
            break;
        }
    }
    if (e != NULL) {
        rem = axl_ssh_penalty_remaining_ms(e->expiry_ms, now);
        if (rem == 0) {
            /* Expired: the source is as if it had never accrued, so this
               accrual is deferred again -- sshd's "fresh penalty". */
            e->active = false;
        }
    } else {
        /* A new source. Ranked the way it would stand after this accrual,
           so an accrual that already exceeds min_sec arrives active. */
        uint64_t mine      = accrual < max_ms ? accrual : max_ms;
        uint64_t my_rank   = penalty_rank(mine > min_ms, mine);
        uint64_t low_rank  = UINT64_MAX;
        unsigned low       = 0;
        bool     have_free = false;

        for (unsigned i = 0; i < AXL_SSH_PENALTY_SOURCES; i++) {
            if (!t->src[i].used) {
                low       = i;
                have_free = true;
                break;
            }
            uint64_t r = penalty_rank(
                t->src[i].active,
                axl_ssh_penalty_remaining_ms(t->src[i].expiry_ms, now));
            if (r < low_rank) {
                low_rank = r;
                low      = i;
            }
        }
        if (have_free || low_rank == 0) {
            result = AXL_SSH_PENALTY_INSERTED;   /* unused, or expired */
        } else if (my_rank > low_rank) {
            result = AXL_SSH_PENALTY_EVICTED;
        } else {
            return AXL_SSH_PENALTY_DROPPED;      /* a tie does not churn */
        }
        e = &t->src[low];
        axl_memset(e, 0, sizeof *e);
        e->used = true;
        axl_memcpy(e->addr, addr, sizeof e->addr);
    }

    rem          = rem + accrual < max_ms ? rem + accrual : max_ms;
    e->expiry_ms = now + rem;
    e->cause     = (uint8_t)ev;
    if (!e->active && rem > min_ms) {
        e->active = true;
        if (activated != NULL) {
            *activated = true;
        }
    }
    if (remaining_ms != NULL) {
        *remaining_ms = rem;
    }
    return result;
}

bool
axl_ssh_penalty_drop_notice(AxlSshPenaltyTable *t, uint64_t now)
{
    /* One INFO line per max_sec -- the longest any penalty lasts -- however
       hard a crowded table is pushed. now >= drop_noted_ms on a monotonic
       clock; a reading that went backwards reports nothing new. */
    uint64_t window = (uint64_t)t->cfg.max_sec * 1000u;

    if (t->drop_noted && (now < t->drop_noted_ms
                          || now - t->drop_noted_ms < window)) {
        return false;
    }
    t->drop_noted    = true;
    t->drop_noted_ms = now;
    return true;
}

const char *
axl_ssh_penalty_cause(uint8_t ev)
{
    switch (ev) {
    case AXL_SSH_PENALTY_AUTHFAIL:
        return "failed authentication";
    case AXL_SSH_PENALTY_NOAUTH:
        return "connections without attempting authentication";
    case AXL_SSH_PENALTY_GRACE_EXCEEDED:
        return "login grace exceeded";
    default:
        return "none";
    }
}
