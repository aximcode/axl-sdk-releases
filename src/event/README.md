Foundational synchronization primitives for async operations: the
one-shot latch (**AxlEvent**), the typed stop-token (**AxlCancellable**),
and the interruptible wait helpers (**AxlWait**).

All three are tightly coupled: `AxlEvent` is the underlying primitive
(a UEFI event plus a signalled flag); `AxlCancellable` is an `AxlEvent`
with stop-token semantics and a use-after-free guard; the wait helpers
drive a throwaway `AxlLoop` until an event fires, a condition holds, a
timeout elapses, or Ctrl-C is received.

Headers:

- `<axl/axl-event.h>` -- AxlEvent and AxlEventHandle
- `<axl/axl-cancellable.h>` -- AxlCancellable
- `<axl/axl-wait.h>` -- `axl_wait_for`, `axl_wait_for_flag/word`,
  `axl_wait_ms`, `axl_wait_for_with_tick`

## A note on naming

"Event" is overloaded in AXL docs. It appears in three places:

- The **event loop** (`AxlLoop`) -- the dispatcher.
- An **event source** -- a thing registered with the loop (timer,
  idle, raw event, ...).
- `AxlEvent` -- the one-shot latch described below; one kind of
  thing the loop can dispatch.

This is the same overload UEFI uses. An `AxlEvent` is a one-shot
latch backed by a UEFI event; the event loop dispatches them.

## When to use what

| I need to... | Use |
|---|---|
| Let an async callback wake my main thread | `AxlEvent` + `axl_event_wait_timeout` |
| Let a caller abort any number of async ops | `AxlCancellable`, pass to the ops |
| Poll a hardware status register (CPU idles) | `axl_wait_for_word` / `axl_wait_for_flag` |
| Interruptible sleep | `axl_wait_ms` |
| Wait for a complex condition, driving a state machine | `axl_wait_for_with_tick` |

For the full concurrency taxonomy across dispatch / coordination /
notification / offload (including `AxlLoop`, `AxlPubsub`, `AxlTask`),
see [`docs/AXL-Concurrency.md`](../../docs/AXL-Concurrency.md).

## AxlEvent -- the one-shot latch

`AxlEvent` is AXL's foundational producer-waiter rendezvous. An async
callback signals; the main thread waits. The CPU idles between events
(not busy-wait), and Ctrl-C interrupts any wait.

```c
AxlEvent *e = axl_event_new();

start_async_op(on_done, e);
if (axl_event_wait_timeout(e, NULL, 5000000) != 0) {
    // -1 timeout, AXL_CANCELLED (-2) on Ctrl-C / cancel
}
axl_event_free(e);

static void on_done(void *user) {
    axl_event_signal(user);  // idempotent, NULL-safe
}
```

The `axl_event_is_set(e)` fast-check reads an internal flag without
driving the loop -- useful for "did my op already finish?" polls.
`axl_event_reset(e)` drops a pending signal so the same event can be
reused across cycles. `AXL_DEFINE_AUTOPTR_CLEANUP` makes RAII-style
lifetimes trivial.

Loop integration: `axl_loop_add_event(loop, handle, cb, data)` takes
an `AxlEventHandle`. For an AXL-managed event, pass
`axl_event_handle(e)`. For a firmware-owned raw event (TCP completion
token, protocol-notify event), pass the handle directly. One entry
point, two sources.

## AxlCancellable -- the typed stop-token

`AxlCancellable` is an `AxlEvent` with stop-token semantics: every
`axl_*_async` op accepts one, and signalling it aborts every op
observing it. Modelled on GLib's `GCancellable`, adapted to AXL's
single-threaded event loop.

Mechanically it's an `AxlEvent`. The distinct type exists to make
the contract visible at call sites: `axl_tcp_connect_async(host,
port, loop, cancel, cb, data)` tells you at a glance which argument
is the stop token.

```c
AXL_AUTOPTR(AxlCancellable) cancel = axl_cancellable_new();

// Any number of ops can share the same cancellable
axl_tcp_connect_async(host, port, loop, cancel, on_connected, ctx);
axl_loop_add_timeout(loop, 5000, timeout_fires_cancel, cancel);
axl_loop_run(loop);

// Each op's callback fires exactly once -- status=0 on success, a
// UEFI error code on protocol failure, or AXL_CANCELLED (-2) if the
// cancellable was signalled first.
```

Common patterns:
- **Timeout**: timer fires → `axl_cancellable_cancel(cancel)` → every
  op tagged with it stops.
- **Subsystem shutdown**: `axl_cancellable_cancel(app->shutdown)`
  aborts every outstanding op registered against `app->shutdown`.
- **User abort**: wire a Ctrl-C handler or UI button to
  `axl_cancellable_cancel` -- same result, different trigger.

Mental model: *`AxlCancellable` is to async ops what `AxlLoop` is to
event sources -- an optional container you tie operations to and tear
down independently.*

Ownership rule: the cancellable must outlive every op that observes
it. Same discipline as `AxlLoop` outliving its sources. `AXL_AUTOPTR`
helps -- declare the cancellable in a scope that outlives every async
op's loop run.

The `AXL_CANCELLED` return (`-2` in `<axl/axl-macros.h>`) covers both
explicit cancellation and the shell break event (Ctrl-C) -- consumers
see one return code for "some external source stopped me."

## Sleep and wait helpers

Two families, sharing one underlying primitive (timer event +
`axl_backend_event_wait`):

**Sleep** (`axl_sleep`, `axl_msleep`, `axl_usleep`) -- void return,
no cancel parameter, ergonomic. Use when all you want is "idle for
N time." Ctrl-C returns early (matching POSIX intuition), but
unlike Linux it does not auto-terminate the app -- execution
continues past the sleep. Apps that want Ctrl-C to exit observe it
at their main-loop boundary.

**Wait** (`axl_wait_ms`, `axl_wait_for_*`) -- int return, optional
`AxlCancellable`, condition predicates. Use when you need to
inspect the reason the wait returned, or when multiple async ops
share a stop token. Return convention: `0` = condition met / time
elapsed, `-1` = timeout, `AXL_CANCELLED` = Ctrl-C or cancel.

### Sleep (ergonomic)

```c
axl_msleep(100);       // idle 100ms, CPU idles, Ctrl-C returns early
axl_sleep(2);          // 2 seconds
axl_usleep(500);       // rounded up to 1ms (millisecond granularity)
```

For sub-millisecond hardware timing, the backend's `Stall` primitive
is still available internally -- not exposed to SDK consumers.

### Zero-callback waits

```c
// Wait until *flag becomes true (CPU idles, 1ms check cadence)
if (axl_wait_for_flag(&my_ready_flag, NULL, 500000) != 0) {
    return -1;
}

// Wait until a memory word stops matching a "not ready" sentinel
(void)axl_wait_for_word(&mmio->status, 0, NULL, 500000);

// Sleep with cancellation support -- the long form of axl_msleep
// when you need cancel-aware interruption or return-code inspection
int rc = axl_wait_ms(session_cancel, 100);
if (rc == AXL_CANCELLED) return AXL_CANCELLED;
```

### Callback form for complex conditions

When the condition needs code or the protocol needs periodic
driving:

```c
static bool is_done(void *ctx) { return ((Dev*)ctx)->state == DONE; }
static void drive  (void *ctx) { ((Dev*)ctx)->advance(ctx); }

axl_wait_for_with_tick(is_done, dev, drive, dev,
                       5000 /*tick_us*/,
                       NULL /*cancel*/, timeout_us);
```

Internally every wait creates a throwaway `AxlLoop`, registers the
relevant sources (event, timeout, optional tick), and runs until
something fires. Callers who already own a loop can build the same
pattern directly with `axl_loop_add_event` + `axl_loop_add_timeout` +
`axl_loop_run`; the helpers exist because that's a lot of boilerplate
for the common case.
