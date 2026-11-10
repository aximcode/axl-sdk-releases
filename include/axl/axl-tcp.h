/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tcp.h
 *
 * TCP socket abstraction. Blocking and async (event-driven) APIs.
 * Async functions integrate with AxlLoop for non-blocking I/O.
 */

#ifndef AXL_TCP_H
#define AXL_TCP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-macros.h>
#include <axl/axl-inet-address.h>   /* AxlIPv4Address (used below); self-contained */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;
typedef struct AxlTcp AxlTcp;
typedef struct AxlCancellable AxlCancellable;

/**
 * @brief How to tear a connection or listener down.
 *
 * Selects the close semantics for @ref axl_tcp_close (and the layers built on
 * it — @ref axl_socket_free, @ref axl_http_server_free):
 *
 * - `AXL_TEARDOWN_GRACEFUL`: orderly close (TCP FIN), then finalizes —
 *   deferred to a later loop tick when a loop is running, so the port is
 *   released once both FINs are exchanged, or about 500 ms after the peer
 *   ACKs a FIN it does not answer (not after TIME_WAIT). The correct, polite
 *   default for ordinary connection drops. It does NOT wait out a pending
 *   send, and a close that would DROP accepted bytes -- a send still queued,
 *   one not yet wholly handed to the firmware, or a stream already cut
 *   short -- is reset instead, so the peer cannot mistake a cut stream for a
 *   complete one (see @ref axl_tcp_close). A send the firmware already holds
 *   in full still goes out ahead of the FIN. To be sure the last bytes are
 *   delivered, close from their send callback.
 * - `AXL_TEARDOWN_RESET`: abortive close (TCP RST). **Discards un-ACKed
 *   in-flight data** and finalizes **synchronously and loop-free** — including,
 *   for a listener, draining the firmware accept backlog and finalizing its
 *   connections' pending deferred closes — so the bound port is immediately
 *   reusable with **no event-loop pumping**. For an in-place server upgrade /
 *   port hand-off where the connections are being discarded anyway. The peer
 *   sees a connection reset.
 */
typedef enum {
    AXL_TEARDOWN_GRACEFUL = 0,   ///< orderly FIN close (default)
    AXL_TEARDOWN_RESET           ///< abortive RST close, port free on return
} AxlTeardown;

// ---------------------------------------------------------------------------
// Blocking TCP API
//
// The blocking wrappers (axl_tcp_connect, axl_tcp_accept,
// axl_tcp_send, axl_tcp_recv) are thin sync shells over the async
// API below. Each call creates its OWN temporary AxlLoop, submits
// the underlying async op against that loop, runs the loop until
// the op completes or times out, and frees the loop. They do NOT
// participate in the caller's event loop.
//
// Implications:
//   - Calling a blocking wrapper from inside a callback that is
//     itself running on an event loop spins up a NESTED loop. The
//     outer loop is effectively paused for the duration — other
//     sources on it do not fire until the blocking call returns.
//     For event-driven apps prefer the _async variants.
//   - Each call pays the cost of loop allocation + teardown. Not a
//     problem for infrequent operations; noticeable in a hot path.
// ---------------------------------------------------------------------------

/**
 * @brief Connect to a remote host via TCP4.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
AXL_WARN_UNUSED int
axl_tcp_connect(
    const char *host,      ///< IPv4 address string or hostname (DNS resolved)
    uint16_t   port,       ///< remote port number
    AxlTcp     **out_sock  ///< receives the connected socket handle
);

/**
 * @brief Connect to a remote host via TCP4, with explicit interface selection.
 *
 * Like axl_tcp_connect but adds optional pinning to a specific
 * local interface by station IP. When @p source_ip is non-NULL and
 * non-zero, the connect uses only the network interface whose IP4
 * config matches that station address — useful when the host has
 * multiple NICs and the destination is reachable via a specific one
 * (e.g. an in-band BMC USB-NIC at 169.254.1.0/24).
 *
 * Pass @p source_ip = NULL or {0,0,0,0} for the auto-pick path:
 *   1. skip interfaces whose station IP is 0.0.0.0
 *   2. prefer an interface whose subnet contains the destination
 *   3. fall back to the first valid (non-zero) interface
 *
 * @return AXL_OK on success, AXL_ERR on failure (including "no
 *     interface matches @p source_ip" when forced).
 */
AXL_WARN_UNUSED int
axl_tcp_connect_via(
    const char            *host,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlTcp               **out_sock
);

/**
 * @brief Connect via TCP4 with an explicit connect-phase timeout.
 *
 * The timeout-aware form of axl_tcp_connect_via: it bounds the SYN /
 * handshake wait with @p connect_timeout_ms instead of the fixed 10 s
 * default the simpler entry points use. A consumer talking to an
 * operator-supplied endpoint (a webhook URL, a REST host) uses this so an
 * unreachable target — a silently-dropped SYN — fails fast instead of
 * stalling the caller's event loop for ~10 s. axl_tcp_connect and
 * axl_tcp_connect_via are exactly this call with @p connect_timeout_ms = 0.
 *
 * @p connect_timeout_ms == 0 keeps the 10 s default (matching the
 * send/recv convention where 0 means "the default", not "forever" — a
 * connect with no deadline is rarely what a caller wants). The timeout is
 * an AXL-side deadline: it fires on the loop and cancels the connect, so it
 * bounds the wait regardless of how the underlying transport behaves.
 *
 * @return AXL_OK on success; AXL_ERR on failure, including a timeout
 *     (the SYN was not answered within @p connect_timeout_ms) and "no
 *     interface matches @p source_ip" when forced.
 */
AXL_WARN_UNUSED int
axl_tcp_connect_timeout(
    const char            *host,               ///< IPv4 string or hostname (DNS resolved)
    uint16_t               port,               ///< remote port number
    const AxlIPv4Address  *source_ip,          ///< pin to this local station IP, or NULL to auto-pick
    size_t                 connect_timeout_ms, ///< connect deadline in ms (0 = 10 s default)
    AxlTcp               **out_sock            ///< receives the connected socket handle
);

/**
 * @brief Create a TCP4 listener on the given port.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
AXL_WARN_UNUSED int
axl_tcp_listen(
    uint16_t port,           ///< local port to listen on
    AxlTcp   **out_listener  ///< receives the listener handle
);

/**
 * @brief Create a TCP4 listener pinned to a specific local interface.
 *
 * Like axl_tcp_listen but takes an optional source IP that
 * selects which network interface to bind on a multi-NIC host. When
 * @p source_ip is NULL or all-zeros, falls through to the auto-pick
 * path used by axl_tcp_listen.
 *
 * @return AXL_OK on success, AXL_ERR on failure (including "no
 *     interface has station IP @p source_ip" when forced).
 */
AXL_WARN_UNUSED int
axl_tcp_listen_via(
    uint16_t              port,
    const AxlIPv4Address *source_ip,
    AxlTcp              **out_listener
);

/**
 * @brief Accept one pending connection on a listener.
 *
 * Unlike axl_tcp_send/_recv where timeout_ms=0 defaults to 10s, a
 * sync accept's semantic role is "wait for an incoming client,"
 * so timeout_ms=0 means wait forever (no timeout source). Pass a
 * positive value to bound the wait; Ctrl-C ends the wait either
 * way via the loop's break observation.
 *
 * @return AXL_OK on success, AXL_ERR on failure. timeout, or cancel.
 */
AXL_WARN_UNUSED int
axl_tcp_accept(
    AxlTcp  *listener,    ///< listener from axl_tcp_listen
    AxlTcp  **out_client, ///< receives the accepted client socket
    size_t  timeout_ms    ///< timeout in ms (0 = wait forever)
);

/**
 * @brief Send data over a connected TCP socket.
 *
 * Blocks until TCP has taken every piece of the send (segmented it into
 * its own send queue), the timeout expires, or the send fails.
 *
 * **This call does not queue.** If another send on @p sock is still
 * pending — one an event loop is driving via @ref axl_tcp_send_async —
 * this returns `AXL_ERR` immediately rather than waiting its turn. A
 * synchronous wrapper's contract is "finished when I return", and it
 * cannot honour that behind a send whose progress a different loop
 * drives; waiting would burn the whole timeout and send nothing. Use
 * @ref axl_tcp_send_async, which queues, when a socket has more than
 * one writer. An ORPHAN PIECE (see @ref axl_tcp_send_async) is not a
 * pending send: this call waits for it to complete, and that wait
 * counts against @p timeout_ms.
 *
 * **A timeout is a partial send, not a broken connection.** When
 * @p timeout_ms expires (typically against a peer advertising a zero
 * window) the send stops at a piece boundary: the piece already handed
 * to the firmware stays armed as the socket's orphan piece and goes out
 * on its own, nothing after it is sent, and the socket stays connected.
 * The call returns `AXL_TIMEOUT` whenever its LAST Transmit has not
 * completed by then -- even when every piece was already armed, so a
 * peer that has stopped reading is still reported. How many of
 * @p data's bytes were committed is reported by
 * @ref axl_tcp_send_get_sent after this returns (it may equal @p size
 * on a timeout). A send cut short with 0 < sent < @p size leaves the
 * stream truncated, which makes the socket's close abortive (see
 * @ref axl_tcp_close). As with BSD `send()` under `SO_SNDTIMEO`, a
 * caller that must deliver the rest resumes from `data + sent`; one
 * that cannot resync (a framed protocol such as TLS or 9P) closes the
 * socket instead.
 *
 * An interrupted call -- its loop quit by something other than the
 * send finishing, such as a Ctrl-C -- ends the same way: the armed
 * piece is left as an orphan, and the call returns `AXL_ERR` with the
 * committed count in @ref axl_tcp_send_get_sent.
 *
 * @p data is never referenced after this returns, whatever the
 * outcome: AxlTcp copies each piece into memory it owns before the
 * firmware sees it.
 *
 * @return AXL_OK when every Transmit of the send completed;
 *     AXL_TIMEOUT when the timeout expired first; AXL_ERR when the send
 *     was refused, failed, or was interrupted (see
 *     @ref axl_tcp_send_get_sent for how far it got).
 */
int
axl_tcp_send(
    AxlTcp     *sock,       ///< connected socket
    const void *data,       ///< buffer to send
    size_t     size,        ///< number of bytes to send
    size_t     timeout_ms   ///< timeout in ms (0 = default 10s)
);

/**
 * @brief Receive data from a connected TCP socket.
 *
 * @return AXL_OK on success, AXL_ERR on failure or timeout.
 */
int
axl_tcp_recv(
    AxlTcp *sock,       ///< connected socket
    void   *buf,        ///< receive buffer
    size_t *size,       ///< on entry, buffer size; on return, bytes received
    size_t  timeout_ms  ///< timeout in ms (0 = default 10s)
);

/**
 * @brief Poll a TCP socket to drive its internal state machine.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_tcp_poll(
    AxlTcp *sock  ///< socket to poll
);

/**
 * @brief Close and free a TCP socket (listener or connected).
 *
 * For a connected socket, #AXL_TEARDOWN_GRACEFUL initiates a graceful
 * close (FIN exchange) -- **unless a send is still pending**, see below.
 * A graceful close returns immediately when called from inside a running
 * event loop:
 * the firmware-level teardown (`Configure(NULL)` + service-binding
 * `DestroyChild` + freeing the AxlTcp) is deferred to the loop, so the
 * caller never blocks on the close path. When called outside a running
 * loop (e.g. from a sync CLI tool, or during shutdown after
 * `axl_loop_run` returned) it falls back to a synchronous wait and
 * finalizes inline before returning.
 *
 * **Both paths finalize when the close owes the wire nothing**, not when
 * the firmware completes it after TIME_WAIT (~2 s for an active close):
 * once both FINs are exchanged (a round trip after ours) or the
 * connection is gone. A peer that ACKs our FIN but does not send its own
 * is given about 500 ms -- counted in poll ticks, so on the loop path it
 * stretches under a slow loop tick (a driver loop's), never shrinks. The
 * synchronous wait is bounded at ~3 s for a peer
 * that never ACKs; the loop keeps such a close until the firmware ends
 * it. Finalizing drops the firmware's TIME_WAIT/FIN_WAIT_2 state
 * silently: a late segment from the peer (a retransmitted FIN, data after
 * our close) is answered with an RST, as for any socket its application
 * has closed. Unless the synchronous ~3 s bound expires first (or the
 * peer resets the connection), nothing the caller sent is lost: every
 * byte before the FIN is ACKed by then. @ref axl_tcp_closes_pending
 * counts the closes a loop has yet to finalize.
 *
 * **Ordering: close before freeing the loop.** Always call
 * `axl_tcp_close` BEFORE `axl_loop_free` on any loop the socket was
 * registered with. Close has to drop loop sources for the socket,
 * and the async-finalize path posts the close-complete event back to
 * the loop. Freeing the loop first leaves both paths dereferencing
 * freed memory.
 *
 * **`sock` outlives this call (GRACEFUL only).** On the graceful async path
 * the AxlTcp struct lives until the loop finalizes the close. With
 * @ref AXL_TEARDOWN_RESET -- requested, or promoted to as below -- the
 * finalize is synchronous and `sock` is freed on return. Either way, do not
 * touch `sock` after this returns.
 *
 * **A close that would drop accepted bytes is abortive.** A GRACEFUL
 * close is promoted to #AXL_TEARDOWN_RESET -- the peer gets an RST, not
 * a FIN, and the finalize is synchronous (`sock` is freed on return) --
 * when accepted bytes are being dropped or a message was cut short:
 *   - a send is still queued (no piece of it handed to the firmware);
 *   - the active send is PARTLY armed (some of its bytes not yet handed
 *     to the firmware);
 *   - the stream is already truncated: some earlier send was retired
 *     with 0 < sent < size (see @ref axl_tcp_send_get_sent), whatever its
 *     status. That stays true for the rest of the connection's life.
 * Those sends' owners are told `AXL_CANCELLED` and their unsent bytes
 * are dropped. A FIN would tell the peer the stream ended cleanly,
 * indistinguishable from a complete shorter one; the RST says it did
 * not. The promotion is logged at DEBUG in the `tcp` domain.
 *
 * A close stays graceful when no accepted byte is being dropped: nothing
 * pending on an untruncated stream; a send that has already FINISHED
 * (its last Transmit completed) whose callback has not run yet; an
 * active send that is FULLY armed (its last piece already handed to the
 * firmware), which a GRACEFUL close retires `AXL_OK` exactly as a cancel
 * would (see @ref axl_tcp_send_async); and an orphan piece on an
 * untruncated stream (one left by a fully-armed send), which TCP sends
 * ahead of the FIN. A caller that needs its last bytes delivered waits
 * for their callback before closing.
 *
 * **A fully-armed send in an ABORTIVE close is cancelled.** When the
 * close resets the connection anyway -- #AXL_TEARDOWN_RESET requested,
 * or promoted because sends are queued behind it or the stream is
 * truncated -- the RST discards the last piece of a fully-armed active
 * send, so that send is retired `AXL_CANCELLED` with
 * @ref axl_tcp_send_get_sent equal to its size. It was not cut short,
 * so it does not itself mark the stream truncated.
 *
 * **Every send callback still owed runs before this returns.** Each
 * accepted @ref axl_tcp_send_async whose bytes the close drops -- queued
 * sends, a partly-armed active send, and in an abortive close a
 * fully-armed one -- gets its callback with `AXL_CANCELLED`, so no caller is left waiting on a
 * callback that cannot come. `AXL_CANCELLED` means NOT DELIVERED: the
 * connection was reset, and the callbacks run only after the firmware
 * has released the connection and dropped its send queue.
 * @ref axl_tcp_send_get_sent still reports how many bytes each send had
 * committed to the firmware, but after an RST that count says nothing
 * about what the peer received. The buffer is the caller's again the
 * moment its callback runs (the firmware only ever held AxlTcp's own
 * copy of it). From inside such a callback the socket reports no
 * addresses (the firmware child is already gone). So does a send that
 * had already FINISHED (or, in a graceful close, was fully armed) but
 * whose callback,
 * scheduled for the loop's next iteration, has not run yet — and that
 * one keeps the status it finished with, `AXL_OK` or `AXL_ERR`. A
 * callback cannot, therefore, tell from its status alone that it is running
 * inside a close: a caller whose callback acts on a successful send (sends
 * more, or tears down on an error) must mark its own state as closing
 * BEFORE calling this. All of these callbacks run inside this call, since
 * the loop may be freed the moment it returns.
 *
 * **Closing again is a no-op.** A socket is torn down once; a second close
 * (typically from a send callback this one just fired) returns without
 * touching it. New sends and receives on a closed socket are refused with
 * `AXL_ERR` rather than armed against released firmware state.
 */
void
axl_tcp_close(
    AxlTcp      *sock,  ///< socket to close (NULL-safe)
    AxlTeardown  mode   ///< AXL_TEARDOWN_GRACEFUL (FIN) or AXL_TEARDOWN_RESET
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP_ARG(AxlTcp, axl_tcp_close, AXL_TEARDOWN_GRACEFUL)
#endif

/**
 * @brief Count the graceful closes deferred to a loop and not yet finalized.
 *
 * A graceful @ref axl_tcp_close made inside a running loop returns at once
 * and leaves the close to that loop, which finalizes it -- releasing the
 * firmware child, the socket's memory and its port -- once the close owes
 * the wire nothing (see @ref axl_tcp_close): normally a round trip after the
 * FIN, about 500 ms for a peer that ACKs our FIN without sending its own, and as
 * long as the firmware takes for one that never ACKs. Until then each such
 * close holds a few allocations, so a heap reading taken while this is
 * non-zero counts them; a reading taken when it is 0 does not. A program
 * draining before `axl_loop_free` runs its loop until this reaches 0.
 *
 * Process-wide: every loop's pending closes. Closes finalized inside the
 * call (abortive, or synchronous with no loop to defer to) are never
 * counted.
 *
 * @return the number of deferred graceful closes not yet finalized.
 */
size_t
axl_tcp_closes_pending(void);

/**
 * @brief Query the local address of a connected or listening socket.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_tcp_get_local_addr(
    AxlTcp   *sock,      ///< socket
    char     *addr,      ///< buffer for dotted-decimal address (min 16 bytes)
    size_t   size,       ///< size of addr buffer
    uint16_t *out_port   ///< receives local port number
);

/**
 * @brief Query the remote address of a connected socket.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_tcp_get_remote_addr(
    AxlTcp   *sock,      ///< connected socket
    char     *addr,      ///< buffer for dotted-decimal address (min 16 bytes)
    size_t   size,       ///< size of addr buffer
    uint16_t *out_port   ///< receives remote port number
);

// ---------------------------------------------------------------------------
// Async TCP — event-driven, integrates with AxlLoop
// ---------------------------------------------------------------------------

/**
 * AxlTcpCallback:
 *
 * Callback for async TCP operations.
 *
 * @p sock is the resulting socket. On accept/connect success this is
 * a freshly-connected socket; on recv/send success it is the same
 * socket the op was started on. On connect/accept *error or cancel*
 * sock is NULL (library closes the partial/listener state
 * internally). On recv/send *error or cancel* sock is the still-valid
 * connected socket — caller retains ownership and can reuse or close
 * it.
 *
 * @p status is an AxlStatus value:
 *   - AXL_OK on success
 *   - AXL_ERR on UEFI error
 *   - AXL_CANCELLED if the cancellable passed to the *_async call
 *     was signalled before completion
 *
 * A send that ends in AXL_ERR or AXL_CANCELLED may have been partly
 * sent; @ref axl_tcp_send_get_sent, called from inside its callback,
 * says how much.
 *
 * Return value controls re-arming for ops that support it:
 *   - `axl_tcp_recv_async`: true = re-arm with same buffer, false = stop
 *   - `axl_tcp_accept_async`: true = keep listening (default), false = stop accepting
 *   - `axl_tcp_connect_async`, `axl_tcp_send_async`: ignored (ops are
 *     one-shot by nature — connect fires once, each send owns its buffer).
 *     Convention: `return true;` in these callbacks.
 *
 * ## Safety: closing the socket from inside the callback
 *
 * For recv/accept (the ops with meaningful return values):
 *   - If you return `true`, the socket/listener MUST remain valid
 *     after the callback returns — the library re-issues the UEFI op
 *     on it. Do NOT call axl_tcp_close in this branch.
 *   - If you return `false`, you MAY call axl_tcp_close on the passed
 *     socket/listener from inside the callback. The library does not
 *     access it after a false return. If you return false without
 *     closing, the caller must eventually close to avoid a UEFI
 *     event/token leak.
 *
 * For accept specifically, @p sock is the newly-accepted *client*, not
 * the listener. Closing the client is always safe regardless of the
 * return value (the library re-issues Accept on the listener).
 *
 * See each *_async function's doc for per-op nuances.
 */
typedef bool (*AxlTcpCallback)(
    AxlTcp   *sock,   ///< socket (may be NULL — see per-op docs)
    AxlStatus status, ///< AXL_OK, AXL_ERR, or AXL_CANCELLED
    void     *data    ///< caller-provided context
) AXL_CB_NOEXCEPT;

/**
 * @brief Async connect — initiates TCP connection, returns immediately.
 *
 * The callback fires from the event loop when the connection completes,
 * fails, or is cancelled via @p cancel. On success the newly connected
 * socket is passed; on error or cancellation the sock pointer is NULL
 * and the partial socket is closed internally. Status is AXL_OK on
 * success, AXL_ERR on UEFI error, or AXL_CANCELLED if @p cancel was
 * signalled.
 *
 * **Cancel is terminal for connect.** The partial socket is closed by
 * the library — caller has nothing to free on a cancel callback.
 * Contrast with accept/recv/send below, which leave their socket
 * intact on cancel.
 *
 * @return AXL_OK if initiated, AXL_ERR on immediate failure.
 */
int
axl_tcp_connect_async(
    const char     *host,    ///< IPv4 address or hostname
    uint16_t        port,    ///< remote port
    AxlLoop        *loop,    ///< event loop to register with
    AxlCancellable *cancel,  ///< optional cancel token (NULL = uncancellable)
    AxlTcpCallback  cb,      ///< callback on completion (return value ignored)
    void           *data     ///< opaque context
);

/**
 * @brief Async sibling of axl_tcp_connect_via.
 *
 * @p source_ip semantics match the synchronous variant.
 */
int
axl_tcp_connect_async_via(
    const char            *host,
    uint16_t               port,
    const AxlIPv4Address  *source_ip,
    AxlLoop               *loop,
    AxlCancellable        *cancel,
    AxlTcpCallback         cb,
    void                  *data
);

/**
 * @brief Async accept — waits for incoming connection via the event loop.
 *
 * The callback fires each time a client connects. Re-arming is
 * controlled by the callback's return value: `true` keeps the
 * listener armed for the next connection, `false` stops accepting.
 * Call `axl_tcp_close` on the listener to tear it down entirely.
 *
 * **Cancel leaves the listener valid.** On cancel, the callback fires
 * with (NULL, AXL_CANCELLED, data) and the listener is no longer
 * armed for accepts, but `axl_tcp_accept_async` can be called again
 * to resume listening. Close the listener with `axl_tcp_close` when
 * done.
 *
 * @return AXL_OK if initiated, AXL_ERR on immediate failure.
 */
int
axl_tcp_accept_async(
    AxlTcp         *listener,  ///< listener from axl_tcp_listen
    AxlLoop        *loop,      ///< event loop
    AxlCancellable *cancel,    ///< optional cancel token (NULL = uncancellable)
    AxlTcpCallback  cb,        ///< callback per accepted client
    void           *data       ///< opaque context
);

/**
 * @brief Async receive — waits for data via the event loop.
 *
 * The callback fires when data arrives in the caller's buffer.
 * Re-arming is controlled by the callback's return value: `true`
 * re-arms the recv with the same buffer (typical for streaming
 * servers); `false` stops receiving (caller may start a fresh recv
 * with a different buffer, or close the socket).
 *
 * **Cancel leaves the socket connected.** On cancel the callback
 * fires with (sock, AXL_CANCELLED, data) — the sock is still a
 * valid connected TCP socket, and the caller can start another
 * recv or send, or close it. Contrast with connect, which destroys
 * the sock on cancel.
 *
 * **A failed call leaves the receive's callback, buffer and loop as they
 * were.** Once the arguments are accepted (valid, and the socket not
 * closed), a receive already armed on the socket is cancelled either way
 * (its callback does not fire); a call refused on its arguments changes
 * nothing. Called from inside the receive callback,
 * a re-arm that fails followed by `return true` therefore keeps the
 * receive that just completed, on its own loop and into its own buffer.
 *
 * @return AXL_OK if initiated, AXL_ERR on immediate failure.
 */
int
axl_tcp_recv_async(
    AxlTcp         *sock,    ///< connected socket
    void           *buf,     ///< receive buffer (must stay valid across re-arms)
    size_t          size,    ///< buffer size
    AxlLoop        *loop,    ///< event loop
    AxlCancellable *cancel,  ///< optional cancel token (NULL = uncancellable)
    AxlTcpCallback  cb,      ///< callback when data received (return bool controls re-arm)
    void           *data     ///< opaque context
);

/**
 * @brief Get the number of bytes received by the last axl_tcp_recv_async.
 *
 * Call from inside the recv callback to get the actual byte count.
 *
 * @return bytes received, or 0 if no recv has completed.
 */
size_t
axl_tcp_recv_get_size(
    AxlTcp *sock  ///< socket from recv callback
);

/**
 * @brief Async send — initiates send, callback on completion.
 *
 * The buffer must stay valid until the callback fires. That borrow is
 * AxlTcp's own, not the firmware's: each piece of the send
 * (#AxlTcpPacing.chunk_bytes at most) is copied into a buffer the
 * socket owns immediately before it is handed to the firmware, so the
 * firmware never holds a reference into @p buf, and once the callback
 * has fired nothing reads it again, whatever the status. The copy
 * buffer is at least one piece long, allocated on the socket's first
 * send and freed when the firmware releases the socket.
 *
 * **Sends are QUEUED, so callers need not serialize.** Submitting
 * while a previous send is still in flight accepts this one and
 * transmits it when its turn comes, FIFO. Callers used to have to
 * invent their own serialisation; several did, and the ones that
 * instead read the old refusal as a hard failure reset healthy
 * connections. See docs/AXL-Tcp-Queue-Design.md.
 *
 * **The buffer is borrowed for the whole wait, not just the
 * transmit.** A queued send holds @a buf until ITS callback fires,
 * which may be well after later code has run.
 *
 * **A send is never refused for capacity.** There is no "try
 * again later" status: the call either accepts the send (and its
 * callback will fire exactly once) or fails outright with
 * `AXL_ERR` — the socket is closed, an argument is invalid, or the
 * token could not be allocated — in which case the callback does
 * NOT fire. Callers therefore need no retry path. This follows
 * EDK2's socket layer, whose `SockSend` buffers rather than
 * rejecting; the watermarks decide submit-now vs defer, they are
 * not a rejection threshold. A transport that refuses the bytes
 * is reported the same way a transport that drops them is:
 * through the callback, with `AXL_ERR`.
 *
 * **The callback is deferred, always.** It never runs inside this
 * call, and never inside the completion handling of another send —
 * it is queued for the event loop's next iteration (EDK2 signals
 * its tokens the same way, via `gBS->SignalEvent`). So a callback
 * may do whatever it likes to the socket, axl_tcp_close()
 * included, without tripping over a transport operation still in
 * progress underneath it. The transmission itself is not delayed:
 * the next queued send is handed to the firmware immediately, and
 * only the notification waits for the tick.
 *
 * **While a send makes progress, @a loop polls instead of sleeping.**
 * EDK2's network stack drains received frames (the peer's ACKs among
 * them) one per 10 ms timer tick unless someone polls it, and a
 * Transmit completes only once TCP has segmented all of it, which the
 * send window -- opened by those ACKs -- gates, so an
 * unpolled 1 MiB send took 7-10 s. AxlTcp therefore polls the TCP
 * driver from an idle source on the send's loop, on every pass of that
 * loop, while a send on it is on the wire; every other source is still
 * checked and dispatched in between. There is ONE such source per
 * loop, shared by every socket sending on it, and it counts against
 * the loop's source table (`AXL_MAX_SOURCES`).
 *
 * *Progress* means a Transmit handed to the firmware: a send starting,
 * or one of its pieces completing. The source is removed when the
 * loop's last send retires -- or, with #AxlTcpPacing.linger_us set, that
 * long after it for a socket with a receive armed on the same loop --
 * and when no send on the loop has made
 * progress for #AxlTcpPacing.idle_backoff_ms; the loop then sleeps
 * until the next Transmit. What that costs:
 *   - A peer that stops reading (a zero window) costs one poll burst
 *     of idle_backoff_ms per stall.
 *   - A peer that reads SLOWLY -- accepting at least one piece per
 *     idle_backoff_ms -- keeps its loop polling for as long as it
 *     keeps reading: on a uniprocessor firmware, one busy core for the
 *     whole transfer, however slow.
 *   - A loop that is polling never reaches its blocking wait, so a
 *     serial Ctrl-C (a 0x03 keystroke; the Shell's break flag is
 *     checked on every pass regardless) is read only once polling
 *     stops: delayed, not lost.
 *
 * Each socket keeps one Transmit in flight, and a piece completes only
 * once TCP has segmented all of it (as far as the send window, opened
 * by the peer's ACKs, allows), so a single send moves at most about
 * #AxlTcpPacing.chunk_bytes per round trip (8 KB at 100 ms is about
 * 80 KB/s); see axl_tcp_set_pacing().
 *
 * **Fallbacks.** A small fixed number of loops (eight) can be pacing at
 * once -- the application's loop plus one nested loop per synchronous
 * axl_tcp_send in progress. A send on a further loop, or on a loop
 * whose source table is full, is not paced: it still completes, at the
 * firmware's pace -- a frame per timer tick, 10 ms unless something raised
 * it (axl_timer_resolution_begin(), axl_net_set_latency()). Each of the two fallbacks logs one
 * warning the first time it happens in the process, and never again.
 *
 * **Driver mode.** Under axl_loop_attach_driver the source runs only
 * inside the driver tick, and how long a tick keeps polling is
 * axl_loop_set_driver_budget_ms().
 *
 * **Cancel stops a send at a piece boundary, and never un-sends one.**
 * Signalling the `AxlCancellable` retires the send, firing its callback
 * on the loop's next iteration, and what that means depends on how far
 * the send got:
 *   - **Still queued** (no piece handed to the firmware yet): cancelled
 *     completely, with `AXL_CANCELLED`. Nothing of it reaches the wire,
 *     axl_tcp_send_get_sent() reports 0, and the stream is NOT
 *     truncated.
 *   - **Active, partly armed** (some pieces handed to the firmware, some
 *     not): `AXL_CANCELLED`. The armed piece is NOT recalled -- it
 *     becomes the socket's orphan piece (below) -- and no further piece
 *     of this send is armed. axl_tcp_send_get_sent() reports the bytes
 *     committed, whole pieces only (between 1 and `size - 1`). The
 *     callback does not wait for the orphan: @p buf is free the moment
 *     it runs. The peer sees this send's message cut short at the
 *     reported count, so the stream is now truncated and the socket's
 *     close will be abortive (see axl_tcp_close()).
 *   - **Active, fully armed** (the last piece already handed to the
 *     firmware): nothing is left to stop, so the send is reported as
 *     done -- `AXL_OK`, with axl_tcp_send_get_sent() equal to `size` --
 *     without waiting for that last Transmit to complete; the piece
 *     becomes the orphan. This is BSD `send()`'s meaning of success (the
 *     bytes are the transport's now), and `AXL_CANCELLED` for a send that
 *     will in fact go out whole would be false. A transport failure
 *     after this point is not reported to this send; a later send or
 *     recv fails if the connection is gone. If that last Transmit has
 *     already COMPLETED when the cancel runs (both became ready in one
 *     loop turn), it is not an orphan: the send reports the status that
 *     piece really completed with -- `AXL_OK`, or `AXL_ERR` if it
 *     failed. (The synchronous axl_tcp_send() reports this case
 *     differently; see there.)
 *   - **Already finished** (its callback is scheduled but has not run):
 *     the cancel is too late and the callback reports the status the
 *     send finished with.
 *
 * AxlTcp never cancels a Transmit the firmware has accepted -- on EDK2's
 * TcpDxe that leaves the data queued with no owner, which asserts on a
 * DEBUG build and corrupts the socket on a RELEASE one -- so a cancel
 * does not make an armed piece come back any sooner.
 *
 * **The orphan piece.** A piece left armed by a cancel or a timeout
 * belongs to the socket, not to any send or loop. It does not count as
 * a pending send for axl_tcp_send(). The next send, sync or async, is
 * armed only after it completes, and that wait counts against the next
 * send's own timeout, so bytes are never interleaved. Against a peer
 * advertising a zero window it may never complete; the next send then
 * waits with it, and the waiting send ends only by its own cancel or
 * timeout, or a close.
 *
 * axl_tcp_close() retires every pending send whose bytes it drops with
 * `AXL_CANCELLED`, so a caller is never left waiting on a callback that
 * cannot come, and delivers a finished or fully-armed send with its
 * real status. See axl_tcp_close() for when a close is abortive; its
 * `AXL_CANCELLED` means the bytes were not delivered.
 *
 * **Cancel leaves the socket connected.** On cancel the callback
 * fires with (sock, AXL_CANCELLED, data) — the sock is still a
 * valid connected TCP socket, and further sends and receives work.
 * Whether the stream is still meaningful is the caller's call: a
 * byte stream resumes from `buf + sent`, while a framed protocol whose
 * message was truncated cannot resync and should close.
 *
 * @return AXL_OK if accepted (submitted or queued), AXL_ERR on failure.
 */
int
axl_tcp_send_async(
    AxlTcp         *sock,    ///< connected socket
    const void     *buf,     ///< send buffer (must stay valid until callback)
    size_t          size,    ///< bytes to send
    AxlLoop        *loop,    ///< event loop
    AxlCancellable *cancel,  ///< optional cancel token (NULL = uncancellable)
    AxlTcpCallback  cb,      ///< callback when send completes (return value ignored)
    void           *data     ///< opaque context
);

/**
 * @brief Bytes of a send that were handed to the firmware.
 *
 * The send counterpart of axl_tcp_recv_get_size(), with BSD `send()`'s
 * partial-count meaning: how many leading bytes of the send's buffer
 * AxlTcp committed to the firmware for transmission. It counts whole
 * pieces (#AxlTcpPacing.chunk_bytes each, the last one shorter): a
 * piece, once handed over, cannot be recalled, so it counts even if the
 * connection later fails before TCP takes all of it. A Transmit the
 * firmware refuses outright is not counted. It is a count of bytes
 * GIVEN TO TCP, not bytes the peer acknowledged.
 *
 * After an axl_tcp_send_async() callback, by status:
 *   - `AXL_OK`: the send's full size (including a send cancelled after
 *     its last piece was armed).
 *   - `AXL_CANCELLED` from a cancellable: 0 if the send was still
 *     queued, otherwise the committed bytes (at most `size - 1`). See
 *     axl_tcp_send_async().
 *   - `AXL_CANCELLED` from axl_tcp_close(): the committed bytes -- the
 *     full size for a fully-armed send whose last piece the RST
 *     discarded; the connection was reset, so none of them is known to
 *     have arrived.
 *   - `AXL_ERR`: the committed bytes, including the piece whose Transmit
 *     failed.
 *
 * After axl_tcp_send() returns:
 *   - `AXL_OK`: the full size.
 *   - `AXL_TIMEOUT`: the committed bytes (0 to `size`; `size` when every
 *     piece was armed but the last Transmit had not completed).
 *   - `AXL_ERR` after an interruption (its loop quit, e.g. Ctrl-C) or a
 *     transport failure: the committed bytes.
 *   - `AXL_ERR` from a refusal (an invalid argument, or another send
 *     pending): 0.
 *
 * Any send retired with 0 < sent < size, whatever its status, leaves the
 * stream truncated, which makes the socket's close abortive (see
 * axl_tcp_close()). A send cancelled while still queued (sent == 0)
 * does not.
 *
 * **When it is valid.** Inside a send callback until the callback's
 * next `axl_tcp_*` call on @p sock, and after axl_tcp_send() returns
 * until the next call on @p sock. Otherwise it returns the count of the
 * most recently delivered or returned send (0 if none).
 *
 * @return bytes of the send committed to the firmware; 0 if @p sock is
 *     NULL.
 */
size_t
axl_tcp_send_get_sent(
    const AxlTcp *sock  ///< socket from a send callback, or just passed to axl_tcp_send()
);

/**
 * @brief Bytes of accepted sends that AxlTcp has not yet handed to the
 *     firmware.
 *
 * The send queue is unbounded by design (a send is never refused for
 * capacity; see axl_tcp_send_async()), so a producer faster than its
 * peer grows it without limit. This is the hook for the caller's OWN
 * backpressure: check it before producing more, and hold off while it
 * is above whatever bound suits the application.
 *
 * It counts every byte of an accepted send that is still in AxlTcp's
 * hands -- exactly the bytes a cancel or a close could still drop
 * without any of them reaching the wire:
 *   - every send still queued behind the active one, in full;
 *   - the part of the ACTIVE send not yet armed.
 * It does NOT count the piece currently armed, the orphan piece
 * included (its bytes are the firmware's, from AxlTcp's copy, and will
 * go out whatever happens), nor any send already finished or retired
 * whose callback has not run yet. So a single large send reads as its
 * size minus one piece while it is on the wire, 0 once its last piece
 * is armed, and a socket busy only with an orphan piece reads 0.
 *
 * AxlTcp also logs a warning, once per crossing, when the bytes queued
 * BEHIND the active send pass 1 MiB. That is a different measure from
 * this one -- it leaves out the active send, so one large send alone
 * never trips it -- and it is a diagnostic, not a limit or a knob.
 *
 * Cancelling a queued send removes its bytes at once; cancelling the
 * active send removes its unarmed remainder at once.
 *
 * The value is a snapshot: exact when read, and until any loop driving
 * a send on @p sock runs (a piece completing arms the next one), or a
 * send on @p sock is submitted, cancelled or closed.
 *
 * @return bytes accepted and not yet handed to the firmware; 0 for a
 *     NULL, closed or listening socket.
 */
size_t
axl_tcp_send_queued_bytes(
    const AxlTcp *sock  ///< connected socket (NULL-safe)
);

// ---------------------------------------------------------------------------
// Transmit pacing (process-wide)
// ---------------------------------------------------------------------------

/**
 * @brief Default #AxlTcpPacing.chunk_bytes: the largest piece of a send
 *     handed to the firmware in one Transmit.
 *
 * Two constraints pull on it.
 *
 * **Burst depth (a floor on safety, a ceiling on size).** TcpDxe emits
 * a whole Transmit's worth of segments (cwnd and window permitting) in
 * one call, with no NIC poll between them. iPXE's SNP driver -- QEMU's
 * e1000 option ROM, and real firmware wherever iPXE drives the NIC --
 * fills at most 15 TX descriptors between polls and answers the 16th
 * frame with EFI_OUT_OF_RESOURCES, which MnpDxe does not retry, so that
 * frame is lost until TCP retransmits it (about a second). A piece
 * must therefore be at most 15 segments: 21,900 bytes at MSS 1460.
 * Measured on QEMU: 32 KB pieces lost 2-17 frames per MiB; 16 KB and
 * 8 KB lost none.
 *
 * **Round trips (a ceiling on speed).** Each socket keeps ONE Transmit
 * in flight, and a piece completes only once TCP has segmented all of
 * it, as far as the send window (opened by the peer's ACKs) allows, so one
 * send moves at most about chunk_bytes per round trip, whatever the
 * link speed: 8 KB is about 80 KB/s at 100 ms. Measured on QEMU (x64,
 * a relay adding a fixed round trip to the guest's path; medians of 3),
 * 1 MiB took 4.1 / 6.8 / 13.4 s with 8 KB at 30 / 50 / 100 ms, 2.2 /
 * 3.5 / 7.0 s with 16 KB, and 10.4 / 11.4 / 14.7 s before pacing. 8 KB
 * is the default because it is the smallest size no slower than before
 * pacing at 100 ms, and its burst margin is worth more than WAN speed.
 * Extrapolating those lines (8 KB grows ~0.13 s per ms of RTT, the
 * pre-pacing code ~0.06), they cross near 120 ms: beyond that RTT the
 * default is SLOWER than before pacing, whatever the back-off.
 *
 * For a single distant client 16 KB is the better choice (twice the
 * per-round-trip ceiling). With several sockets sending large messages
 * at once, 16 KB measured multi-second stalls on iPXE -- 1.6-2.6 s rounds
 * with seven clients receiving 16,000-byte WebSocket frames, and no
 * frame the SNP layer refused, so the cause is unknown (possibly loss
 * below it) -- where 8 KB measured none. On a
 * sub-millisecond LAN the round trip is not the limit: in the
 * foreground on QEMU, 8 KB measured no slower than 16 KB. Keeping two
 * Transmit tokens in flight per socket is the design that would lift
 * this ceiling without deepening the burst; it is not implemented.
 */
#define AXL_TCP_PACING_CHUNK_DEFAULT_BYTES      8192u

/// Smallest #AxlTcpPacing.chunk_bytes axl_tcp_set_pacing() accepts.
#define AXL_TCP_PACING_CHUNK_MIN_BYTES          1024u

/**
 * @brief Largest #AxlTcpPacing.chunk_bytes axl_tcp_set_pacing() accepts.
 *
 * 32 KB is the pre-pacing piece size, up to 23 segments at MSS 1460 and
 * so past iPXE's 15 (see #AXL_TCP_PACING_CHUNK_DEFAULT_BYTES). Values
 * above 21,900 are for NIC drivers that defer rather than drop (EDK2's
 * VirtioNetDxe returns EFI_NOT_READY, which MnpDxe retries), or for
 * high-RTT peers where the per-round-trip ceiling dominates.
 */
#define AXL_TCP_PACING_CHUNK_MAX_BYTES          32768u

/**
 * @brief Default #AxlTcpPacing.idle_backoff_ms: how long a loop keeps
 *     polling after the last progress on any of its sends.
 *
 * It must exceed the round-trip time to the peer: the ACKs for a piece
 * arrive one RTT after it is sent, and if polling has already stopped
 * they drain at MnpDxe's 10 ms per frame, which makes a send to a peer
 * whose RTT exceeds the back-off SLOWER than before pacing. 500 ms
 * keeps polling across most WAN round trips; a slower path, such as a
 * distant VPN, wants it raised (the README's tuning example uses 1000 ms
 * for a ~600 ms VPN). It does not make a distant send fast, because
 * above about 120 ms RTT the per-round-trip ceiling of
 * #AXL_TCP_PACING_CHUNK_DEFAULT_BYTES already makes the default slower
 * than before pacing. The cost is a stalled or trickling peer (see
 * axl_tcp_send_async): one poll burst this long per zero-window stall,
 * and continuous polling for a reader that accepts at least one piece
 * per back-off.
 */
#define AXL_TCP_PACING_IDLE_BACKOFF_DEFAULT_MS  500u

/**
 * @brief Largest #AxlTcpPacing.idle_backoff_ms axl_tcp_set_pacing()
 *     accepts.
 *
 * Ten seconds is past any round trip a firmware client meets; beyond
 * it the setting would only prolong the burn on a stalled peer.
 */
#define AXL_TCP_PACING_IDLE_BACKOFF_MAX_MS      10000u

/**
 * @brief Default #AxlTcpPacing.linger_us: 0, no linger.
 *
 * Off by default because its effect depends on the NIC and its driver
 * (see axl_tcp_set_pacing()), and a platform should measure before it
 * pays the polling.
 */
#define AXL_TCP_PACING_LINGER_DEFAULT_US        0u

/**
 * @brief Largest #AxlTcpPacing.linger_us axl_tcp_set_pacing() accepts.
 *
 * 20 ms: two of EDK2's default 10 ms timer ticks, past any round trip a linger is meant for
 * (a peer on the same LAN answering a reply at once). Beyond it the loop
 * would only poll a peer that is not coming back soon.
 */
#define AXL_TCP_PACING_LINGER_MAX_US            20000u

/**
 * @brief Process-wide transmit-pacing settings; see axl_tcp_set_pacing().
 */
typedef struct {
    /// Largest piece of a send per Transmit, in bytes;
    /// #AXL_TCP_PACING_CHUNK_MIN_BYTES..#AXL_TCP_PACING_CHUNK_MAX_BYTES.
    uint32_t chunk_bytes;
    /// Polling continues this long after a loop's last progress, in ms;
    /// 0..#AXL_TCP_PACING_IDLE_BACKOFF_MAX_MS. 0 turns loop-driven
    /// polling off entirely (the pre-pacing behaviour: a send advances
    /// at the firmware's pace, 10 ms per frame by default); pieces are still bounded by
    /// chunk_bytes and each Transmit is still preceded by one poll.
    uint32_t idle_backoff_ms;
    /// After a socket's last send retires with a receive armed on the
    /// same loop, keep polling it this long, in microseconds;
    /// 0..#AXL_TCP_PACING_LINGER_MAX_US. 0 (#AXL_TCP_PACING_LINGER_DEFAULT_US)
    /// turns it off, and so does an idle_backoff_ms of 0. See
    /// axl_tcp_set_pacing() for when to set it.
    uint32_t linger_us;
} AxlTcpPacing;

/**
 * @brief Fill @p out with the default pacing settings
 *     (#AXL_TCP_PACING_CHUNK_DEFAULT_BYTES,
 *     #AXL_TCP_PACING_IDLE_BACKOFF_DEFAULT_MS,
 *     #AXL_TCP_PACING_LINGER_DEFAULT_US).
 *
 * NULL is ignored.
 */
void
axl_tcp_pacing_defaults(
    AxlTcpPacing *out  ///< receives the defaults
);

/**
 * @brief Read the pacing settings currently in force.
 *
 * For read-modify-write: get, change the fields you mean to, set. NULL
 * is ignored.
 */
void
axl_tcp_get_pacing(
    AxlTcpPacing *out  ///< receives the current settings
);

/**
 * @brief Set the transmit pacing for every AxlTcp socket in this process.
 *
 * Process-wide, not per socket or per loop: it describes the firmware
 * network stack and NIC driver underneath, which every socket shares.
 * The intended use is once, before the first listen or connect.
 *
 * It may nevertheless be called at any time, from any loop callback,
 * because every value is read where it is used and nothing already
 * handed to the firmware depends on them:
 *   - a new chunk_bytes applies from the next Transmit, including the
 *     next piece of a send already on the wire. The piece already in
 *     the firmware completes at the size it was armed with, and the
 *     send is advanced by exactly that armed length, so a change
 *     mid-send neither skips nor repeats bytes;
 *   - a new idle_backoff_ms applies from the next progress check of a
 *     loop that is polling (every pass), and, for a loop that has
 *     already backed off, from its next Transmit. Setting 0 stops every
 *     polling loop on its next pass;
 *   - a new linger_us applies to sends that retire after the call; a
 *     socket already lingering keeps the deadline it was given.
 *
 * The call is all-or-nothing: an invalid @p cfg changes nothing.
 *
 * Which knob to turn when:
 *   - chunk_bytes: lower it for a path MSS well below 1460 on an
 *     iPXE-driven NIC (keep chunk_bytes / MSS at most 15); raise it only
 *     for a NIC driver that defers rather than drops, or when one
 *     send's speed to a distant peer matters more than burst safety
 *     (throughput per send is about chunk_bytes per round trip).
 *   - idle_backoff_ms: raise it for peers further away than the default
 *     covers; lower it to cap the CPU a stalled peer costs, accepting
 *     slower sends to peers whose RTT exceeds it. It does not bound a
 *     trickle reader (see axl_tcp_send_async).
 *   - linger_us: for a request/response peer -- an SFTP client with one
 *     request in flight, an RPC client -- that sends its next request
 *     the moment a reply arrives. Once the reply's send retires nothing
 *     polls the stack, so the request waits for MnpDxe's 10 ms timer and,
 *     under axl_loop_attach_driver, for the next driver tick as well --
 *     unless axl_net_set_latency() is fast at the time (it is by default,
 *     for a window after any activity), which polls on its own.
 *     Lingering keeps the socket's loop polling (it is a busy idle
 *     source, so it also extends a driver tick, within its budget) and
 *     the request is drained and answered at once. Measured on QEMU
 *     (sshd, a 10 ms driver tick, a one-request-at-a-time 16 MiB sftp
 *     get): 9000 us took virtio-net from 1.5 to 16.3 MB/s and e1000
 *     from 3.0 to 13.3; aarch64 TCG on virtio-net from 3.2 to 4.1. But
 *     on QEMU's e1000 the extra polling made uploads slower (0.9 MB/s at
 *     one request in flight, 3.6 at 64, against 5.5), through a QEMU
 *     defect that drops frames when its receive ring is refilled often.
 *     So it is off by default: set it after measuring on the platform.
 *     The cost is a polling loop for up to linger_us after every reply;
 *     an idle connection stops polling within it. A linger lasts only
 *     while the receive that justifies it does: the receive completing,
 *     being cancelled or torn down ends it, as do the socket's close and
 *     the pacer's back-off (idle_backoff_ms), so no pacing state outlives
 *     the loop.
 *
 * @return AXL_OK; AXL_INVALID if a field is out of its range (nothing
 *     changed). A NULL @p cfg restores the defaults and returns AXL_OK.
 */
AXL_WARN_UNUSED int
axl_tcp_set_pacing(
    const AxlTcpPacing *cfg  ///< new settings, or NULL for the defaults
);

// ---------------------------------------------------------------------------
// Fault injection (testing)
// ---------------------------------------------------------------------------

/**
 * @brief Make the Nth subsequent send RETIREMENT find nowhere to schedule
 *     its callback.
 *
 * Every accepted @ref axl_tcp_send_async is RETIRED exactly once: when it
 * finishes (`AXL_OK` or `AXL_ERR`), when its cancellable fires
 * (`AXL_CANCELLED`), or when the transport fails to arm it (`AXL_ERR`).
 * Retiring parks the token and schedules its callback for the loop's next
 * iteration, on the loop's defer queue or, when that is full, on a
 * one-shot timer. When BOTH are full the token stays parked unscheduled,
 * and the callback runs only when @ref axl_tcp_close delivers it -- with
 * the status the send retired with, not `AXL_CANCELLED`.
 *
 * After this call with N > 0, the Nth subsequent retirement -- counting
 * ALL of the kinds above, process-wide, across every socket -- takes that
 * path as if both pools were full; then the counter resets to 0
 * (disabled). It logs its own DEBUG line ("completion held (injected by
 * axl_tcp_fail_next_send_schedule)"), never the error line a REAL
 * exhaustion writes, so the two cannot be confused.
 *
 * It is how a test holds a finished send's callback open across a close
 * deterministically: the window between a send retiring and its callback
 * running is otherwise one loop iteration wide.
 *
 * **Hazard: a synchronous @ref axl_tcp_send whose retirement is the held
 * one HANGS.** Its private loop quits only from that callback, which now
 * comes only at close, and its timeout does not rescue it: the timeout
 * cancels the send's cancellable, and that reaches nothing -- retiring the
 * send already detached it from its cancellable. The call returns only on
 * Ctrl-C, and then `AXL_ERR` -- for a send that may well have succeeded.
 * Arm this only where every send in the window is asynchronous.
 *
 * Pass 0 to disable. Available in both DEBUG and RELEASE builds, like
 * axl_mem_fail_next_alloc(): the cost is one branch per retirement.
 */
void
axl_tcp_fail_next_send_schedule(
    size_t n  ///< hold the Nth next retirement (1 = next, 0 = disabled)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_TCP_H */
