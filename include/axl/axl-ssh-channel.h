/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-ssh-channel.h
 * @brief Session channels on an AxlSshServer: `ssh host`, `ssh -tt host`,
 *     `ssh host cmd` and subsystems such as `sftp`, delivered to the
 *     application through hooks.
 *
 * ```c
 * static int
 * on_shell(AxlSshChannel ch, void *ud)
 * {
 *     return AXL_OK;                       // accept; output follows below
 * }
 *
 * static void
 * on_data(AxlSshChannel ch, const uint8_t *buf, size_t len, void *ud)
 * {
 *     AxlSshServer *s = ud;
 *     if (axl_ssh_channel_write(s, ch, buf, len) == AXL_BUSY) {
 *         // behind: stop writing to ch until its writable hook runs
 *     }
 * }
 *
 * static const AxlSshChannelOps ops = {
 *     .shell = on_shell,
 *     .data  = on_data,
 * };
 * if (axl_ssh_server_set_channel_ops(s, &ops, s) != AXL_OK) {
 *     // only before listening, on a live server
 * }
 * ```
 *
 * **Opt-in.** Until axl_ssh_server_set_channel_ops() installs hooks, every
 * `CHANNEL_OPEN` is refused with `SSH_OPEN_UNKNOWN_CHANNEL_TYPE` -- the same
 * reason code a server built before channels existed sends, and the
 * connection stays up as it did then. What differs is the refusal's
 * description, which is now empty: ssh(1) prints
 * `open failed: unknown channel type:` with nothing after the colon. With
 * hooks installed, only the `session` type (RFC 4254 section 6.1) is ever
 * accepted; every other type (`x11`, `direct-tcpip`, `forwarded-tcpip`, ...)
 * is still refused with `SSH_OPEN_UNKNOWN_CHANNEL_TYPE`.
 *
 * **Handles, not pointers.** A channel is an #AxlSshChannel, an integer. The
 * realistic mistake is a reference kept past close -- an application that
 * writes to its clients from its own timer, long after the hook that opened
 * them -- and with an integer that mistake is an #AXL_NOT_FOUND rather than
 * a use-after-free. **Every `axl_ssh_channel_*` call on a handle that is 0,
 * was never issued, or belongs to a channel that has closed returns
 * #AXL_NOT_FOUND and does nothing** (the pointer getters return NULL) --
 * including a call made from inside the channel's own close hook, a call
 * made while the channel's connection is being dropped -- from the moment
 * the server decides to drop it, before the close hook has run -- and a call
 * naming a server that has been freed: the server is recognised without
 * being read, as axl_ssh_auth_complete() recognises one. The one argument
 * checked before the handle is a NULL @a server, which is #AXL_INVALID.
 *
 * **Every call is made from the server's loop** -- from a hook, a timer, a
 * deferred call or another service's callback on the loop the server was
 * created on, never from another core.
 *
 * **What a hook may call.** Every `axl_ssh_channel_*` call is allowed from
 * every channel hook, on any channel -- including the one the hook is for --
 * and so is every server setter, under that setter's own rule about when it
 * may be called, and axl_ssh_server_free(). None of them runs a hook of the
 * same channel inside itself: axl_ssh_channel_close() and
 * axl_ssh_channel_exit() take effect on the wire at once or from the loop,
 * and the close hook they lead to is never called while another hook of that
 * channel is on the stack. A hook MAY dispatch the loop
 * (axl_loop_iterate_until()); while it does, its connection parses no input
 * at all -- every channel of that connection waits -- because data hooks are
 * handed a pointer into the connection's one payload buffer (see the data
 * hook). Other connections keep running, and their hooks may run nested
 * inside it: channel hooks are NOT one-at-a-time server-wide the way
 * authentication hooks are, and the authentication hook budget
 * (axl_ssh_server_set_auth_hook_budget_ms()) does not watch them.
 *
 * **So a hook must never wait for anything its OWN connection's peer has to
 * send** -- a `WINDOW_ADJUST` (and so a writable hook on any channel of that
 * connection that is waiting on the peer's window), more data, or a CLOSE.
 * None of it is read until the hook returns, so such a wait never ends: it
 * deadlocks the connection until it drops. Emulating a blocking write by
 * iterating the loop until writable is exactly that mistake. Write, return,
 * and continue from the writable hook.
 *
 * **Writing is all-or-nothing, bounded, and edge-signalled.**
 * axl_ssh_channel_write() queues the whole buffer or nothing; past a
 * per-channel limit or a server-wide total it returns #AXL_BUSY, and the
 * channel's writable hook later says when to try again. There is no partial
 * write (a caller fanning one stream out to many clients would have to track
 * an offset per client), no blocking write (a blocked writer on a shared loop
 * stalls every other service on it), and no unbounded queue (a client that
 * stops reading would pin firmware heap without limit). A caller that gets
 * #AXL_BUSY can mark the client behind, stop writing, and repaint it from its
 * own state when writable fires -- a byte stream cannot drop data and stay
 * correct, so the library never drops any. A small **fair-share floor** (see
 * axl_ssh_channel_write()) keeps channels whose peers have stopped reading
 * from starving every other channel of the server-wide total.
 *
 * **Output leaves in write order, stdout and stderr interleaved as written**,
 * in packets of at most 32 KB of data, never more than the peer's window or
 * its maximum packet allows. A request's reply, when the peer asked for one
 * -- `OPEN_CONFIRMATION`, `CHANNEL_SUCCESS` or `CHANNEL_FAILURE` -- is queued
 * the moment its hook returns, ahead of any channel data, and channel data is
 * only ever queued later, from the loop; so anything a hook writes reaches
 * the peer AFTER the reply to the request that hook answered. A request whose
 * peer asked for no reply still runs its hook; nothing is sent for it.
 *
 * **Inbound, a channel is confirmed with a window of 0** and a 32 KB maximum
 * packet, and is granted its #AXL_SSH_CHANNEL_WINDOW_SIZE window -- a
 * `WINDOW_ADJUST` queued right behind the `CHANNEL_SUCCESS` -- only when its
 * shell, exec or subsystem hook accepts. That is sshd's order: data a peer
 * sends before the start is discarded and never reaches the data hook.
 * Bytes are handed to the data hook as they are decrypted; the server holds
 * none for the application. WHEN the peer's window is reopened is the
 * channel's window mode (axl_ssh_channel_set_window_mode()): by default as
 * bytes arrive, so the window never closes and paces nothing; in
 * #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME, only as the application reports bytes
 * consumed (axl_ssh_channel_consumed()), so a reader that falls behind closes
 * the peer's window and the peer stops sending -- sshd's backpressure. Either
 * way a `WINDOW_ADJUST` is sent, from the loop, once the window has fallen
 * more than three maximum packets below its size or below half of it, and
 * some credit is due (OpenSSH's channel_check_window()).
 *
 * **Ending a channel.** axl_ssh_channel_exit() is the ordinary end: it lets
 * the queued output drain, then sends `exit-status`, EOF and CLOSE, in that
 * order -- an order sshd also produces, which every client accepts.
 * axl_ssh_channel_close() is the way out: it discards whatever is queued and
 * sends CLOSE now. They differ on purpose -- a drain waits on the peer's
 * window, and a peer that never opens it would hold an exit open for ever.
 *
 * **When the PEER closes**, the server answers with its own CLOSE at once.
 * Whatever output is still queued is discarded: nothing -- no data, no
 * `exit-status` -- is sent on a channel after the peer's CLOSE, as sshd sends
 * nothing after it; a pending axl_ssh_channel_exit() drain ends there.
 *
 * **When the SERVER closes first** -- axl_ssh_channel_close(), or an exit
 * drain completing -- the channel's handle is retired at once, but its wire
 * channel stays reserved until the peer's own CLOSE arrives. Until then,
 * whatever the peer sent before it saw our CLOSE -- data, EOF,
 * `WINDOW_ADJUST`, requests -- is ignored: data still counts against the
 * window, requests are not answered, and none of it ends the connection (RFC
 * 4254 section 5.3 allows those messages to cross, and sshd ignores them the
 * same way). **The cost:** that reserved channel still counts toward the
 * connection's session cap (axl_ssh_server_set_max_sessions()), so a peer
 * that never answers our CLOSE holds the slot for the connection's life.
 *
 * **What ends the whole connection instead**:
 *   - a channel message naming a channel number the server does not hold --
 *     one it never confirmed, or one whose CLOSE has been both sent and
 *     received (sshd's "referred to nonexistent channel"). The exception is
 *     `WINDOW_ADJUST`, which sshd ignores for a channel it does not hold,
 *     and so does AxlSsh;
 *   - a `WINDOW_ADJUST` that would take the peer's window past 2^32 - 1,
 *     sshd's "adjust overflows remote window";
 *   - data more than 10% past the window the server granted. Up to 10% over
 *     is accepted and delivered, as sshd's grace allows. A single data
 *     packet larger than the 32 KB maximum is dropped WITHOUT counting
 *     against the window, and the connection stays up, also as sshd does;
 *   - on an #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME channel, a data packet that
 *     would take the bytes delivered and not yet consumed past
 *     #AXL_SSH_CHANNEL_UNCONSUMED_MAX. Checked before the data hook sees the
 *     packet, and an empty packet does not reset it -- unlike sshd, whose
 *     grace an empty packet restores, so a peer alternating overruns with
 *     empty packets is never stopped there. A peer that honours the window
 *     never reaches it;
 *   - extended data from the peer after its EOF, as in sshd;
 *   - BEFORE the session starts only: an `exec` command, `subsystem` name
 *     (when the subsystem hook is installed) or `pty-req` terminal name with
 *     a NUL byte anywhere but its last byte, and a second `pty-req` on a
 *     channel that already has a pty (sshd's "you already have a pty"). See
 *     the exec, subsystem and pty hooks for the details sshd shares.
 *     **Once a shell, exec or subsystem has started, every `pty-req`,
 *     `shell`, `exec` and `subsystem` is refused with `CHANNEL_FAILURE`
 *     unparsed** -- so none of these rules can end a started session's
 *     connection -- as sshd dispatches those requests only before the start;
 *   - any channel message that does not parse: a truncated or overlong
 *     `CHANNEL_DATA`, `WINDOW_ADJUST`, EOF, CLOSE or request header, a
 *     malformed `window-change`, and the like -- as sshd ends the connection
 *     on a channel message it cannot parse. The fields of a request that is
 *     refused unparsed (above, and under "Not offered") are never read, so
 *     they cannot end it.
 *
 * **Once axl_ssh_channel_exit() is pending** on a channel, every `pty-req`,
 * `shell`, `exec` and `subsystem` on it is refused with `CHANNEL_FAILURE`
 * unparsed, without calling a hook, as after a start: the channel is ending.
 *
 * **Subsystems are the application's.** The library implements none --
 * `sftp` included -- and runs nothing itself: a `subsystem` request reaches
 * the subsystem hook with the name the peer sent, the hook decides, and the
 * accepted channel then carries the subsystem's byte stream through the
 * ordinary data, eof, writable and close hooks, exactly as an exec's does.
 * Without the hook, every `subsystem` request is refused.
 *
 * **Not offered**, by the server's scope rather than as an oversight: the
 * `env`, `x11-req`, `auth-agent-req@openssh.com`, `signal` and `break`
 * requests -- and any request name not described here -- are refused with
 * `CHANNEL_FAILURE` when the peer asked for a reply, and the channel stays
 * usable; so is `subsystem` when no subsystem hook is installed. There are
 * no channel types but `session`, so no port forwarding; and `exit-signal`
 * is never sent. Terminal modes in a `pty-req` are parsed and ignored.
 *
 * **Open-failure reasons deliberately differ from sshd's.** sshd refuses
 * every session open it will not serve with `SSH_OPEN_CONNECT_FAILED` (2),
 * its session cap included. AxlSsh sends the code RFC 4254 section 5.1
 * defines for each case instead: `SSH_OPEN_RESOURCE_SHORTAGE` (4) past the
 * session cap, so ssh(1) reports "resource shortage" rather than a
 * misleading "connect failed", and `SSH_OPEN_ADMINISTRATIVELY_PROHIBITED`
 * (1) when the application's open hook refuses -- sshd has no such hook, so
 * there is no parity to keep there.
 */

#ifndef AXL_SSH_CHANNEL_H
#define AXL_SSH_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-ssh-server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle for one session channel.
 *
 * An integer, not a pointer, so that a handle kept past its channel's close
 * is refused with #AXL_NOT_FOUND instead of written through freed memory --
 * the same shape as #AxlSshAuthRequest. 0 is never issued. Handles count up
 * from one counter shared by every server in the image, separate from the
 * authentication-request counter, and wrap only after 2^32 - 1 channels in
 * total, so a stale handle -- from this server, or from one freed before this
 * one was created -- cannot name a new channel before then.
 *
 * **Handle 0 is #AXL_NOT_FOUND here, where axl_ssh_auth_complete() calls a
 * zero request #AXL_INVALID. The difference is deliberate:** a
 * zero-initialised "no channel" field in application state then behaves
 * exactly like a channel that has already closed, which is what
 * `axl_ssh_channel_close(server, 0)` should mean. Do not "fix" it toward the
 * authentication precedent.
 *
 * A handle is the application's name for the channel. It is not promised to
 * equal the channel number either side uses on the wire.
 */
typedef uint32_t AxlSshChannel;

/**
 * @brief A terminal's type and size, as the pty and resize hooks receive it.
 *
 * Filled from a `pty-req` (RFC 4254 section 6.2) for the pty hook, and from a
 * `window-change` (section 6.7) for the resize hook, which carries no type,
 * so @a term is NULL there. The struct and everything it points to are valid
 * only during the hook; copy what is needed later. The encoded terminal modes
 * of a `pty-req` are parsed and ignored: the server has no line discipline to
 * apply them to, and the application sees raw input either way.
 */
typedef struct {
    const char *term;       ///< `TERM` value, NUL-terminated copy, e.g.
                            ///< "xterm"; NULL in the resize hook
    uint32_t    cols;       ///< width in characters, as the peer sent it
    uint32_t    rows;       ///< height in rows, as the peer sent it
    uint32_t    width_px;   ///< width in pixels, 0 when the peer does not say
    uint32_t    height_px;  ///< height in pixels, 0 when the peer does not say
} AxlSshPty;

/**
 * @brief Application hooks for session channels.
 *
 * Copied by axl_ssh_server_set_channel_ops(); any hook may be NULL, with the
 * meaning each hook states. Every hook receives the @a ud given to
 * axl_ssh_server_set_channel_ops().
 *
 * **Request hooks answer with their return value.** For `open`, `pty`,
 * `shell`, `exec` and `subsystem`, #AXL_OK accepts and any other value
 * refuses; the reply, when the peer asked for one, is queued the moment the
 * hook returns.
 * A hook cannot answer later -- unlike authentication, nothing here is worth
 * waiting on: a session that needs time to start can accept at once and
 * write when it is ready.
 *
 * **Every string and buffer a hook receives is valid only during that
 * call**: copy what is needed later.
 */
typedef struct {
    /**
     * @brief A peer is opening a `session` channel.
     *
     * Called only on an authenticated connection that is below its session
     * cap (axl_ssh_server_set_max_sessions()); an open past the cap is
     * refused with `SSH_OPEN_RESOURCE_SHORTAGE` without calling this, and
     * the connection stays up.
     *
     * @a principal is the one axl_ssh_auth_complete() adopted for this
     * connection, or NULL when none was given. The server still owns it, and
     * it stays valid until that connection's release_principal, which is
     * called only after the close hook of every channel the connection
     * opened. @a user is the name the connection authenticated as, the
     * peer's bytes (not checked for UTF-8), NUL-terminated.
     *
     * @a ch is live when the hook is called: the hook may attach state with
     * axl_ssh_channel_set_user_data() and may write, and anything written
     * leaves after the `OPEN_CONFIRMATION`. A hook that dispatches the loop
     * can see that change under it: if the connection is dropped meanwhile,
     * every call on @a ch returns #AXL_NOT_FOUND from then on (see the file
     * comment), and -- unless the hook then refuses -- the channel's close
     * hook follows once this hook has returned.
     *
     * Returning anything but #AXL_OK refuses the open with
     * `SSH_OPEN_ADMINISTRATIVELY_PROHIBITED` (see the file comment for why
     * that differs from sshd). A refused channel was never opened: its
     * handle is retired when the hook returns, anything it wrote is
     * discarded, and **its close hook is never called** -- so release
     * anything attached with axl_ssh_channel_set_user_data() before
     * returning the refusal.
     *
     * **Ending the channel from inside this hook.** axl_ssh_channel_close()
     * called here and followed by #AXL_OK sends `OPEN_CONFIRMATION` then
     * CLOSE, and the close hook follows later, as for any close. Followed by
     * a refusal, the close is folded into the refusal: the peer sees only
     * the open failure, and no close hook is called. axl_ssh_channel_exit()
     * here is allowed too, as it is before any shell, exec or subsystem (see
     * it).
     *
     * NULL accepts every open.
     */
    int  (*open)(AxlSshChannel ch, void *principal, const char *user,
                 void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer asks for a pseudo-terminal (`pty-req`).
     *
     * See #AxlSshPty for what @a pty carries and how long it lives. #AXL_OK
     * accepts. Accepting records nothing in the server beyond "this channel
     * has a pty", which is what lets the resize hook fire for it.
     *
     * At most one pty per channel, and only before `shell` or `exec`: a
     * `pty-req` after either has started is refused unparsed, without
     * calling this. Before the start, one on a channel that already has a
     * pty ends the connection, as sshd's "you already have a pty" does, and
     * so does a @a term with a NUL byte anywhere but its last byte; ONE
     * trailing NUL is accepted and dropped, as sshd's string parser accepts
     * it.
     *
     * NULL refuses every pty, which leaves `ssh -tt` working without one.
     * **With no pty hook, every `pty-req` is refused unparsed** -- a
     * malformed terminal name or a second request is then a plain
     * `CHANNEL_FAILURE`, never a disconnect -- mirroring sshd, which checks
     * whether ptys are permitted before it parses the request.
     */
    int  (*pty)(AxlSshChannel ch, const AxlSshPty *pty,
                void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer asks for an interactive shell (`shell`).
     *
     * #AXL_OK starts the session and grants the channel its inbound window
     * (see the file comment). One `shell` or `exec` per channel (RFC 4254
     * section 6.5): a second request of either kind is refused unparsed,
     * without calling a hook. NULL refuses.
     */
    int  (*shell)(AxlSshChannel ch, void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer asks to run one command (`exec`) -- `ssh host cmd`.
     *
     * @a cmd is the command as sent, @a len bytes long, and ALSO
     * NUL-terminated (`cmd[len]` is 0), so it may be used as a C string; it
     * is valid only during the call. @a len may be 0 (`ssh host ''`).
     * Before the session starts, a command with a NUL byte anywhere but its
     * last byte never reaches this hook: it ends the connection, as sshd's
     * parse of the same request does -- whether or not this hook is
     * installed. ONE trailing NUL is accepted, as sshd accepts it, and is not
     * counted in @a len. After the start, every `exec` is refused unparsed.
     *
     * #AXL_OK starts the session and grants the channel its inbound window;
     * the usual shape is to write the command's output and then call
     * axl_ssh_channel_exit() with its status, from inside this hook or later.
     * One `shell` or `exec` per channel, as for the shell hook. NULL refuses.
     */
    int  (*exec)(AxlSshChannel ch, const char *cmd, size_t len,
                 void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer asks for a subsystem (`subsystem`, RFC 4254 section
     *     6.5) -- `sftp`, which scp(1) also uses since OpenSSH 9.0.
     *
     * @a name is the subsystem name as sent, @a len bytes long, and ALSO
     * NUL-terminated (`name[len]` is 0); it is valid only during the call,
     * so copy it to keep it. @a len may be 0. The library recognises no name:
     * the hook alone decides which subsystems exist, and refuses every name
     * it does not serve -- sshd's "subsystem not found" -- by returning
     * anything but #AXL_OK.
     *
     * #AXL_OK starts the session and grants the channel its inbound window,
     * exactly as the exec hook's acceptance does. From then on the channel
     * is the subsystem's: the peer's bytes arrive through the data hook,
     * the subsystem's replies leave through axl_ssh_channel_write() paced by
     * the writable hook, the peer's end of input is the eof hook, and the
     * application ends it with axl_ssh_channel_exit() or
     * axl_ssh_channel_close(). A subsystem protocol frames its own messages
     * inside that byte stream; the data hook's chunks carry no message
     * boundaries.
     *
     * The rules are the exec hook's: one `shell`, `exec` or `subsystem` per
     * channel; after the start, and once axl_ssh_channel_exit() is pending,
     * every `subsystem` is refused unparsed without calling this. Before the
     * start, a name with a NUL byte anywhere but its last byte, or bytes
     * after the name, ends the connection, as sshd's parse of the same
     * request does; ONE trailing NUL is accepted and not counted in @a len.
     *
     * **NULL refuses every `subsystem` unparsed** -- a malformed one is then
     * a plain `CHANNEL_FAILURE`, never a disconnect -- so an application
     * that installs no subsystem hook sees exactly the behaviour of a server
     * built before this hook existed.
     */
    int  (*subsystem)(AxlSshChannel ch, const char *name, size_t len,
                      void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer's terminal changed size (`window-change`).
     *
     * @a size carries the new character and pixel sizes; its @a term is
     * NULL. Called only for a channel whose pty hook accepted a pty; on any
     * other channel the hook is not called. It has no return value because
     * RFC 4254 section 6.7 sends the request without asking for a reply; a
     * peer that asks anyway is answered `CHANNEL_SUCCESS`, pty or not, as
     * sshd answers it. NULL ignores every resize.
     */
    void (*resize)(AxlSshChannel ch, const AxlSshPty *size,
                   void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief Data from the peer (`CHANNEL_DATA`), in order.
     *
     * **@a buf points INTO the connection's payload buffer -- it is not a
     * copy** -- and is valid only during the call. That is why the
     * connection parses nothing while any hook of any of its channels is on
     * the stack: a hook that dispatches the loop would otherwise have the
     * next packet decrypted over the bytes it is still reading. @a len is
     * never 0.
     *
     * Called only on a started session -- after the shell, exec or subsystem
     * hook accepted -- because only then does the channel have a window;
     * data a peer sends before that is discarded (see the file comment). Data
     * sent right behind the start arrives here after the start hook returned,
     * in the order it was sent.
     *
     * Extended data from the peer (stderr, which no client sends), data
     * after the peer's EOF, and data while no data hook is installed are
     * discarded and credited back to the peer by the server, in either
     * window mode. Every byte this hook receives is credited according to
     * the channel's window mode: at once by default, or, in
     * #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME, only when the application reports
     * it with axl_ssh_channel_consumed(). @a buf is still valid only during
     * this call -- copy what is consumed later.
     */
    void (*data)(AxlSshChannel ch, const uint8_t *buf, size_t len,
                 void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The peer will send no more data (`CHANNEL_EOF`).
     *
     * Called at most once, and no data hook follows it. The channel stays
     * open and writable -- `printf 'a\n' | ssh host cat` relies on that: the
     * command sees end of input, writes the rest of its output and exits.
     *
     * Data the peer sends after its EOF anyway is counted against the window
     * and discarded, and the connection stays up; extended data after its
     * EOF ends the connection. Both are sshd's rules. NULL ignores the EOF.
     */
    void (*eof)(AxlSshChannel ch, void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief A channel refused with #AXL_BUSY may write again.
     *
     * **Edge-triggered.** It fires once per refusal and never unprompted: a
     * channel that was never refused never sees it, and any number of
     * refusals before it fires are owed ONE call. It fires only once the
     * limit that refused the write has eased AND the refused write would
     * now FIT -- "below half" alone can already be true at the refusal (a
     * 200 KB write onto 100 KB queued under a 256 KB limit), and firing then
     * would hand the application a writable with nothing changed and spin
     * its retry. axl_ssh_channel_write() says which limit a refusal is on.
     * With L the LARGEST length refused since the last writable, Q the
     * channel's queued bytes, and T the server's queued bytes in all:
     *   - refused on the channel's own limit: once Q is below half that
     *     limit AND Q + L fits within it;
     *   - refused on the server-wide total: once T is below half the total
     *     AND T + L fits within it -- OR, if the channel's queue held bytes
     *     at ANY refusal on the total since the last writable, once that
     *     queue has drained to empty, whichever comes first. The drain
     *     condition is what makes the fair-share floor reachable: an empty
     *     channel may always queue a write of up to the floor, so a channel
     *     refused on a total that stalled peers are holding still learns it
     *     may write again. It needs a real drain since a refusal: when every
     *     refusal on the total found the queue ALREADY empty -- each write
     *     was itself larger than the floor -- the writable waits for the
     *     total alone, because an empty queue cannot drain further. Split
     *     such output into floor-sized writes instead (see
     *     axl_ssh_channel_write());
     *   - refused on both limits since the last writable: both conditions.
     *
     * Every condition is reachable, because a refused L never exceeds the
     * per-channel limit, which never exceeds the total (a longer write is
     * #AXL_ERR, not #AXL_BUSY). **The promise: when writable fires, a retry
     * of the largest refused write succeeds unless other writes intervene
     * -- except a retry larger than the floor that an emptied queue
     * released**, which the floor admits only up to its own size.
     *
     * It is called from the loop, **never from inside
     * axl_ssh_channel_write()** or any other `axl_ssh_channel_*` call, so
     * writing from it cannot recurse into it. A write it makes may be
     * refused again, which owes another call. Nothing is owed once the
     * channel has exited (axl_ssh_channel_exit()) or closed. NULL means the
     * application learns nothing, and must retry on its own schedule.
     */
    void (*writable)(AxlSshChannel ch, void *ud) AXL_CB_NOEXCEPT;
    /**
     * @brief The channel has closed.
     *
     * **Called exactly once for every channel that was opened, whatever
     * ended it**: the peer's CLOSE; the peer answering the server's own CLOSE
     * (after axl_ssh_channel_close(), or once an axl_ssh_channel_exit()
     * drain completes); the connection dropping for any reason; or
     * axl_ssh_server_free(). Never for an open the open hook refused.
     *
     * **@a chan_data is the pointer last attached with
     * axl_ssh_channel_set_user_data()** (NULL when none was), handed over so
     * the hook can release it: it is the hook's to free. The pattern is
     * release_principal's -- a hook whose job is cleanup receives the thing
     * to clean up -- and it is why the handle can be retired first without
     * stranding the application's state.
     *
     * **@a ch is already retired when this runs**: every call on it, from
     * inside this hook or afterwards, returns #AXL_NOT_FOUND and does
     * nothing, and the getters return NULL -- axl_ssh_channel_user_data()
     * included; @a chan_data is how the hook reaches that pointer. It is
     * never called while another hook of the same channel is on the stack:
     * a close that happens under one -- axl_ssh_channel_close() from inside
     * the data hook, say -- is delivered when that hook returns. It is never
     * called from inside an `axl_ssh_channel_*` call; axl_ssh_server_free()
     * is the one call that makes it directly (see
     * axl_ssh_server_set_channel_ops()).
     *
     * For every channel of a connection, this runs before that connection's
     * release_principal, so a principal a channel holds is still valid here.
     * NULL means channels need no cleanup.
     */
    void (*close)(AxlSshChannel ch, void *chan_data,
                  void *ud) AXL_CB_NOEXCEPT;
} AxlSshChannelOps;

/**
 * @brief Default number of session channels one connection may hold open --
 *     sshd's `MaxSessions` default.
 *
 * `ssh -o ControlMaster` is what reaches it: every session multiplexed
 * through one master is a channel on one connection.
 */
#define AXL_SSH_MAX_SESSIONS_DEFAULT  10u

/**
 * @brief Largest cap axl_ssh_server_set_max_sessions() accepts.
 *
 * The size of each connection's channel table, fixed at compile time and
 * equal to the default, so the setter can only LOWER the cap. Raising it is
 * a rebuild, deliberately: the table is per connection, and at the
 * sixteen-connection cap (#AXL_SSH_MAX_STARTUPS_MAX) ten channels each is
 * already 160 channels the server-wide write total
 * (#AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT) has to be shared between. It is
 * also the divisor of the fair-share floor (axl_ssh_channel_write()).
 */
#define AXL_SSH_MAX_SESSIONS_MAX  10u

/**
 * @brief Default bytes one channel may hold queued before a write is refused
 *     with #AXL_BUSY -- 256 KB.
 *
 * Counts bytes accepted by axl_ssh_channel_write() and
 * axl_ssh_channel_write_stderr() that have not yet been handed to the
 * connection's transmit queue, stdout and stderr together.
 */
#define AXL_SSH_CHANNEL_WRITE_LIMIT_DEFAULT  (256u * 1024u)

/**
 * @brief Default bytes ALL channels of one server may hold queued before a
 *     write is refused with #AXL_BUSY -- 4 MB.
 *
 * Exists because the per-channel limit alone does not bound the heap: 16
 * connections x 10 channels x 256 KB is 40 MB of firmware memory. With the
 * fair-share floor (axl_ssh_channel_write()) the queued bytes can reach at
 * most TWICE this total -- still bounded, and stated.
 */
#define AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT  (4u * 1024u * 1024u)

/**
 * @brief Largest server-wide total axl_ssh_server_set_channel_write_limit()
 *     accepts -- 64 MB, sixteen times the default.
 *
 * Far past any heap a firmware SSH server should commit to queued output,
 * and where the likeliest slip lands: a size multiplied by 1024 once too
 * often. Refused rather than accepted, like the other setters' maxima. Note
 * that the fair-share floor (axl_ssh_channel_write()) lets queued bytes
 * reach up to TWICE the total, so this cap allows up to 128 MB queued.
 */
#define AXL_SSH_CHANNEL_WRITE_TOTAL_MAX  (64u * 1024u * 1024u)

/**
 * @brief The inbound window a channel is granted when its session starts --
 *     2 MB, OpenSSH's session default (`CHAN_SES_WINDOW_DEFAULT`).
 *
 * The most the peer may have sent and not yet been credited back. Fixed:
 * sshd has no setting for it either.
 */
#define AXL_SSH_CHANNEL_WINDOW_SIZE  (2u * 1024u * 1024u)

/**
 * @brief Most bytes an #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME channel can have
 *     delivered to its data hook and not yet reported consumed.
 *
 * The window plus sshd's 10% grace for a peer that overruns it. A buffer of
 * this size holds everything such a channel can ever be owed: a peer that
 * would pass it is disconnected before the data hook sees the packet that
 * would cross it (see the file comment).
 */
#define AXL_SSH_CHANNEL_UNCONSUMED_MAX \
    (AXL_SSH_CHANNEL_WINDOW_SIZE + AXL_SSH_CHANNEL_WINDOW_SIZE / 10u)

/**
 * @brief When a channel's inbound window is credited back to the peer.
 *
 * See axl_ssh_channel_set_window_mode().
 */
typedef enum {
    /** The default: every byte is credited back as it reaches the data
        hook, so the window never closes and paces nothing -- the peer may
        keep sending whatever the application does with the bytes. */
    AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL = 0,
    /** A byte handed to the data hook is credited back only once the
        application reports it with axl_ssh_channel_consumed(), so a reader
        that falls behind closes the peer's window -- sshd's backpressure,
        where a session whose process stops reading its pipe stops its
        client. */
    AXL_SSH_CHANNEL_WINDOW_ON_CONSUME = 1
} AxlSshChannelWindowMode;

/**
 * @brief Install session-channel hooks -- the opt-in that makes the server
 *     accept `session` channels at all.
 *
 * Copies @a ops, so the caller's struct need not outlive the call. NULL
 * removes every hook and restores the server's behaviour before any were
 * installed: every `CHANNEL_OPEN` refused with
 * `SSH_OPEN_UNKNOWN_CHANNEL_TYPE`. A struct whose hooks are all NULL is NOT
 * that: channels then open, and every `pty`, `shell`, `exec` and `subsystem`
 * on them is refused.
 *
 * Call before axl_ssh_server_listen() -- like axl_ssh_server_set_auth_ops(),
 * hooks do not change under channels already open, so no channel can be
 * opened by one set of hooks and closed by another.
 *
 * **@a ud must stay valid until the last close hook has run**, which can be
 * AFTER axl_ssh_server_free() returns: free drops every connection and calls
 * the close hook of each of their channels before it returns, except for a
 * connection with a hook of its own on the stack at that moment -- an
 * authentication hook, or any of its channels' hooks -- whose channels are
 * closed from the loop once that hook returns, as axl_ssh_server_free()
 * already documents for release_principal. Each close comes before that
 * connection's release_principal, as everywhere else.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server; #AXL_ERR if the server
 *     is already listening, or not live -- already freed, or being freed by
 *     a call still on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_channel_ops(
    AxlSshServer           *server,  ///< server from axl_ssh_server_new()
    const AxlSshChannelOps *ops,     ///< hooks to copy, or NULL to remove them
    void                   *ud       ///< passed to every hook
);

/**
 * @brief Set how many session channels one connection may hold open --
 *     sshd's `MaxSessions`.
 *
 * Replaces #AXL_SSH_MAX_SESSIONS_DEFAULT. A channel counts from its open
 * until its close hook has been called -- which, for a channel the server
 * closed first, is not until the peer's CLOSE arrives (see the file
 * comment). An open past the cap is refused with
 * `SSH_OPEN_RESOURCE_SHORTAGE` and the connection stays up. To offer no
 * sessions at all, install no channel hooks; 0 is not that, and is refused.
 *
 * **May be called at any time from the server's loop**, including while it
 * is listening and from a hook. It applies to connections accepted
 * afterwards; a connection already open keeps the cap it was accepted with,
 * as axl_ssh_server_set_max_password_failures() does and for sshd's reason:
 * each of its connections runs under the configuration in force when it
 * arrived. Lowering it therefore closes nothing.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server, @a count 0, or
 *     @a count over #AXL_SSH_MAX_SESSIONS_MAX; #AXL_ERR if the server is not
 *     live -- already freed, or being freed by a call still on the stack
 *     somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_max_sessions(
    AxlSshServer *server,  ///< server from axl_ssh_server_new()
    uint32_t      count    ///< channels one connection may hold open
);

/**
 * @brief Set how much output may be queued, per channel and server-wide,
 *     before a write is refused with #AXL_BUSY.
 *
 * Replaces #AXL_SSH_CHANNEL_WRITE_LIMIT_DEFAULT and
 * #AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT. Both count bytes the application has
 * written and the server has not yet handed to a connection's transmit
 * queue; see axl_ssh_channel_write() for how a write is judged against them,
 * including the fair-share floor derived from @a server_total, and the
 * writable hook for when a refused channel hears it may write again.
 *
 * @a per_channel is also the largest write that can EVER succeed: a single
 * write longer than it is refused with #AXL_ERR, not #AXL_BUSY.
 *
 * Call before axl_ssh_server_listen(). These size the server's heap use
 * rather than tune a running policy, and changing them under queued output
 * would move the thresholds a refused channel is already waiting on.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server, either value 0,
 *     @a per_channel above @a server_total, or @a server_total above
 *     #AXL_SSH_CHANNEL_WRITE_TOTAL_MAX; #AXL_ERR if the server is already
 *     listening, or not live -- already freed, or being freed by a call
 *     still on the stack somewhere in this dispatch.
 */
AXL_WARN_UNUSED int
axl_ssh_server_set_channel_write_limit(
    AxlSshServer *server,       ///< server from axl_ssh_server_new()
    size_t        per_channel,  ///< bytes one channel may hold queued
    size_t        server_total  ///< bytes all of the server's channels may
                                ///< hold queued together
);

/**
 * @brief Queue output on a channel's stdout (`CHANNEL_DATA`).
 *
 * **All or nothing.** The whole of @a buf is copied into the channel's
 * queue, or none of it is. @a buf is the caller's again when this returns
 * either way. A write of @a len bytes, onto a channel already holding Q
 * bytes, on a server whose channels hold T bytes in all, is judged in this
 * order:
 *   1. **The channel's own limit** (axl_ssh_server_set_channel_write_limit()
 *      @a per_channel): Q + @a len above it is #AXL_BUSY, a refusal ON THE
 *      CHANNEL LIMIT.
 *   2. **The fair-share floor:** Q + @a len at or below
 *      `server_total / (AXL_SSH_MAX_STARTUPS_MAX * AXL_SSH_MAX_SESSIONS_MAX)`
 *      -- 26,214 bytes at the defaults, the total divided by the most
 *      channels the server can ever hold -- is accepted WITHOUT consulting
 *      the total, even when the total is already reached. A total below
 *      160 bytes makes the floor 0, which means NO floor: every write past
 *      the channel limit check is then judged against the total alone. That
 *      is allowed, not refused -- it is the strict bound some callers want.
 *   3. **The server-wide total:** otherwise, T + @a len above
 *      @a server_total is #AXL_BUSY, a refusal ON THE TOTAL.
 *
 * Why the floor: without it, channels whose peers have stopped reading --
 * every one full to its own limit -- would hold the whole total, and every
 * healthy channel's write would be refused for as long as they stall,
 * which is liveness handed to whoever stalls. With it, each channel keeps a
 * floor's worth of room per round trip regardless. The bytes the floor
 * admits count toward T like any others. A channel the floor admits holds
 * at most one floor's worth, and the floor times the most channels the
 * server can hold is @a server_total, so T never exceeds twice
 * @a server_total.
 *
 * **Under total pressure, only writes no larger than the floor keep
 * flowing, so split larger output -- a full-screen repaint, say -- into
 * floor-sized writes.** A single write larger than the floor, refused on
 * the total with the channel's queue empty, is owed its writable only once
 * the total falls below half and the write fits, which stalled peers can
 * postpone indefinitely; floor-sized pieces are accepted as soon as the
 * channel's own queue drains.
 *
 * Every #AXL_BUSY queues nothing and owes the channel ONE call of its
 * writable hook, which fires only once a retry of the refused write would
 * fit -- with one exception: a refusal on the total that found the
 * channel's queue non-empty is also released when that queue drains to
 * empty, and a retry then fits only if it is no larger than the floor (see
 * the writable hook for the exact conditions).
 *
 * The bytes leave from the loop, never inside this call: in write order with
 * axl_ssh_channel_write_stderr()'s, in packets of at most 32 KB, never past
 * the peer's window. A write made inside a hook leaves after the reply to
 * that hook's request. Writing is allowed from the open hook onward, and
 * after the peer's EOF -- that only ends the peer's direction.
 *
 * @return #AXL_OK when queued, and for @a len 0, which queues and sends
 *     nothing (@a buf may then be NULL); #AXL_BUSY as above; #AXL_ERR when
 *     @a len is larger than the per-channel limit -- it can never succeed, so
 *     #AXL_BUSY would leave a caller waiting for a writable that cannot help
 *     -- or after axl_ssh_channel_exit() on this channel, which ended its
 *     output; #AXL_NO_RESOURCES when the bytes cannot be copied;
 *     #AXL_INVALID on a NULL @a server, or a NULL @a buf with @a len above
 *     0; #AXL_NOT_FOUND when @a ch is not an open channel of a live server,
 *     checked before everything but the NULL @a server. Only #AXL_BUSY owes
 *     a writable.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_write(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to write to
    const void    *buf,     ///< bytes to queue (copied)
    size_t         len      ///< how many
);

/**
 * @brief Queue output on a channel's stderr (`CHANNEL_EXTENDED_DATA`, type
 *     `SSH_EXTENDED_DATA_STDERR`).
 *
 * Exactly axl_ssh_channel_write() -- the same limits, floor and total,
 * counted together with stdout, the same refusals and the same writable hook
 * -- except for the stream. The two streams share one queue and leave in the
 * order written, so output interleaved on a terminal stays interleaved;
 * `ssh host cmd` without a pty delivers this to the client's stderr.
 *
 * @return as axl_ssh_channel_write().
 */
AXL_WARN_UNUSED int
axl_ssh_channel_write_stderr(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to write to
    const void    *buf,     ///< bytes to queue (copied)
    size_t         len      ///< how many
);

/**
 * @brief End a channel the ordinary way: drain its output, then report an
 *     exit status.
 *
 * Returns at once. The queued output still leaves, and only then does the
 * server send `exit-status` with @a status, EOF and CLOSE, in that order --
 * an order sshd also produces, and the one that lets `ssh host cmd` exit
 * with @a status after printing everything. Allowed whether or not the peer
 * has sent its EOF, and before any shell, exec or subsystem has started --
 * from the open hook, say -- in which case the same three messages go out.
 *
 * After it, the channel is still open until the server's CLOSE is sent: the
 * getters and axl_ssh_channel_set_user_data() still work, but every write is
 * refused with #AXL_ERR and no writable hook is owed. Once the drain
 * completes and CLOSE is sent, the handle is retired, as after
 * axl_ssh_channel_close(). The drain waits on the peer's window, so a peer
 * that never opens it holds the channel open; axl_ssh_channel_close() is the
 * way out of that. The peer's own CLOSE ends the drain early, discarding
 * what is left (see the file comment). The close hook follows when the
 * peer's CLOSE arrives or the connection drops.
 *
 * @return #AXL_OK; #AXL_ERR when this channel has already exited;
 *     #AXL_INVALID on a NULL @a server; #AXL_NOT_FOUND when @a ch is not an
 *     open channel of a live server.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_exit(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to end
    uint32_t       status   ///< exit status the client reports
);

/**
 * @brief Close a channel now, discarding its queued output.
 *
 * Sends CLOSE at once -- the application's way out, including from an
 * axl_ssh_channel_exit() whose drain the peer's window is holding. Nothing
 * still queued is sent, no exit status is sent unless one already was, and
 * the handle is retired when this returns: every later call on it returns
 * #AXL_NOT_FOUND. The channel's wire slot stays reserved until the peer's
 * CLOSE, counting toward the session cap meanwhile (see the file comment).
 * The close hook still follows, exactly once -- when the peer answers with
 * its own CLOSE or the connection drops, and never inside this call. Called
 * from inside one of the channel's own hooks, the close hook waits for that
 * hook to return; called from inside the open hook, see that hook.
 *
 * Not `AXL_WARN_UNUSED`: its one non-argument failure, #AXL_NOT_FOUND, means
 * the channel is already closed, which is what the caller wanted.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server; #AXL_NOT_FOUND when
 *     @a ch is not an open channel of a live server.
 */
int
axl_ssh_channel_close(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch       ///< channel to close
);

/**
 * @brief Choose when a channel's inbound window is credited back: as bytes
 *     arrive (the default), or only as the application consumes them.
 *
 * Every channel starts in #AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL. Allowed at any
 * time from the server's loop, and from every hook. **Set it in the shell,
 * exec or subsystem hook and no byte is ever delivered under the other
 * mode**: the window is granted only when that hook accepts.
 *
 *   - **Switching to #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME** applies to bytes
 *     delivered from then on -- NOT to the buffer a data hook is handling
 *     when it switches, which was credited before the hook was called.
 *   - **Switching back to #AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL** credits every
 *     unconsumed byte at once. That is how an application abandoning the
 *     stream stops owing reports; a later axl_ssh_channel_consumed() for
 *     those bytes is #AXL_INVALID.
 *
 * **Choosing #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME commits the application to
 * two things.** It holds up to #AXL_SSH_CHANNEL_UNCONSUMED_MAX bytes for the
 * channel itself -- the server holds none, and the data hook's buffer is
 * valid only during its call. And it eventually reports, or switches back
 * for, every byte it was handed -- bytes it chose to drop included. A
 * channel that stops consuming holds its peer for ever, as an exit drain
 * waits on the peer's window; axl_ssh_channel_exit() and
 * axl_ssh_channel_close() still work, and neither credits anything. **A
 * consumer that waits for a whole message before consuming any of it
 * deadlocks if a message can be larger than #AXL_SSH_CHANNEL_WINDOW_SIZE**:
 * the peer cannot send the rest until something is consumed.
 *
 * Sends nothing itself: a `WINDOW_ADJUST` the switch makes due is sent from
 * the loop. Runs no hook.
 *
 * @return #AXL_OK, also when the channel is already in @a mode;
 *     #AXL_INVALID on a NULL @a server, or a @a mode that is not one of the
 *     #AxlSshChannelWindowMode values; #AXL_NOT_FOUND when @a ch is not an
 *     open channel of a live server, checked before @a mode.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_set_window_mode(
    AxlSshServer            *server,  ///< server the channel belongs to
    AxlSshChannel            ch,      ///< channel to set
    AxlSshChannelWindowMode  mode     ///< when inbound bytes are credited
);

/**
 * @brief Report that the application is done with @a n bytes its data hook
 *     was handed, so the peer may send that many more.
 *
 * For an #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME channel. @a n is a COUNT, not a
 * position: bytes may be consumed in any order, and in as many calls as
 * suits the application. **All or nothing**: an @a n larger than the
 * channel's unconsumed bytes (axl_ssh_channel_unconsumed()) credits nothing
 * and is #AXL_INVALID -- as is any @a n above 0 in
 * #AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL, where nothing is ever unconsumed.
 *
 * Allowed from every hook -- the data hook included, for bytes of its own
 * buffer, which then credits the same bytes as
 * #AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL would, though its `WINDOW_ADJUST`
 * leaves from the loop rather than as each packet is parsed, so the peer
 * may see fewer, larger adjusts -- from the loop afterwards, after the
 * peer's EOF, and while an axl_ssh_channel_exit() drain is pending.
 *
 * Sends nothing itself: the `WINDOW_ADJUST` is sent from the loop, under the
 * rule in the file comment, so many calls between two loop iterations cost
 * one packet. The peer is never granted more than
 * #AXL_SSH_CHANNEL_WINDOW_SIZE in all. Runs no hook.
 *
 * @return #AXL_OK, also for @a n 0, which does nothing; #AXL_INVALID on a
 *     NULL @a server, or when @a n is more than the channel's unconsumed
 *     bytes; #AXL_NOT_FOUND when @a ch is not an open channel of a live
 *     server -- including once the server has sent its CLOSE -- checked
 *     before @a n.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_consumed(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel the bytes arrived on
    size_t         n        ///< bytes the application is done with
);

/**
 * @brief How many bytes a channel's data hook was handed that the
 *     application has not yet reported consumed.
 *
 * Always 0 in #AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL, and never more than
 * #AXL_SSH_CHANNEL_UNCONSUMED_MAX. An application that is idle -- every byte
 * it was handed processed -- reads 0 here; a larger count is input it is
 * still holding, or has dropped without reporting.
 *
 * Its callers today are AxlSsh's own tests -- the unit tests, and the
 * ssh-server-selftest fixture's "hoard" exec, which checks a channel reads
 * 0 when idle and reports what is still held at EOF. An `int` and an
 * out-parameter rather than a `size_t` return, because 0 is the answer such
 * a check is looking for: a getter that also returned 0 for a channel that
 * does not exist would report "nothing owed" about a channel it never
 * found.
 *
 * @return #AXL_OK, with @a out set; #AXL_INVALID on a NULL @a server, or,
 *     for an open channel, a NULL @a out; #AXL_NOT_FOUND when @a ch is not
 *     an open channel of a live server. On every failure @a out is left
 *     untouched.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_unconsumed(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to ask about
    size_t        *out      ///< receives the unconsumed byte count
);

/**
 * @brief Attach one pointer of application state to a channel.
 *
 * Replaces whatever was attached before. The server never reads, copies or
 * frees @a p; it hands the last value to the close hook as its
 * @a chan_data, which is where to release it. NULL is a valid value.
 *
 * @return #AXL_OK; #AXL_INVALID on a NULL @a server; #AXL_NOT_FOUND when
 *     @a ch is not an open channel of a live server.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_set_user_data(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to attach @a p to
    void          *p        ///< state to attach, or NULL
);

/**
 * @brief The pointer last attached with axl_ssh_channel_set_user_data().
 *
 * Inside the close hook, the channel is already retired and this returns
 * NULL; the hook receives the pointer as its @a chan_data instead.
 *
 * @return the pointer, or NULL when none was attached, @a server is NULL, or
 *     @a ch is not an open channel of a live server. Those NULLs cannot be
 *     told apart here; axl_ssh_channel_set_user_data() can.
 */
void *
axl_ssh_channel_user_data(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch       ///< channel to ask about
);

/**
 * @brief The user name the channel's connection authenticated as.
 *
 * The same name the open hook received: NUL-terminated, the peer's bytes and
 * not checked for UTF-8. Held by the connection and valid until the handle
 * is retired (see axl_ssh_channel_close()) -- which for a channel the server
 * closed first is well before its close hook. Copy it to keep it longer.
 *
 * @return the name, or NULL on a NULL @a server or when @a ch is not an
 *     open channel of a live server.
 */
const char *
axl_ssh_channel_user(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch       ///< channel to ask about
);

/**
 * @brief The principal the channel's connection authenticated with.
 *
 * The pointer axl_ssh_auth_complete() adopted, still owned by the server and
 * valid until that connection's release_principal, which follows the close
 * hook of every channel on it.
 *
 * @return the principal, or NULL when the login carried none, @a server is
 *     NULL, or @a ch is not an open channel of a live server. Those NULLs
 *     cannot be told apart here; axl_ssh_channel_user() can.
 */
void *
axl_ssh_channel_principal(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch       ///< channel to ask about
);

/**
 * @brief The address and port the channel's connection came from.
 *
 * The values read when the connection was accepted -- the same ones the
 * authentication audit log prints as `from A port P`.
 *
 * @return #AXL_OK, with @a addr holding the IPv4 address in network order
 *     (`addr[0]` is the first dotted-decimal octet) and @a port the source
 *     port; #AXL_ERR when the connection's address could not be read at
 *     accept; #AXL_INVALID on a NULL @a server, or, for an open channel, a
 *     NULL @a addr or @a port; #AXL_NOT_FOUND when @a ch is not an open
 *     channel of a live server. On every failure @a addr and @a port are
 *     left untouched.
 */
AXL_WARN_UNUSED int
axl_ssh_channel_peer(
    AxlSshServer  *server,  ///< server the channel belongs to
    AxlSshChannel  ch,      ///< channel to ask about
    uint8_t        addr[4], ///< receives the source IPv4 address
    uint16_t      *port     ///< receives the source TCP port
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_SSH_CHANNEL_H */
