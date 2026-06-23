/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-loop.c
    GLib-style event loop for UEFI. Supports timers, keyboard input,
    idle callbacks, protocol notifications, and built-in Ctrl-C.
    FUSE-style next_event/dispatch_event primitives for advanced control.

    Migrated from AxlLoop.c(EDK2-style) to GLib-style API.
**/

#include "axl-loop-internal.h"
#include "../runtime/axl-registry-internal.h"
#include "../runtime/axl-signal-internal.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>

AXL_LOG_DOMAIN("loop");

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
    loop->intercept_break = true;   // serial Ctrl-C → quit (GUI apps opt out)

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
        axl_backend_event_close(loop->driver_timer);
        loop->driver_timer = NULL;
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

void
axl_loop_set_intercept_break(AxlLoop *loop, bool intercept)
{
    if (loop != NULL) {
        loop->intercept_break = intercept;
    }
}

bool
axl_loop_intercept_break(AxlLoop *loop)
{
    return loop != NULL ? loop->intercept_break : false;
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

// ---------------------------------------------------------------------------
// Event primitives
// ---------------------------------------------------------------------------

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
    size_t          fired_index;
    bool            has_idle;

    if (loop == NULL) {
        return -1;
    }

    /* Auto-set running on first next_event call(for manual loop usage) */
    if (!loop->running) {
        loop->running = true;
        loop->quit_requested = false;
    }

    loop->pending_source = -1;

    /* 1. Check Ctrl-C (the shell's ExecutionBreak).  Always drain it so the
       signal doesn't latch, but only QUIT when the app still wants Ctrl-C to
       mean quit — a GUI app that owns Ctrl+C (editor Copy) opts out and lets
       the 0x03 keystroke reach its keypress source instead. */
    if (axl_backend_shell_break_flag() && loop->intercept_break) {
        _axl_signal_on_break();
        axl_loop_quit(loop);
        return -1;
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
     * drained by polling ReadKeyStrokeEx on the poll-timer tick below,
     * which returns partials. */
    event_count = 0;
    for (i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].active && loop->sources[i].event != NULL &&
            loop->sources[i].type != SOURCE_IDLE &&
            loop->sources[i].type != SOURCE_KEYPRESS) {
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
     * for Ctrl-C. */
    if (event_count == 0 && !has_idle && !blocking) {
        return 1;
    }

    /* 4. Wait for or check events */
    if (blocking && !has_idle) {
        /* Add poll timer for periodic deferred-work draining */
        event_to_source[event_count] = (size_t)-1;
        event_array[event_count] = loop->poll_timer;
        event_count++;

        /* Add shell break event for immediate Ctrl-C (if available, and the
         * app still wants Ctrl-C to mean quit). */
        if (loop->break_event != NULL && loop->intercept_break) {
            event_to_source[event_count] = (size_t)-2;
            event_array[event_count] = loop->break_event;
            event_count++;
        }

        /* Add the intrinsic ConIn keypress event ONLY when no user
         * SOURCE_KEYPRESS source is active — otherwise the user source
         * already has the same EFI_EVENT in event_array[] above and
         * its dispatch path will read + screen for serial Ctrl-C. The
         * intrinsic exists so a server with no key sources still wakes
         * on a serial 0x03 and breaks. */
        bool has_user_keypress = false;
        for (size_t k = 0; k < loop->source_count; k++) {
            if (loop->sources[k].active &&
                loop->sources[k].type == SOURCE_KEYPRESS) {
                has_user_keypress = true;
                break;
            }
        }
        if (!has_user_keypress && loop->keypress_event != NULL) {
            event_to_source[event_count] = (size_t)-3;
            event_array[event_count] = loop->keypress_event;
            event_count++;
        }

        if (axl_backend_event_wait(event_count, event_array,
                                   &fired_index) != AXL_OK) {
            return 1;
        }

        /* Break event fired — Ctrl-C */
        if (event_to_source[fired_index] == (size_t)-2) {
            _axl_signal_on_break();
            axl_loop_quit(loop);
            return -1;
        }

        /* Intrinsic keypress fired — read with modifier state, intercept
         * serial Ctrl-C (UnicodeChar=0x03 with no modifiers — a real
         * keyboard Ctrl+C carries the CTRL bit, so modifiers != 0),
         * drop everything else (no user source wants it). */
        if (event_to_source[fired_index] == (size_t)-3) {
            uint16_t scan = 0, uni = 0;
            uint32_t mods = 0;
            if (axl_backend_console_read_key_ex(&scan, &uni, &mods) == AXL_OK) {
                if (uni == 0x03 && mods == 0 && loop->intercept_break) {
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
         * poll interval (POLL_INTERVAL_MS), imperceptible for input.
         *
         * NOTE: this drain runs only in the blocking run-loop path. The
         * non-blocking dispatch path (driver mode via
         * axl_loop_attach_driver, or any direct axl_loop_dispatch(loop,
         * false)) does NOT poll keypress — SOURCE_KEYPRESS is excluded from
         * the non-blocking CheckEvent loop too. No in-tree consumer pairs a
         * keyboard source with non-blocking dispatch (driver loops are
         * headless network services), so no key is ever stranded in
         * practice. A future non-blocking keyboard consumer must drain
         * keypress in the CheckEvent path as well. */
        if (event_to_source[fired_index] == (size_t)-1) {
            for (size_t k = 0; k < loop->source_count; k++) {
                if (loop->sources[k].active &&
                    loop->sources[k].type == SOURCE_KEYPRESS) {
                    loop->pending_source = (int)k;
                    return 0;
                }
            }
            return 1;
        }

        loop->pending_source = (int)event_to_source[fired_index];
        return 0;
    }

    /* Non-blocking: check each event */
    for (i = 0; i < event_count; i++) {
        if (axl_backend_event_check(event_array[i]) == 0) {
            loop->pending_source = (int)event_to_source[i];
            return 0;
        }
    }

    return 1;
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
                && loop->intercept_break) {
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
    int drained = 0;
    for (drained = 0; drained < AXL_MAX_SOURCES * 2; drained++) {
        if (axl_loop_dispatch(loop, /*blocking=*/false) != 0) {
            break;  /* nothing pending */
        }
        if (loop->quit_requested) {
            break;
        }
    }

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
        axl_warning("axl_loop_attach_driver: loop already attached "
                    "- call axl_loop_detach_driver first");
        return AXL_ERR;
    }

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
    axl_backend_event_close(loop->driver_timer);
    loop->driver_timer = NULL;
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

int
axl_loop_iterate_until(
    AxlLoop  *loop,
    AxlEvent *done,
    uint64_t  timeout_us
    )
{
    bool     done_flag    = false;
    bool     timeout_flag = false;
    AxlSourceId done_src    = 0;
    AxlSourceId timeout_src = 0;
    int      rc           = -1;

    if (loop == NULL) {
        return AXL_CANCELLED;
    }

    if (done != NULL) {
        done_src = axl_loop_add_event(loop, axl_event_handle(done),
                                      iterate_flag_cb, &done_flag);
    }
    if (timeout_us > 0) {
        uint32_t ms = (uint32_t)((timeout_us + 999) / 1000);
        timeout_src = axl_loop_add_timeout(loop, ms,
                                           iterate_flag_cb, &timeout_flag);
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
    rc = done_flag ? 0 : -1;

cleanup:
    if (done_src != 0) {
        axl_loop_remove_source(loop, done_src);
    }
    if (timeout_src != 0) {
        axl_loop_remove_source(loop, timeout_src);
    }
    return rc;
}

