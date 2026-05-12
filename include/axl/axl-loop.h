/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-loop.h:
 *
 * AxlLoop — event loop with timer, keyboard, idle, protocol
 * notification, and raw event sources. The model maps directly
 * onto GLib: AxlLoop is the AXL counterpart of GMainLoop,
 * axl_loop_run / axl_loop_quit play the role of g_main_loop_run /
 * g_main_loop_quit, axl_loop_add_timer is g_timeout_add, and so on.
 * If you have written a GLib daemon, the shape is the same — what
 * differs is the source kinds: AXL adds raw-EFI-event sources
 * (axl_loop_add_event) so any UEFI event (TCP completion tokens,
 * protocol-notify, AxlEvent instances via axl_event_handle) drops
 * straight into the loop without polling.
 */

#ifndef AXL_LOOP_H
#define AXL_LOOP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-event.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;

/**
 * AxlSourceType:
 *
 * Identifies the kind of event source in the loop.
 */
typedef enum {
    AXL_SOURCE_TIMER,      ///< repeating timer
    AXL_SOURCE_TIMEOUT,    ///< one-shot timer (auto-removed after firing)
    AXL_SOURCE_KEYPRESS,   ///< console keyboard input
    AXL_SOURCE_IDLE,       ///< fires every iteration before blocking wait
    AXL_SOURCE_PROTOCOL,   ///< UEFI protocol install notification
    AXL_SOURCE_EVENT       ///< raw EFI event handle (caller-owned)
} AxlSourceType;

/**
 * AxlInputKey:
 *
 * Keyboard input. Mirrors UEFI EFI_INPUT_KEY layout.
 */
typedef struct {
    uint16_t scan_code;    ///< function/arrow key scan code (0 for printable chars)
    uint16_t unicode_char; ///< printable character (0 for special keys)
} AxlInputKey;

/// Return from callback to keep the source active.
#define AXL_SOURCE_CONTINUE  true
/// Return from callback to remove the source from the loop.
#define AXL_SOURCE_REMOVE    false

/**
 * AxlLoopCallback:
 *
 * Generic event callback.
 * Return AXL_SOURCE_CONTINUE to keep the source active, or
 * AXL_SOURCE_REMOVE to remove it. To quit the loop, call
 * axl_loop_quit() from inside the callback.
 */
typedef bool (*AxlLoopCallback)(
    void *data ///< opaque caller data
);

/**
 * AxlKeyCallback:
 *
 * Key press callback.
 * Return AXL_SOURCE_CONTINUE to keep the source active, or
 * AXL_SOURCE_REMOVE to remove it. To quit the loop, call
 * axl_loop_quit() from inside the callback.
 */
typedef bool (*AxlKeyCallback)(
    AxlInputKey key, ///< key data
    void       *data ///< opaque caller data
);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Create a new event loop.
 *
 * @return new AxlLoop, or NULL on failure.
 */
AxlLoop *
axl_loop_new_impl(const char *file, int line);

/**
 * Captures the caller's file/line for leak reporting via the tier-1
 * resource registry. See docs/AXL-Lifecycle.md §4.2.1.
 */
#define axl_loop_new() axl_loop_new_impl(__FILE__, __LINE__)

/**
 * @brief Free an event loop and close all internal events.
 */
void
axl_loop_free(
    AxlLoop *loop  ///< loop to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlLoop, axl_loop_free)
#endif

/**
 * @brief Signal the loop to quit. Safe to call from callbacks.
 */
void
axl_loop_quit(
    AxlLoop *loop  ///< loop to quit
);

/**
 * @brief Check if the loop is running.
 *
 * @return true if running and not quit-requested.
 */
bool
axl_loop_is_running(
    AxlLoop *loop  ///< loop to check
);

/**
 * @brief Add a cleanup callback fired on exit (FIFO order).
 */
void
axl_loop_add_cleanup(
    AxlLoop        *loop, ///< loop
    AxlLoopCallback cb,   ///< callback fired on exit (FIFO order)
    void           *data  ///< opaque data
);

// ---------------------------------------------------------------------------
// Event primitives (FUSE-style)
// ---------------------------------------------------------------------------

/**
 * @brief Wait for (or check) the next event.
 *
 * @return 0 if event pending (call axl_loop_dispatch_event),
 *         1 if non-blocking and nothing ready,
 *        -1 if Ctrl-C detected (loop should exit).
 */
int
axl_loop_next_event(
    AxlLoop *loop,    ///< event loop
    bool     blocking ///< true to block until event, false to return immediately
);

/**
 * @brief Dispatch the pending event from the last axl_loop_next_event call.
 */
void
axl_loop_dispatch_event(
    AxlLoop *loop  ///< event loop
);

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

/**
 * @brief Single iteration: axl_loop_next_event + axl_loop_dispatch_event.
 *
 * @return 0 on event dispatched, 1 if not ready, -1 on Ctrl-C.
 */
int
axl_loop_dispatch(
    AxlLoop *loop,    ///< event loop
    bool     blocking ///< true to block, false for non-blocking
);

/**
 * @brief Run the event loop until quit. Fires cleanup callbacks on exit.
 *
 * @return 0 on normal exit, -1 on Ctrl-C.
 */
int
axl_loop_run(
    AxlLoop *loop  ///< event loop
);

/**
 * @brief Drive the loop's dispatch from a firmware-managed periodic
 *     timer (DXE driver mode).
 *
 * `axl_loop_run` is the foreground driver — it owns `TPL_APPLICATION`
 * and blocks in `gBS->WaitForEvent`. UEFI driver entry points have
 * no foreground caller: `DriverEntry` returns to the firmware after
 * publishing protocols. Without a foreground caller, sources never
 * dispatch and timers never fire, so anything async in the loop is
 * dead.
 *
 * `axl_loop_attach_driver` installs a periodic firmware-managed
 * `EVT_TIMER | EVT_NOTIFY_SIGNAL` event at `TPL_CALLBACK` whose
 * notify drains the loop in non-blocking mode every @p interval_ms.
 * Idle callbacks, defer-queue work, and source events all dispatch
 * from this notify exactly as they would inside `axl_loop_run`.
 * `DriverEntry` calls this and returns; `DriverUnload` calls
 * `axl_loop_detach_driver`.
 *
 * **TPL contract.** UEFI 2.11 §7.1 allows only `TPL_CALLBACK` or
 * `TPL_NOTIFY` for `EVT_NOTIFY_SIGNAL` events — there is no signal
 * queue at `TPL_APPLICATION`. We use `TPL_CALLBACK`. Co-located
 * firmware drivers (TCP4 / MNP / SNP) run their own state machines
 * at the same `TPL_CALLBACK` level, so the FIFO notify queue
 * alternates fairly between them and us as long as **our notify
 * stays short**.
 *
 * **Notify-budget rule.** The consumer's loop sources must run
 * fast. Each tick runs at `TPL_CALLBACK` and drains every source
 * with a signaled event, calling each callback exactly once before
 * returning (capped at 2× `AXL_MAX_SOURCES` per tick as a runaway
 * guard — hitting the cap is logged). If a source callback does
 * heavy work
 * (large allocation, synchronous I/O, blocking protocol calls), it
 * holds `TPL_CALLBACK` for that whole duration and starves
 * co-located firmware drivers that need the same TPL — at best you
 * see latency spikes, at worst connection-refused on a co-located
 * TCP4. Keep source callbacks under ~1 ms; defer slow work via
 * `axl_defer_call_later` to break it up across ticks.
 *
 * **Boot Services TPL ceiling.** `gBS->WaitForEvent` is unavailable
 * above `TPL_APPLICATION`, so the dispatch is non-blocking-only.
 * The sources you can use safely from driver mode are the same
 * sources `axl_loop_run` supports (timers, idle, raw events,
 * pubsub) — anything that would internally call `WaitForEvent`
 * (notably `axl_loop_iterate_until` with a non-zero timeout) is
 * not safe inside a source callback.
 *
 * Typical period: 50 ms — frequent enough for a responsive HTTP
 * server, sparse enough to leave headroom. Pick lower for
 * latency-sensitive pubsub delivery; pick higher for cost-sensitive
 * idle workloads.
 *
 * Idempotent-fail: returns AXL_ERR if the loop is already attached
 * (call `axl_loop_detach_driver` first to change the period).
 *
 * @return AXL_OK on success, AXL_ERR if @p loop is NULL, already
 *     attached, or the firmware refused the timer.
 */
int
axl_loop_attach_driver(
    AxlLoop  *loop,         ///< loop to attach (must already exist)
    uint64_t  interval_ms   ///< dispatch period in ms (typical: 50)
);

/**
 * @brief Tear down a driver-mode loop attachment.
 *
 * Cancels the periodic timer, drains any in-flight notify, and
 * frees the timer's bridging context. Pair with
 * `axl_loop_attach_driver` from `DriverUnload`. NULL-safe; safe to
 * call on a loop that was never attached (returns AXL_ERR).
 *
 * Order in `DriverUnload`: detach the loop FIRST, then unregister
 * any protocols, then free the loop. Detaching first guarantees no
 * notify is in flight when consumer state goes away.
 *
 * @return AXL_OK on success, AXL_ERR if not currently attached.
 */
int
axl_loop_detach_driver(
    AxlLoop  *loop          ///< loop to detach
);

// ---------------------------------------------------------------------------
// Event sources (return source ID, 0 on failure)
// ---------------------------------------------------------------------------

/**
 * @brief Add a repeating timer.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_timer(
    AxlLoop        *loop,        ///< event loop
    uint32_t        interval_ms, ///< timer interval in milliseconds
    AxlLoopCallback cb,          ///< callback fired each interval
    void           *data         ///< opaque data
);

/**
 * @brief Add a one-shot timeout (auto-removed after firing).
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_timeout(
    AxlLoop        *loop,     ///< event loop
    uint32_t        delay_ms, ///< timeout delay in milliseconds
    AxlLoopCallback cb,       ///< callback fired on timeout (one-shot, auto-removed)
    void           *data      ///< opaque data
);

/**
 * @brief Add a key press handler.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_key_press(
    AxlLoop       *loop, ///< event loop
    AxlKeyCallback cb,   ///< key press callback
    void          *data  ///< opaque data
);

/**
 * @brief Add an idle callback (fired every iteration before wait).
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_idle(
    AxlLoop        *loop, ///< event loop
    AxlLoopCallback cb,   ///< idle callback (fired every iteration before wait)
    void           *data  ///< opaque data
);

/**
 * @brief Add a protocol install notification.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_protocol_notify(
    AxlLoop        *loop, ///< event loop
    void           *guid, ///< protocol GUID to watch (void* to avoid EFI_GUID in header)
    AxlLoopCallback cb,   ///< callback on protocol install
    void           *data  ///< opaque data
);

/**
 * @brief Add a raw event handle to the loop.
 *
 * Fires cb when the event is signaled. The caller owns the event —
 * the loop does NOT close it on removal. Use this to integrate
 * TCP completion tokens, custom protocol events, or any EFI_EVENT
 * into the main loop without polling.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
uint32_t
axl_loop_add_event(
    AxlLoop        *loop,   ///< event loop
    AxlEventHandle  event,  ///< event handle (from axl_event_handle or a firmware-owned EFI_EVENT)
    AxlLoopCallback cb,     ///< callback when event is signalled
    void           *data    ///< opaque data
);

/**
 * @brief Remove an event source by ID.
 */
void
axl_loop_remove_source(
    AxlLoop  *loop,      ///< event loop
    uint32_t  source_id  ///< ID returned by axl_loop_add_*
);

// ---------------------------------------------------------------------------
// Nested-wait primitive
// ---------------------------------------------------------------------------

/**
 * @brief Iterate a running loop until an event fires or a timeout elapses,
 *        without quitting the loop.
 *
 * This is the nested-wait primitive for callers inside a loop callback
 * that need to wait for a producer to signal completion. Unlike
 * axl_event_wait_timeout (which spins up a throwaway loop and freezes
 * the caller's outer loop), this function drives the caller's own
 * loop — the outer loop's existing sources keep firing for the
 * duration of the wait. It does NOT set the loop's quit flag, so
 * the enclosing axl_loop_run (if any) resumes normally after this
 * returns.
 *
 * Typical use: a source callback that needs to wait on an async
 * producer without starving the rest of the loop's timers.
 *
 * @return 0 if @a done was signalled, -1 on timeout, AXL_CANCELLED on
 *     Ctrl-C or invalid argument.
 */
int
axl_loop_iterate_until(
    AxlLoop  *loop,       ///< loop to drive (caller's outer loop, typically)
    AxlEvent *done,       ///< event to wait on (NULL = only timeout/cancel wakes)
    uint64_t  timeout_us  ///< timeout in microseconds (0 = no timeout, wait forever)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_LOOP_H */
