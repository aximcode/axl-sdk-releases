/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-loop.c
    GLib-style event loop for UEFI. Supports timers, keyboard input,
    idle callbacks, protocol notifications, and built-in Ctrl-C.
    FUSE-style next_event/dispatch_event primitives for advanced control.

    Migrated from AxlLoop.c(EDK2-style) to GLib-style API.
**/

#include "../backend/axl-backend.h"   /* axl_backend_notify_track_state */
#include "axl-loop-internal.h"
#include "../runtime/axl-atexit-internal.h"   /* _axl_atexit_is_driver_image */
#include "../runtime/axl-registry-internal.h"
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("loop");

/* event_to_source[] markers for the loop's own events in a wait array.
   Every other entry is the slot index of a user source. */
#define WAIT_POLL_TIMER  ((size_t)-1)
#define WAIT_BREAK       ((size_t)-2)
#define WAIT_KEYPRESS    ((size_t)-3)

/* "At once", for the idle detection in driver_dispatch_notify: a halt that
   ended this long before a tick ended in the interrupt that queued it, and
   an idle pass this long after the end of a tick or of a halt means the
   foreground did not run in between. Microseconds separate the real
   events; a foreground that did run is a timer period (10 ms on EDK2) or
   a piece of its work away. */
#define DRIVER_IDLE_RECENT_US  1000u

/* An extended tick does not start a pass that, as long as the one before
   it, would end less than this before the next tick is due. A tick that
   runs into the next one's interrupt has that tick queued behind it, and
   the queued tick finds no fresh halt -- the foreground never got the CPU
   in between -- so it reads the foreground as busy, and the extension
   takes three more ticks to come back. Measured: one tick in three
   extended with 4 ms passes at a 20 ms tick. A long signalled callback
   still overruns -- nothing predicts it -- which is what the queued-tick
   rule in driver_dispatch_notify is for. */
#define DRIVER_TICK_GUARD_US   500u

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

/* Source ids come from a single PROCESS-GLOBAL monotonic counter, not a
   per-loop one. Per-loop ids (each loop counting from 1) put the same id on
   every loop, so a source id that outlived its loop — e.g. one registered on an
   ephemeral sync-wrapper loop (axl_tcp_recv/send) — could be removed from a
   DIFFERENT loop and silently delete an unrelated source: the adbf5461
   second-server-dead-accept class. With a global counter a stale id never
   matches a source on another loop, so the cross-loop removal is a harmless
   no-op and the whole class is unrepresentable. AxlSourceId is 64-bit so the
   counter never wraps in any realistic process lifetime (id consumption is per
   source REGISTRATION, not per firing); 0 stays the "no source" sentinel and is
   skipped on the (unreachable) wrap. Single-threaded UEFI: no locking needed. */
static AxlSourceId g_next_source_id = 1;

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static void driver_events_close(AxlLoop *loop);
static uint64_t driver_period_us(const AxlLoop *loop);

/* Every driver-attached loop, for _axl_loop_driver_fast; and the tick it
   asks for (0: none). Both touched only at TPL_CALLBACK, where the latency
   policy's own timer notify reads them. */
static AxlLoop  *g_driver_loops;
static uint32_t  g_driver_fast_us;
static void driver_idle_reset(AxlLoop *loop);

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static AxlSourceId
add_source(AxlLoop *loop, SourceType type, AxlEventHandle event,
           bool owns_event, void *callback, void *data)
{
    LoopSource *src;
    size_t i;

    if (loop == NULL || callback == NULL) {
        return 0;
    }

    /* SOURCE_EVENT slot reuse drops Cancel + CloseEvent on the cb's
       behalf, so a caller passing owns_event=true would leak the EFI
       event when the slot is later reused. No such caller exists
       today; reject defensively. */
    if (type == SOURCE_EVENT && owns_event) {
        axl_error("add_source: SOURCE_EVENT with owns_event=true is unsupported");
        return 0;
    }

    /* Reuse inactive slots before appending */
    src = NULL;
    for (i = 0; i < loop->source_count; i++) {
        if (!loop->sources[i].active) {
            src = &loop->sources[i];
            break;
        }
    }

    if (src == NULL) {
        if (loop->source_count >= AXL_MAX_SOURCES) {
            axl_error("add_source: AXL_MAX_SOURCES (%d) exhausted - "
                      "registration failed (type=%d). Bump the limit "
                      "or audit callers leaking sources.",
                      AXL_MAX_SOURCES, (int)type);
            return 0;
        }
        src = &loop->sources[loop->source_count];
        loop->source_count++;
    }

    if (g_next_source_id == 0) {
        g_next_source_id = 1;   /* skip the 0 sentinel (64-bit: never reached) */
    }
    src->id = g_next_source_id++;
    src->type = type;
    src->active = true;
    src->owns_event = owns_event;
    src->busy = false;
    src->event = event;
    src->fn.cb = (AxlLoopCallback)callback;
    src->data = data;

    return src->id;
}

static void
fire_cleanups(AxlLoop *loop)
{
    size_t i;

    for (i = 0; i < loop->cleanup_count; i++) {
        if (loop->cleanups[i] != NULL) {
            loop->cleanups[i](loop->cleanup_data[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/* Registry destructor wrapper — passed to _axl_registry_add so the registry
 * sweep frees the loop without statically referencing axl_loop_free (lets
 * --gc-sections drop the loop from apps that never create one). */
static void
loop_registry_dtor(void *resource)
{
    axl_loop_free((AxlLoop *)resource);
}

AxlLoop *
axl_loop_new_impl(const char *file, int line)
{
    AxlLoop  *loop;

    loop = axl_calloc(1, sizeof(AxlLoop));
    if (loop == NULL) {
        return NULL;
    }

    loop->pending_source = -1;
    loop->defer_next_id = 1;
    loop->pubsub_next_sub_id = 1;
    loop->driver_budget_ms = AXL_LOOP_DRIVER_BUDGET_DEFAULT_MS;
    loop->driver_idle_budget_ms = AXL_LOOP_DRIVER_BUDGET_MAX_MS;

    axl_ring_buf_init_fixed(&loop->defer_ring, loop->defer_buf,
                          sizeof(loop->defer_buf), sizeof(DeferEntry),
                          0, NULL);

    /* Poll timer for periodic deferred-work draining and (if no shell)
       Ctrl-C detection.  When the shell is available, the break event
       gives immediate Ctrl-C response without polling. */
    if (axl_backend_event_create_timer(&loop->poll_timer) != AXL_OK) {
        axl_error("failed to create poll timer");
        axl_free(loop);
        return NULL;
    }
    axl_backend_event_set_timer(loop->poll_timer, AXL_TIMER_PERIODIC,
                                POLL_INTERVAL_MS * MS_TO_100NS);

    loop->break_event = axl_backend_shell_break_event();

    /* Borrow ConIn->WaitForKey for serial-Ctrl-C interception. The
     * loop adds it to the WaitForEvent array (when no user keypress
     * source is registered) so a 0x03 byte from a serial console wakes
     * the loop and we can signal break — fully event-driven, no
     * RegisterKeyNotify (which would put OVMF's ConSplitter into a
     * TPL_NOTIFY-level key polling loop and starve TCP4). */
    loop->keypress_event = axl_backend_console_wait_for_key();

    loop->_registry_handle =
        _axl_registry_add(AXL_RES_LOOP, loop, loop_registry_dtor, file, line);

    return loop;
}

void
axl_loop_free(AxlLoop *loop)
{
    size_t i;
    size_t leaked_external = 0;

    if (loop == NULL) {
        return;
    }

    /* Freeing a loop while a SOURCE_EVENT with caller-owned handle is
       still active is a lifetime ordering bug: the consumer holds both
       this loop pointer and a live source id. If it later calls
       axl_loop_remove_source on us we dereference freed memory. That
       is exactly the UAF that hit AxlTestNet's UDP-async teardown in
       commit de64928. Warn loudly here so the next occurrence surfaces
       immediately instead of as a poisoned-memory #GP downstream.

       Note: SOURCE_IDLE also has owns_event=false, but it has no event
       handle and no consumer-facing source id risk (idle callbacks
       that never returned AXL_SOURCE_REMOVE are fine — the loop just
       stops dispatching them). Same for pending SOURCE_PROTOCOL. The
       narrow hazard is SOURCE_EVENT with owns_event=false. */
    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active &&
            loop->sources[i].type == SOURCE_EVENT &&
            !loop->sources[i].owns_event) {
            leaked_external++;
            axl_error("axl_loop_free: caller-owned event source id=%llu "
                      "still active - tear down the resource "
                      "(socket/tcp/custom event) BEFORE freeing the "
                      "loop it was registered against",
                      (unsigned long long)loop->sources[i].id);
        }
        if (loop->sources[i].active && loop->sources[i].owns_event &&
            loop->sources[i].event != NULL) {
            axl_backend_event_close(loop->sources[i].event);
        }
        /* A busy idle source polls something on this loop's behalf and is
           removed by its owner when that work ends. Still active here means
           the owner still believes it has work on a loop that is going away
           -- the same ordering bug as above, one step removed. */
        if (loop->sources[i].active && loop->sources[i].type == SOURCE_IDLE &&
            loop->sources[i].busy) {
            axl_error("axl_loop_free: busy idle source id=%llu still active - "
                      "finish or cancel the work it polls for BEFORE freeing "
                      "the loop",
                      (unsigned long long)loop->sources[i].id);
        }
    }
    /* AxlTcp finalizes a loop's graceful closes from one sweeper rather than
       a source per close, so a close still pending -- the same ordering bug
       the per-close source used to show -- is reported here in the same
       words, which the rebind and teardown tests look for. */
    if (loop->tcp_close_sweeper != 0 && _axl_loop_has_source(loop, loop->tcp_close_sweeper)) {
        leaked_external++;
        axl_error("axl_loop_free: caller-owned event source id=%llu "
                  "still active - TCP closes are pending on this loop; tear "
                  "down the sockets (or let their closes complete) BEFORE "
                  "freeing the loop",
                  (unsigned long long)loop->tcp_close_sweeper);
    }
    if (leaked_external > 0) {
        axl_error("axl_loop_free: %zu caller-owned event source(s) "
                  "still active - free will proceed but consumers may "
                  "crash on next use",
                  leaked_external);
    }

    if (loop->poll_timer != NULL) {
        axl_backend_event_close(loop->poll_timer);
    }

    /* Driver-mode safety net: if the consumer forgot to call
       axl_loop_detach_driver, do it here so the periodic timer
       doesn't fire into freed-loop memory. axl_backend_event_close
       cancels the timer + closes + frees the bridge context. */
    if (loop->driver_timer != NULL) {
        axl_warning("axl_loop_free: detaching abandoned driver-mode "
                    "timer - DriverUnload should call "
                    "axl_loop_detach_driver before axl_loop_free");
        driver_events_close(loop);
    }

    _axl_registry_remove(loop->_registry_handle);
    axl_pubsub_reset_internal(loop);
    axl_free(loop);
}

void
axl_loop_quit(AxlLoop *loop)
{
    if (loop != NULL) {
        loop->quit_requested = true;
    }
}

bool
axl_loop_is_running(AxlLoop *loop)
{
    if (loop == NULL) {
        return false;
    }

    return loop->running && !loop->quit_requested;
}

/* Whether this loop runs in the FOREGROUND: an application image's loop that
   is not driver-attached. Anything a driver image runs is resident code, not
   the foreground -- including a short-lived loop it creates and never
   attaches (a sync wait, a sync TCP/UDP/DNS wrapper), which the Shell's break
   would otherwise end, and whose read would take that break from the Shell. */
static bool
loop_is_foreground(const AxlLoop *loop)
{
    return loop->driver_timer == NULL && !_axl_atexit_is_driver_image();
}

/* Whether Ctrl-C means "quit this loop". An explicit choice wins. Without
   one, a foreground loop intercepts and resident code does not: the Shell's
   Ctrl-C belongs to whatever the Shell is running, and a quit would stop a
   driver's loop for good (the tick re-arms `running` but never clears
   quit_requested). */
static bool
loop_intercepts_break(const AxlLoop *loop)
{
    if (loop->intercept_break_set) {
        return loop->intercept_break;
    }
    return loop_is_foreground(loop);
}

void
axl_loop_set_intercept_break(AxlLoop *loop, bool intercept)
{
    if (loop != NULL) {
        loop->intercept_break     = intercept;
        loop->intercept_break_set = true;
    }
}

bool
axl_loop_intercept_break(AxlLoop *loop)
{
    return loop != NULL && loop_intercepts_break(loop);
}

void
axl_loop_add_cleanup(AxlLoop *loop, AxlLoopCallback cb, void *data)
{
    if (loop == NULL || cb == NULL || loop->cleanup_count >= AXL_MAX_SOURCES) {
        return;
    }

    loop->cleanups[loop->cleanup_count] = cb;
    loop->cleanup_data[loop->cleanup_count] = data;
    loop->cleanup_count++;
}

// ---------------------------------------------------------------------------
// Loop-callback re-entrancy depth
//
// UEFI is single-threaded, so a single process-global depth tracks whether
// control is currently inside ANY loop's dispatched callback (source, timer,
// idle, keypress, or a drained deferred-work item). The synchronous wait
// primitive (src/event/axl-wait.c) reads this to detect — and warn about — a
// blocking wait invoked from within a loop callback, which nests a new loop
// re-entrantly (the bug class in docs/AXL-Loop-Reentrancy-Plan.md). The
// counter nests correctly: a callback that pumps another loop bumps it again.
// ---------------------------------------------------------------------------

static unsigned g_loop_cb_depth;

void
_axl_loop_cb_enter(void)
{
    static bool tracked;

    /* A callback the backend abandons (a deferred axl_exit) never reaches
       its _axl_loop_cb_leave; the trampoline puts this back instead. */
    if (!tracked) {
        axl_backend_notify_track_state(&g_loop_cb_depth);
        tracked = true;
    }
    g_loop_cb_depth++;
}

void
_axl_loop_cb_leave(void)
{
    if (g_loop_cb_depth > 0) {
        g_loop_cb_depth--;
    }
}

bool
_axl_loop_in_callback(void)
{
    return g_loop_cb_depth > 0;
}

bool
_axl_loop_has_source(
    const AxlLoop *loop,
    AxlSourceId    id
    )
{
    if (loop == NULL || id == 0) {
        return false;
    }
    for (size_t i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active && loop->sources[i].id == id) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Event primitives
// ---------------------------------------------------------------------------

/* A wait entry's position in the rotation. User sources rank by slot index
   and the loop's own events after all of them, in the order next_event
   appends them, so a wait array built in that order is sorted by key. */
static size_t
wait_key(
    size_t  mapped
    )
{
    switch (mapped) {
    case WAIT_POLL_TIMER: return AXL_MAX_SOURCES;
    case WAIT_BREAK:      return AXL_MAX_SOURCES + 1;
    case WAIT_KEYPRESS:   return AXL_MAX_SOURCES + 2;
    default:              return mapped;
    }
}

/* Index of the first entry at or past the loop's rotor, wrapping to 0 when
   the rotor is past the last entry. event_to_source[] must be key-sorted. */
static size_t
wait_start(
    const AxlLoop  *loop,
    const size_t   *event_to_source,
    size_t          count
    )
{
    for (size_t j = 0; j < count; j++) {
        if (wait_key(event_to_source[j]) >= loop->wait_rotor) {
            return j;
        }
    }
    return 0;
}

/* Reverse entries [lo, hi) of both parallel wait arrays. */
static void
wait_reverse(
    AxlEventHandle  *events,
    size_t          *event_to_source,
    size_t           lo,
    size_t           hi
    )
{
    while (lo + 1 < hi) {
        hi--;
        AxlEventHandle e = events[lo];
        size_t         m = event_to_source[lo];
        events[lo] = events[hi];
        event_to_source[lo] = event_to_source[hi];
        events[hi] = e;
        event_to_source[hi] = m;
        lo++;
    }
}

/* Rotate both parallel wait arrays left so entry @a start comes first --
   gBS->WaitForEvent has no "start here" argument, so the array itself must
   begin at the rotor. Three reversals: in place, no second stack array. */
static void
wait_rotate(
    AxlEventHandle  *events,
    size_t          *event_to_source,
    size_t           count,
    size_t           start
    )
{
    if (start == 0 || start >= count) {
        return;
    }
    wait_reverse(events, event_to_source, 0, start);
    wait_reverse(events, event_to_source, start, count);
    wait_reverse(events, event_to_source, 0, count);
}

int
axl_loop_next_event(AxlLoop *loop, bool blocking)
{
    size_t          i;
    size_t          event_count;
    /* +3 sentinels: poll timer + break event + intrinsic keypress
       (the keypress slot was added by 76df737 but the array sizing
       lagged behind, off-by-one writing the third sentinel into
       whatever followed in the stack frame — observed as a stale
       AxlEventHandle the next iteration handed to gBS->CheckEvent). */
    AxlEventHandle  event_array[AXL_MAX_SOURCES + 3];
    size_t          event_to_source[AXL_MAX_SOURCES + 3];
    size_t          fired_index = 0;
    size_t          start;
    size_t          selected;
    bool            has_idle;
    bool            wait;
    int             keypress_src;   /* first active SOURCE_KEYPRESS, or -1 */

    if (loop == NULL) {
        return -1;
    }

    /* Auto-set running on first next_event call(for manual loop usage) */
    if (!loop->running) {
        loop->running = true;
        loop->quit_requested = false;
    }

    loop->pending_source = -1;

    /* 1. Check Ctrl-C (the shell's ExecutionBreak).  CheckEvent CONSUMES the
       signal, so who reads it matters:
         - an intercepting loop reads it and quits;
         - a FOREGROUND loop that does not intercept (a GUI app that owns
           Ctrl+C for Copy) still drains it, so the signal does not latch and
           a later library yield (_axl_poll_break) does not exit the app on a
           Copy pressed minutes ago;
         - a resident loop (driver-attached, or any loop in a driver image)
           that does not intercept never touches it. The Shell's script
           runner polls the same event between lines, and resident code
           reading it first would swallow every Ctrl-C the user (or a remote
           console) sends the Shell. */
    bool intercept = loop_intercepts_break(loop);
    if (intercept || loop_is_foreground(loop)) {
        bool brk = axl_backend_shell_break_flag();   /* reads AND clears */
        if (brk && intercept) {
            _axl_signal_on_break();
            axl_loop_quit(loop);
            return -1;
        }
    }

    if (loop->quit_requested) {
        return -1;
    }

    /* 1b. Drain deferred work queue */
    axl_defer_drain_internal(loop);

    if (loop->quit_requested) {
        return -1;
    }

    /* 2. Fire idle callbacks */
    has_idle = false;
    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active &&
            loop->sources[i].type == SOURCE_IDLE) {
            has_idle = true;
            _axl_loop_cb_enter();
            bool keep_idle = loop->sources[i].fn.cb(loop->sources[i].data);
            _axl_loop_cb_leave();
            if (!keep_idle) {
                loop->sources[i].active = false;
            }
            if (loop->quit_requested) {
                return -1;
            }
        }
    }

    /* 3. Build event array from non-idle active sources.
     *
     * SOURCE_KEYPRESS is intentionally EXCLUDED: its EFI_EVENT is the Ex
     * protocol's WaitForKeyEx, whose firmware notify (EDK2 UsbKbDxe /
     * Ps2KeyboardDxe) DISCARDS modifier-only "partial" keystrokes and
     * only signals for a full key. Waiting on it therefore (a) never
     * wakes us on a held modifier and (b) silently drops the partial the
     * moment WaitForEvent checks it — so live modifier state (Shift+wheel
     * / Ctrl+click) could never be tracked. Keypress sources are instead
     * drained by polling ReadKeyStrokeEx on the poll-timer tick, in BOTH
     * the blocking and the non-blocking branch below (keypress_src). */
    event_count = 0;
    keypress_src = -1;
    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active &&
            loop->sources[i].type == SOURCE_KEYPRESS) {
            if (keypress_src < 0) {
                keypress_src = (int)i;
            }
            continue;
        }
        if (loop->sources[i].active && loop->sources[i].event != NULL &&
            loop->sources[i].type != SOURCE_IDLE) {
            event_to_source[event_count] = i;
            event_array[event_count] = loop->sources[i].event;
            event_count++;
        }
    }

    /* No user sources, no idle callbacks. Non-blocking: nothing to
     * check, return 1 ("no event fired"). Blocking: fall through and
     * wait on the loop's intrinsic events (poll timer + break event)
     * so axl_loop_run idles instead of busy-spinning. The yield-test
     * end-to-end test exercises exactly this path — main() calls
     * axl_loop_run on the default loop with no other sources to wait
     * for Ctrl-C. A keypress source counts as work even though it is
     * not in event_array: the non-blocking branch polls it below. */
    if (event_count == 0 && !has_idle && !blocking && keypress_src < 0) {
        return 1;
    }

    /* 4. Append the loop's own events. Their wait keys sort after every
     * user source (wait_key), so the array stays key-sorted and the
     * rotation below treats them as ordinary participants. */
    /* Deferred work queued since the drain in step 1b -- by a deferred
     * callback itself -- is due on the NEXT iteration (axl-defer.h's "next
     * tick"). Waiting now would hold it until the poll timer or some
     * unrelated event, so while any is queued this iteration only CHECKS
     * its events. (Work an idle callback queues never waited: a loop with
     * an idle source does not block.) It checks the SAME events it would
     * have waited on, the loop's own included: a deferred callback that
     * re-queues itself for ever must not hide a serial Ctrl-C (0x03 on
     * ConIn, which only the intrinsic keypress event below sees when no
     * Shell turns it into its break). */
    bool own_events = blocking && !has_idle;
    wait = own_events && axl_ring_buf_get_length(&loop->defer_ring) == 0;
    if (own_events) {
        /* Poll timer: periodic deferred-work draining, and the drain
         * point for keypress sources (see WAIT_POLL_TIMER below). */
        event_to_source[event_count] = WAIT_POLL_TIMER;
        event_array[event_count] = loop->poll_timer;
        event_count++;

        /* Add shell break event for immediate Ctrl-C (if available, and the
         * app still wants Ctrl-C to mean quit). */
        if (loop->break_event != NULL && intercept) {
            event_to_source[event_count] = WAIT_BREAK;
            event_array[event_count] = loop->break_event;
            event_count++;
        }

        /* Add the intrinsic ConIn keypress event ONLY when no user
         * SOURCE_KEYPRESS source is active — a user source is drained on
         * the poll tick instead, and its dispatch path reads + screens
         * for serial Ctrl-C. The intrinsic exists so a server with no key
         * sources still wakes on a serial 0x03 and breaks. */
        if (keypress_src < 0 && loop->keypress_event != NULL) {
            event_to_source[event_count] = WAIT_KEYPRESS;
            event_array[event_count] = loop->keypress_event;
            event_count++;
        }
    } else if (keypress_src >= 0 && loop->poll_timer != NULL) {
        /* Keypress drain for every caller that does not reach the blocking
         * wait: a driver-mode pump (axl_loop_attach_driver, which a GUI
         * hosting a blocking nested Shell uses — see axterm), a direct
         * axl_loop_dispatch(loop, false), and any BLOCKING loop that also
         * has an idle source (`blocking && has_idle` lands here).
         *
         * Gated on the poll timer rather than on the source's own
         * WaitForKeyEx for two reasons. (a) That event's firmware notify
         * DISCARDS modifier-only partial keystrokes, so checking it would
         * destroy the live modifier state the blocking path is careful to
         * preserve. (b) CheckEvent on a periodic timer succeeds once per
         * period, which bounds this to one drain per tick — dispatch_event
         * then empties the whole queue. An always-ready keypress source
         * would instead spin driver_dispatch_notify's "drain until nothing
         * pending" loop to its per-tick budget on every tick.
         *
         * It takes part in the rotation like any other entry, so an
         * always-ready event source cannot strand keystrokes either. */
        event_to_source[event_count] = WAIT_POLL_TIMER;
        event_array[event_count] = loop->poll_timer;
        event_count++;
    }

    if (event_count == 0) {
        return 1;   /* non-blocking with only idle sources: all dispatched */
    }

    /* 5. Wait for or check events, starting one past the last selection.
     *
     * ONE source is dispatched per iteration, and both ways of picking it
     * favour the front of the array: gBS->WaitForEvent returns the
     * LOWEST-index signalled event, and the check loop scans from 0. A
     * source that is ready on every iteration — a listener under a
     * connection flood, a callback that re-signals its own event — would
     * therefore win every iteration and starve everything after it,
     * including the loop's own events. Starting each wait at wait_rotor
     * makes selection round-robin among whatever is signalled; with
     * nothing competing the first ready entry still wins at once, so an
     * idle loop's latency is unchanged. Ctrl-C does not depend on its
     * place here: step 1 checks the shell break flag before every wait. */
    start = wait_start(loop, event_to_source, event_count);
    if (wait) {
        wait_rotate(event_array, event_to_source, event_count, start);
        if (axl_backend_event_wait(event_count, event_array,
                                   &fired_index) != AXL_OK) {
            return 1;
        }
    } else {
        size_t k;

        for (k = 0; k < event_count; k++) {
            fired_index = (start + k) % event_count;
            if (axl_backend_event_check(event_array[fired_index]) == 0) {
                break;
            }
        }
        if (k == event_count) {
            return 1;
        }
    }
    selected = event_to_source[fired_index];
    loop->wait_rotor = wait_key(selected) + 1;

    /* Break event fired — Ctrl-C */
    if (selected == WAIT_BREAK) {
        _axl_signal_on_break();
        axl_loop_quit(loop);
        return -1;
    }

    /* Intrinsic keypress fired — read with modifier state, intercept
     * serial Ctrl-C (UnicodeChar=0x03 with no modifiers — a real
     * keyboard Ctrl+C carries the CTRL bit, so modifiers != 0),
     * drop everything else (no user source wants it). */
    if (selected == WAIT_KEYPRESS) {
        uint16_t scan = 0, uni = 0;
        uint32_t mods = 0;
        if (axl_backend_console_read_key_ex(&scan, &uni, &mods) == AXL_OK) {
            if (uni == 0x03 && mods == 0 && loop_intercepts_break(loop)) {
                _axl_signal_on_break();
                axl_loop_quit(loop);
                return -1;
            }
        }
        return 1;
    }

    /* Poll timer fired — periodic wakeup, AND the drain point for
     * keypress sources. WaitForKeyEx discards modifier-only partial
     * keystrokes (see the event-array build above), so a keypress
     * source is polled here instead: hand it to dispatch_event, which
     * drains the firmware key queue via ReadKeyStrokeEx (a no-op when
     * nothing is queued). Partials refresh live modifier state; full
     * keys deliver a KEY_DOWN. Key latency is therefore bounded by the
     * poll interval (POLL_INTERVAL_MS), imperceptible for input. */
    if (selected == WAIT_POLL_TIMER) {
        if (keypress_src >= 0) {
            loop->pending_source = keypress_src;
            return 0;
        }
        return 1;
    }

    loop->pending_source = (int)selected;
    return 0;
}

void
axl_loop_dispatch_event(AxlLoop *loop)
{
    LoopSource      *src;
    bool             cont;
    AxlInputKey      akey;
    AxlSourceId      dispatched_id;

    if (loop == NULL || loop->pending_source < 0) {
        return;
    }

    src = &loop->sources[loop->pending_source];
    loop->pending_source = -1;

    if (!src->active) {
        return;
    }

    /* Snapshot id so we can detect slot reuse during cb: a callback
     * may remove this source and add another, and add_source can land
     * in this same just-freed slot. Without this check the epilogue
     * below would deactivate the new occupant. The remaining
     * post-cb manipulation is safe to skip on reuse — we never
     * own_event a SOURCE_EVENT, and SOURCE_TIMEOUT slots don't get
     * reused mid-dispatch (timeouts don't trigger remove+add). */
    dispatched_id = src->id;

    /* Dispatch based on source type */
    if (src->type == SOURCE_KEYPRESS) {
        /* Drain ALL keys queued this tick. The wake path for keypress is
         * the poll timer (WaitForKeyEx is not waited on — its firmware
         * notify discards modifier-only partials; see next_event's
         * event-array build), so we poll ReadKeyStrokeEx here, which
         * returns every queued key INCLUDING partials. Draining one key
         * per poll tick would cap throughput at 1 key / poll interval and
         * lag fast typing / paste / typematic bursts, so loop until the
         * firmware queue is empty.
         *
         * Read with shift state so we can screen for serial Ctrl-C
         * (UnicodeChar=0x03, KeyShiftState=0 — what TerminalDxe emits over
         * a wire that carries no shift bits). Intercepted here so a key
         * source registered by the user doesn't have to know about it. */
        cont = true;
        for (;;) {
            akey.modifiers = 0;
            if (axl_backend_console_read_key_ex(&akey.scan_code,
                                                &akey.unicode_char,
                                                &akey.modifiers) != AXL_OK) {
                break;   /* queue drained */
            }
            if (akey.unicode_char == 0x03 && akey.modifiers == 0
                && loop_intercepts_break(loop)) {
                _axl_signal_on_break();
                axl_loop_quit(loop);
                return;
            }
            // intercept_break off: 0x03 falls through to the app (a GUI editor
            // maps Ctrl+C to Copy via axl_input_ctrl_letter's serial-fold).
            _axl_loop_cb_enter();
            cont = src->fn.key_cb(akey, src->data);
            _axl_loop_cb_leave();
            if (src->id != dispatched_id) {
                /* Callback removed this source and the slot was reused —
                 * the new occupant owns its own lifecycle. Stop here. */
                return;
            }
            if (!cont) {
                break;   /* AXL_SOURCE_REMOVE — stop draining, run epilogue */
            }
        }
    } else {
        _axl_loop_cb_enter();
        cont = src->fn.cb(src->data);
        _axl_loop_cb_leave();
    }

    /* Slot reused during cb? Skip epilogue — touching the new
     * occupant would corrupt it. The cb that performed the
     * remove+add already owns the new source's lifecycle. */
    if (src->id != dispatched_id) {
        return;
    }

    /* AXL_SOURCE_REMOVE: deactivate this source (loop continues).
       To quit the loop, callbacks call axl_loop_quit(). */
    if (!cont) {
        if (src->owns_event && src->event != NULL) {
            axl_backend_event_close(src->event);
            src->event = NULL;
        }
        src->active = false;
    }

    /* Auto-remove one-shot sources */
    if (cont && src->type == SOURCE_TIMEOUT) {
        if (src->owns_event && src->event != NULL) {
            axl_backend_event_close(src->event);
            src->event = NULL;
        }
        src->active = false;
    }
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

int
axl_loop_dispatch(AxlLoop *loop, bool blocking)
{
    int rc;

    rc = axl_loop_next_event(loop, blocking);
    if (rc == 0) {
        axl_loop_dispatch_event(loop);
    }

    return rc;
}

int
axl_loop_run(AxlLoop *loop)
{
    int rc;

    if (loop == NULL) {
        return -1;
    }

    loop->running = true;
    loop->quit_requested = false;

    while (!loop->quit_requested) {
        rc = axl_loop_next_event(loop, true);
        if (rc == -1) {
            break;
        }
        axl_loop_dispatch_event(loop);
    }

    fire_cleanups(loop);
    loop->running = false;

    return loop->quit_requested ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Driver-mode dispatch — see axl_loop_attach_driver doxygen for the
// TPL trapdoor this exists to dodge.
// ---------------------------------------------------------------------------

/* Consecutive full-budget driver ticks before the drain-cap diagnostic
   escalates from debug (a normal burst) to a warning (sustained overload /
   runaway). A single burst drains in a handful of ticks — bounded by the WS
   outbound queue's depth / the per-tick budget — well under this, so a
   legitimate redraw never trips the warning. */
#define DRIVER_DRAIN_CAP_SUSTAINED_TICKS  20u

/* Calibrate the monotonic clock OUTSIDE a tick, once per loop, and record
   whether it worked. The first read in a process may stall 10 ms (x64 TSC
   calibration), and a FAILED calibration is not memoised, so every later
   read would stall again -- neither may happen at TPL_CALLBACK inside a
   tick. */
static void
driver_clock_warm(AxlLoop *loop)
{
    AxlTimespec ts;

    if (loop->clock_warm) {
        return;
    }
    loop->clock_warm = true;
    loop->clock_ok   = axl_backend_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) == AXL_OK;
}

static bool
driver_now_us(AxlLoop *loop, uint64_t *out)
{
    AxlTimespec ts;

    if (!loop->clock_ok
        || axl_backend_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return false;
    }
    *out = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    return true;
}

/* The budget a tick may spend, in us. Busy foreground: min(budget,
   interval / 2). A tick that found the foreground waiting (fg_waiting):
   min(idle budget, interval * 9/10), but never less than the busy figure;
   an idle budget of 0 leaves the busy figure. A budget of 0 is no
   extension at all. */
static uint64_t
driver_budget_us(const AxlLoop *loop, bool fg_waiting)
{
    uint64_t ms   = loop->driver_budget_ms;
    uint64_t idle = loop->driver_idle_budget_ms;

    if (ms == 0) {
        return 0;
    }
    /* Whole milliseconds, as always: a 1 ms tick's half is 0, i.e. no
       extension -- one pass per tick, which a unit test pins and which also
       bounds the CPU a shortened tick (_axl_loop_driver_fast) costs. The
       tick is the one in force, so a shortened tick clamps the same way. */
    uint64_t period_ms = driver_period_us(loop) / 1000u;
    if (ms > period_ms / 2u) {
        ms = period_ms / 2u;
    }
    if (fg_waiting) {
        if (idle > period_ms * 9u / 10u) {
            idle = period_ms * 9u / 10u;
        }
        if (idle > ms) {
            ms = idle;
        }
    }
    return ms * 1000u;
}

/* The tick a driver-attached loop is running at, in microseconds: its
   attached interval, or the latency policy's shorter one while fast. */
static uint64_t
driver_period_us(const AxlLoop *loop)
{
    uint64_t us = loop->driver_interval_ms * 1000u;
    if (g_driver_fast_us != 0 && g_driver_fast_us < us) {
        us = g_driver_fast_us;
    }
    return us;
}

/* Re-arm one attached loop's tick at its current period. */
static void
driver_rearm(AxlLoop *loop)
{
    axl_backend_event_set_timer(loop->driver_timer, AXL_TIMER_PERIODIC,
                                driver_period_us(loop) * 10u);
}

void
_axl_loop_driver_fast(uint32_t tick_us)
{
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    g_driver_fast_us = tick_us;
    for (AxlLoop *l = g_driver_loops; l != NULL; l = l->driver_next) {
        driver_rearm(l);
    }
    axl_backend_tpl_restore(old);
}

/* An idle pass of the foreground's gBS->WaitForEvent, at the pass itself
   (TPL_NOTIFY, before the CPU halt; see axl_backend_event_create_idle_notify).
   A continuously waiting foreground makes its next pass at once after each
   tick or halt ends; one that comes later means the foreground ran first.
   TPL_NOTIFY: one raw counter read (RDTSC / CNTPCT_EL0, no firmware call)
   and stores, nothing else. */
static void
driver_pass_notify(void *ctx)
{
    AxlLoop *loop = ctx;
    uint64_t now  = axl_backend_counter_ticks();

    if (loop->driver_ref_ct == 0 || now < loop->driver_ref_ct
        || now - loop->driver_ref_ct >= loop->driver_recent_ticks) {
        loop->driver_ran = true;
    }
}

/* The halt that followed an idle pass ended (TPL_CALLBACK): the foreground
   gets its next chance to run now. Runs ahead of any tick the waking
   interrupt queued. */
static void
driver_wake_notify(void *ctx)
{
    AxlLoop *loop = ctx;

    loop->driver_wake_ct = axl_backend_counter_ticks();
    loop->driver_ref_ct  = loop->driver_wake_ct;
}

static bool
loop_has_busy_idle(const AxlLoop *loop)
{
    size_t i;

    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active && loop->sources[i].type == SOURCE_IDLE
            && loop->sources[i].busy) {
            return true;
        }
    }
    return false;
}

static void
driver_dispatch_notify(void *ctx)
{
    AxlLoop *loop = (AxlLoop *)ctx;

    if (loop == NULL || loop->quit_requested) {
        return;
    }

    /* Auto-set running on first notify so axl_loop_dispatch /
       axl_loop_next_event don't bail with the foreground-mode
       "loop not running" check. */
    if (!loop->running) {
        loop->running = true;
    }

    /* Drain everything pending this tick. Non-blocking is the only
       legal choice (gBS->WaitForEvent returns EFI_UNSUPPORTED above
       TPL_APPLICATION).

       Earlier this called axl_loop_dispatch exactly once per tick.
       That one-event-per-tick budget starved completion handlers
       under realistic HTTP load: a recv-data callback synchronously
       submits a Transmit (which TCP4 typically completes inline),
       but the corresponding tx-event was only checked on the *next*
       50 ms tick. Each tick handled the older accept signal first,
       so 8 sequential requests filled the conn pool with active=true
       slots whose on_response_sent never ran. The 9th connection saw
       NO FREE SLOT and the listener appeared wedged.

       The fix matches the doxygen contract on axl_loop_attach_driver
       ("processes whatever's pending in this tick before returning"):
       loop until axl_loop_dispatch returns "nothing ready". Each
       iteration calls next_event afresh, which rebuilds event_array
       from loop->sources, so a callback that frees a sock (and its
       sources) here drops them out of the next iter automatically —
       no new lifetime exposure beyond what dispatch_event's
       slot-reuse snapshot (line 428) already handles for one cb.

       The cap is a safety net for a pathological cb that re-arms an
       always-signaled source — it puts an upper bound on how long
       we hold TPL_CALLBACK in any single tick. AXL_MAX_SOURCES is
       the upper bound on event_count, so capping at 2× that lets a
       legitimate burst (accept + N×(recv+send) for the full source
       table) drain in one tick while still bounding pathological
       runaway. */
    /* Busy idle sources (axl_loop_add_idle_busy) extend the tick: a pass
       that dispatches nothing does not end it while one exists and the
       budget has not elapsed. The budget only decides when to stop STARTING
       passes -- signalled callbacks are counted against the dispatch cap,
       not timed -- and passes that find only idle work do not count toward
       that cap. The clock is read only if a calibration done OUTSIDE a tick
       succeeded (see driver_clock_warm). */
    int      drained  = 0;
    uint64_t start_us = 0;
    /* Budget 0 is no extension at all: no clock to read. */
    bool     have_now = loop->driver_budget_ms != 0 && driver_now_us(loop, &start_us);
    /* Does this tick find the foreground waiting -- and has it been, all
       period? Three facts, from the EDK2 idle-loop members (raw counter
       ticks; driver_recent_ticks is 0 without a counter, and then nothing
       counts as waiting):

       1. WAITING NOW. The halt that the interrupt queuing this tick ended
          was the foreground's (driver_wake_ct, stamped just ahead of this
          tick). A foreground that was busy at the interrupt made no halt.
       2. DID NOT RUN THIS PERIOD. It was waiting at the tick that began
          the period (fact 1 held there), and every idle pass since came
          at once after the end of that tick or of a halt (driver_ran; the
          end-of-tick comment below says why the first half is needed).
          Waiting at the tick instants is not enough: EDK2 fires every timer
          on its system tick, so a foreground's wake lands on the very
          interrupt that queues a driver tick, and a renderer that works
          between short waits is "waiting" at every tick it is sampled. An
          extension there runs before the woken foreground does: one doing
          4 ms of every 10 beside a 10 ms tick ran at half its frame rate.
       3. NOR THE PERIOD BEFORE (driver_prev_idle). A foreground that waited
          through one whole period and wakes on the next interrupt would
          still lose an extended tick; two whole periods means it is mostly
          waiting, which is when the idle budget's latency is the
          documented trade-off. The Shell at its prompt extends from the
          third tick after it goes idle: the first still sees the pass
          that began its wait come after it ran, the second has no idle
          period before it.

       A tick QUEUED BEHIND OUR OWN is none of these: no halt has ended
       since the previous tick did (driver_ref_ct is still that tick's end,
       later than the last halt's), and it started at once after. That
       tick ran into this one's interrupt -- a long last pass, or a long
       signalled callback -- so the foreground never had the CPU in
       between and nothing about it has changed. Such a tick neither
       extends nor moves the detector: read as "not waiting", it set
       driver_ran and cost the extension three ticks to come back after
       every overrun (on aarch64 TCG, an SFTP download's ticks ran
       extended, queued, ran, idle-without-predecessor, over and over).
       The window is DRIVER_IDLE_RECENT_US, the same "at once" as the facts.
       A truly queued tick starts microseconds after ours, but the deadline
       stop ends an extended tick about DRIVER_TICK_GUARD_US early, so a
       foreground that runs in that gap and is cut by the next tick also
       reads as queued: its work is not counted as running. That needs an
       extended tick before it (two idle periods) and a burst of about
       1.5 ms or less in all -- the sub-millisecond tolerance of fact 2,
       slightly widened. A tighter window would misread a waiting Shell
       whose re-entry into WaitForEvent takes longer than that on TCG, and
       bring back the overrun cost. */
    uint64_t now_ct    = have_now ? axl_backend_counter_ticks() : 0;
    bool     queued    = have_now && loop->driver_recent_ticks != 0
                         && loop->driver_ref_ct > loop->driver_wake_ct
                         && now_ct - loop->driver_ref_ct < loop->driver_recent_ticks;
    bool     waiting   = have_now && !queued && loop->driver_recent_ticks != 0
                         && loop->driver_wake_ct != 0
                         && now_ct - loop->driver_wake_ct < loop->driver_recent_ticks;
    /* When the next tick is due, from the interrupt that queued this one
       (the waiting foreground's halt ended in it, microseconds ago): one
       interval later. EDK2 fires a periodic timer on the first 10 ms
       system tick at or after its trigger time, so that holds for an
       interval that is a multiple of the system tick (10, 20, 50 ms). At
       15 ms the real gaps alternate 20 / 10 ms: on the short side the
       deadline is late and the tick overruns (the queued-tick rule absorbs
       it), on the long side it stops early. */
    uint64_t due_us    = 0;
    if (waiting) {
        uint64_t age = (now_ct - loop->driver_wake_ct) * 1000u / loop->driver_recent_ticks;
        due_us = start_us + driver_period_us(loop) - age;
    }
    bool     idle      = waiting && !loop->driver_ran;
    bool     extend_id = idle && loop->driver_prev_idle;
    if (!queued) {
        loop->driver_prev_idle = idle;
    }
    uint64_t budget_us = driver_budget_us(loop, extend_id);
    /* The extension is cut short to end in time for the next tick
       (DRIVER_TICK_GUARD_US), never below the busy-foreground budget. */
    uint64_t floor_us  = extend_id ? driver_budget_us(loop, false) : budget_us;
    uint64_t pass_us   = start_us;
    /* Whether a busy source EXISTS is decided on every idle-only pass, not
       here: one is commonly added from a callback in this very tick (a
       request handler's send arming the transmit pacer), and it must extend
       the tick it was added in. Only the clock is read once, up front. */
    bool     extend   = budget_us != 0 && have_now;

    while (drained < AXL_MAX_SOURCES * 2) {
        int rc = axl_loop_dispatch(loop, /*blocking=*/false);

        if (loop->quit_requested || rc < 0) {
            break;
        }
        if (rc == 0) {
            drained++;
            continue;
        }
        /* Nothing dispatched this pass. Work deferred in it -- by a deferred
           callback, after the drain at its top -- is due on the next pass
           (axl_loop_run never blocks while the queue holds work), so it
           keeps the tick going like a busy idle source, within the same
           budget and deadline. Ending the tick here held each hop of a
           defer chain for a whole period: sshd's channel flush defers its
           transport flush, and an SFTP reply waited a tick for it. Without
           the bound, a callback that defers itself ran the tick to the
           dispatch cap, ticks queued back to back, and the foreground
           starved; a long chain also cost the idle extension its overruns.
           Budget 0 (no extension) leaves deferred work to the next tick. */
        bool deferred = axl_ring_buf_get_length(&loop->defer_ring) != 0;
        if (!extend || (!deferred && !loop_has_busy_idle(loop))) {
            break;
        }
        uint64_t now_us = 0;
        if (!driver_now_us(loop, &now_us) || now_us - start_us >= budget_us) {
            break;
        }
        if (now_us - start_us >= floor_us
            && now_us + (now_us - pass_us) + DRIVER_TICK_GUARD_US > due_us) {
            break;
        }
        pass_us = now_us;
    }

    /* The foreground's next chance to run starts now: only an idle pass
       at once after this is a foreground that did not run (fact 2) --
       unless it was NOT waiting when this tick fired. Then it was mid-work,
       and it resumes the moment this tick ends: its next pass can come a
       few microseconds later and still follow real work (a work phase cut
       by the tick, whose tail is short). Measured on aarch64: a renderer's
       4 ms of work split 4.9 / 0.02 ms by a tick read as a whole idle
       period, and with the wait that followed made two -- an extended tick
       on its next wake in 49 of 56 runs. */
    if (!queued) {
        loop->driver_ran = !have_now || !waiting;
    }
    loop->driver_ref_ct = have_now ? axl_backend_counter_ticks() : 0;

    /* Draining the full per-tick budget with work still pending is NORMAL
       back-pressure: the rest is handled on the next tick(s), nothing is lost
       and the loop does not wedge. A single busy tick happens whenever a
       consumer fans out a burst (e.g. a full-screen console redraw broadcasting
       many WS frames), so it's only traced at debug. Only a SUSTAINED run is a
       real signal — a runaway callback re-arming an always-signaled event, or
       output produced faster than it can drain — so warn ONCE when the streak
       crosses the threshold, then stay quiet until it clears. (The WS outbound
       queue's drop-oldest back-pressure already bounds a single burst to a few
       ticks, well under the threshold.) */
    if (drained == AXL_MAX_SOURCES * 2) {
        loop->drain_cap_streak++;
        if (loop->drain_cap_streak == 1) {
            axl_debug("driver_dispatch_notify: notify drain budget (%d) reached "
                      "this tick - excess deferred to the next tick (normal "
                      "under an output burst)",
                      AXL_MAX_SOURCES * 2);
        } else if (loop->drain_cap_streak == DRIVER_DRAIN_CAP_SUSTAINED_TICKS) {
            axl_warning("driver_dispatch_notify: notify drain budget (%d) hit "
                        "for %u consecutive ticks - sustained overload (a "
                        "runaway callback re-arming an always-signaled event, "
                        "or output produced faster than it drains). Coalesce "
                        "output or raise the attach_driver tick rate.",
                        AXL_MAX_SOURCES * 2,
                        (unsigned)DRIVER_DRAIN_CAP_SUSTAINED_TICKS);
        }
    } else {
        loop->drain_cap_streak = 0;
    }
}

int
axl_loop_attach_driver(AxlLoop *loop, uint64_t interval_ms)
{
    AxlEventHandle  event;

    if (loop == NULL || interval_ms == 0) {
        return AXL_ERR;
    }

    if (loop->driver_timer != NULL) {
        axl_debug("axl_loop_attach_driver: loop already attached "
                  "- call axl_loop_detach_driver first");
        return AXL_ERR;
    }

    /* Before the timer exists: no tick can run until this returns. */
    driver_clock_warm(loop);
    loop->driver_interval_ms = interval_ms;

    if (axl_backend_event_create_notify_timer(
            driver_dispatch_notify, loop,
            interval_ms * MS_TO_100NS, &event) != AXL_OK) {
        axl_error("axl_loop_attach_driver: backend refused timer");
        return AXL_ERR;
    }

    /* Don't touch loop->running here — driver_dispatch_notify
       lazy-sets it on the first tick, which means a foreground
       axl_loop_run still owns the running-flag invariant if both
       paths happen to be active. axl_loop_quit clears it; that's
       the only intended toggle. */
    loop->driver_timer = event;
    {
        uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
        loop->driver_next = g_driver_loops;
        g_driver_loops    = loop;
        if (g_driver_fast_us != 0) {
            driver_rearm(loop);   /* attached while the network is fast */
        }
        axl_backend_tpl_restore(old);
    }

    /* Optional: firmware that refuses the idle-loop group (or never
       signals it) leaves every tick at the busy-foreground budget. */
    driver_idle_reset(loop);
    loop->driver_recent_ticks = 0;
    /* A clock that failed to calibrate above has no frequency either, and
       asking again would only repeat the calibration's stall. */
    uint64_t hz     = loop->clock_ok ? axl_backend_counter_hz() : 0;
    uint64_t recent = hz * DRIVER_IDLE_RECENT_US / 1000000u;
    if (recent != 0) {
        if (axl_backend_event_create_idle_notify(driver_pass_notify, loop, true,
                                                 &loop->driver_pass_event) != AXL_OK) {
            axl_debug("axl_loop_attach_driver: no idle-loop notify; ticks keep "
                      "the busy-foreground budget");
        } else if (axl_backend_event_create_idle_notify(driver_wake_notify, loop, false,
                                                        &loop->driver_wake_event)
                   != AXL_OK) {
            /* Most likely the notify table is full: give the slot back
               rather than hold it until detach, where it could cost an
               AxlTaskPool its before-ExitBootServices hook. */
            axl_backend_event_close(loop->driver_pass_event);
            loop->driver_pass_event = NULL;
            axl_debug("axl_loop_attach_driver: no idle-loop notify; ticks keep "
                      "the busy-foreground budget");
        } else {
            loop->driver_recent_ticks = recent;
        }
    }
    return AXL_OK;
}

int
axl_loop_detach_driver(AxlLoop *loop)
{
    if (loop == NULL || loop->driver_timer == NULL) {
        return AXL_ERR;
    }

    /* Backend close cancels the timer first, then CloseEvent
       drains any in-flight notify, then the bridging context is
       freed — see axl_backend_event_close_dbg in
       axl-backend-native-event.c. Don't clear loop->running:
       attach didn't set it (driver_dispatch_notify did, lazily),
       and clearing it now would step on a foreground caller if
       one happens to be running concurrently. */
    driver_events_close(loop);
    return AXL_OK;
}

/* Close the driver tick and the idle-loop notify (when there is one). */
static void
driver_events_close(AxlLoop *loop)
{
    uintptr_t old = axl_backend_tpl_raise(TPL_CALLBACK);
    for (AxlLoop **pp = &g_driver_loops; *pp != NULL; pp = &(*pp)->driver_next) {
        if (*pp == loop) {
            *pp = loop->driver_next;
            break;
        }
    }
    loop->driver_next = NULL;
    axl_backend_tpl_restore(old);
    axl_backend_event_close(loop->driver_timer);
    loop->driver_timer = NULL;
    if (loop->driver_pass_event != NULL) {
        axl_backend_event_close(loop->driver_pass_event);
        loop->driver_pass_event = NULL;
    }
    if (loop->driver_wake_event != NULL) {
        axl_backend_event_close(loop->driver_wake_event);
        loop->driver_wake_event = NULL;
    }
    loop->driver_recent_ticks = 0;
    driver_idle_reset(loop);
}

/* Forget everything the idle-loop members recorded. */
static void
driver_idle_reset(AxlLoop *loop)
{
    loop->driver_ref_ct    = 0;
    loop->driver_wake_ct   = 0;
    loop->driver_ran       = true;
    loop->driver_prev_idle = false;
}

int
axl_loop_set_driver_budget_ms(AxlLoop *loop, uint32_t ms)
{
    if (loop == NULL || ms > AXL_LOOP_DRIVER_BUDGET_MAX_MS) {
        return AXL_INVALID;
    }
    driver_clock_warm(loop);
    loop->driver_budget_ms = ms;
    return AXL_OK;
}

int
axl_loop_set_driver_idle_budget_ms(AxlLoop *loop, uint32_t ms)
{
    if (loop == NULL || ms > AXL_LOOP_DRIVER_BUDGET_MAX_MS) {
        return AXL_INVALID;
    }
    driver_clock_warm(loop);
    loop->driver_idle_budget_ms = ms;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Event sources
// ---------------------------------------------------------------------------

AxlSourceId
axl_loop_add_timer(AxlLoop *loop, uint32_t interval_ms,
                   AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;

    if (loop == NULL || cb == NULL || interval_ms == 0) {
        return 0;
    }

    if (axl_backend_event_create_timer(&event) != AXL_OK) {
        axl_error("failed to create timer event");
        return 0;
    }

    if (axl_backend_event_set_timer(event, AXL_TIMER_PERIODIC,
                                    interval_ms * MS_TO_100NS) != AXL_OK) {
        axl_error("failed to set timer");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_TIMER, event, true, (void *)cb, data);
}

AxlSourceId
axl_loop_add_timeout(AxlLoop *loop, uint32_t delay_ms,
                     AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;

    if (loop == NULL || cb == NULL || delay_ms == 0) {
        return 0;
    }

    if (axl_backend_event_create_timer(&event) != AXL_OK) {
        axl_error("failed to create timeout event");
        return 0;
    }

    if (axl_backend_event_set_timer(event, AXL_TIMER_RELATIVE,
                                    delay_ms * MS_TO_100NS) != AXL_OK) {
        axl_error("failed to set timeout");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_TIMEOUT, event, true, (void *)cb, data);
}

AxlSourceId
axl_loop_add_key_press(AxlLoop *loop, AxlKeyCallback cb, void *data)
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }

    /* Use ConIn's existing WaitForKey event — don't close it */
    return add_source(loop, SOURCE_KEYPRESS,
                      axl_backend_console_wait_for_key(),
                      false, (void *)cb, data);
}

AxlSourceId
axl_loop_add_idle(AxlLoop *loop, AxlLoopCallback cb, void *data)
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }

    return add_source(loop, SOURCE_IDLE, NULL, false, (void *)cb, data);
}

AxlSourceId
axl_loop_add_idle_busy(AxlLoop *loop, AxlLoopCallback cb, void *data)
{
    AxlSourceId id;
    size_t      i;

    if (loop == NULL || cb == NULL) {
        return 0;
    }

    id = add_source(loop, SOURCE_IDLE, NULL, false, (void *)cb, data);
    for (i = 0; id != 0 && i < loop->source_count; i++) {
        if (loop->sources[i].id == id) {
            loop->sources[i].busy = true;
            break;
        }
    }
    return id;
}

AxlSourceId
axl_loop_add_protocol_notify(AxlLoop *loop, void *guid,
                             AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;
    void           *registration;

    if (loop == NULL || guid == NULL || cb == NULL) {
        return 0;
    }

    if (axl_backend_event_create(&event) != AXL_OK) {
        axl_error("failed to create protocol notify event");
        return 0;
    }

    if (axl_backend_event_register_protocol_notify(guid, event,
                                                   &registration) != AXL_OK) {
        axl_error("failed to register protocol notify");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_PROTOCOL, event, true, (void *)cb, data);
}

AxlSourceId
axl_loop_add_event(AxlLoop *loop, AxlEventHandle event,
                   AxlLoopCallback cb, void *data)
{
    if (loop == NULL || event == NULL || cb == NULL) {
        return 0;
    }

    /* Caller owns the event — loop does NOT close it on removal */
    return add_source(loop, SOURCE_EVENT, event, false, (void *)cb, data);
}

void
axl_loop_remove_source(AxlLoop *loop, AxlSourceId source_id)
{
    size_t i;

    if (loop == NULL || source_id == 0) {
        return;
    }

    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].id == source_id && loop->sources[i].active) {
            if (loop->sources[i].owns_event &&
                loop->sources[i].event != NULL) {
                axl_backend_event_close(loop->sources[i].event);
                loop->sources[i].event = NULL;
            }
            loop->sources[i].active = false;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Nested-wait primitive: drive the loop until a condition fires, without
// quitting it. Outer sources keep firing during the wait — see §5.6 of
// docs/AXL-Lifecycle.md.
// ---------------------------------------------------------------------------

static bool
iterate_flag_cb(void *data)
{
    *(bool *)data = true;
    return AXL_SOURCE_REMOVE;
}

AxlStatus
axl_loop_iterate_until(
    AxlLoop  *loop,
    AxlEvent *done,
    uint64_t  timeout_us
    )
{
    bool      done_flag    = false;
    bool      timeout_flag = false;
    AxlSourceId done_src    = 0;
    AxlSourceId timeout_src = 0;
    AxlStatus rc           = AXL_TIMEOUT;

    if (loop == NULL) {
        return AXL_CANCELLED;
    }

    if (done != NULL) {
        done_src = axl_loop_add_event(loop, axl_event_handle(done),
                                      iterate_flag_cb, &done_flag);
        if (done_src == 0) {
            return AXL_NO_RESOURCES;
        }
    }
    if (timeout_us > 0) {
        uint32_t ms = (uint32_t)((timeout_us + 999) / 1000);
        timeout_src = axl_loop_add_timeout(loop, ms,
                                           iterate_flag_cb, &timeout_flag);
        if (timeout_src == 0) {
            if (done_src != 0) {
                axl_loop_remove_source(loop, done_src);
            }
            return AXL_NO_RESOURCES;
        }
    }

    while (!done_flag && !timeout_flag) {
        int r = axl_loop_next_event(loop, /*blocking=*/true);
        if (r < 0) {
            rc = AXL_CANCELLED;
            goto cleanup;
        }
        if (r == 0) {
            axl_loop_dispatch_event(loop);
        }
    }
    rc = done_flag ? AXL_OK : AXL_TIMEOUT;

cleanup:
    if (done_src != 0) {
        axl_loop_remove_source(loop, done_src);
    }
    if (timeout_src != 0) {
        axl_loop_remove_source(loop, timeout_src);
    }
    return rc;
}

