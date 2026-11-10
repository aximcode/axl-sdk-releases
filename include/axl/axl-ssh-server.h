/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-ssh-server.h
 * @brief An SSH server over AxlTcp and AxlLoop.
 *
 * ```c
 * AxlLoop *loop = axl_loop_new();
 * AxlSshServer *s = axl_ssh_server_new(loop, 22);
 * axl_ssh_server_listen(s);
 * axl_loop_run(loop);
 * ```
 *
 * **P3 — password authentication.** A stock `ssh` authenticates by Ed25519
 * public key or by password. The password is checked by the application
 * through axl_ssh_server_set_auth_ops(), and its verdict may arrive later
 * through axl_ssh_auth_complete() -- so an expensive hash can run on
 * another core instead of stalling the loop. AxlSsh stores no credential.
 * An unauthorized key is refused without dropping the connection; a bad
 * signature under an authorized key ends it, and so does a third wrong
 * password -- #AXL_SSH_PASSWORD_FAILURES_DEFAULT, half OpenSSH's
 * `MaxAuthTries` of 6, deliberately, and not something a real user meets:
 * `ssh(1)`'s own `NumberOfPasswordPrompts` default is 3, so it gives up
 * first either way. AxlSsh also counts only GUESSES against that cap, where
 * `MaxAuthTries` counts publickey queries too -- which is the behaviour
 * `IdentitiesOnly=yes` exists to escape; probing is bounded separately.
 * axl_ssh_server_set_max_password_failures() moves it, at any time,
 * for connections accepted afterwards.
 *
 * A source whose connections keep ending that way is then refused at accept
 * for a while, as OpenSSH's `PerSourcePenalties` does -- on by default; see
 * #AxlSshPenalties.
 *
 * **Authentication hooks run one at a time for the whole server**, so a hook
 * that blocks instead of answering later delays every other login behind it.
 * That is not enforced away -- it cannot be, since the library cannot take a
 * hook off the stack -- but it IS reported: see
 * axl_ssh_server_set_auth_hook_budget_ms(), which also explains what the
 * report cannot see.
 *
 * **Session channels are opt-in** (axl-ssh-channel.h). Until
 * axl_ssh_server_set_channel_ops() installs hooks, a successful login is
 * followed by a clean `CHANNEL_OPEN_FAILURE`; with them, `ssh host`,
 * `ssh -tt host` and `ssh host cmd` open `session` channels that the
 * application serves through those hooks.
 *
 * **Ed25519 comes with it.** The only host key algorithm is `ssh-ed25519`
 * (see #AXL_SSH_HOSTKEY_NAME). Its provider is opt-in for other images
 * because its tables are ~120 KB, but axl_ssh_server_new() calls
 * axl_pk_enable_ed25519() itself, so an AxlSsh application needs no flag
 * and no call of its own.
 *
 * **How a connection ends.** A connection AxlSsh ends for a protocol reason
 * -- the password cap or the request cap ("Too many authentication
 * failures"), a changed user or service, an unknown service, a protocol or
 * channel error, a rekey before authentication, a bad signature -- is sent
 * an SSH_MSG_DISCONNECT, and **every DISCONNECT sent on a healthy stream is
 * delivered or bounded**: the server sends nothing after it, keeps reading
 * (and discarding) until the peer hangs up -- ssh(1) prints the reason and
 * closes at once -- and then resets the connection; a peer that does not
 * hang up within #AXL_SSH_DISCONNECT_LINGER_MS is reset anyway. The
 * connection's channels, principal and hooks are released when the
 * DISCONNECT is sent, not when the socket goes. A stream that is already
 * broken -- a send TCP refused or lost -- is reset at once, since the peer
 * could not decrypt anything sent after the hole.
 *
 * Ended **abortively, with no DISCONNECT**, as sshd's cleanup_exit ends
 * them: an input flood ("input backlog"), the login grace
 * (sshd-session.c:223), the keepalive and unused-connection timeouts
 * (serverloop.c:117-118, 249-250), accept-time refusals (penalty,
 * MaxStartups, server full) and eviction, the peer's own close or a receive
 * error, and axl_ssh_server_free(). A connection waiting for its peer to
 * hang up is the first one eviction takes, whatever its state.
 *
 * DEBUG lines tell the endings apart: `linger: peer closed after N ms,
 * disconnect sent` (the peer hung up after the DISCONNECT was handed to TCP
 * and that send completed -- the firmware completes a send when it takes
 * the data, so this is not proof the peer read it), `linger: peer closed
 * before the disconnect was sent`, and `linger: no close within 1000 ms;
 * reset`.
 *
 * **Not a general SSH server.** No built-in shell or command execution --
 * a session is whatever the application's channel hooks make it -- no port
 * forwarding, no agent forwarding, no X11, one algorithm per slot. Those
 * are security decisions; see
 * `docs/superpowers/specs/2026-08-21-axl-ssh-design.md` §4.
 */

#ifndef AXL_SSH_SERVER_H
#define AXL_SSH_SERVER_H

#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-loop.h>
#include <axl/axl-ssh-core.h>
#include <axl/axl-ssh-host-key.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An SSH server bound to one port on one event loop.
 *
 * Create with axl_ssh_server_new(), start with axl_ssh_server_listen(), free
 * with axl_ssh_server_free().
 */
typedef struct AxlSshServer AxlSshServer;

/**
 * @brief Create a server and generate its host key.
 *
 * The host key is a freshly generated Ed25519 key, held in memory for the
 * life of the server. **On its own it does not survive a restart**, so a
 * client using `StrictHostKeyChecking` would see a new key each run. A
 * server that is to keep its identity is given its key before it listens,
 * with axl_ssh_server_set_host_key(): the stored one from
 * axl_ssh_host_key_nv_load_or_create() (axl-ssh-host-key.h), or one the
 * application keeps itself.
 *
 * Does not bind the port — call axl_ssh_server_listen().
 *
 * @return a new server (free with axl_ssh_server_free), or NULL on bad
 *     arguments, out of memory, an image that did not link the Ed25519
 *     provider (see the file comment), or when 8 servers are already live
 *     in the image.
 */
AxlSshServer *
axl_ssh_server_new(
    AxlLoop  *loop,  ///< event loop to run on (must outlive the server)
    uint16_t  port   ///< TCP port to bind when listening
);

/**
 * @brief Authorize one public key, given an OpenSSH `authorized_keys` line.
 *
 * The line is `ssh-ed25519 <base64> [comment]` — exactly what
 * `ssh-keygen -t ed25519` writes into `id_ed25519.pub`. Up to 16 keys may be
 * authorized; adding a key that is already authorized succeeds and consumes
 * no slot.
 *
 * **An options prefix is refused, not ignored.** A line beginning
 * `command="..."`, `no-pty` or `from=...` returns #AXL_INVALID: AxlSsh
 * implements none of those restrictions, and silently dropping the prefix
 * would authorize a key its author meant to narrow.
 *
 * **What this does NOT check is whether the key is cryptographically
 * usable.** A well-formed line carrying a non-canonical encoding or a
 * small-order point is accepted here and refused later, when a signature
 * under it fails to verify — the Ed25519 provider validates at the one door
 * every signature passes through rather than at every import site. The
 * practical consequence is worth knowing: a corrupted key can load
 * successfully and still never log anyone in.
 *
 * @return #AXL_OK when the key is authorized (or already was);
 *     #AXL_NOT_FOUND when @a line is blank or a `#` comment, which adds
 *     nothing and is not an error; #AXL_INVALID on a NULL argument, a
 *     malformed line, or a key algorithm other than `ssh-ed25519`;
 *     #AXL_NO_RESOURCES when all 16 slots are in use; #AXL_ERR when
 *     @a server is not live -- already freed, or being freed by a call
 *     still on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_add_authorized_key(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    const char   *line     ///< one OpenSSH authorized_keys line
);

/**
 * @brief Authorize every key in an `authorized_keys` file.
 *
 * Reads @a path whole, then applies each line through
 * axl_ssh_server_add_authorized_key(). Blank lines and `#` comments are
 * skipped. The file is UTF-8 (a BOM allowed) or UTF-16LE with a BOM -- what
 * the UEFI Shell's `echo ... > file` and `edit` write -- read through
 * axl_text_to_utf8().
 *
 * **Transactional.** The file is parsed into a temporary set first and
 * spliced in only if every line is good; one malformed line fails the call
 * and leaves the server's authorized keys exactly as they were. A key file
 * that is half-applied is worse than one that is rejected — the operator
 * would have no way to tell which half.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL argument, any malformed line, or
 *     UTF-16 that does not convert (an unpaired surrogate, an odd byte);
 *     #AXL_NO_RESOURCES when the file holds more than 16 distinct keys
 *     (and for nothing else); #AXL_ERR when the file cannot be read or
 *     there is no memory to read or decode it, or when @a server is not live
 *     -- already freed, or being freed by a call still on the stack
 *     somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_load_authorized_keys(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    const char   *path     ///< path to an authorized_keys file (UTF-8 or UTF-16LE + BOM)
);

/**
 * @brief Largest banner, in bytes, axl_ssh_auth_complete() will accept.
 *
 * One byte more is refused with #AXL_INVALID, not truncated.
 *
 * For the pre-authentication banner every connection that reaches
 * authentication is sent, see axl_ssh_server_set_banner() and
 * #AXL_SSH_BANNER_MAX.
 */
#define AXL_SSH_AUTH_BANNER_MAX  1024u

/**
 * @brief An application's answer to one authentication request.
 */
typedef enum {
    AXL_SSH_AUTH_ACCEPT       = 0, /**< Authenticated; the principal is
                                        adopted by the server. */
    AXL_SSH_AUTH_DENY         = 1, /**< Refused. */
    AXL_SSH_AUTH_CHANGE_FIRST = 2  /**< The password is right but must be
                                        changed first. Refused exactly like
                                        DENY; it exists so a banner can say
                                        why -- and, like DENY, it CONSUMES
                                        one of the connection's password
                                        attempts, so a run of them as long as
                                        the cap
                                        (#AXL_SSH_PASSWORD_FAILURES_DEFAULT,
                                        or whatever
                                        axl_ssh_server_set_max_password_failures()
                                        set) disconnects a user who typed the
                                        right password every time. At a cap
                                        of 1 the banner this verdict carries
                                        rides the disconnect, and both are
                                        delivered (see "How a connection
                                        ends" in the file comment).

                                        There is no in-band change. That is
                                        not a gap: sshd does not implement
                                        RFC 4252 section 8's change flow
                                        either -- its `password` method
                                        answers the change form "password
                                        change not supported" and counts a
                                        failure, and its real route for an
                                        expired password is PAM plus
                                        `keyboard-interactive`, which AxlSsh
                                        does not offer by design. So this
                                        verdict is what sshd does on the
                                        wire. */
} AxlSshAuthVerdict;

/**
 * @brief Handle for one authentication request awaiting a verdict.
 *
 * An integer, not a pointer into the connection: a completion that arrives
 * after its connection has gone is refused instead of written through freed
 * memory. 0 is never issued. Handles count up from one counter shared by
 * every server in the image and wrap only after 2^32 - 1 requests in total,
 * so a stale handle -- from this server, or from one freed before this one
 * was created -- cannot match a new request before then.
 */
typedef uint32_t AxlSshAuthRequest;

/**
 * @brief Application hooks for authentication.
 *
 * Copied by axl_ssh_server_set_auth_ops(); any hook may be NULL. Every
 * request a hook receives must eventually be answered with
 * axl_ssh_auth_complete() -- or left to the login grace, which drops a
 * connection still unauthenticated after 60 seconds.
 *
 * **Prefer to answer from axl_ssh_auth_complete() rather than block inside
 * the hook.** Return from the hook right away and deliver the verdict
 * later -- from an AxlAsync done callback once a hash finishes on another
 * core, say. That shape is immune to the serialization cost below: it never
 * holds the one-hook-at-a-time slot for any measurable time. Reach for it
 * for any check that is not already cheap, such as a PBKDF2 or bcrypt
 * password hash.
 *
 * **When the check truly cannot be made asynchronous, a hook -- and
 * release_principal -- may instead run the loop**, dispatching it with
 * axl_loop_iterate_until(), the nested wait the event loop documents as safe
 * from any source callback. This is the fallback, not the preferred shape:
 * to wait on real work -- checking the password against a remote service,
 * say -- start it with an asynchronous API and wait on its done event, and
 * treat any result other than the one waited for as DENY:
 * axl_loop_iterate_until() returns #AXL_NO_RESOURCES, having waited on
 * nothing, when it cannot register its wake sources. **A synchronous
 * network call is not this wait.** It runs a private loop of its own and
 * freezes the server's loop -- every connection, timer and service sharing
 * it -- for as long as the call takes, and the loop library is deprecating
 * synchronous waits from inside callbacks. While a hook dispatches, every
 * other source keeps firing: receives and closes on any connection, the
 * login grace, the failure replies, and completions for any request,
 * including the one this hook received. If the server drops that
 * connection meanwhile -- the login grace expires, or a receive on it fails
 * -- the server keeps the connection until the hook returns and then drops
 * it without acting on anything the hook answered: axl_ssh_auth_complete()
 * still returns #AXL_OK for a verdict given before the hook returns, and a
 * principal adopted with it is still released exactly once; a verdict given
 * after that returns #AXL_NOT_FOUND. The wait costs the latency the loop
 * documents, for every connection on it.
 *
 * **A hook that blocks this way makes authentication serial, and the cost
 * is server-wide, not just the hook's own.** One hook runs at a time for
 * the whole server. While one is on the stack, every other connection's
 * authentication input waits unread, and each is read when its turn comes,
 * in the order the input arrived. So a hook that waits W seconds is W
 * seconds of latency for every login queued behind it -- and with up to 16
 * connections available to one peer, that cost is (peer connections) x W,
 * not just W: before the rule, each connection could instead start its
 * hook inside the one below it, so a peer could hold a legitimate login
 * (and its login grace) open for as long as it kept connections coming. A
 * waiting connection's grace keeps running: it is still dropped 60 seconds
 * after it connected, whether or not its turn has come. Answering from
 * axl_ssh_auth_complete() instead, as recommended above, is immune to this
 * cost entirely.
 *
 * **When a hook does hold the server that long, the audit log says so**
 * (axl_ssh_server_set_auth_hook_budget_ms()): one line when the hook returns
 * over its budget, naming the method, the user, the source, the hold and how
 * many connections were queued behind it, and a best-effort line while it is
 * still running. That is REPORTING ONLY -- nothing is refused or cancelled
 * at the budget -- and there is one case it cannot see at all, described
 * there. A connection the reaper takes while it is still waiting its turn
 * says so too, in its timeout line.
 *
 * **release_principal is not part of that one-at-a-time rule, and its own
 * nesting has no cap but the connection count (16 today).** A
 * release_principal that dispatches the loop can be resumed by the drop of
 * another authenticated connection, whose own release_principal dispatches
 * again, and so on: unlike a hook, it parses no input, so it cannot start a
 * second hook by nesting, and its connection is already being torn down, so
 * there is no later turn to give it instead. The trade is a deeper stack
 * during a burst of disconnects, not a longer wait for anyone: capping
 * releases the way hooks are capped would only defer freeing those
 * connections, and a release that never returns would then defer it
 * forever, which is worse than the stack a release_principal that waits
 * already costs.
 *
 * **A hook may free the server**, and so may release_principal or anything
 * their dispatch runs -- see axl_ssh_server_free().
 *
 * **The authentication audit log.**
 *
 * Every authentication decision is written to the AXL log at INFO, the
 * default level, so it reaches a firmware console with nothing configured --
 * a failure subject to the rate limit below, which counts what it holds
 * back. sshd's shape, and deliberately close to it, because the same people
 * read both:
 *
 *     ssh: Accepted publickey for admin from 10.0.2.2 port 55400
 *     ssh: Failed password for admin from 10.0.2.2 port 55404
 *     ssh: Failed publickey (key not authorized) for root from 10.0.2.2 port 55410
 *     ssh: Failed publickey (bad signature) for root from 10.0.2.2 port 55412
 *     ssh: maximum authentication attempts exceeded for admin from 10.0.2.2 port 55404
 *     ssh: Timeout before authentication for connection from 10.0.2.2 port 55418
 *     ssh: Timeout before authentication for connection from 10.0.2.2 port 55422 after waiting 47320 ms behind another connection's authentication hook
 *     ssh: Disconnecting authenticating user root 10.0.2.2 port 55420: Change of username or service not allowed: (root,ssh-connection) -> (admin,ssh-connection)
 *     ssh: Slow password hook: authentication was held 4210 ms, 3 connections waiting, for admin from 10.0.2.2 port 55404
 *     ssh: Stalled password hook: authentication held 4210 ms so far, 3 connections waiting, for admin from 10.0.2.2 port 55404
 *
 * The last three are not refusals. The first of them is the login grace
 * naming what the connection was waiting ON, and the other two are the hook
 * watchdog (axl_ssh_server_set_auth_hook_budget_ms()) reporting the
 * APPLICATION'S own hook holding the server -- the one pair of lines here
 * that describes the server's own slowness rather than a peer's attempt.
 *
 * The user name is the peer's bytes, so it is truncated to 63 characters
 * and anything outside printable ASCII becomes `.` -- a name is a
 * diagnostic here, never something a hook decides on. **No password, key
 * blob or fingerprint is ever logged.**
 *
 * A FAILURE line says why, which sshd has no need to: AxlSsh has no user
 * database, so "invalid user" is not a thing it can report, and the useful
 * distinction is instead whether the application judged the attempt at all.
 * A bare `password` or `publickey` is a hook's own DENY, and
 * `password (change required)` its #AXL_SSH_AUTH_CHANGE_FIRST; any other
 * parenthesised reason means AxlSsh refused it before any hook ran -- a
 * refused user name, a method that is not offered, a key that is not
 * authorized, a password request that is empty, over-long, NUL-bearing or
 * the RFC 4252 section 8 change form, or a **bad signature**.
 *
 * `publickey (bad signature)` is the one worth watching for. Almost every
 * other refusal is something a legitimate user can produce by accident; that
 * one is a peer that offered an AUTHORIZED key and could not prove it holds
 * the private half. Of the refusals above, it is the only one that ends the
 * connection the first time.
 *
 * **What else ends a connection before it authenticates, and what it
 * leaves in the log:**
 *   - **Running out of attempts** -- when the application has refused as
 *     many passwords as the password cap allows (#AXL_SSH_AUTH_CHANGE_FIRST
 *     counts; AxlSsh's own refusal of an empty, over-long or NUL-bearing
 *     password does not), or at an authentication request past the
 *     twentieth of any kind. At INFO, in sshd's words for its
 *     `MaxAuthTries` (which sshd logs at ERROR):
 *     `maximum authentication attempts exceeded`, naming the connection's
 *     user. After a refused password it follows that attempt's
 *     `Failed password` line.
 *   - **A request naming a different user** -- RFC 4252 section 5 has the
 *     server flush the accumulated authentication state or disconnect, and
 *     every count above is per connection, so AxlSsh disconnects, as sshd
 *     does, and logs it at INFO in sshd's words -- the `Disconnecting`
 *     line in the sample above, both names sanitised like any other. A
 *     connection therefore authenticates as one user or not at all, and
 *     every OTHER line that names a user on it names that one (the
 *     `Disconnecting` line names two: the one before, and the one
 *     refused).
 *   - **The login grace running out** -- at INFO, as
 *     `Timeout before authentication for connection from A port P`: sshd's
 *     words for the same event, where sshd's listener adds a pid and AxlSsh
 *     names the port. A connection that was STILL WAITING ITS TURN when the
 *     reaper took it -- queued behind an authentication hook, which is a
 *     cost the server imposed and not something the peer did -- carries a
 *     reason after the port, and sshd has no counterpart for it:
 *     `... port P after waiting N ms behind another connection's
 *     authentication hook`, or `... after waiting N ms in the
 *     authentication queue` when nothing was on the stack and it was simply
 *     behind someone who had waited longer. A connection that was not
 *     queued gets the reasonless line, unchanged: it sent nothing, and
 *     saying more would blame the server for that. The two are separate
 *     kinds for the rate limit below.
 *   - **Anything else** -- a service other than `ssh-connection`, a key
 *     re-exchange before authentication (after it, a rekey is served: see
 *     axl_ssh_server_set_rekey_limit()), a malformed or out-of-order
 *     message, a signature the server ran out of memory checking, the
 *     peer closing or the connection failing -- ends it with nothing from
 *     the SSH server above DEBUG: those are
 *     protocol failures, the peer leaving, or the server's own, not
 *     authentication decisions. Some leave no line of their own at all (a
 *     peer's EOF, a read error).
 *
 * **An idle authenticated connection ended** is logged at INFO too, in
 * sshd's words: `terminating inactive connection from user USER A port P`
 * (axl_ssh_server_set_unused_timeout()) or `Timeout, client not responding
 * from user USER A port P` (axl_ssh_server_set_client_alive()).
 *
 * Failures are rate-limited to one line per minute per KIND for the whole
 * server, with the count of what was suppressed carried on the next line of
 * that kind; every suppressed line is still logged at DEBUG. Each kind --
 * ordinary refusals, bad signatures, attempts exceeded, timeouts, timeouts
 * on a connection that was still queued, changes of user, a hook that
 * returned over its budget, a hook still running over it, a connection
 * evicted and one refused past `MaxStartups`
 * (axl_ssh_server_set_max_startups()), an inactive connection ended, and an
 * unresponsive client ended: twelve -- has a window of its own, so no kind
 * can push another's first line off INFO: every kind is cheap for somebody,
 * and a cheaper kind sharing a window would hide the other. The timeout and
 * hook pairs are exactly that argument again: an ordinary timeout is the
 * cheapest event there is, so sharing its window would let a peer opening
 * and abandoning connections hide the first queued-timeout line -- the one
 * kind that names the server's own fault -- and a stalled hook always goes
 * on to produce a slow-return line, so sharing theirs would let the first
 * hide the second. Without the bound a peer reconnecting in a loop turns its
 * own failures into console output, which is the same argument that keeps
 * penalty refusals at DEBUG; with it, the console sees at most one failure
 * line per kind a minute. Successes are NOT rate-limited: producing one
 * needs a credential that already works.
 *
 * **Messages AxlSsh does not implement.**
 *
 * A message AxlSsh has no handler for is answered with
 * `SSH_MSG_UNIMPLEMENTED` (RFC 4253 section 11.4, a MUST), carrying the
 * sequence number the peer used. `SSH_MSG_IGNORE` and `SSH_MSG_DEBUG` are
 * ignored instead, as section 11 requires, and so is any authentication
 * message once a login has succeeded (RFC 4252 section 5.1). Replies are
 * bounded at 8 per connection; past that the message is ignored, which is
 * what AxlSsh did for all of them before. The bound never ends a
 * connection.
 *
 * A stock `ssh(1)` draws none of these in an ordinary session, `ssh -R`
 * included: a `GLOBAL_REQUEST` is answered with RFC 4254 section 4's
 * `REQUEST_FAILURE`, since AxlSsh implements no global request.
 */
typedef struct {
    /**
     * @brief Check a password (RFC 4252 section 8).
     *
     * @a password is a NUL-terminated copy that is valid ONLY during this
     * call and is scrubbed when it returns: an implementation that answers
     * later must take its own copy, and should scrub it. @a user is a
     * NUL-terminated name held by the connection, also valid only during
     * this call -- copy it to use it later. **It is the peer's bytes and is
     * NOT checked for UTF-8**, so a banner built from it can be refused by
     * axl_ssh_auth_complete(); see its notes. NULL means password
     * authentication is not offered.
     *
     * **@a password_len is never 0.** An empty password is refused before
     * this hook is called, which is OpenSSH's `PermitEmptyPasswords no`
     * default: sshd short-circuits before PAM is consulted, and AxlSsh --
     * which holds no credentials of its own -- short-circuits before the
     * application is. A store that happens to hold an unset or empty
     * credential hash therefore cannot be walked into from the wire. The
     * refusal still costs the attempt its minimum failure time and the same
     * per-source penalty as any other refused attempt.
     *
     * Also never delivered here: the RFC 4252 section 8 password-CHANGE
     * form, a password over 1024 bytes, and one containing an embedded NUL
     * (the NUL-terminated copy would be a different password from the one
     * sent). All four are refusals, not successes, and none of them counts
     * toward the per-connection password cap -- they are not guesses this
     * hook ever judged.
     */
    void (*password)(AxlSshAuthRequest req, const char *user,
                     const char *password, size_t password_len,
                     void *user_data) AXL_CB_NOEXCEPT;
    /**
     * @brief Decide who a publickey login is.
     *
     * Runs only AFTER a valid signature by a key already authorized with
     * axl_ssh_server_add_authorized_key(); it cannot admit a key that is
     * not. A DENY here is an ordinary failure and the client may try
     * something else. @a user (a NUL-terminated name held by the
     * connection, the peer's bytes and **not checked for UTF-8** -- see
     * axl_ssh_auth_complete()) and @a key_blob are valid only during this
     * call; an implementation that answers later must copy what it needs. NULL
     * accepts every signed login with a NULL principal, whatever the user
     * name -- except that a name longer than 256 bytes, or one containing a
     * NUL byte, is refused whatever the method, with or without hooks.
     */
    void (*publickey)(AxlSshAuthRequest req, const char *user,
                      const uint8_t *key_blob, size_t blob_len,
                      void *user_data) AXL_CB_NOEXCEPT;
    /**
     * @brief Release a principal adopted with #AXL_SSH_AUTH_ACCEPT.
     *
     * Called exactly once per non-NULL principal, when its connection
     * closes. NULL means principals need no release.
     */
    void (*release_principal)(void *principal, void *user_data) AXL_CB_NOEXCEPT;
} AxlSshAuthOps;

/**
 * @brief Install authentication hooks.
 *
 * Copies @a ops, so the caller's struct need not outlive the call. NULL
 * removes every hook: publickey logins then succeed with a NULL principal
 * for any user name the server accepts at all (at most 256 bytes, no NUL),
 * and password authentication is not offered.
 *
 * Call before axl_ssh_server_listen() -- hooks do not change under requests
 * already in flight.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server; #AXL_ERR if the server is
 *     already listening, or not live -- already freed, or being freed by a
 *     call still on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_auth_ops(
    AxlSshServer        *server,    ///< server from axl_ssh_server_new()
    const AxlSshAuthOps *ops,       ///< hooks to copy, or NULL to remove them
    void                *user_data  ///< passed to every hook
);

/**
 * @brief Deliver the verdict for a pending authentication request.
 *
 * May be called from inside the hook that received @a req, which behaves
 * like a synchronous check, or at any later time from the server's loop --
 * for example from an AxlAsync done callback after hashing on another core.
 * Until it is called, that connection reads no further input: RFC 4252
 * section 5.1 requires a server to finish one request before reading the
 * next.
 *
 * **Ownership of @a principal.** On #AXL_OK with #AXL_SSH_AUTH_ACCEPT the
 * server adopts it and hands it to the release_principal hook exactly once
 * when the connection closes. On any other return the caller still owns it.
 * Only #AXL_SSH_AUTH_ACCEPT may carry a principal.
 *
 * **A late, repeated or orphaned completion is refused.** If no connection is
 * waiting on @a req -- the client left, the login grace expired, or the
 * request was already answered -- this returns #AXL_NOT_FOUND and changes
 * nothing. So does a completion that arrives after axl_ssh_server_free(): a
 * freed @a server is recognised without being read. That is the call the
 * natural teardown order delivers -- free the server, then axl_async_free(),
 * which runs the done callbacks of work still pending. Handles are unique
 * across every server in the image until 2^32 - 1 requests in total
 * (see AxlSshAuthRequest), so a completion meant for a freed server cannot be
 * applied to a request on a new one.
 *
 * **Still cancel outstanding work before freeing the server.** The refusal
 * makes a late completion safe, not useful: the work behind it was wasted,
 * and it may hold its own copy of a password until it finishes.
 *
 * **It calls no application code and does not dispatch the loop.** The
 * verdict is applied here -- the success is sent, or the failure is
 * scheduled for its minimum time -- and the connection resumes reading from
 * the loop afterwards, never inside this call. So it cannot re-enter the
 * caller: an AxlAsync done callback that completes a request keeps the loop
 * it was called from, and a hook that completes another connection's request
 * does not run that connection's next hook inside itself.
 *
 * **Timing.** A failure is replied to no earlier than a per-user minimum
 * measured from the request's arrival, borrowed from OpenSSH (spec section
 * 9a). That hides the reply's timing only when the check costs the same for
 * every user: a check that returns early for an unknown user leaks it, and
 * nothing here can undo that. The password failure that reaches the
 * connection's cap (#AXL_SSH_PASSWORD_FAILURES_DEFAULT, or whatever
 * axl_ssh_server_set_max_password_failures() had set when it was accepted)
 * ends the connection.
 *
 * **The banner is the RFC 4252 section 5.4 field**, so it must be valid
 * UTF-8, exactly as axl_ssh_server_set_banner()'s must -- one RFC field, one
 * rule on both paths. `""` means NO banner, not an empty one on the wire.
 *
 * **A PEER can make that refusal happen, so handle it.** The `user` a hook
 * receives is the peer's bytes: on_userauth checks its length and refuses an
 * embedded NUL, and never checks it for UTF-8. An application that builds a
 * banner from it -- `"%s must change their password"` -- therefore gets
 * #AXL_INVALID for a name the peer chose. On #AXL_INVALID **the request is
 * still pending**, so complete it again, without the banner or with a fixed
 * one; a hook that only logs the error leaves the client waiting out the
 * login grace and the source charged for a timeout.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server, a zero @a req, an unknown
 *     @a verdict, a non-NULL @a principal with any verdict but
 *     #AXL_SSH_AUTH_ACCEPT, or a @a banner longer than
 *     #AXL_SSH_AUTH_BANNER_MAX bytes or not valid UTF-8; #AXL_NOT_FOUND when
 *     no connection is waiting on @a req; #AXL_NO_RESOURCES when the banner
 *     cannot be copied. On #AXL_INVALID and #AXL_NO_RESOURCES the request is
 *     still pending and the caller still owns @a principal.
 */
AXL_WARN_UNUSED int
axl_ssh_auth_complete(
    AxlSshServer      *server,     ///< the server whose hook received @a req
    AxlSshAuthRequest  req,        ///< handle passed to the hook
    AxlSshAuthVerdict  verdict,    ///< the answer
    void              *principal,  ///< with ACCEPT, who logged in; else NULL
    const char        *banner      ///< UTF-8 text sent before the reply, or
                                   ///< NULL or "" for none
);

/**
 * @brief Default milliseconds an authentication hook may hold the server
 *     before it is reported.
 *
 * Chosen against what the stall COSTS, not as a round number.
 * Authentication is serial server-wide, so a hook that holds for W
 * milliseconds delays every other connection's authentication by up to
 * (connections waiting) x W. At the 16-connection cap and the default
 * login grace (#AXL_SSH_LOGIN_GRACE_DEFAULT_SEC) that starts reaping
 * legitimate logins near W = 3.7 seconds, so one second reports the
 * problem while it is still only slowness. A shorter grace moves that
 * threshold down in proportion (axl_ssh_server_set_login_grace()), and
 * wants a smaller budget with it.
 *
 * Reporting is ON at this value unless
 * axl_ssh_server_set_auth_hook_budget_ms() changes it.
 */
#define AXL_SSH_AUTH_HOOK_BUDGET_DEFAULT_MS  1000u

/**
 * @brief Largest budget axl_ssh_server_set_auth_hook_budget_ms() accepts.
 *
 * The same hour as #AXL_SSH_LOGIN_GRACE_MAX_SEC, so the two bounds tell one
 * story: nothing about one connection's authentication is interesting
 * beyond it. The cap carries little safety weight on a knob that enforces
 * nothing -- it is here for that consistency and to catch seconds typed
 * where milliseconds were meant.
 */
#define AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS  3600000u

/**
 * @brief Set how long an authentication hook may hold the server before it
 *     is reported.
 *
 * **This changes REPORTING ONLY. Nothing is refused, delayed or cancelled
 * at this budget, and that is deliberate:** a knob that changed
 * enforcement would invite an operator to set it and believe they were
 * protected, when the library cannot take a hook off the stack. What it
 * buys is that a server-wide stall appears in the audit log an operator
 * already reads, named, instead of as unexplained slowness.
 *
 * Authentication hooks run ONE AT A TIME server-wide
 * (axl_ssh_server_set_auth_ops()), so a hook that blocks for W
 * milliseconds holds every other connection's authentication input for
 * that long -- and a queued connection's login grace keeps running while
 * it waits (axl_ssh_server_set_login_grace()). The supported way to avoid
 * that entirely is to answer later through axl_ssh_auth_complete() rather
 * than to block.
 *
 * **Two reports, and they are not the same event:**
 * - when a hook RETURNS having held the server for at least @a ms, one
 *   line naming the METHOD, the user, the source, the milliseconds held
 *   and how many connections were waiting behind it -- the queue depth is
 *   what tells an operator the server-wide cost rather than the local one.
 *   This one lands whenever the hold could be MEASURED: like the login
 *   grace, a failed monotonic-clock reading suppresses it rather than
 *   inventing a duration, and unlike the grace's line there is nothing
 *   useful left to say without one;
 * - while a hook is STILL RUNNING and has held the server for at least
 *   @a ms, one line saying so, at most ONE per hook call. It is
 *   BEST-EFFORT: it rides the same periodic sweep that enforces the login
 *   grace, so a hook that returns within one sweep of passing the budget
 *   may produce none. It exists for the hook that does not return in a
 *   sweep, and for that a lag of one sweep is immaterial.
 *
 * **sshd has no equivalent**, and this is not a parity feature: sshd forks
 * a process per connection, so one slow PAM stack costs one process and
 * there is nothing server-wide to report. The line's SHAPE follows the
 * audit log's (the method, the user, `from A port P`); the event is
 * AxlSsh's own.
 *
 * **What it cannot see.** A hook that blocks WITHOUT dispatching the loop
 * produces no mid-hook report, and none at all if it never returns:
 * every source, timer and deferred call on this loop -- including the
 * sweep this report rides -- is dispatched from the stack that hook is
 * holding. (A firmware notify at a raised TPL can still preempt an
 * application-mode loop, which is how co-located drivers keep running; it
 * is not something this server can log from, and in driver mode, where the
 * hook itself runs at TPL_CALLBACK, it does not preempt at all.) A hook
 * that can genuinely wedge must be watched from OUTSIDE this loop: the
 * UEFI boot-services watchdog (axl_watchdog_set(), <axl/axl-watchdog.h>)
 * resets the platform when nothing pets it, and a BMC or platform watchdog
 * does the same from further out. **This budget is a diagnostic, not a
 * recovery.**
 *
 * **What it does NOT measure.** The hook CALL, not the time to a verdict.
 * A hook that returns promptly and answers later through
 * axl_ssh_auth_complete() is never reported here however long the verdict
 * takes, because it holds nothing; a verdict that never arrives is caught
 * by the login grace instead. `release_principal` is not covered either:
 * it is application code and it may dispatch the loop, but it holds no
 * authentication slot (see axl_ssh_server_set_auth_ops()).
 *
 * **The budget is read once, when a hook is entered.** A change made while
 * a hook is running does not reach that hook -- it is measured against the
 * budget in force when it started -- and takes effect at the next one.
 * That is the rule axl_ssh_server_set_max_password_failures() follows for
 * a connection already accepted, and it also stops a misbehaving hook
 * disarming the report about itself.
 *
 * May be called at any time from the server's loop, including while it is
 * listening and from inside a hook.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server or @a ms over
 *     #AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS; #AXL_ERR if the server is not live
 *     -- already freed, or being freed by a call still on the stack
 *     somewhere in this dispatch. **@a ms == 0 is VALID** and turns both
 *     reports off. That differs from axl_ssh_server_set_login_grace() and
 *     axl_ssh_server_set_max_password_failures(), which refuse 0 because a
 *     disabled grace and a cap of nothing are exploitable; this knob
 *     enforces nothing, so turning it off costs only the report.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_auth_hook_budget_ms(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      ms       ///< budget, or 0 to report neither
);

/**
 * @brief Default seconds a connection has, from accept, to authenticate.
 *
 * What axl_ssh_server_listen()'s reaper enforces on every connection until
 * axl_ssh_server_set_login_grace() is called to change it.
 *
 * **Half OpenSSH's `LoginGraceTime`, which is 120 seconds, and that is
 * deliberate** -- pass 120 for sshd parity. The grace is an outage window
 * here in a way it is not for sshd: AxlSsh PAUSES its accept at the
 * 16-connection cap, so 16 silent peers deafen the server for one grace
 * period, and doubling the default doubles that window. sshd never stops
 * listening, so it has no equivalent exposure. Against that, AxlSsh
 * tolerates 16 unauthenticated connections where sshd's `MaxStartups`
 * starts dropping at 10 -- a larger capacity with a shorter grace is a
 * coherent pair, not an accident.
 */
#define AXL_SSH_LOGIN_GRACE_DEFAULT_SEC  60u

/**
 * @brief Largest grace, in seconds, axl_ssh_server_set_login_grace() will
 *     accept.
 *
 * A grace this long already outlasts any plausible use. A value above it is
 * far more likely a mistake -- seconds confused with milliseconds, say --
 * than an intended setting, so it is refused rather than accepted and left
 * to quietly pin a connection's slot for over an hour.
 */
#define AXL_SSH_LOGIN_GRACE_MAX_SEC  3600u

/**
 * @brief Set how long an unauthenticated connection may hold its slot.
 *
 * Replaces #AXL_SSH_LOGIN_GRACE_DEFAULT_SEC as the deadline
 * axl_ssh_server_listen()'s reaper enforces from accept to
 * USERAUTH_SUCCESS.
 *
 * **Granularity.** The reaper is a periodic sweep, not a per-connection
 * timer: a drop lands somewhere in [@a seconds, @a seconds + tick), never
 * earlier. axl_ssh_server_listen() sizes that tick when it arms the reaper
 * -- the finest of 5 seconds, half of @a seconds, and half of each idle
 * period (axl_ssh_server_set_unused_timeout(),
 * axl_ssh_server_set_client_alive()) -- so a short grace set here is not
 * silently rounded up to a fixed 5-second tick. At the defaults every grace
 * of 10 seconds or more ticks exactly as before. That tick then runs for the
 * life of the server, not just during any one connection's grace: refusing
 * @a seconds == 0 floors it at 500 ms, but a 1-second grace still means a
 * sweep over every connection twice a second for as long as the server
 * listens, against the reaper's own "coarse on purpose" design.
 *
 * **A queued connection's grace keeps running.** Authentication hooks run
 * one at a time server-wide (see axl_ssh_server_set_auth_ops()); while one
 * is on the stack, every other connection's authentication input waits its
 * turn, and its login grace is not paused for the wait. A grace shorter
 * than (connections queued) x (that hook's own latency) reaps logins that
 * did nothing wrong. Size @a seconds for the slowest hook this server
 * installs and the full connection cap, not for how fast one legitimate
 * login usually completes.
 *
 * **When that does happen it is now visible from both ends.** The reap
 * names what the connection was waiting on rather than reading as an
 * ordinary silent peer (see axl_ssh_server_set_auth_ops()'s audit-log
 * section), and the hook that held it up is reported in its own right
 * (axl_ssh_server_set_auth_hook_budget_ms(), whose default budget is
 * derived from THIS grace and the connection cap -- shortening @a seconds
 * lowers the latency at which a hook starts costing logins, and wants a
 * smaller budget set with it).
 *
 * Call before axl_ssh_server_listen() -- like axl_ssh_server_set_auth_ops(),
 * the grace does not change under connections already being timed.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server, @a seconds == 0 -- a
 *     disabled grace hands a peer the exact denial of service the grace
 *     exists to prevent -- or @a seconds over #AXL_SSH_LOGIN_GRACE_MAX_SEC;
 *     #AXL_ERR if the server is already listening, or not live -- already
 *     freed, or being freed by a call still on the stack somewhere in this
 *     dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_login_grace(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      seconds  ///< accept -> authenticated, in seconds
);

/**
 * @brief Default number of wrong passwords that ends a connection.
 *
 * **OpenSSH's `MaxAuthTries` is 6; 3 here is deliberate, and not a divergence
 * to close.** The two count different things. `MaxAuthTries` is one budget
 * over every failed method, publickey queries included -- the situation
 * `IdentitiesOnly=yes` is documented for, an agent offering many identities,
 * is exactly the one that exhausts it before the right key or the password
 * prompt is reached. This cap counts only wrong passwords, which are guesses;
 * publickey queries are not, and are bounded separately (twenty
 * authentication requests of any kind per connection). And `ssh(1)` gives up
 * after its own `NumberOfPasswordPrompts`, also 3 by default, so a stock
 * client never reaches a larger cap: raising it buys an interactive user
 * nothing and gives a scripted guesser more guesses per connection.
 */
#define AXL_SSH_PASSWORD_FAILURES_DEFAULT  3u

/**
 * @brief Largest cap axl_ssh_server_set_max_password_failures() accepts.
 *
 * Every password is also an authentication request, and a connection is
 * dropped once it sends a TWENTY-FIRST of any method, so a cap of N is
 * reached only by a client that sends at most 20 - N other requests first:
 * the `none` probe, and its key offers (two for a key the server accepts but
 * the application refuses). `ssh(1)` offers its keys before it asks for a
 * password. Ten keeps room for ten such requests -- the probe and nine key
 * offers, which is exactly what the compile-time assert beside
 * `MAX_AUTH_REQUESTS` reserves. Past that, the request bound ends the
 * connection before the cap does -- with the same "Too many authentication
 * failures", the one text sshd sends for both.
 */
#define AXL_SSH_PASSWORD_FAILURES_MAX  10u

/**
 * @brief Set how many wrong passwords end a connection -- AxlSsh's
 *     counterpart of sshd's `MaxAuthTries`, counting guesses only.
 *
 * Replaces #AXL_SSH_PASSWORD_FAILURES_DEFAULT. When a connection's wrong
 * passwords reach @a count, the reply owed to that last one -- after the same
 * minimum failure time as any other failure, and after any banner its verdict
 * carried -- is SSH_MSG_DISCONNECT ("Too many authentication failures",
 * sshd's own text, auth.c:343) instead of USERAUTH_FAILURE, as sshd's
 * `auth_maxtries_exceeded` disconnects in place of the failure reply. It is
 * delivered, banner first (see "How a connection ends" in the file comment),
 * so ssh(1) prints "Received disconnect from ...: Too many authentication
 * failures". At @a count 1 a #AXL_SSH_AUTH_CHANGE_FIRST verdict's banner
 * only ever rides that disconnect, and the user still sees it.
 *
 * What counts is narrower than `MaxAuthTries`: only a password the
 * application's hook judged and refused -- a #AXL_SSH_AUTH_DENY or
 * #AXL_SSH_AUTH_CHANGE_FIRST verdict. A password request refused before any
 * hook sees it -- a refused user name, an empty password, one over 1024 bytes
 * or carrying an embedded NUL, the RFC 4252 section 8 change form, or any
 * password when no password hook is installed -- is a failure with the full
 * minimum time and penalty, but not a guess the application checked, so it
 * does not count; nor does any publickey request or the `none` probe. All of
 * those are still bounded by the per-connection request limit (see
 * #AXL_SSH_PASSWORD_FAILURES_MAX).
 *
 * **What raising it costs.** The per-source penalty (#AxlSshPenalties)
 * accrues per CONNECTION, not per guess, so this is also the number of
 * guesses each unit of penalty buys an attacker: doubling it doubles a
 * scripted guesser's rate against a server whose penalties are otherwise
 * unchanged. That is true of sshd too, whose penalty is per connection.
 *
 * To offer no password authentication at all, install no password hook
 * (axl_ssh_server_set_auth_ops()); 0 is not that, and is refused.
 *
 * **May be called at any time from the server's loop**, including while it is
 * listening and from an authentication hook -- the same rule
 * axl_ssh_server_set_banner() follows. It applies to connections accepted
 * afterwards; a connection
 * already open keeps the cap it was accepted with -- what sshd does when its
 * configuration is reloaded, since each connection is served by a process
 * forked with the configuration in force when it arrived.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server, @a count 0, or @a count
 *     over #AXL_SSH_PASSWORD_FAILURES_MAX; #AXL_ERR if the server is not
 *     live -- already freed, or being freed by a call still on the stack
 *     somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_max_password_failures(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      count    ///< wrong passwords that end a connection
);

/**
 * @brief Default number of unauthenticated connections admitted before the
 *     server starts evicting to make room.
 *
 * AxlSsh's counterpart of sshd's `MaxStartups` -- though the enforcement
 * differs (see axl_ssh_server_set_max_startups() and deviation 3 in
 * `docs/superpowers/specs/2026-08-21-axl-ssh-design.md` section 12a.7):
 * sshd's `10:30:100` refuses NEW connections with rising probability past
 * ten; this admits up to this many at once and, past it, EVICTS the one
 * least far along in key exchange -- or, with none left in it, refuses --
 * rather than leaving admission to chance.
 */
#define AXL_SSH_MAX_STARTUPS_DEFAULT  10u

/**
 * @brief Largest cap axl_ssh_server_set_max_startups() accepts -- exactly
 *     the connection cap.
 *
 * Raising this without also raising the connection cap is meaningless:
 * every unauthenticated connection is also a connection, so a pre-auth cap
 * above the total can never be reached -- the server never holds more than
 * sixteen connections of any kind, regardless of what this setter is given.
 * The connection cap itself has no public name (`MAX_CONNS` in
 * axl-ssh-server.c is internal); this constant is how a caller learns it.
 */
#define AXL_SSH_MAX_STARTUPS_MAX  16u

/**
 * @brief Set how many unauthenticated connections may be held at once --
 *     AxlSsh's counterpart of sshd's `MaxStartups`, enforced differently.
 *
 * Replaces #AXL_SSH_MAX_STARTUPS_DEFAULT. **What counts** is every
 * connection that has not reached USERAUTH_SUCCESS, in ANY state --
 * including one still waiting on an authentication hook's verdict, which
 * has already finished key exchange and sent a real request. That is
 * broader than "still exchanging keys": a connection that is
 * authenticating, just slowly, still holds a pre-auth slot. So does one
 * being torn down while its authentication hook runs: it counts toward
 * the cap until it is gone.
 *
 * **Past the cap, the server evicts before it refuses.** While fewer than
 * @a count connections are unauthenticated -- and the server is below its
 * sixteen-connection total -- a new arrival is admitted outright. Once
 * either cap is reached, the server EVICTS one unauthenticated connection
 * that has not yet finished key exchange and admits the new one in its
 * place: the one LEAST FAR ALONG in it (still waiting for the peer's
 * identification string, then its KEXINIT, then its key exchange message,
 * then its NEWKEYS), and the OLDEST among equals. If every unauthenticated
 * connection has already finished key exchange -- there is no such victim
 * -- the new connection is REFUSED instead: an abortive close, nothing
 * sent, exactly like a per-source penalty refusal. An evicted connection
 * ends unauthenticated and is penalised like any other (`noauth`, see
 * #AxlSshPenalties); a refused one never became a connection and accrues
 * nothing.
 *
 * **What that protects, and what it does not.** Progress ranks first
 * because age alone gives a real client a clock: it would survive only
 * about @a count further arrivals, so a sustained flood could evict it
 * mid-key-exchange -- or while its user answers ssh(1)'s host-key prompt.
 * Ranked by progress, a flood that opens connections and says nothing
 * cannot displace a client that has sent its KEXINIT, one round trip after
 * connecting, whether the flood has stopped or is still arriving (tested to
 * about sixty connections a second). It does NOT stop a flood whose peers
 * send their own KEXINIT and key exchange message -- that costs them no
 * cryptography, and at equal progress age decides again -- nor one that
 * completes real key exchanges: those are past eviction, so new arrivals
 * are refused until the login grace reaps them, and each such connection
 * accrues its source a penalty. sshd has no defence against the last case
 * either beyond its per-source penalties and login grace.
 *
 * **Lowering the value evicts nothing by itself.** It changes only what the
 * NEXT arrival is compared against, exactly like
 * axl_ssh_server_set_max_password_failures(). A server already over a
 * newly-lowered cap -- open connections do not shrink to fit it -- applies
 * the ordinary admission rule at each arrival: it evicts one pre-auth
 * connection for it, or refuses it, as above. An eviction swaps one
 * connection for another, so admission never brings the count down; only
 * connections ending on their own -- authenticating, closing, or reaped by
 * the login grace -- return the server to under the cap.
 *
 * **May be called at any time from the server's loop**, including while it
 * is listening and from an authentication hook -- the same rule
 * axl_ssh_server_set_max_password_failures() and
 * axl_ssh_server_set_banner() follow.
 *
 * **sshd's `MaxStartups N:P:M` refuses new connections with rising
 * probability between N and M; this evicts instead of gambling** (deviation
 * 3, `docs/superpowers/specs/2026-08-21-axl-ssh-design.md` section 12a.7) --
 * sixteen total slots leave no room for sshd's three-point curve, and
 * against a flood that never sends a KEXINIT eviction is the stronger
 * defence: it cannot displace a client that has sent one, where a
 * probabilistic refusal still lets it win some of the time.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server, @a count 0, or @a count
 *     over #AXL_SSH_MAX_STARTUPS_MAX; #AXL_ERR if the server is not live --
 *     already freed, or being freed by a call still on the stack somewhere
 *     in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_max_startups(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      count    ///< unauthenticated connections held at once
);

/**
 * @brief Largest pre-authentication banner, in bytes,
 *     axl_ssh_server_set_banner() will accept.
 *
 * 4096 bytes, because this banner carries the text operators are REQUIRED to
 * show: the DoD Notice and Consent Banner (DoDI 8500.01; the STIG
 * requirement for SSH login notices) alone is about 1.3 KB, which the 1024
 * bytes of #AXL_SSH_AUTH_BANNER_MAX would refuse. Far inside one SSH packet
 * (#AXL_SSH_MAX_PACKET).
 */
#define AXL_SSH_BANNER_MAX  4096u

/**
 * @brief Set text sent to every client before it authenticates -- sshd's
 *     `Banner`.
 *
 * Sent as SSH_MSG_USERAUTH_BANNER (RFC 4252 section 5.4), ahead of the reply
 * to a connection's first authentication request, so `ssh(1)` prints it
 * before its first prompt. Whether it is DISPLAYED is the client's choice --
 * the RFC lets a client turn banners off, and OpenSSH 9.9p1 measurably does:
 * it prints the banner at its default level and at `-v` (once, not twice),
 * and suppresses it under `-q` and under `-o LogLevel=ERROR` -- the two
 * levels measured. **The two suppress differently, and only one of them is
 * silence.** Measured with a login that fails, so the client has an error of
 * its own to print: at `-o LogLevel=ERROR` the banner is gone and the error
 * line is still there; at `-q` nothing is printed at all. So a compliance
 * notice does not reach a script that quietens its client, whatever this
 * server sends -- and a quiet capture from such a script is not evidence the
 * banner was never sent.
 *
 * Every connection that reaches authentication gets it, whatever user name
 * or method it asks with, as sshd's does -- AxlSsh has no user database, and
 * the banner is sent before any hook could say whether the name exists. A
 * connection refused at accept by the per-source penalty, or one whose first
 * request names a service other than `ssh-connection`, never reaches
 * authentication and gets nothing.
 *
 * **Once per connection.** The decision is made when the server reads a
 * connection's first authentication request: it is sent whatever banner is
 * set at that moment, or none, and never one later. If the banner cannot be
 * sent, the connection is dropped rather than authenticated without it.
 *
 * **Like axl_ssh_server_set_max_password_failures() and the authorized-key
 * calls, and unlike the other `set_` calls, this may be called at any time
 * from the server's loop** -- including while it is listening, and from an
 * authentication hook. sshd reads its `Banner` file afresh for each
 * connection, so an edit reaches the next login without a restart; this does
 * the same. The banner is content, not an enforcement parameter: nothing is
 * timed or counted against it.
 *
 * The text is copied, so @a banner need not outlive the call. NULL or an
 * empty string removes the banner, which is the default -- sshd's
 * `Banner none`.
 *
 * **Sent as given.** It must be valid UTF-8, the encoding RFC 4252 section
 * 5.4 specifies for this field (checked with axl_utf8_validate()); that is
 * conformance, caught at configuration time, not a security filter. Nothing
 * is otherwise filtered or rewritten: the text is the operator's, and the
 * RFC puts control-character filtering on the client -- so what an operator
 * writes is not always what a user sees. Measured against OpenSSH 9.9p1,
 * capturing its stderr to a file: TAB, LF and CR pass through UNCHANGED, so
 * LF and CRLF both break the line and a lone CR is delivered as one; every
 * OTHER control byte becomes a four-character octal escape, with ESC
 * arriving as a literal backslash followed by `033`, so a colour code in the
 * text is shown rather than obeyed. Non-ASCII passes through only when the
 * client's locale is UTF-8: under `LC_ALL=C` each byte above 0x7F is escaped
 * the same way, so a two-byte `e`-acute arrives as eight characters. And a
 * banner with no final newline -- what a settings-page textarea submits --
 * leaves its last line unterminated for the client too: whatever `ssh(1)`
 * prints next continues that line. `test-ssh-policy-qemu.sh` re-measures all
 * of this on every suite run rather than leaving it a claim made once.
 *
 * Separate from the banner an authentication verdict may carry
 * (axl_ssh_auth_complete()), which is per request and bounded by
 * #AXL_SSH_AUTH_BANNER_MAX: a connection can be sent both, this one first.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server, or a @a banner longer than
 *     #AXL_SSH_BANNER_MAX bytes -- refused, not truncated: a truncated
 *     mandated notice is a non-compliant one that looks compliant -- or not
 *     valid UTF-8; #AXL_NO_RESOURCES when it cannot be copied; #AXL_ERR if
 *     the server is not live -- already freed, or being freed by a call
 *     still on the stack somewhere in this dispatch. On every failure the
 *     previous banner stays in force.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_banner(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    const char   *banner   ///< UTF-8 text, or NULL or "" for none
);

/**
 * @brief Default bytes one key may carry in either direction before the
 *     server starts a key re-exchange: 1 GiB.
 *
 * OpenSSH's `RekeyLimit default none` for chacha20-poly1305@openssh.com,
 * which works out to the same 1 GiB (2^30 / 8 blocks of 8 bytes).
 */
#define AXL_SSH_REKEY_LIMIT_DEFAULT  1073741824ull

/**
 * @brief Smallest non-zero byte limit axl_ssh_server_set_rekey_limit()
 *     accepts: 16.
 *
 * sshd's own floor: OpenSSH 9.9p1 refuses a `RekeyLimit` from 1 to 15 as
 * "too small" (servconf.c, and readconf.c for the client). Every exchange
 * costs the server an ECDH and an Ed25519 signature, so a limit under one
 * packet would spend them on every packet.
 */
#define AXL_SSH_REKEY_LIMIT_MIN  16u

/**
 * @brief Set how many bytes one key may carry before the server rekeys --
 *     sshd's `RekeyLimit`, without its time half.
 *
 * Replaces #AXL_SSH_REKEY_LIMIT_DEFAULT. The count is per DIRECTION, since
 * the two directions change key at different packets: bytes sent under the
 * current outbound key, and bytes received under the current inbound key,
 * each counted from that direction's last key exchange. When either passes
 * @a bytes on an authenticated connection the server sends its KEXINIT, and
 * the exchange runs as one the client started would. A client may rekey on
 * its own at any time after authentication (`ssh -o RekeyLimit=...`), and
 * the server takes part whatever this is set to. 0 means never by bytes;
 * 1 to 15 are refused, as sshd refuses them (#AXL_SSH_REKEY_LIMIT_MIN).
 *
 * **A rekey after 2^31 packets in either direction happens regardless**,
 * even at 0: the sequence number on the wire is 32 bits, and it is also the
 * cipher's nonce, so a key must not see it wrap. That is OpenSSH's
 * `MAX_PACKETS`.
 *
 * **There is no time limit.** sshd's `RekeyLimit` takes a second, time
 * argument and its default is none, so a stock sshd never rekeys an idle
 * connection either -- see `docs/superpowers/specs/2026-08-21-axl-ssh-design.md`
 * section 12a.4. Before authentication no rekey is started, and a client's
 * KEXINIT ends the connection, as it did before rekey existed.
 *
 * **A rekey is a transient.** From the server's KEXINIT until its NEWKEYS,
 * no channel data reaches the wire (RFC 4253 section 7.1 allows only
 * transport messages in that window): the one packet whose sending started
 * the exchange may already be packetized, and is held and sealed after the
 * server's NEWKEYS, but nothing more is packetized meanwhile.
 * axl_ssh_channel_write() keeps accepting output up to its limits, so an
 * application sees an exchange, at most, as a short #AXL_BUSY followed by
 * its writable hook.
 *
 * **May be called at any time from the server's loop**, including while it
 * is listening and from a hook. It applies to connections accepted
 * afterwards; a connection already open keeps the limit it was accepted
 * with, as axl_ssh_server_set_max_password_failures() does and for sshd's
 * reason: each of its connections runs under the configuration in force
 * when it arrived.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server, or @a bytes from 1 to
 *     #AXL_SSH_REKEY_LIMIT_MIN - 1 (sshd's "too small"); #AXL_ERR if the
 *     server is not live -- already freed, or being freed by a call still
 *     on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_rekey_limit(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint64_t      bytes    ///< bytes per key per direction; 0 = never by bytes
);

/**
 * @brief Default seconds an authenticated connection may go with no channel
 *     in use before the server ends it: 300.
 *
 * sshd's `UnusedConnectionTimeout`, whose own default is none. **On here,
 * deliberately** (spec section 12a.7, deviation 2): sshd spends one process
 * on an idle connection, while here the whole server is 16 slots, and an
 * authenticated connection is never evicted to make room for another. Five
 * minutes leaves an `ssh -o ControlPersist` master its room.
 */
#define AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC  300u

/**
 * @brief Default seconds of silence from an authenticated peer before the
 *     server sends it a keepalive probe: 60.
 *
 * sshd's `ClientAliveInterval`, whose own default is 0 (off; sshd relies on
 * `TCPKeepAlive`). **On here, deliberately**, for the reason
 * #AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC gives (spec section 12a.7,
 * deviation 2).
 */
#define AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC    60u

/**
 * @brief Default number of unanswered keepalive probes tolerated: 3, sshd's
 *     own `ClientAliveCountMax` default.
 */
#define AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX    3u

/**
 * @brief Largest unused timeout or client-alive interval, in seconds, the
 *     setters accept: 30 days.
 *
 * sshd has no cap. One is set here so that a typo -- `-1` converted to
 * 4294967295, say -- is refused rather than quietly becoming "never".
 */
#define AXL_SSH_IDLE_MAX_SEC                2592000u

/**
 * @brief End an authenticated connection that has had no channel in use
 *     for @a seconds -- sshd's `UnusedConnectionTimeout`.
 *
 * Replaces #AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC. 0 turns it off, which is
 * sshd's own default.
 *
 * **Only an AUTHENTICATED connection is timed.** Before USERAUTH_SUCCESS a
 * connection is bounded by the login grace
 * (axl_ssh_server_set_login_grace()); the clock here starts at
 * USERAUTH_SUCCESS.
 *
 * **"In use"** is sshd's `channel_still_open`: a session channel whose
 * shell or exec hook has ACCEPTED (axl-ssh-channel.h), from that moment
 * until its slot is freed -- after both sides' CLOSE, so a half-closed or
 * closing channel still counts. A session that was opened and never
 * started does NOT count, as sshd skips a LARVAL channel: a peer cannot
 * hold its slot by opening a channel and doing nothing. A channel hook on
 * the stack also counts. The clock restarts when the last channel in use
 * is freed, and the connection ends once it has stood unused for MORE than
 * @a seconds.
 *
 * **The end is silent and abortive**, as sshd's is (serverloop.c,
 * `cleanup_exit(255)` after the log line): no SSH_MSG_DISCONNECT, a TCP
 * reset, and the slot freed at once. Nothing is charged to the source's
 * penalty (axl_ssh_server_set_penalties()) -- the peer authenticated. One
 * line is written at INFO, sshd's words with sshd's "user" preamble:
 *
 *     terminating inactive connection from user USER A.B.C.D port N
 *
 * USER is the authenticated name, sanitised for the log as every audit
 * line's is. Like the other audit lines it is rate-limited to one INFO line
 * a minute of its kind; the rest are written at debug level -- the first of
 * a window exactly as shown, a later one with
 * ` (and N more since the previous line, at debug level only)` appended.
 *
 * **Granularity.** The reaper's sweep does the timing, not a timer per
 * connection, and axl_ssh_server_listen() sizes its tick to the finest of
 * 5 seconds, half the login grace, half @a seconds and half the
 * client-alive interval (spec section 12a.7, deviation 10). So a drop lands
 * between @a seconds and @a seconds plus one tick after the last channel
 * in use was freed, or after USERAUTH_SUCCESS if none has been.
 *
 * Call before axl_ssh_server_listen(), for the reason
 * axl_ssh_server_set_login_grace() gives: the reaper's tick is sized once,
 * at listen.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server or @a seconds over
 *     #AXL_SSH_IDLE_MAX_SEC; #AXL_ERR if the server is already listening,
 *     or not live -- already freed, or being freed by a call still on the
 *     stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_unused_timeout(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      seconds  ///< seconds with no channel in use; 0 = never
);

/**
 * @brief Probe an authenticated peer that has gone silent, and end it when
 *     it stops answering -- sshd's `ClientAliveInterval` and
 *     `ClientAliveCountMax`.
 *
 * Replaces #AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC and
 * #AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX. An @a interval_sec of 0 turns it off,
 * which is sshd's own default; a @a count_max of 0 probes but never ends
 * the connection, as sshd does.
 *
 * **Only an AUTHENTICATED connection is probed**; before USERAUTH_SUCCESS
 * the login grace bounds it (axl_ssh_server_set_login_grace()).
 *
 * **The probe** is `keepalive@openssh.com` with want_reply set and no data,
 * exactly as sshd sends it: a CHANNEL_REQUEST on the first of the peer's
 * channels on which neither side has sent CLOSE, and otherwise a
 * GLOBAL_REQUEST. sshd's `channel_find_open` would also pick a closing
 * channel, on which OpenSSH answers nothing; this server never does (spec
 * section 12a.7, deviation 11). OpenSSH, and any client following RFC
 * 4254's want_reply rule, answers it with a failure, which is all that is
 * wanted. It is logged at debug level only; sshd logs none.
 *
 * **What resets what** (sshd's serverloop.c and packet.c). Any bytes from
 * the peer restart the silence clock. Every packet decoded after
 * authentication, whatever its type, sets the unanswered count back to 0
 * -- so does an answer to a probe, which is accepted silently rather than
 * drawing SSH_MSG_UNIMPLEMENTED. When @a interval_sec has passed since the
 * peer was last heard, or since the last probe was due, the count goes up;
 * past @a count_max the connection ends, otherwise a probe goes out.
 *
 * **The drop bound.** A peer silent since time T is ended no later than
 * (@a count_max + 1) x (@a interval_sec + tick) after T, the tick being the
 * reaper's (axl_ssh_server_set_unused_timeout() says how it is sized). At
 * the defaults that is 4 x 65 s; at an interval of 2 s and a count of 2,
 * with a 1 s tick, it is 9 s -- except while this server holds the input
 * (below), which suspends the count. A probe is sent on every tick it is
 * due, even while the server is sending, where sshd probes only when its
 * poll found nothing to do (spec section 12a.7, deviation 9): a client that
 * answers sees nothing different.
 *
 * **The end is silent and abortive**, as sshd's is: no SSH_MSG_DISCONNECT,
 * a TCP reset, the slot freed at once, and nothing charged to the source's
 * penalty. One line at INFO, sshd's words, rate-limited as the other audit
 * lines are -- the first of a window exactly as shown, a later one with
 * ` (and N more since the previous line, at debug level only)` appended:
 *
 *     Timeout, client not responding from user USER A.B.C.D port N
 *
 * **While the peer is not reading** -- the server has stopped reading its
 * input because the replies already owed to it cannot be sent -- a due probe
 * is NOT queued, but it still counts, and the clock is still set as if it
 * had been sent (spec section 12a.7, deviation 8). A peer that is not
 * reading cannot answer, and a probe queued per interval would grow a dead
 * connection's output without bound at @a count_max 0. So a peer that stops
 * reading is ended within the drop bound however full its queue is; sshd
 * queues the probe regardless.
 *
 * **While this server holds the input** -- one of its channel hooks on the
 * stack dispatching the loop, or the connection's turn to parse queued
 * behind another connection's authentication hook -- nothing is probed and
 * nothing is counted: the peer's answers are waiting unread because of the
 * server, not the peer. The unused timeout is not affected.
 *
 * **During a key re-exchange** the probe is held with the other
 * connection-layer packets and sent after the server's NEWKEYS
 * (axl_ssh_server_set_rekey_limit()).
 *
 * Any count is accepted, but `UINT32_MAX` never ends the connection: the
 * unanswered count is 32 bits and wraps to 0 before it can exceed it, so
 * that value probes forever, as 0 does. Separately, the periods are
 * computed in 64-bit milliseconds, where no interval up to
 * #AXL_SSH_IDLE_MAX_SEC can overflow.
 *
 * Call before axl_ssh_server_listen(), for the reason
 * axl_ssh_server_set_login_grace() gives.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL server or @a interval_sec over
 *     #AXL_SSH_IDLE_MAX_SEC; #AXL_ERR if the server is already listening,
 *     or not live -- already freed, or being freed by a call still on the
 *     stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_client_alive(
    AxlSshServer *server,       ///< server from axl_ssh_server_new()
    uint32_t      interval_sec, ///< silence before a keepalive probe; 0 = off
    uint32_t      count_max     ///< unanswered probes tolerated; 0 = never drop
);

/**
 * @brief Default accrual for a connection that was refused at least one
 *     authentication attempt, in seconds.
 */
#define AXL_SSH_PENALTY_AUTHFAIL_DEFAULT_SEC  5u

/**
 * @brief Default accrual for a connection that ended without making an
 *     authentication attempt at all, in seconds.
 */
#define AXL_SSH_PENALTY_NOAUTH_DEFAULT_SEC  1u

/**
 * @brief Default accrual for a connection the login-grace reaper dropped, in
 *     seconds.
 */
#define AXL_SSH_PENALTY_GRACE_EXCEEDED_DEFAULT_SEC  10u

/**
 * @brief Default accrual a source must EXCEED before anything is refused, in
 *     seconds.
 */
#define AXL_SSH_PENALTY_MIN_DEFAULT_SEC  15u

/**
 * @brief Default ceiling on one source's accrued penalty, in seconds.
 */
#define AXL_SSH_PENALTY_MAX_DEFAULT_SEC  600u

/**
 * @brief Largest value axl_ssh_server_set_penalties() accepts in any field of
 *     an #AxlSshPenalties, in seconds.
 *
 * A day is already past any throttle that makes sense on a firmware service,
 * and it is where the likeliest mistake lands: 600 seconds typed as 600000 --
 * the milliseconds-for-seconds slip #AXL_SSH_LOGIN_GRACE_MAX_SEC also guards
 * against -- is nearly seven days of refusing an address. It is also what
 * keeps the arithmetic inside 32 bits: with every term bounded here, no sum
 * of a remaining penalty and one accrual can overflow.
 */
#define AXL_SSH_PENALTY_FIELD_MAX_SEC  86400u

/**
 * @brief Most ranges axl_ssh_server_set_penalty_exempt() accepts in one
 *     list; its "Up to 8 ranges" paragraph says why 8.
 */
#define AXL_SSH_PENALTY_EXEMPT_MAX  8u

/**
 * @brief How long a source address is refused after connections from it end
 *     badly.
 *
 * Modelled on OpenSSH 9.9p1's `PerSourcePenalties` -- enabled by default
 * there, enabled by default here, and faithful to it in arithmetic and
 * defaults but NOT in its full event set (see "One OpenSSH event is
 * deliberately absent" below). **What it exists to fix:**
 * the password cap (axl_ssh_server_set_max_password_failures()) ends one
 * connection and nothing more, so a peer that reconnects gets a full cap of
 * fresh guesses at once, for ever. This is the only bound on guessing ACROSS
 * connections; the per-connection cap cannot be one.
 *
 * **Which accrual a connection earns, in this exact order.** At most one
 * applies, and the order is a promise, not an implementation accident:
 *
 *   1. **A connection that reached USERAUTH_SUCCESS accrues NOTHING**,
 *      whatever else it did on the way. This test comes FIRST and depends on
 *      nothing below it, exactly as the `authenticated` exemption does in
 *      axl_ssh_reap_reason() (`src/net/axl-ssh-reap.h`). That is not
 *      stylistic symmetry: this server has already shipped the bug that shape
 *      prevents, when one field carried both "authenticated" and "the clock
 *      could not be read" and the login grace silently stopped existing. A
 *      successful login that had a wrong password before it is the common
 *      typo, reachable with these defaults on any connection, and it must
 *      fall out of rule 1 rather than out of the ordering of rules 2 to 4.
 *      **Nor does a connection the SERVER failed**: one it could not set up
 *      (a resource failure before the peer said a word) or could not time
 *      (reaped because the clock was unreadable at accept). Charging those
 *      would let pressure a peer may have induced -- a full loop source
 *      table, say -- penalise legitimate operators, so they skip the ladder
 *      entirely rather than land in rule 4.
 *   2. @a grace_exceeded_sec -- the login-grace reaper dropped it
 *      (axl_ssh_server_set_login_grace()), because the grace ran out.
 *   3. @a authfail_sec -- the connection made at least one real
 *      authentication attempt and the server refused it, **by either
 *      method**. Stated as the outcome rather than as a wire message, because
 *      the two do not coincide (the forgery case below sends no failure
 *      reply at all). Deliberately BROADER than
 *      the password cap, which counts wrong passwords only and excludes
 *      a refused publickey login on the grounds that it is not a guess: that
 *      is right for a per-connection guess cap and wrong here, where the only
 *      question is whether the connection ended badly. Reusing the cap's
 *      counter would leave a scripted publickey prober accruing @a noauth_sec
 *      instead, which is the under-penalty this rule exists to close. A
 *      refused user name, an unknown method, an unauthorized key offered by a
 *      client walking its key list, and an application DENY all count -- and
 *      **so does a signature that fails to verify**, which sends no
 *      USERAUTH_FAILURE at all: the key WAS authorized, and the forged
 *      signature ends the connection by DISCONNECT instead of a failure
 *      reply. Reading the rule off the wire messages alone would drop
 *      forgery through to rule 4 and charge the mildest tier for the case
 *      that most deserves the harshest. (A signature the server could not
 *      CHECK, because it ran out of memory doing so, is no forgery: that
 *      connection is charged like any other the server drops mid-parse, by
 *      what it did before.) The password cap's disconnect needs no
 *      such clause, because the attempt that reaches the cap is refused
 *      through the same path as every other, and the ladder reads that
 *      refusal, not the wire message that answered it. (Reading it off the
 *      wire would be wrong at a cap of 1, where the FIRST failure is the
 *      DISCONNECT and no USERAUTH_FAILURE is ever sent.)
 *      **The `none` probe does not count**, and that exclusion is load-bearing:
 *      RFC 4252 section 5.2's query is answered with a USERAUTH_FAILURE, it
 *      is the first thing every stock client sends, and counting it would
 *      accrue @a authfail_sec on every connection ever made and leave rule 4
 *      unreachable. It is the same request this server already exempts from
 *      the minimum failure time, and the same one sshd exempts.
 *   4. @a noauth_sec -- anything else: a scan that connected and left, a
 *      client that only ever asked what methods exist, and a connection
 *      dropped for a protocol error (see below).
 *
 * The order is FIXED and causal; it is not sorted by size. A caller that sets
 * @a noauth_sec above @a authfail_sec still gets @a authfail_sec for a
 * connection that failed to authenticate. A throttle whose ordering changes
 * with its configuration is one nobody can reason about.
 *
 * **Two OpenSSH events are deliberately absent, so this is three of its
 * five.** The first is `crash` (90 s), which names sshd's own
 * privilege-separation child dying while serving a connection -- a strong
 * signal that someone is working on a memory-safety bug. AxlSsh has no such
 * process: there is no fork and no monitor, and its nearest analogue is a
 * malformed packet, which ends the connection through the ordinary
 * protocol-error path. That is the parser WORKING, not the server falling
 * over, so it accrues @a noauth_sec under rule 4 rather than a 90-second
 * penalty of its own. The second is `refuseconnection` (10 s), which
 * penalises a client sshd turned away under its `RefuseConnection` option;
 * AxlSsh has no per-match connection policy, so there is nothing that could
 * ever raise it. Do not read this as full fidelity to `PerSourcePenalties`.
 *
 * **The arithmetic, exactly.** Each source has a remaining penalty, zero
 * until something accrues. When a connection from that source ends, the
 * accrual chosen above makes the remaining penalty
 * `min(remaining + accrual, max_sec)`. Remaining burns down in real time,
 * whether or not anything is being refused, because it is held as an absolute
 * deadline on the monotonic clock rather than a countdown something has to
 * tick. **A penalty ACTIVATES the moment remaining is STRICTLY GREATER than
 * @a min_sec, and once active it is enforced until it has burned down to
 * nothing**: @a min_sec decides when refusing starts, not how long it lasts.
 * Until then the penalty is deferred -- recorded, accruing, burning down, and
 * refusing nothing. An active penalty that runs out leaves the source as if
 * it had never accrued, so its next accrual is deferred again.
 *
 * Both halves are what the stock server does, measured against OpenSSH 9.9p1
 * with its defaults: four consecutive failed connections were each allowed,
 * sshd logged `activating ipv4 penalty of 19 seconds` as the fourth ended
 * (four 5-second accruals, less the time between them), and every connection
 * for the next 20 seconds was refused -- the WHOLE penalty, not the four
 * seconds of it above the threshold. Exactly @a min_sec does not activate:
 * the third accrual reached 15 and the fourth connection was still allowed.
 *
 * **Why a threshold at all, rather than refusing from the first failure.** It
 * is not a weaker rule, it is what keeps the feature off the person it is
 * meant to protect. With the defaults a source may end FOUR whole connections
 * badly before anything is refused, so an operator who mistypes a password
 * three times with the defaults, gets disconnected, reconnects and types it
 * correctly is never refused -- and that reconnect is the exact sequence
 * this feature would
 * otherwise punish. Below the threshold the accrual is still recorded and
 * still burning down, so four bad connections close together activate a
 * penalty and four spread over several minutes do not -- which is most of the
 * difference between a script and a person.
 *
 * **What a refused connection sees.** The TCP connection is accepted and
 * closed at once, before the identification line: no key exchange, no delay,
 * and no authentication hook is called. Here AxlSsh differs from sshd in one
 * visible detail. sshd writes the single line `Not allowed at this time`
 * before it closes, which ssh(1) shows only in its debug output
 * (`banner line 0`); AxlSsh writes nothing and closes ABORTIVELY (RST), so
 * the peer sees the connection reset during identification with no text.
 * Both halves are deliberate, and the second is load-bearing. A line queued
 * ahead of an RST can be discarded with it, so writing one would promise
 * text the peer may never see. And a GRACEFUL close cannot be deferred here:
 * a socket fresh from accept has had no asynchronous operation, so closing
 * it from inside the accept callback waits INLINE for the FIN handshake -- a
 * round trip, 500 ms for a peer that ACKs our FIN without closing, up to
 * ~3 s for one that never ACKs it -- per refused connection, with every
 * other service on the loop stalled for it. A peer
 * reconnecting during its own penalty would turn the throttle into exactly
 * the denial of service it exists to prevent. The abortive close has no such
 * wait: EDK2 completes it synchronously, inside Close(). If a line before
 * the refusal is ever wanted, it needs a different design -- an asynchronous
 * send, then the RST from its completion -- never a graceful close. A refused
 * connection **consumes no connection
 * slot**: nothing is allocated, the count the connection cap watches does not
 * move, and the accept stays armed. Refusing therefore costs the server
 * strictly less than serving the same peer did before.
 *
 * **A refused connection accrues nothing**, and this is load-bearing. If
 * knocking during a penalty extended it, a peer that reconnects in a loop --
 * which is what a script does -- would hold itself refused for ever, and a
 * throttle that never expires is the permanent lockout this design rejects.
 * The penalty always burns down. sshd agrees, measured: twenty refused
 * connections, one a second, left its 19-second penalty expiring on time.
 *
 * **Penalties do not touch connections already established.** Enforcement
 * happens once, at accept. A connection that is already up when a penalty
 * activates runs on under its own login grace, as it does on OpenSSH.
 *
 * **The table.** Keyed by the peer's exact IPv4 address -- EFI_TCP4 is
 * IPv4-only, and OpenSSH's default netblock is a /32, so exact matching is
 * faithful rather than a simplification. **256 sources** are remembered, and
 * the number is sized against how many addresses a guesser can field, not
 * against the connection cap, because **a full table is a bypass, not a
 * slowdown**: a source that cannot be recorded is not throttled at all (see
 * below). It was 32 once, argued from the 16-connection cap -- which bounds
 * live connections and says nothing about addresses -- and a flat management
 * LAN fields 33 addresses trivially. 256 puts the threshold past what a /24
 * can field, for about 4 KB that sit in the server for its whole life;
 * sshd's own threshold for the same effect is 65536. Entries below the
 * threshold occupy slots like any other; a source exempted by
 * axl_ssh_server_set_penalty_exempt() occupies none, because it is matched
 * before the table is consulted at all.
 * **An accrual for an address the table already tracks updates that entry in
 * place and evicts nothing**; eviction is only ever about finding room for a
 * source that is not tracked yet. **Retention is ranked ACTIVE FIRST, then by
 * time left.** An active entry -- still refusing its source -- outranks every
 * deferred one however little it has left, because an active penalty spends
 * its whole enforced decay passing back down through the same range a
 * deferred one lives in: ranked by time left alone, a refusal with 3 seconds
 * to run would lose its slot to a deferred entry at 14 that has never refused
 * anything, and the penalised source would walk straight back in. So when the
 * table is full an expired entry goes first, then the deferred entry with the
 * least time left, and an active entry only when every slot is active -- the
 * one with the least left, whose early release is the smallest. A new source
 * is ranked the same way (active on arrival if its first accrual already
 * exceeds @a min_sec) and takes the lowest-ranked slot only if it outranks
 * it; a tie, or a newcomer every entry outranks, drops the new accrual
 * instead. The table therefore keeps the 256 worst offenders rather than
 * the 256 most recent, which is what stops a burst of one-second scans from
 * evicting a source that is being refused. The price of that choice is the
 * bypass: once every slot outranks a new source, its accruals are dropped
 * and it is not tracked, so it guesses with no cross-connection limit for as
 * long as the table stays crowded. The first such drop in each @a max_sec is
 * logged at INFO, so a crowded table is visible at the default level; every
 * one is logged at DEBUG.
 *
 * **A successful login clears nothing.** It accrues nothing -- that is rule 1
 * -- but it does not forgive what earlier connections accrued, and neither
 * does sshd: measured, a deferred 10-second penalty survived a successful
 * login in between, and the next failure found it still there. Rule 1 is
 * measured too -- six sshd connections that each offered an unauthorized key
 * and then an authorized one accrued nothing. A source that guesses its way
 * to one valid credential must not thereby erase what it accrued guessing at
 * the next; on a server with one
 * account, a success means the guesser is already in, so an eraser would buy
 * nothing but a faster run at whatever comes after. Only time clears a
 * penalty.
 *
 * **Without a readable monotonic clock nothing is enforced and nothing
 * accrues.** axl_ssh_server_listen() already refuses to bind without one, so
 * this covers a counter that dies afterwards, and it is the same rule the
 * login grace follows: a decision must never be manufactured from a
 * meaningless reading. It gives a peer nothing, because the reaper drops
 * every unauthenticated connection on a server in that state anyway. A peer
 * whose address cannot be read at all is treated the same way.
 *
 * **What this does NOT buy, stated plainly.** Over any long run a source can
 * still end one failed connection for every @a authfail_sec seconds: each
 * adds that much, time removes it at one second per second, and the deferred
 * threshold is credit it may spend in a burst. With the defaults a patient
 * guesser gets about four connections -- four times the password cap, so
 * twelve guesses with the defaults -- then roughly
 * twenty seconds refused, over and over. The ceiling binds only when
 * failures arrive faster than they decay, which takes concurrent
 * connections. So this is a throttle and an audit signal, not a lockout and
 * not a substitute for a password worth guessing at. It is also keyed by
 * address: hosts sharing one NAT are penalised together; a guesser spread
 * across many addresses is bounded per address and not in total; and **one
 * that keeps more than 256 of its addresses tracked at once can leave its
 * next address untracked entirely**, guessing with no cross-connection limit
 * from it (the table paragraph above). A flat management LAN makes all three
 * easy. Addresses cannot be forged outright, since a penalty needs a
 * completed TCP handshake to accrue. axl_ssh_server_set_penalty_exempt()
 * answers the shared-address half of that, and buys its own weakness: an
 * exempt range is exempt for whoever is on it. There is also **no way to ask
 * whether a source is currently being refused**; the log is the only
 * visibility. An activation is logged at INFO, naming the address, the
 * penalty and its cause. Each refused connection is logged at DEBUG only,
 * where sshd logs every one at INFO: a peer reconnecting in a loop would
 * otherwise turn its own refusals into a flood on a firmware console.
 *
 * Fill with axl_ssh_penalties_default() and adjust, rather than by
 * initialising fields one at a time: a zero left in @a min_sec or @a max_sec
 * by an incomplete initialiser is a different configuration, not a default.
 */
typedef struct {
    uint32_t authfail_sec;        ///< rule 3: a refused attempt, either method
    uint32_t noauth_sec;          ///< rule 4: no attempt was made at all
    uint32_t grace_exceeded_sec;  ///< rule 2: the login-grace reaper dropped it
    uint32_t min_sec;             ///< accrual above which a penalty activates
    uint32_t max_sec;             ///< ceiling on one source's accrued penalty
} AxlSshPenalties;

/**
 * @brief Fill @a out with the defaults -- the same settings a server that
 *     never calls axl_ssh_server_set_penalties() already runs.
 *
 * Every field takes its `AXL_SSH_PENALTY_*_DEFAULT_SEC` value, which are
 * OpenSSH 9.9p1's, so tuning one field is a call to this followed by one
 * assignment and leaves the rest where sshd has them.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a out.
 */
AXL_WARN_UNUSED int
axl_ssh_penalties_default(
    AxlSshPenalties *out  ///< receives the default settings
);

/**
 * @brief Tune, or turn off, the per-source penalties.
 *
 * Copies @a penalties, so the caller's struct need not outlive the call.
 * **NULL turns penalties off**, which restores the behaviour of a server
 * without them: nothing is remembered across connections, so a peer that
 * exhausts its password cap may reconnect and spend a full cap more
 * immediately, without limit. Nothing else offers that bound, so turn them
 * off only where something in front of the server already does.
 *
 * Call before axl_ssh_server_listen() -- like axl_ssh_server_set_auth_ops()
 * and axl_ssh_server_set_login_grace(), this does not change under a server
 * that is already accepting.
 *
 * **The combinations that would silently disable enforcement are refused, not
 * accepted.** @a min_sec must be below @a max_sec, or the accrual is capped
 * at or under the threshold it has to exceed and nothing can ever be refused;
 * and at least one of the three accruals must be non-zero, or nothing ever
 * accrues. Both configurations look like tuning and behave like NULL, which
 * is worse than either: turning penalties off is a decision that should read
 * as one at the call site.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server, on a field above
 *     #AXL_SSH_PENALTY_FIELD_MAX_SEC, on @a min_sec not below @a max_sec, or
 *     on all three accruals being zero; #AXL_ERR if the server is already
 *     listening, or not live -- already freed, or being freed by a call still
 *     on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_penalties(
    AxlSshServer          *server,    ///< server from axl_ssh_server_new()
    const AxlSshPenalties *penalties  ///< settings to copy, or NULL to disable
);

/**
 * @brief Name source ranges that penalties never apply to.
 *
 * **The case this answers.** A penalty is keyed by source address, and on a
 * management network one address is often every operator: a BMC behind a jump
 * host sees all administrative traffic arrive from that one host, and a flat
 * management LAN is one subnet shared by everyone who is allowed in. Without
 * an exemption, one operator fat-fingering a password throttles the rest of
 * them. sshd's `PerSourcePenaltyExemptList` exists for exactly this; like
 * sshd's, **this list is empty by default and exempts nothing**.
 *
 * **What exempt means, precisely.** A connection from an exempt range is
 * never refused by a penalty AND never accrues one -- both checks happen
 * before anything else the penalties do, so an exempt source never even
 * occupies a slot in the 256-source table and a busy management LAN cannot
 * flush it. **It does NOT relax the password cap**, which is a
 * different mechanism at a different scope: it bounds guesses inside one
 * connection and has nothing to do with source addresses. Do not wire this
 * into it. An exemption therefore buys an exempt peer unlimited connections,
 * each still worth exactly the password cap in guesses -- which is the
 * pre-penalty behaviour, and the whole trust decision being made here.
 *
 * **The entry form** is what an operator already writes in `sshd_config`:
 * dotted-decimal with an optional prefix length, `"10.0.0.0/8"`,
 * `"192.0.2.7/32"`. It is read by axl_ipv4_parse_cidr(), the CIDR parser this
 * tree already has, so the `/` split, both halves and the `N > 32` rejection
 * are its business and its tests' -- not a second parser written here. It
 * also reports whether a prefix was present at all, which is what makes the
 * bare form below a policy AxlSsh states rather than a parsing accident. A
 * bare `"192.0.2.7"` means `/32` -- the narrowest reading, so a dropped
 * suffix can never silently widen a range. An address matches an entry when
 * its top `prefix` bits equal the entry's. Wildcards (`10.0.*`) are NOT
 * accepted, unlike sshd: one syntax is one matcher to get right, and every
 * range a glob can express is a prefix already. Entries are stored
 * numerically, so nothing of @a ranges is kept after this returns and the
 * caller's strings need not outlive the call -- the same copy-at-set-time
 * rule axl_ssh_server_set_auth_ops() follows.
 *
 * **Up to #AXL_SSH_PENALTY_EXEMPT_MAX (8) ranges.** An exempt list names
 * infrastructure, not people: a jump host is one `/32`, a site is one prefix
 * however large, and 8 leaves room for a second site and spares in 64 bytes
 * that live in the server for good.
 * It is also scanned on every accept, where short is free. A ninth entry is
 * refused rather than dropped, so a bound that turns out too small says so
 * loudly at configuration time instead of silently un-exempting somebody.
 *
 * **All or nothing**, like axl_ssh_server_load_authorized_keys(): every entry
 * is parsed first and the list is installed only if all of them are good, so
 * one typo fails the call and leaves the previous list exactly as it was. A
 * half-applied exempt list is worse than a rejected one -- the operator would
 * have no way to tell which half.
 *
 * **A malformed entry is an error, not a range.** Refused: an address that
 * does not parse, a prefix over 32, a prefix that is empty or has anything
 * after it -- all of those from axl_ipv4_parse_cidr() -- and, AxlSsh's own
 * check, an address with bits set below its prefix (`"192.0.2.0/8"`, whose
 * `0.0.2.0` is host bits). The last is sshd's rule too, and it catches
 * `"10.1.2.3/8"` written for `"10.0.0.0/8"`.
 *
 * **`/0` is refused as POLICY, not as malformed input.** It parses perfectly
 * well -- it is a passing case of axl_ipv4_parse_cidr()'s own tests -- and it
 * is refused for the reason axl_ssh_server_set_penalties() refuses its
 * silently-inert combinations: exempting the entire internet is turning
 * penalties off, and that should read as
 * `axl_ssh_server_set_penalties(server, NULL)` at the call site rather than
 * hide in one entry of an exempt list.
 *
 * **Independent of axl_ssh_server_set_penalties().** Either may be called
 * first, and neither clears the other -- so disabling and later re-enabling
 * penalties does not quietly forget the exemptions. Call both before
 * axl_ssh_server_listen().
 *
 * @return #AXL_OK, including for @a count == 0, which clears the list;
 *     #AXL_INVALID on a NULL @a server, a NULL @a ranges with a non-zero
 *     @a count, a NULL entry, any malformed entry, or a well-formed `/0`;
 *     #AXL_NO_RESOURCES when @a count is above #AXL_SSH_PENALTY_EXEMPT_MAX;
 *     #AXL_ERR if the server is already listening, or not live -- already
 *     freed, or being freed by a call still on the stack somewhere in this
 *     dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_penalty_exempt(
    AxlSshServer      *server,  ///< server from axl_ssh_server_new()
    const char *const *ranges,  ///< entries to parse and copy, or NULL to clear
    size_t             count    ///< how many entries @a ranges holds
);

/**
 * @brief Check one penalty-exempt range by the rule
 *     axl_ssh_server_set_penalty_exempt() applies, without a server.
 *
 * `A.B.C.D` or `A.B.C.D/N`, exactly as one entry of that list is judged --
 * the same function decides both, so a configuration loader can refuse a
 * bad range at the line it came from before any server exists. Refused:
 * NULL, an address that does not parse, a prefix over 32, a prefix that is
 * empty or has anything after it, a well-formed `/0` (policy, as there),
 * and an address with bits set below its prefix (`10.1.2.3/8`). A bare
 * address is a /32. Pure: no state, no allocation. It judges ONE entry;
 * the list-length bound, #AXL_SSH_PENALTY_EXEMPT_MAX, is the caller's to
 * count, and a list over it is refused by axl_ssh_server_set_penalty_exempt()
 * as #AXL_NO_RESOURCES.
 *
 * @return #AXL_OK, or #AXL_INVALID.
 */
AXL_WARN_UNUSED int
axl_ssh_penalty_range_check(
    const char *range   ///< the range text
);

/**
 * @brief Replace the server's host key with one the caller brings.
 *
 * The server ADOPTS @a key on #AXL_OK: it frees it (axl_pk_key_free()
 * zeroises) when the server is freed or when a later call replaces it. The
 * key it replaces -- the ephemeral one axl_ssh_server_new() generated, or an
 * earlier call's -- is zeroised and freed here. On any failure the caller
 * still owns @a key.
 *
 * **Before axl_ssh_server_listen() only.** Rekey signs with the host key
 * again, and ssh(1) refuses a host key that changes during a rekey, so a
 * live replacement would end every session at its next rekey. To change the
 * key of a running server, free it and build a new one -- what a restarted
 * sshd does.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL argument, a key that is not
 *     #AXL_PK_ED25519, or one with no private half; #AXL_ERR if the server is
 *     already listening, or not live (already freed, or being freed by a call
 *     still on the stack somewhere in this dispatch).
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_host_key(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    AxlPkKey     *key      ///< Ed25519 private key; adopted on success
);

/**
 * @brief The host key's fingerprint, as axl_ssh_host_key_fingerprint()
 *        gives it: what a client is shown at first contact.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL argument or @a cap below
 *     #AXL_SSH_FINGERPRINT_SIZE; #AXL_ERR if the server is not live.
 */
AXL_WARN_UNUSED int
axl_ssh_server_host_key_fingerprint(
    const AxlSshServer *server,  ///< server
    char               *out,     ///< [out] NUL-terminated fingerprint
    size_t              cap      ///< size of @a out
);

/**
 * @brief The host key as a public line, as axl_ssh_host_key_public_line()
 *        gives it, for provisioning known_hosts.
 *
 * @return as axl_ssh_server_host_key_fingerprint(), with
 *     #AXL_SSH_PUBLIC_LINE_SIZE.
 */
AXL_WARN_UNUSED int
axl_ssh_server_host_key_public_line(
    const AxlSshServer *server,  ///< server
    char               *out,     ///< [out] NUL-terminated line
    size_t              cap      ///< size of @a out
);

/**
 * @brief The longest a disconnecting connection waits for its peer to hang
 *        up before it is reset.
 *
 * A connection AxlSsh ends with a DISCONNECT keeps reading (and discarding)
 * until the peer closes -- the evidence that it read the reason; ssh(1)
 * prints it and hangs up at once -- and is then reset. A peer that does not
 * close within this bound is reset anyway, so a peer that stops reading
 * cannot hold a connection or stall the server; the deadline is fixed when
 * the DISCONNECT is sent and nothing the peer sends moves it.
 *
 * 1 s: the wait covers one round trip (the DISCONNECT out, the client's FIN
 * back), and on the networks this serves -- a LAN or a BMC's management
 * link -- that is milliseconds, so 1 s is two orders of magnitude of
 * headroom. What a longer bound would cost is the lingering connections
 * themselves -- an AxlTcp and a firmware TCP child each, and their slots,
 * although a lingering connection is the first eviction takes. It does NOT
 * cover a lost segment: TCP's minimum retransmission timeout is 1 s
 * (RFC 6298 2.4), so a DISCONNECT whose segment is lost is reset before it is
 * resent and is not recovered -- which is no worse than the reset every
 * DISCONNECT got before.
 */
#define AXL_SSH_DISCONNECT_LINGER_MS  1000u

/**
 * @brief Bind the port and start accepting connections.
 *
 * Returns as soon as the listener is armed; connections are served from the
 * loop. Call at most once per server.
 *
 * @return #AXL_OK, or #AXL_ERR on a NULL server, a port already in use, a
 *     server that is already listening, a server that is not live --
 *     already freed, or being freed by a call still on the stack somewhere
 *     in this dispatch -- or **a server that can authenticate nobody** --
 *     no authorized key and no password hook -- since binding the port
 *     would offer network surface for nothing. Or **a platform with no
 *     readable monotonic clock**, which is what enforces the login grace on
 *     an unauthenticated connection and times every per-source penalty
 *     (#AxlSshPenalties). Both are the same argument: a server
 *     that cannot do the thing that keeps it safe should not present the
 *     network surface.
 *
 * **sshd binds regardless in both cases, and the difference is
 * deliberate** -- do not "fix" this toward parity. sshd runs on a host with
 * a working clock and an administrator watching a log; a DXE driver has
 * neither, so a refusal at `listen()` is the only place either condition
 * can be reported to anyone.
 */
AXL_WARN_UNUSED int
axl_ssh_server_listen(
    AxlSshServer *server  ///< server from axl_ssh_server_new()
);

/**
 * @brief Stop listening, drop every live connection, and free the server.
 *
 * NULL-safe. A server already freed -- including one whose own free is
 * still finishing, see below -- is ignored here rather than freed a second
 * time: this removes @a server from the live-server table before touching
 * anything else, so a later call naming the SAME pointer finds no match,
 * whether or not that pointer's memory has actually been released yet.
 * **That is not the same as safety against a stale pointer forever.** The
 * check is by address, not by identity: once the memory IS actually
 * released, axl_ssh_server_new() may place a brand new, unrelated server at
 * that same address, and this call -- or axl_ssh_server_listen(),
 * axl_ssh_server_set_auth_ops(), axl_ssh_server_set_login_grace(),
 * axl_ssh_server_set_penalties(), axl_ssh_server_set_penalty_exempt(),
 * axl_ssh_server_set_max_password_failures(),
 * axl_ssh_server_set_max_startups(),
 * axl_ssh_server_set_banner(),
 * axl_ssh_server_set_auth_hook_budget_ms(),
 * axl_ssh_server_set_unused_timeout(), axl_ssh_server_set_client_alive(),
 * axl_ssh_server_set_host_key(), axl_ssh_server_host_key_fingerprint(),
 * axl_ssh_server_host_key_public_line(),
 * or the authorized-key calls
 * -- would then find it live and act on it instead. Do not keep a pointer to a server
 * past freeing it. A late axl_ssh_auth_complete() is the one call this does
 * not threaten: its handles are unique across the whole image, so a stale
 * one still matches nothing on whatever server now sits at that address.
 * Zeroizes the host key. Must be called BEFORE the loop it was created on is
 * freed — connections are closed abortively so that no finalize is left
 * queued on a loop that is about to disappear.
 *
 * **May be called from anywhere on that loop**, including from an
 * authentication hook, from release_principal, or from anything their loop
 * dispatch runs — a web handler that turns SSH off while a password check
 * waits, say. The server always STOPS by the time this returns: the port is
 * released, the host key and the timing secret are zeroised, and every
 * connection is dropped -- **except the one whose own hook is on the stack
 * right now**, which this call only marks for drop. What can wait, for that
 * one connection, is not only freeing the server's memory but that
 * connection's own release_principal, if it holds a principal: both finish
 * from the loop once the frames beneath that hook return, which is often
 * after this call has already returned to ITS caller. **Do not tear down
 * the principal registry or the user_data a hook was given just because this
 * call has returned** -- that release can still be owed. Two consequences:
 *   - the connection whose own hook is running is closed, and its principal
 *     released if it held one, a moment after this call returns rather than
 *     inside it;
 *   - a loop that is never dispatched again after such a call leaks the
 *     (zeroised) server struct AND never makes that last release_principal
 *     call at all — so free the server, let the loop run, then free the
 *     loop, which is the order the paragraph above already asks for.
 */
void
axl_ssh_server_free(
    AxlSshServer *server  ///< server to free, or NULL
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlSshServer, axl_ssh_server_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_SSH_SERVER_H */
