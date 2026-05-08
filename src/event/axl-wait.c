/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-wait.c
    Interruptible wait helpers + shared internal primitive that all
    wait-style AXL APIs build on (AxlEvent, axl_wait_*, Tier 4
    per-protocol helpers). Every path ultimately delegates to
    AxlLoop — there is no event multiplexing or Ctrl-C handling
    implemented here.
**/

#include "axl-wait-internal.h"
#include "axl-cancellable-internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <axl/axl-log.h>
#include <axl/axl-loop.h>
#include <axl/axl-wait.h>

AXL_LOG_DOMAIN("wait");

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

/* GCC's warn_unused_result isn't silenced by a plain (void) cast; use
   an assign-then-ignore pattern. The sleep family genuinely doesn't
   care about AXL_CANCELLED — Ctrl-C returns early anyway, and void
   return matches POSIX ergonomics. */
#define AXL_IGNORE_RC(expr) do { int _rc = (expr); (void)_rc; } while (0)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    AxlLoop   *loop;
    AxlCondFn  cond_fn;
    void      *cond_ctx;
    AxlTickFn  tick_fn;
    void      *tick_ctx;
    bool       condition_met;
    bool       timed_out;
    bool       cancelled;
} WaitCtx;

typedef struct {
    volatile const uint64_t *word;
    uint64_t                 not_ready_value;
} WordCondCtx;

/* Default tick cadence when caller needs periodic condition checks
   but has not supplied a specific period. 1 ms matches the granularity
   of the old `axl_backend_stall(1000)` idiom this API replaces. */
#define AXL_WAIT_DEFAULT_TICK_US  1000ULL

// ---------------------------------------------------------------------------
// Loop callbacks
// ---------------------------------------------------------------------------

static bool
on_wait_event(void *data)
{
    WaitCtx *w = (WaitCtx *)data;

    w->condition_met = true;
    axl_loop_quit(w->loop);
    return AXL_SOURCE_REMOVE;
}

static bool
on_wait_timeout(void *data)
{
    WaitCtx *w = (WaitCtx *)data;

    w->timed_out = true;
    axl_loop_quit(w->loop);
    return AXL_SOURCE_REMOVE;
}

static bool
on_wait_cancel(void *data)
{
    WaitCtx *w = (WaitCtx *)data;

    w->cancelled = true;
    axl_loop_quit(w->loop);
    return AXL_SOURCE_REMOVE;
}

static bool
on_wait_tick(void *data)
{
    WaitCtx *w = (WaitCtx *)data;

    if (w->tick_fn != NULL) {
        w->tick_fn(w->tick_ctx);
    }
    if (w->cond_fn != NULL && w->cond_fn(w->cond_ctx)) {
        w->condition_met = true;
        axl_loop_quit(w->loop);
        return AXL_SOURCE_REMOVE;
    }
    return AXL_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Internal primitive (shared by AxlEvent / axl_wait_* / Tier 4)
// ---------------------------------------------------------------------------

AxlStatus
_axl_event_wait_timeout_with_tick(
    AxlEventHandle  event,
    AxlCondFn       cond_fn,
    void           *cond_ctx,
    AxlTickFn       tick_fn,
    void           *tick_ctx,
    uint64_t        tick_us,
    AxlCancellable *cancel,
    uint64_t        timeout_us
    )
{
    WaitCtx        w;
    AxlLoop       *loop;
    AxlEventHandle cancel_event;
    uint32_t       event_source;
    uint32_t       timeout_source;
    uint32_t       tick_source;
    uint32_t       cancel_source;
    uint64_t       tick_ms;
    uint64_t       timeout_ms;
    bool           need_tick;
    int            rc;

    /* Fast path: already cancelled. Report immediately without loop. */
    if (axl_cancellable_is_cancelled(cancel)) {
        return AXL_CANCELLED;
    }

    /* Fast path: condition already holds. No loop, no allocation. */
    if (cond_fn != NULL && cond_fn(cond_ctx)) {
        return AXL_OK;
    }

    loop = axl_loop_new();
    if (loop == NULL) {
        axl_error("failed to create wait loop");
        return AXL_ERR;
    }

    w.loop = loop;
    w.cond_fn = cond_fn;
    w.cond_ctx = cond_ctx;
    w.tick_fn = tick_fn;
    w.tick_ctx = tick_ctx;
    w.condition_met = false;
    w.timed_out = false;
    w.cancelled = false;
    event_source = 0;
    timeout_source = 0;
    tick_source = 0;
    cancel_source = 0;

    if (event != NULL) {
        event_source = axl_loop_add_event(loop, event, on_wait_event, &w);
    }

    if (timeout_us > 0) {
        timeout_ms = timeout_us / 1000;
        if (timeout_ms == 0) {
            timeout_ms = 1;  /* round sub-ms timeouts up */
        }
        timeout_source = axl_loop_add_timeout(loop, (uint32_t)timeout_ms,
                                              on_wait_timeout, &w);
    }

    cancel_event = _axl_cancellable_event(cancel);
    if (cancel_event != NULL) {
        cancel_source = axl_loop_add_event(loop, cancel_event,
                                           on_wait_cancel, &w);
    }

    /* Install a tick if the caller asked for one, or if we have a
       condition to poll but nothing else that would wake the loop. */
    need_tick = (tick_us > 0) ||
                (cond_fn != NULL && tick_fn == NULL && event == NULL);
    if (need_tick) {
        tick_ms = tick_us / 1000;
        if (tick_ms == 0) {
            tick_ms = AXL_WAIT_DEFAULT_TICK_US / 1000;
        }
        tick_source = axl_loop_add_timer(loop, (uint32_t)tick_ms,
                                         on_wait_tick, &w);
    }

    rc = axl_loop_run(loop);

    /* Explicitly deregister each source before freeing the loop. The
       loop would free cleanly either way, but an active caller-owned
       event source at loop_free triggers the lifetime-ordering
       warning added to protect consumers from UAF — our ephemeral
       loop isn't a UAF risk (nobody else holds the source id) but we
       should model the expected pattern anyway. */
    if (event_source != 0) {
        axl_loop_remove_source(loop, event_source);
    }
    if (timeout_source != 0) {
        axl_loop_remove_source(loop, timeout_source);
    }
    if (tick_source != 0) {
        axl_loop_remove_source(loop, tick_source);
    }
    if (cancel_source != 0) {
        axl_loop_remove_source(loop, cancel_source);
    }
    axl_loop_free(loop);

    if (w.condition_met) {
        return AXL_OK;
    }
    if (w.cancelled) {
        return AXL_CANCELLED;
    }
    if (rc == -1 && !w.timed_out) {
        return AXL_CANCELLED;  /* Ctrl-C / shell break */
    }
    return AXL_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Tier 1 — sleep (void return, no cancel, ergonomic)
// ---------------------------------------------------------------------------

void
axl_sleep(uint64_t seconds)
{
    AXL_IGNORE_RC(axl_wait_ms(NULL, seconds * 1000));
}

void
axl_msleep(uint64_t milliseconds)
{
    AXL_IGNORE_RC(axl_wait_ms(NULL, milliseconds));
}

void
axl_usleep(uint64_t microseconds)
{
    /* Round sub-ms durations up to 1ms — the underlying wait
       primitive uses millisecond-granularity timers. Callers who
       need sub-millisecond precision should use the backend's
       Stall primitive directly (internal, not exposed). */
    AXL_IGNORE_RC(axl_wait_ms(NULL, (microseconds + 999) / 1000));
}

// ---------------------------------------------------------------------------
// Tier 2 — zero-callback convenience
// ---------------------------------------------------------------------------

static bool
flag_cond(void *ctx)
{
    volatile const bool *flag = (volatile const bool *)ctx;
    return *flag;
}

AxlStatus
axl_wait_for_flag(
    volatile const bool *flag,
    AxlCancellable      *cancel,
    uint64_t             timeout_us
    )
{
    if (flag == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(NULL, flag_cond, (void *)flag,
                                             NULL, NULL, 0,
                                             cancel, timeout_us);
}

static bool
word_cond(void *ctx)
{
    WordCondCtx *c = (WordCondCtx *)ctx;
    return *c->word != c->not_ready_value;
}

AxlStatus
axl_wait_for_word(
    volatile const uint64_t *word,
    uint64_t                 not_ready_value,
    AxlCancellable          *cancel,
    uint64_t                 timeout_us
    )
{
    WordCondCtx ctx;

    if (word == NULL) {
        return AXL_ERR;
    }

    ctx.word = word;
    ctx.not_ready_value = not_ready_value;
    return _axl_event_wait_timeout_with_tick(NULL, word_cond, &ctx,
                                             NULL, NULL, 0,
                                             cancel, timeout_us);
}

AxlStatus
axl_wait_ms(
    AxlCancellable *cancel,
    uint64_t        ms
    )
{
    AxlStatus rc;

    if (ms == 0) {
        return AXL_OK;
    }

    rc = _axl_event_wait_timeout_with_tick(NULL, NULL, NULL,
                                           NULL, NULL, 0,
                                           cancel, ms * 1000);
    /* Pure sleep has no condition or event, so the timeout firing IS
       the success case. The primitive can't tell sleep apart from a
       failed-to-satisfy wait, so map AXL_TIMEOUT → AXL_OK here.
       AXL_CANCELLED propagates through unchanged. */
    return (rc == AXL_TIMEOUT) ? AXL_OK : rc;
}

// ---------------------------------------------------------------------------
// Tier 3 — callback form
// ---------------------------------------------------------------------------

AxlStatus
axl_wait_for(
    AxlCondFn       cond_fn,
    void           *cond_ctx,
    AxlCancellable *cancel,
    uint64_t        timeout_us
    )
{
    if (cond_fn == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(NULL, cond_fn, cond_ctx,
                                             NULL, NULL, 0,
                                             cancel, timeout_us);
}

AxlStatus
axl_wait_for_with_tick(
    AxlCondFn       cond_fn,
    void           *cond_ctx,
    AxlTickFn       tick_fn,
    void           *tick_ctx,
    uint64_t        tick_us,
    AxlCancellable *cancel,
    uint64_t        timeout_us
    )
{
    if (cond_fn == NULL) {
        return AXL_ERR;
    }
    return _axl_event_wait_timeout_with_tick(NULL, cond_fn, cond_ctx,
                                             tick_fn, tick_ctx, tick_us,
                                             cancel, timeout_us);
}
