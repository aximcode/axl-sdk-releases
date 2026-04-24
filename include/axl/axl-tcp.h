/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-tcp.h:
 *
 * TCP socket abstraction. Blocking and async (event-driven) APIs.
 * Async functions integrate with AxlLoop for non-blocking I/O.
 */

#ifndef AXL_TCP_H
#define AXL_TCP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;
typedef struct AxlTcp AxlTcp;
typedef struct AxlCancellable AxlCancellable;

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
 * @return 0 on success, -1 on failure.
 */
int
axl_tcp_connect(
    const char *host,      ///< IPv4 address string or hostname (DNS resolved)
    uint16_t   port,       ///< remote port number
    AxlTcp     **out_sock  ///< receives the connected socket handle
);

/**
 * @brief Create a TCP4 listener on the given port.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_tcp_listen(
    uint16_t port,           ///< local port to listen on
    AxlTcp   **out_listener  ///< receives the listener handle
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
 * @return 0 on success, -1 on failure, timeout, or cancel.
 */
int
axl_tcp_accept(
    AxlTcp  *listener,    ///< listener from axl_tcp_listen
    AxlTcp  **out_client, ///< receives the accepted client socket
    size_t  timeout_ms    ///< timeout in ms (0 = wait forever)
);

/**
 * @brief Send data over a connected TCP socket.
 *
 * @return 0 on success, -1 on failure or timeout.
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
 * @return 0 on success, -1 on failure or timeout.
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
 * @return 0 on success, -1 on failure.
 */
int
axl_tcp_poll(
    AxlTcp *sock  ///< socket to poll
);

/**
 * @brief Close and free a TCP socket (listener or connected).
 */
void
axl_tcp_close(
    AxlTcp *sock  ///< socket to close (NULL-safe)
);

/**
 * @brief Query the local address of a connected or listening socket.
 *
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
 * @p status is:
 *   - 0 on success
 *   - -1 on UEFI error
 *   - AXL_CANCELLED (-2) if the cancellable passed to the *_async
 *     call was signalled before completion
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
    AxlTcp *sock,   ///< socket (may be NULL — see per-op docs)
    int     status, ///< 0, -1, or AXL_CANCELLED
    void   *data    ///< caller-provided context
);

/**
 * @brief Async connect — initiates TCP connection, returns immediately.
 *
 * The callback fires from the event loop when the connection completes,
 * fails, or is cancelled via @p cancel. On success the newly connected
 * socket is passed; on error or cancellation the sock pointer is NULL
 * and the partial socket is closed internally. Status is 0 on success,
 * -1 on UEFI error, or AXL_CANCELLED (-2) if @p cancel was signalled.
 *
 * **Cancel is terminal for connect.** The partial socket is closed by
 * the library — caller has nothing to free on a cancel callback.
 * Contrast with accept/recv/send below, which leave their socket
 * intact on cancel.
 *
 * @return 0 if initiated, -1 on immediate failure.
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
 * @return 0 if initiated, -1 on immediate failure.
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
 * @return 0 if initiated, -1 on immediate failure.
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
 * The buffer must stay valid until the callback fires.
 *
 * **No preemption.** Calling this while a previous send is still
 * in flight returns -1. To interrupt an in-flight send, pass an
 * `AxlCancellable` to the original call and signal it; the cancel
 * callback fires cleanly with `AXL_CANCELLED`. Alternatively,
 * `axl_tcp_close(sock)` tears down every pending op at once.
 *
 * **Cancel leaves the socket connected.** On cancel the callback
 * fires with (sock, AXL_CANCELLED, data) — the sock is still a
 * valid connected TCP socket. Partial bytes may have been
 * transmitted before cancel took effect; the caller should treat
 * the send outcome as unknown and resync at the application level
 * if needed.
 *
 * @return 0 if initiated, -1 on immediate failure or if a send is
 *   already in flight.
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

#ifdef __cplusplus
}
#endif

#endif /* AXL_TCP_H */
