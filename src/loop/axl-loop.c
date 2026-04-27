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
// Internal helpers
// ---------------------------------------------------------------------------

static uint32_t
add_source(AxlLoop *loop, SourceType type, AxlEventHandle event,
           bool owns_event, void *callback, void *data)
{
    LoopSource *src;
    size_t i;

    if (loop == NULL || callback == NULL) {
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
            return 0;
        }
        src = &loop->sources[loop->source_count];
        loop->source_count++;
    }

    src->id = loop->next_id++;
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

AxlLoop *
axl_loop_new_impl(const char *file, int line)
{
    AxlLoop  *loop;

    loop = axl_calloc(1, sizeof(AxlLoop));
    if (loop == NULL) {
        return NULL;
    }

    loop->next_id = 1;
    loop->pending_source = -1;
    loop->defer_next_id = 1;
    loop->pubsub_next_sub_id = 1;

    axl_ring_buf_init_fixed(&loop->defer_ring, loop->defer_buf,
                          sizeof(loop->defer_buf), sizeof(DeferEntry),
                          0, NULL);

    /* Poll timer for periodic deferred-work draining and (if no shell)
       Ctrl-C detection.  When the shell is available, the break event
       gives immediate Ctrl-C response without polling. */
    if (axl_backend_event_create_timer(&loop->poll_timer) != 0) {
        axl_error("failed to create poll timer");
        axl_free(loop);
        return NULL;
    }
    axl_backend_event_set_timer(loop->poll_timer, AXL_TIMER_PERIODIC,
                                POLL_INTERVAL_MS * MS_TO_100NS);

    loop->break_event = axl_backend_shell_break_event();
    loop->_registry_handle = _axl_registry_add(AXL_RES_LOOP, loop, file, line);

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
            axl_error("axl_loop_free: caller-owned event source id=%u "
                      "still active — tear down the resource "
                      "(socket/tcp/custom event) BEFORE freeing the "
                      "loop it was registered against",
                      loop->sources[i].id);
        }
        if (loop->sources[i].active && loop->sources[i].owns_event &&
            loop->sources[i].event != NULL) {
            axl_backend_event_close(loop->sources[i].event);
        }
    }
    if (leaked_external > 0) {
        axl_error("axl_loop_free: %zu caller-owned event source(s) "
                  "still active — free will proceed but consumers may "
                  "crash on next use",
                  leaked_external);
    }

    if (loop->poll_timer != NULL) {
        axl_backend_event_close(loop->poll_timer);
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
// Event primitives
// ---------------------------------------------------------------------------

int
axl_loop_next_event(AxlLoop *loop, bool blocking)
{
    size_t          i;
    size_t          event_count;
    AxlEventHandle  event_array[AXL_MAX_SOURCES + 2];    /* +2: poll timer + break event */
    size_t          event_to_source[AXL_MAX_SOURCES + 2];
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

    /* 1. Check Ctrl-C */
    if (axl_backend_shell_break_flag()) {
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
            if (!loop->sources[i].fn.cb(loop->sources[i].data)) {
                loop->sources[i].active = false;
            }
            if (loop->quit_requested) {
                return -1;
            }
        }
    }

    /* 3. Build event array from non-idle active sources */
    event_count = 0;
    for (i = 0; i < loop->source_count; i++) {
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

        /* Add shell break event for immediate Ctrl-C (if available) */
        if (loop->break_event != NULL) {
            event_to_source[event_count] = (size_t)-2;
            event_array[event_count] = loop->break_event;
            event_count++;
        }

        if (axl_backend_event_wait(event_count, event_array,
                                   &fired_index) != 0) {
            return 1;
        }

        /* Break event fired — Ctrl-C */
        if (event_to_source[fired_index] == (size_t)-2) {
            _axl_signal_on_break();
            axl_loop_quit(loop);
            return -1;
        }

        /* Poll timer fired — just a periodic wakeup */
        if (event_to_source[fired_index] == (size_t)-1) {
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

    if (loop == NULL || loop->pending_source < 0) {
        return;
    }

    src = &loop->sources[loop->pending_source];
    loop->pending_source = -1;

    if (!src->active) {
        return;
    }

    /* Dispatch based on source type */
    if (src->type == SOURCE_KEYPRESS) {
        if (axl_backend_console_read_key(&akey.scan_code,
                                         &akey.unicode_char) != 0) {
            return;
        }
        cont = src->fn.key_cb(akey, src->data);
    } else {
        cont = src->fn.cb(src->data);
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
// Event sources
// ---------------------------------------------------------------------------

uint32_t
axl_loop_add_timer(AxlLoop *loop, uint32_t interval_ms,
                   AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;

    if (loop == NULL || cb == NULL || interval_ms == 0) {
        return 0;
    }

    if (axl_backend_event_create_timer(&event) != 0) {
        axl_error("failed to create timer event");
        return 0;
    }

    if (axl_backend_event_set_timer(event, AXL_TIMER_PERIODIC,
                                    interval_ms * MS_TO_100NS) != 0) {
        axl_error("failed to set timer");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_TIMER, event, true, (void *)cb, data);
}

uint32_t
axl_loop_add_timeout(AxlLoop *loop, uint32_t delay_ms,
                     AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;

    if (loop == NULL || cb == NULL || delay_ms == 0) {
        return 0;
    }

    if (axl_backend_event_create_timer(&event) != 0) {
        axl_error("failed to create timeout event");
        return 0;
    }

    if (axl_backend_event_set_timer(event, AXL_TIMER_RELATIVE,
                                    delay_ms * MS_TO_100NS) != 0) {
        axl_error("failed to set timeout");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_TIMEOUT, event, true, (void *)cb, data);
}

uint32_t
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

uint32_t
axl_loop_add_idle(AxlLoop *loop, AxlLoopCallback cb, void *data)
{
    if (loop == NULL || cb == NULL) {
        return 0;
    }

    return add_source(loop, SOURCE_IDLE, NULL, false, (void *)cb, data);
}

uint32_t
axl_loop_add_protocol_notify(AxlLoop *loop, void *guid,
                             AxlLoopCallback cb, void *data)
{
    AxlEventHandle  event;
    void           *registration;

    if (loop == NULL || guid == NULL || cb == NULL) {
        return 0;
    }

    if (axl_backend_event_create(&event) != 0) {
        axl_error("failed to create protocol notify event");
        return 0;
    }

    if (axl_backend_event_register_protocol_notify(guid, event,
                                                   &registration) != 0) {
        axl_error("failed to register protocol notify");
        axl_backend_event_close(event);
        return 0;
    }

    return add_source(loop, SOURCE_PROTOCOL, event, true, (void *)cb, data);
}

uint32_t
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
axl_loop_remove_source(AxlLoop *loop, uint32_t source_id)
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
// docs/AXL-Runtime.md.
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
    uint32_t done_src     = 0;
    uint32_t timeout_src  = 0;
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

