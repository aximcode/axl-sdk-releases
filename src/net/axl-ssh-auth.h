/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-auth.h
    Server-role authorization: the authorized_keys store and its line parser.

    Deliberately NOT in axl-ssh-internal.h. That header states in its own
    comment that nothing there may assume a role, because everything in it is
    what the P6 client consumes unchanged. This is the server's answer to
    "may this key in?", and a client has no use for it. If a client ever
    wants the mirror-image known_hosts parser, promoting this one THEN is a
    smaller mistake than publishing a shape now that the client has had no
    chance to disagree with -- the same argument axl-ssh-core.h already makes
    about the codec.
**/
#ifndef AXL_SSH_AUTH_H
#define AXL_SSH_AUTH_H

#include "axl-ssh-internal.h"   /* AXL_SSH_ED25519_PUB_LEN */
#include <axl/axl-ssh-server.h> /* AxlSshAuthRequest */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Authorized keys held per server.

    A firmware command channel has one administrative identity and a handful
    of operator keys, not a user database. Sixteen slots is 512 bytes of fixed
    storage, no allocation anywhere on the authorization path, and no list to
    walk off the end of. */
#define AXL_SSH_MAX_AUTHORIZED_KEYS  16u

/* AXL_SSH_ED25519_PUB_LEN comes from axl-ssh-internal.h -- it is role-neutral
   (a P6 client needs it for the host key) and must NOT be defined here, or
   the two would drift. */

/** Longest base64 field the line parser will look at.

    An ssh-ed25519 authorized_keys blob is always exactly 68 base64 characters
    (51 bytes: string("ssh-ed25519") || string(32 bytes)). The ceiling is
    generous against that and still refuses a line crafted to make us copy an
    unbounded field onto the stack. */
#define AXL_SSH_AUTHKEY_B64_MAX  128u

/** The keys one server will authenticate. Zero-initialise; there is no
    constructor and nothing to free. */
typedef struct {
    uint8_t  key[AXL_SSH_MAX_AUTHORIZED_KEYS][AXL_SSH_ED25519_PUB_LEN];
    unsigned n;   /**< slots used. */
} AxlSshAuthKeys;

/** Parse one authorized_keys line into its raw Ed25519 public key.

    Accepts exactly `ssh-ed25519 <base64> [comment]`. An OpenSSH options
    prefix (`command="..."`, `no-pty`, `from=...`) is REFUSED rather than
    skipped: AxlSsh implements no options, and a line whose author meant to
    restrict a key must never be silently unrestricted.

    @a line need not be NUL-terminated; @a len bounds it.

    @return #AXL_OK with @a out32 filled; #AXL_NOT_FOUND when the line is
    blank or a `#` comment, which is not an error and means "nothing here";
    #AXL_INVALID on a NULL argument or any other line. */
int axl_ssh_authkey_parse(const char *line, size_t len, uint8_t *out32);

/** Parse a line and add its key to @a ks, ignoring a duplicate.

    A key that is well-formed but cryptographically useless -- a non-canonical
    encoding, or a point of small order -- is ACCEPTED here and refused later,
    when a signature under it fails to verify. That is the Ed25519 provider's
    deliberate design (src/net/axl-pk-ed25519.c): validation lives at the one
    door every signature passes through, rather than being repeated at every
    import site. The practical consequence is worth knowing: a typo'd key can
    be loaded successfully and still never log anyone in.

    @return #AXL_OK (added, or already present); #AXL_NOT_FOUND for a blank or
    comment line; #AXL_INVALID for a malformed line;
    #AXL_NO_RESOURCES when all #AXL_SSH_MAX_AUTHORIZED_KEYS slots are used. */
int axl_ssh_authkeys_add_line(AxlSshAuthKeys *ks, const char *line, size_t len);

/** Is @a pub32 one of the authorized keys?

    Not a constant-time compare, deliberately. Authorized keys are PUBLIC, and
    RFC 4252 section 7's query form discloses membership by design -- that is
    what USERAUTH_PK_OK means. A constant-time compare here would protect
    nothing and would imply to the next reader that something secret lives in
    this array. */
bool axl_ssh_authkeys_contains(const AxlSshAuthKeys *ks, const uint8_t *pub32);

/* ---------------------------------------------------------------------------
 * Minimum failure time (spec section 9a). The arithmetic of OpenSSH's
 * auth2.c -- user_specific_delay() and ensure_minimum_time_since() -- read
 * from openssh-portable on 2026-09-17, as pure functions so the unit test
 * pins them directly. The MECHANISM is not borrowed: OpenSSH nanosleeps in a
 * process per connection, and AxlSsh schedules the reply on a timer.
 * ------------------------------------------------------------------------- */

/** OpenSSH's MIN_FAIL_DELAY_SECONDS (0.005), in microseconds. */
#define AXL_SSH_FAIL_DELAY_MIN_US  5000u

/** OpenSSH's MAX_FAIL_DELAY_SECONDS (5.0): past this much elapsed time no
    delay is added. */
#define AXL_SSH_FAIL_DELAY_MAX_US  5000000u

/** Bytes of per-server random secret the per-user delay is keyed with. */
#define AXL_SSH_TIMING_SECRET_LEN  32u

/** The per-user minimum failure time: 5 ms plus 0-4.29 ms taken from
    SHA-512(@a secret || @a user). The same user always gets the same value
    for one secret, so repeated failures show no jitter to average away.

    Every failure path -- a NULL @a secret, a NULL @a user with a non-zero
    @a user_len, a digest failure -- returns the SAME value for every user
    (the largest the per-user term can produce), so a failure here cannot
    become the oracle the delay exists to hide.

    @return microseconds, in [#AXL_SSH_FAIL_DELAY_MIN_US, 9294]. */
uint32_t axl_ssh_fail_delay_target_us(const uint8_t *secret, size_t secret_len,
                                      const char *user, size_t user_len);

/** How long to wait before sending a failure reply, given how long the
    request has already taken. OpenSSH's ensure_minimum_time_since(): if the
    request already ran past @a target_us, the target doubles until it is
    ahead again, so the reply time is a step function of the target rather
    than of the check's own cost; past #AXL_SSH_FAIL_DELAY_MAX_US elapsed,
    nothing is added.

    @return microseconds still to wait; 0 when @a target_us is 0. */
uint64_t axl_ssh_fail_reply_delay_us(uint64_t elapsed_us, uint64_t target_us);

/** Can a server with this configuration authenticate anyone? The listen
    gate, as a predicate: at least one authorized key, or a password handler.
    A publickey principal op alone does not count -- it maps a login that an
    authorized key already allowed, and cannot allow one itself. */
bool axl_ssh_listen_has_method(unsigned authorized_keys, bool have_password);

/** Longest password handed to an application hook. Longer ones are refused
    without being examined -- the length is the peer's choice, and the copy
    the hook sees is a fixed buffer this sizes. */
#define AXL_SSH_AUTH_PASSWORD_MAX  1024u

/** May this RFC 4252 section 8 `password` request reach the application?

    A predicate rather than four conditions inline, so the unit test pins
    each refusal by itself; the connection-level consequences (the minimum
    failure time, the penalty accrual) belong to the caller either way.

    False for:
      - the CHANGE form (@a change): refused unexamined, spec section 13.5.
        sshd refuses it too -- `/usr/libexec/openssh/sshd-session` carries
        `password change not supported` and no CHANGEREQ machinery -- so this
        is parity, not strictness.
      - an EMPTY password: OpenSSH's `PermitEmptyPasswords no`, the default,
        short-circuits before the backend is consulted, so an empty password
        never reaches PAM. Neither does one reach a hook here. AxlSsh holds
        no credentials, so without this a consumer whose store happens to
        hold an unset or empty hash has a bypass one layer below anything it
        can see.
      - a password longer than #AXL_SSH_AUTH_PASSWORD_MAX.
      - a password with an EMBEDDED NUL: the NUL-terminated copy a hook gets
        would silently be a different password from the one sent.

    NONE of these counts toward the password cap, which bounds GUESSES the
    application actually checked; the userauth-request cap bounds these. That
    is one place AxlSsh is looser than sshd, whose `MaxAuthTries` counts an
    empty password as an attempt -- immaterial, since a refusal that can
    never succeed buys an attacker nothing, and `ssh(1)` stops after
    `NumberOfPasswordPrompts` (3) regardless.

    @a pw may be NULL only when @a pw_len is 0.

    @return true to call the hook; false to refuse without examining it. */
bool axl_ssh_password_reaches_hook(bool change, const uint8_t *pw,
                                   uint32_t pw_len);

/* ---------------------------------------------------------------------------
 * The authentication audit log's rate limit.
 *
 * sshd logs every outcome at its default LogLevel INFO -- `Accepted publickey
 * for U from A port P ssh2`, `Failed password for invalid user U from A port
 * P ssh2`, read off this box's journal and out of
 * /usr/libexec/openssh/sshd-session's own format strings. That is the single
 * most consumed piece of sshd output there is, and "who tried to log in and
 * failed" is the first question anyone asks a BMC after an incident.
 *
 * A firmware console is not syslog, though. sshd's implicit bound is one
 * process per connection and syslog underneath; here a peer reconnecting in a
 * loop would turn its own failures into console output, which is the flood
 * argument the penalty path already made twice (refusals at DEBUG, and
 * axl_ssh_penalty_drop_notice's one line per penalty lifetime). This is the
 * same shape, sized for attempts instead of table pressure.
 * ------------------------------------------------------------------------- */

/** How often a failed authentication may be announced at INFO, in
    milliseconds.

    One minute. A source that keeps failing activates a per-source penalty
    within two rounds (authfail 5 s against min 15 s) and is then refused at
    accept, logging nothing at all -- so this bound only has to cover the ramp
    and the distributed case the ladder cannot throttle. Nothing is lost by
    it: the next line that IS logged carries the count of what was
    suppressed, and every suppressed failure is still logged at DEBUG. */
#define AXL_SSH_AUTH_LOG_WINDOW_MS  60000u

/** The rate limiter's state; zero-initialise. One per KIND of line per
    server -- axl-ssh-server.c keeps one for each AuthLogKind, because a
    shared window lets whichever kind is cheaper hide the other's first
    line (it let a free unauthorized-key refusal hide a forgery). See
    AxlSshServer's auth_log. */
typedef struct {
    uint64_t noted_ms;    /**< when the last INFO line went out. */
    uint32_t suppressed;  /**< failures since, logged only at DEBUG. */
    bool     noted;       /**< a line has been logged at least once. */
} AxlSshAuthLogRate;

/** Should THIS authentication failure be announced at INFO?

    True for the first failure ever, then false until
    #AXL_SSH_AUTH_LOG_WINDOW_MS has passed since the last one announced.
    Records the announcement when it returns true.

    @a have_now is false when the monotonic clock could not be read. The
    FIRST failure is still announced then -- an audit trail with no entries
    is worse than a late one, and axl_ssh_server_listen() already refuses a
    platform whose clock cannot be read, so this is an exceptional reading
    rather than a steady state. Later ones are suppressed and counted, which
    is what stops a broken clock becoming an unbounded console flood.

    An announcement made without a reading anchors the window at zero, so
    the next readable failure is due once the clock itself reads a full
    window -- immediately on a server that has been up that long, and
    otherwise when it reaches the window. It is NOT unconditionally due at
    once, and nothing depends on it being: what was announced stays
    announced, and what was suppressed is counted and reported by the next
    line that does go out.

    @a suppressed (optional) receives how many failures were suppressed since
    the last announcement, and is meaningful only when this returns true; the
    count saturates rather than wrapping.

    @return true to log this failure at INFO, false to log it at DEBUG. */
bool axl_ssh_auth_log_due(AxlSshAuthLogRate *r, uint64_t now, bool have_now,
                          uint32_t *suppressed);

/* ---------------------------------------------------------------------------
 * What axl_ssh_auth_complete relies on before it reads a server (P3 security
 * review MED-4): a handle no other request anywhere in the image carries, and
 * a table saying whether the server is still live.
 * ------------------------------------------------------------------------- */

/** Servers that may be live at once in one image; axl_ssh_server_new refuses
    one more. The live-server table in axl-ssh-server.c has exactly this many
    slots -- its comment says why the number is 8. */
#define AXL_SSH_LIVE_SERVERS_MAX  8u

/** Advance a request-handle counter and return the handle it now holds.

    Never returns 0, the invalid handle: the step after 0xFFFFFFFF is 1. The
    server keeps ONE counter for every server in the image and issues every
    handle through this, so a handle cannot recur on any instance before
    2^32 - 1 requests in total. A pure step over the caller's counter so the
    unit test can pin the wrap without issuing four billion requests. */
AxlSshAuthRequest axl_ssh_auth_request_next(AxlSshAuthRequest *last);

/* ---------------------------------------------------------------------------
 * RFC 4252 section 5: the user name may not change on a connection.
 * ------------------------------------------------------------------------- */

/** Does a USERAUTH_REQUEST's user name differ from the one this connection
    has been authenticating as?

    RFC 4252 section 5: "the server MUST flush any accumulated authentication
    states if they change. If it is unable to flush an authentication state,
    it MUST disconnect". sshd disconnects ("Change of username or service
    not allowed"), and so does AxlSsh: the password cap and the request cap
    are per connection, so carrying them to a new name would be exactly the
    state the RFC says to flush -- and flushing them would hand a peer three
    fresh guesses per rename.

    @a kept is the connection's stored copy of the first name -- its first
    @a kept_len bytes, where @a kept_len is the smaller of @a kept_wire_len
    and the store's size -- and @a kept_wire_len is that name's full length
    on the wire. @a user / @a user_len is the new request's name, as sent.

    Exact for any name that fits the store, NULs included. Two names longer
    than the store that agree on every kept byte and on their length read as
    the same name: both are refused unexamined (AxlSsh hands no hook a name
    that long), so nothing a peer could gain rests on telling them apart.

    @return true when the name changed. */
bool axl_ssh_userauth_user_changed(const uint8_t *kept, size_t kept_len,
                                   uint32_t kept_wire_len,
                                   const uint8_t *user, uint32_t user_len);

/* ---------------------------------------------------------------------------
 * The audit log's user-change line, the longest line it writes.
 * ------------------------------------------------------------------------- */

/** Bytes of a peer-supplied user name an audit line carries, NUL included:
    63 characters, sanitised by axl-ssh-server.c's copy_user_for_log. */
#define AXL_SSH_LOG_USER_MAX  64u

/** Bytes of the peer string "A port P", NUL included. The longest the
    server writes is "255.255.255.255 port 65535", 26 characters. */
#define AXL_SSH_LOG_PEER_MAX  32u

/** sshd's line for RFC 4252 section 5's refusal, in AxlSsh's shape: the OLD
    name twice, the peer, the NEW name once. */
#define AXL_SSH_USER_CHANGE_FMT \
    "Disconnecting authenticating user %s %s: Change of username or" \
    " service not allowed: (%s,ssh-connection) -> (%s,ssh-connection)"

/** A buffer that holds the user-change line whole, derived rather than
    chosen: the format's own characters less its four `%s` (8), the OLD name
    twice and the NEW name once at AXL_SSH_LOG_USER_MAX - 1 each, the peer at
    AXL_SSH_LOG_PEER_MAX - 1 (its buffer's bound, not the 26 it reaches
    today), and the NUL. 339 bytes. */
#define AXL_SSH_USER_CHANGE_LINE_MAX                                       \
    ((sizeof AXL_SSH_USER_CHANGE_FMT - 1u) - 8u                            \
     + 3u * (AXL_SSH_LOG_USER_MAX - 1u) + (AXL_SSH_LOG_PEER_MAX - 1u) + 1u)

/** Format the user-change line into @a out (@a out_size bytes): @a was and
    @a now are the old and new user names, already sanitised, and @a peer
    is "A port P". Truncates rather than overruns; a buffer of
    #AXL_SSH_USER_CHANGE_LINE_MAX never truncates for inputs within
    #AXL_SSH_LOG_USER_MAX and #AXL_SSH_LOG_PEER_MAX. */
void axl_ssh_format_user_change(char *out, size_t out_size, const char *was,
                                const char *now, const char *peer);

/* ---------------------------------------------------------------------------
 * The login-grace line's reason, and the authentication-hook watchdog's two
 * lines (P3 security re-review NEW-1, resolved 2026-09-26: hooks stay
 * strictly serial and the stall is made VISIBLE instead).
 *
 * Here beside the user-change line and for the same reason: an audit line is
 * an interface, so its exact bytes are pinned by a unit test that needs no
 * clock, no socket and no loop, and the buffer each one needs is DERIVED
 * from its own format rather than chosen.
 * ------------------------------------------------------------------------- */

/** Digits in the largest uint64_t -- 18446744073709551615. */
#define AXL_SSH_U64_DIGITS_MAX  20u

/** Digits in the largest uint32_t -- 4294967295. */
#define AXL_SSH_U32_DIGITS_MAX  10u

/** The longer of the two method names an audit line carries, NUL included. */
#define AXL_SSH_METHOD_MAX  (sizeof "publickey")

/** sshd's line for a login grace that ran out, with AxlSsh's source port and
    a REASON where sshd has a pid. The reason is empty for the connection
    that was simply silent, so that line is byte-for-byte what it always
    was. */
#define AXL_SSH_TIMEOUT_FMT \
    "Timeout before authentication for connection from %s%s"

/** Where a reaped connection had been waiting: behind a hook on the stack. */
#define AXL_SSH_TIMEOUT_WHERE_HOOK \
    "behind another connection's authentication hook"

/** ...or behind a connection that had been waiting longer (auth_may_parse's
    other queueing reason, with no hook on the stack). */
#define AXL_SSH_TIMEOUT_WHERE_QUEUE  "in the authentication queue"

/** The reason clause, when the wait could be measured. */
#define AXL_SSH_TIMEOUT_TAIL_FMT       " after waiting %llu ms %s"

/** ...and when it could not: a clock reading failed, so the line says WHERE
    it waited and does not invent a duration. */
#define AXL_SSH_TIMEOUT_TAIL_NO_MS_FMT " after waiting %s"

/** A buffer that holds the reason clause whole. */
#define AXL_SSH_TIMEOUT_TAIL_MAX                                           \
    ((sizeof AXL_SSH_TIMEOUT_TAIL_FMT - 1u) - 6u + AXL_SSH_U64_DIGITS_MAX  \
     + (sizeof AXL_SSH_TIMEOUT_WHERE_HOOK - 1u) + 1u)

/** A buffer that holds the whole login-grace line, longest reason included. */
#define AXL_SSH_TIMEOUT_LINE_MAX                                           \
    ((sizeof AXL_SSH_TIMEOUT_FMT - 1u) - 4u + (AXL_SSH_LOG_PEER_MAX - 1u)  \
     + (AXL_SSH_TIMEOUT_TAIL_MAX - 1u) + 1u)

/** The watchdog's line for a hook that RETURNED having held the one-hook
    slot for at least the budget. The queue depth is in it because that is
    what tells an operator the SERVER-WIDE cost rather than the local one. */
#define AXL_SSH_HOOK_SLOW_FMT                                              \
    "Slow %s hook: authentication was held %llu ms, %u connections"        \
    " waiting, for %s from %s"

/** The watchdog's line for a hook that is STILL RUNNING over its budget. */
#define AXL_SSH_HOOK_STALLED_FMT                                           \
    "Stalled %s hook: authentication held %llu ms so far, %u connections"  \
    " waiting, for %s from %s"

/** A buffer that holds either hook line whole: the format's own characters
    less its five conversions (12), the longer method name, the largest
    duration and depth, a full-length user name and the longest peer string.
    Derived from the STALLED format, the longer of the two. */
#define AXL_SSH_HOOK_LINE_MAX                                              \
    ((sizeof AXL_SSH_HOOK_STALLED_FMT - 1u) - 12u                          \
     + (AXL_SSH_METHOD_MAX - 1u) + AXL_SSH_U64_DIGITS_MAX                  \
     + AXL_SSH_U32_DIGITS_MAX + (AXL_SSH_LOG_USER_MAX - 1u)                \
     + (AXL_SSH_LOG_PEER_MAX - 1u) + 1u)

/** Format the login-grace line into @a out (@a out_size bytes).

    @a was_queued is the connection's resume_due AS THE REAPER FOUND IT --
    "still waiting its turn to parse when we took it", which is exactly the
    state NEW-1 creates. @a hook_running separates the two reasons a
    connection can be in it (auth_may_parse): a hook on the stack anywhere in
    the server, or somebody who has been waiting longer. When @a was_queued
    is false the line is what it has always been, with no reason at all: the
    peer simply sent nothing, and blaming the server for that would be worse
    than saying nothing.

    @a have_waited is false when the wait could not be measured -- the clock
    reading at queue_join failed, or this one did -- and the line then names
    WHERE the connection waited without a duration.

    Truncates rather than overruns; #AXL_SSH_TIMEOUT_LINE_MAX never
    truncates for a @a peer within #AXL_SSH_LOG_PEER_MAX. */
void axl_ssh_format_timeout(char *out, size_t out_size, const char *peer,
                            bool was_queued, bool hook_running,
                            bool have_waited, uint64_t waited_ms);

/** Format one of the hook watchdog's two lines into @a out (@a out_size
    bytes): the STILL-RUNNING one when @a stalled, the RETURNED one
    otherwise.

    @a is_password picks the method name, as every other audit line does.
    @a held_ms is how long the hook has held the one-hook slot, @a waiting
    how many connections were queued behind it when this was measured, @a
    user the sanitised user name and @a peer "A port P".

    Truncates rather than overruns; #AXL_SSH_HOOK_LINE_MAX never truncates
    for inputs within #AXL_SSH_LOG_USER_MAX and #AXL_SSH_LOG_PEER_MAX. */
void axl_ssh_format_hook_hold(char *out, size_t out_size, bool stalled,
                              bool is_password, uint64_t held_ms,
                              unsigned waiting, const char *user,
                              const char *peer);

/* ---------------------------------------------------------------------------
 * The signed publickey request (RFC 4252 section 7): whether its signature
 * verifies, and -- when it does not -- WHOSE failure that was.
 * ------------------------------------------------------------------------- */

/** What checking the signature on a signed publickey request established. */
typedef enum {
    AXL_SSH_SIG_VALID     = 0, /**< it verifies under the key */
    AXL_SSH_SIG_FORGED    = 1, /**< the PEER's doing: the signature field is
                                    malformed, or it does not verify */
    AXL_SSH_SIG_UNCHECKED = 2  /**< OURS: the check could not be made -- an
                                    allocation failed, here or inside the
                                    verify -- so there is no verdict */
} AxlSshSigCheck;

/** Check the signature on a signed publickey request.

    @a req is the whole USERAUTH_REQUEST payload as it arrived, message
    number first, and @a signed_len is where its signature string starts:
    string(@a session_id) followed by every byte before that is what RFC 4252
    section 7 signs. @a pub is the Ed25519 key the request carries
    (#AXL_SSH_ED25519_PUB_LEN bytes), already found in the authorized set.

    THREE answers, not two, because what the server does next turns on whose
    failure it was. FORGED is a peer that offered an authorized key and could
    not prove it holds the private half: the connection ends, the forgery
    penalty accrues, and the audit log says `bad signature` -- the line an
    operator is told to watch for. UNCHECKED is this machine out of memory,
    and saying `bad signature` about it would accuse the peer of the
    server's own fault. Neither ever authenticates.

    The peer's bytes are judged BEFORE anything is allocated, so a malformed
    signature field is FORGED whatever the memory state.

    @return #AXL_SSH_SIG_VALID, #AXL_SSH_SIG_FORGED or #AXL_SSH_SIG_UNCHECKED;
    UNCHECKED too for a NULL argument or @a signed_len past @a req_len, which
    are the caller's errors, not the peer's. */
AxlSshSigCheck axl_ssh_userauth_sig_check(const uint8_t *session_id,
                                          size_t sid_len, const uint8_t *pub,
                                          const uint8_t *req, size_t req_len,
                                          size_t signed_len);

#endif /* AXL_SSH_AUTH_H */
