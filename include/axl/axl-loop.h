/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-loop.h
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

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-event.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlLoop AxlLoop;   ///< Opaque event loop.

/**
 * AxlSourceId:
 *
 * Opaque handle for a registered loop source, returned by the
 * `axl_loop_add_*` functions and passed to `axl_loop_remove_source`.
 * 0 is never a valid id (it means "no source").
 *
 * Ids are allocated from a single PROCESS-GLOBAL monotonic counter, so
 * every live source across every loop has a distinct id. This is what
 * makes a stale id (one that outlived its loop) safe to pass to
 * `axl_loop_remove_source` on a different loop: it matches nothing, so
 * the removal is a no-op rather than deleting an unrelated source.
 * 64-bit so the counter never wraps in any realistic process lifetime.
 */
typedef uint64_t AxlSourceId;

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
 * Keyboard input. `scan_code` / `unicode_char` mirror UEFI
 * EFI_INPUT_KEY; `modifiers` carries the normalized held-modifier and
 * lock state (AXL_INPUT_MOD_* bits from <axl/axl-input.h>), or 0 when
 * the platform can't report it (no SimpleTextInputEx / serial).
 */
typedef struct {
    uint16_t scan_code;    ///< function/arrow key scan code (0 for printable chars)
    uint16_t unicode_char; ///< printable character (0 for special keys)
    uint32_t modifiers;    ///< AXL_INPUT_MOD_* held + lock state (0 if unavailable)
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
) AXL_CB_NOEXCEPT;

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
) AXL_CB_NOEXCEPT;

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
 * @brief Control whether a bare Ctrl-C quits the loop.
 *
 * When intercepting, the loop treats a modifier-less Ctrl-C
 * (`UnicodeChar == 0x03, KeyShiftState == 0` — what a serial/TerminalDxe
 * console emits) and the shell break event as "quit the loop". A GUI app
 * that wants Ctrl+C for its own use (an editor mapping it to Copy) sets
 * this @c off: the 0x03 byte is then delivered to the app's keypress
 * source instead, and the shell break event is drained but ignored (so a
 * stale break cannot end the app at a later library yield). The app is then
 * responsible for its own exit affordance (a Quit command / Ctrl+Q).
 *
 * **Default.** With no call to this function, a loop intercepts only in
 * the foreground: a loop in an APPLICATION image that is not attached with
 * axl_loop_attach_driver(). It does NOT intercept while driver-attached, and
 * a loop created in a DRIVER image never intercepts by default, attached or
 * not — that includes the short-lived loop a synchronous wait builds
 * (axl_wait_ms(), the sync TCP/UDP/DNS wrappers). Resident code is not the
 * foreground, and the Shell's break belongs to whatever the Shell runs. A
 * resident loop that does not intercept never reads the shell break event
 * at all — reading consumes it, and resident code would otherwise swallow
 * every Ctrl-C meant for the Shell's script runner. Detaching an app's loop
 * restores the foreground default. An explicit call wins in every case;
 * opting a driver-attached loop in means the Shell's next Ctrl-C stops it
 * dispatching for the rest of its residency.
 *
 * The trade-off is deliberate: a driver whose entry point blocks in a
 * synchronous wait can no longer be aborted with Ctrl-C, and library work in
 * a driver image never tries to end the image on a break (a resident image
 * cannot exit). Bound such waits with a timeout or an AxlCancellable.
 *
 * @note Modified-bit Ctrl+C (a real keyboard reporting the CTRL state)
 *     was never intercepted and is unaffected by this flag.
 */
void
axl_loop_set_intercept_break(
    AxlLoop *loop,        ///< event loop
    bool     intercept    ///< true = Ctrl-C quits; false = deliver (default: see above)
);

/**
 * @brief Query whether the loop intercepts Ctrl-C right now (see
 *     axl_loop_set_intercept_break): the explicit choice if one was made,
 *     otherwise true in the foreground (an app image's loop that is not
 *     driver-attached) and false while driver-attached or in a driver image.
 */
bool
axl_loop_intercept_break(
    AxlLoop *loop  ///< event loop
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
 * Idle callbacks all run first; then ONE signalled source is selected
 * for the next axl_loop_dispatch_event. Selection is round-robin: each
 * call starts looking just past the source selected last, so a source
 * that stays signalled (one that re-signals its own event, or a
 * listener under a connection flood) is selected again only after every
 * other signalled source has had a turn. No other order is promised.
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
 * **Ctrl-C.** While attached, the loop leaves the Shell's break alone
 * unless axl_loop_set_intercept_break() opted it in; see there.
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
 * fast. Each tick runs at `TPL_CALLBACK` and dispatches signalled
 * sources one at a time, in the round-robin order of
 * axl_loop_next_event, until none is signalled (capped at
 * 2× `AXL_MAX_SOURCES` dispatches per tick as a runaway guard —
 * hitting the cap is logged). A source that re-signals itself is
 * dispatched again within the tick, but only after every other
 * signalled source has had its turn. If a source callback does
 * heavy work
 * (large allocation, synchronous I/O, blocking protocol calls), it
 * holds `TPL_CALLBACK` for that whole duration and starves
 * co-located firmware drivers that need the same TPL — at best you
 * see latency spikes, at worst connection-refused on a co-located
 * TCP4. Keep source callbacks under ~1 ms; defer slow work via
 * axl_defer() (`<axl/axl-defer.h>`) to break it up across ticks.
 *
 * **Boot Services TPL ceiling.** `gBS->WaitForEvent` is unavailable
 * above `TPL_APPLICATION`, so this tick's own dispatch is
 * non-blocking-only. A nested blocking wait reached from a source
 * callback — a synchronous network op (`axl_udp_send`,
 * `axl_http_post`, a DNS lookup) or `axl_loop_iterate_until` with a
 * timeout, both of which spin up a nested `axl_loop_run` — is still
 * **safe**: the backend wait detects the raised TPL and falls back to
 * a `CheckEvent` sweep instead of `WaitForEvent`, so it makes progress
 * and returns rather than wedging. It becomes a **latency** concern
 * instead of a safety one, but the blast radius is wide: the nested
 * wait busy-holds `TPL_CALLBACK` for its whole duration (the
 * notify-budget rule above), which stalls *every other connection
 * serviced by the same pump*, not just the current one. A wait that
 * carries a timeout self-limits to that deadline (the sync network ops
 * all pass one); a deadline-less wait whose condition never resolves
 * holds `TPL_CALLBACK` indefinitely — so always give such waits a
 * timeout, keep them short, or gate the slow op off the driver pump.
 *
 * Typical period: 50 ms — frequent enough for a responsive HTTP
 * server, sparse enough to leave headroom. Pick lower for
 * latency-sensitive pubsub delivery; pick higher for cost-sensitive
 * idle workloads. Prefer a multiple of the firmware's timer tick (10 ms
 * on EDK2): EDK2 fires a periodic timer on the first system tick at or
 * after its due time, so a 15 ms period really alternates 20 and 10 ms,
 * and an extended tick (axl_loop_set_driver_idle_budget_ms()) that times
 * its stop from the period then sometimes runs into the next one.
 *
 * Busy idle sources (axl_loop_add_idle_busy()) extend a tick for up to
 * the loop's driver budget (axl_loop_set_driver_budget_ms()), or for its
 * idle budget (axl_loop_set_driver_idle_budget_ms()) when the tick finds
 * the foreground waiting. Attaching may stall once for 10 ms on x64, to
 * calibrate the clock the budgets read before any tick needs it. It
 * also joins EDK2's idle-loop event group, to learn when the foreground
 * is waiting; firmware that refuses or never signals the group only
 * leaves every tick at the busy-foreground budget. The loop's two members
 * of the group take two more slots of the backend's notify table, beside
 * the tick's timer.
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

/**
 * @brief Default per-tick budget of a driver-mode loop, in ms; see
 *     axl_loop_set_driver_budget_ms().
 *
 * Against a typical 50 ms tick, the extension adds about a tenth of the
 * tick (5 ms plus one pass), and only while a busy idle source exists.
 */
#define AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS  5u

/**
 * @brief Largest budget axl_loop_set_driver_budget_ms() accepts, in ms.
 *
 * Half of a typical 50 ms tick. The budget actually used is also
 * clamped to half of the attached interval (see
 * axl_loop_set_driver_budget_ms()), so with the foreground busy the
 * EXTENSION never exceeds about half a tick plus one pass, whatever the
 * period. It is also the largest idle budget
 * (axl_loop_set_driver_idle_budget_ms()) and that budget's default:
 * with the foreground waiting, a tick extends to at most nine tenths of
 * the interval and never past this value. Time spent in signalled
 * callbacks is not the extension's and is not bounded by it.
 */
#define AXL_LOOP_DRIVER_BUDGET_MAX_MS      25u

/**
 * @brief Set how long a driver-mode tick keeps iterating while a busy
 *     idle source exists.
 *
 * A busy idle source (axl_loop_add_idle_busy()) is one whose work
 * depends on being called often -- it polls something that makes
 * progress only while polled. `axl_loop_run` calls it on every pass.
 * A driver-mode tick (axl_loop_attach_driver) drains until nothing is
 * signalled, calling idle sources once per pass, and then returns, so
 * whatever the source is waiting for tends to arrive after the tick has
 * ended.
 *
 * With a budget, a tick in which at least one busy idle source is
 * active keeps making passes -- idle callbacks, then any signalled
 * source, in the normal round-robin. This budget bounds every tick that
 * does not find the foreground waiting: min(@p ms, attached interval /
 * 2). A tick that does find it waiting uses the idle budget instead
 * (axl_loop_set_driver_idle_budget_ms()). Plain idle sources
 * (axl_loop_add_idle) never extend a tick, and a tick with no busy idle
 * source is unchanged. 0 means no extension at all, whatever the idle
 * budget. A new loop starts with #AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS.
 *
 * **When a tick ends.** At the first pass that dispatches nothing once
 * neither a busy idle source nor deferred work (axl_defer()) remains, or
 * the budget has elapsed; or when the dispatch cap (2 x `AXL_MAX_SOURCES`
 * signalled dispatches) is reached; or on quit. Deferred work keeps a
 * tick going as a busy idle source does -- work a deferred callback
 * defers runs on the next pass, not the next tick -- within the same
 * budget, and with a budget of 0 it waits for the next tick. Signalled
 * work still drains after the budget has elapsed, exactly as in a tick
 * without one.
 *
 * **What the budget bounds.** Only when the tick stops STARTING passes.
 * It does not bound a signalled callback's own time (their NUMBER is
 * capped, not their duration), nor the overrun of the last pass, nor a
 * nested wait inside a tick callback: such a wait (axl_loop_iterate_until,
 * a synchronous axl_udp_send or axl_http_post) runs busy idle sources
 * like plain ones until it returns. Passes that find only idle work do
 * not count toward the dispatch cap, nor toward the sustained-overload
 * warning it feeds.
 *
 * **The clock.** If the monotonic clock cannot be read, the tick is not
 * extended, as with a budget of 0. The clock's one-time calibration (a
 * 10 ms stall on x64, the first time a process reads it) is done by this
 * call and by axl_loop_attach_driver, so the loop itself never pays it
 * inside a tick; if that calibration failed, ticks never read the clock
 * and are not extended. A failed calibration is not remembered by the
 * clock, so any OTHER reader in a tick callback pays the stall again --
 * AxlTcp's transmit pacing reads it at most once more, then stops.
 *
 * **The trade-off.** The whole tick runs at TPL_CALLBACK, and nothing
 * below that level runs while it does. The extension adds at most about
 * the budget plus one pass per tick -- so a busy foreground Shell sharing
 * the CPU loses about that much per tick period (about 10 % at the
 * defaults on a 50 ms tick), and only while a busy idle source exists.
 * Time in signalled callbacks and nested waits is governed by the
 * notify-budget rule on axl_loop_attach_driver, as before. Firmware
 * drivers at TPL_CALLBACK (TCP4, MNP, SNP) share the level.
 *
 * May be called at any time, attached or not; it takes effect from the
 * next tick and has no effect on `axl_loop_run` or `axl_loop_dispatch`.
 * @p ms is uint32_t while axl_loop_attach_driver's interval is
 * uint64_t; the range here is small enough that the narrower type
 * loses nothing.
 *
 * @return AXL_OK; AXL_INVALID if @p loop is NULL or @p ms exceeds
 *     #AXL_LOOP_DRIVER_BUDGET_MAX_MS (nothing changed).
 */
AXL_WARN_UNUSED int
axl_loop_set_driver_budget_ms(
    AxlLoop  *loop,  ///< loop (attached or not)
    uint32_t  ms     ///< busy-foreground budget; 0 = no extension at all
);

/**
 * @brief Set how long a driver-mode tick may extend when it finds the
 *     foreground waiting.
 *
 * The half-interval clamp of axl_loop_set_driver_budget_ms() keeps a
 * resident service from taking more than half the CPU from a foreground
 * that wants it. A foreground that is waiting -- a Shell at its prompt,
 * an application in gBS->WaitForEvent -- wants none of it, and the
 * clamp then only idles the CPU: measured with sshd (10 ms tick), an
 * SFTP download was held to half the CPU while the other half sat idle,
 * and its speed followed the budget in proportion.
 *
 * **When a tick counts the foreground as waiting.** Only when it has
 * been waiting -- not running -- for the whole of the two periods before
 * the tick. EDK2 signals its idle-loop event group from every idle pass
 * of gBS->WaitForEvent and then halts the CPU until the next interrupt;
 * the loop notes each pass and each halt's end, and a period counts as
 * idle when the foreground was waiting at the tick that began it, its
 * every pass in it came at once after the end of that tick or of a halt
 * (it never ran), and its last halt ended in the interrupt that queued
 * this tick (it is still waiting). Waiting at
 * the tick instants is not enough: EDK2 fires every timer on its 10 ms
 * system tick, so a foreground's wake lands on the very interrupt that
 * queues a driver tick, and an extension there runs before the woken
 * foreground does -- measured, a renderer doing 4 ms of work per frame
 * beside a 10 ms tick ran at half its frame rate. Two whole idle periods,
 * not one, because a foreground that waited through one period and wakes
 * on the next interrupt would still lose that tick. The Shell at its
 * prompt waits through every period and extends from the third tick
 * after it goes idle -- once two whole periods have passed. A tick that
 * was queued behind the loop's own previous one (which ran into its
 * interrupt) gave the foreground no chance to run in between: it does
 * not extend, and it leaves that record as it was, so the tick after
 * it extends again. "Queued" means no halt ended since the previous tick
 * did and this one started within 1 ms of its end -- so a foreground
 * burst of under about 1.5 ms right after an extended tick also goes
 * uncounted. This rests on EDK2 mechanisms, not UEFI ones: firmware that does not signal the
 * group, orders its members otherwise, or has no cycle counter gives
 * every tick the busy-foreground budget.
 *
 * **The budget used.** In a tick that finds the foreground waiting:
 * min(@p ms, attached interval x 9/10), and never less than the
 * busy-foreground budget that tick would have had -- the foreground
 * keeps at least a tenth of the period. Within that, an extended tick
 * stops starting passes once one more, as long as the last, would end
 * less than half a millisecond before the next tick is due (never below
 * the busy-foreground budget), so the foreground gets the CPU and the
 * next tick finds it waiting. 0 turns the idle extension off,
 * so every tick uses the busy-foreground budget. A budget of 0 from
 * axl_loop_set_driver_budget_ms() turns off ALL extension and overrides
 * this one. A new loop starts with #AXL_LOOP_DRIVER_BUDGET_MAX_MS, which
 * makes the default min(interval x 9/10, #AXL_LOOP_DRIVER_BUDGET_MAX_MS):
 * 9 ms of a 10 ms tick, 25 ms of a 50 ms one.
 *
 * **The trade-off.** A waiting foreground answers a key up to one
 * extended tick late: the extension is at most this budget, plus the
 * overrun of the last pass and the time in signalled callbacks
 * described on axl_loop_set_driver_budget_ms(). A consumer that needs a
 * tighter bound (a GUI host promising keypress-to-redraw latency) sets a
 * smaller value AND a driver budget no larger -- the idle budget never
 * goes below the busy one, so an idle budget of 1 still gives the default
 * 5 ms. Everything else axl_loop_set_driver_budget_ms() says
 * about how a tick ends, the clock, and TPL_CALLBACK applies here too.
 *
 * May be called at any time, attached or not; it takes effect from the
 * next tick and has no effect on `axl_loop_run` or `axl_loop_dispatch`.
 *
 * @return AXL_OK; AXL_INVALID if @p loop is NULL or @p ms exceeds
 *     #AXL_LOOP_DRIVER_BUDGET_MAX_MS (nothing changed).
 */
AXL_WARN_UNUSED int
axl_loop_set_driver_idle_budget_ms(
    AxlLoop  *loop,  ///< loop (attached or not)
    uint32_t  ms     ///< waiting-foreground budget; 0 = no idle extension
);

// ---------------------------------------------------------------------------
// Event sources (return source ID, 0 on failure)
// ---------------------------------------------------------------------------

/**
 * @brief Add a repeating timer.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
AxlSourceId
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
AxlSourceId
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
AxlSourceId
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
AxlSourceId
axl_loop_add_idle(
    AxlLoop        *loop, ///< event loop
    AxlLoopCallback cb,   ///< idle callback (fired every iteration before wait)
    void           *data  ///< opaque data
);

/**
 * @brief Add a BUSY idle callback: an idle source whose work needs it
 *     called often, not merely once per wake-up.
 *
 * It behaves exactly like a plain idle source (axl_loop_add_idle())
 * everywhere except the driver tick's own loop: it is called once per
 * pass, before the wait, and while it exists the loop does not block.
 * In driver mode, a tick in which at least one busy idle source is
 * active keeps making passes for up to the loop's driver budget
 * (axl_loop_set_driver_budget_ms()); plain idle sources never extend a
 * tick. Use it for a source that polls something which progresses only
 * while polled, and remove it as soon as there is nothing to poll --
 * while it exists, every driver tick spends its whole budget. Remove it
 * with axl_loop_remove_source(), or by returning #AXL_SOURCE_REMOVE
 * from the callback. It takes one slot of the loop's source table
 * (`AXL_MAX_SOURCES`), like any source.
 *
 * @return source ID for axl_loop_remove_source, or 0 if @p loop or
 *     @p cb is NULL or the source table is full.
 */
AxlSourceId
axl_loop_add_idle_busy(
    AxlLoop        *loop, ///< event loop
    AxlLoopCallback cb,   ///< callback, fired on every pass before the wait
    void           *data  ///< opaque data
);

/**
 * @brief Add a protocol install notification.
 *
 * @return source ID for axl_loop_remove_source, or 0 on failure.
 */
AxlSourceId
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
AxlSourceId
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
    AxlLoop     *loop,      ///< event loop
    AxlSourceId  source_id  ///< ID returned by axl_loop_add_*
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
 * Fails closed: if @a done is non-NULL, or @a timeout_us is nonzero, and
 * the corresponding wake source cannot be registered (the loop's fixed
 * source table is full), nothing is waited on -- any source that WAS
 * registered is removed and AXL_NO_RESOURCES is returned immediately.
 * A caller must treat any result other than the one it was actually
 * waiting for as failure; do not assume AXL_TIMEOUT means "waited and
 * timed out" without also checking for AXL_NO_RESOURCES.
 *
 * @return AXL_OK if @a done was signalled; AXL_TIMEOUT if the deadline
 *     elapsed first; AXL_CANCELLED on Ctrl-C or a NULL @a loop;
 *     AXL_NO_RESOURCES if a requested wake source could not be
 *     registered.
 */
AxlStatus
axl_loop_iterate_until(
    AxlLoop  *loop,       ///< loop to drive (caller's outer loop, typically)
    AxlEvent *done,       ///< event to wait on (NULL = only timeout/cancel wakes)
    uint64_t  timeout_us  ///< timeout in microseconds (0 = no timeout, wait forever)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_LOOP_H */
