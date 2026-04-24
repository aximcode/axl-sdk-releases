Event loop with timer, keyboard, idle, protocol notification, and raw event
sources. GLib-inspired main loop with FUSE-style primitives.

Also includes the deferred work queue (**AxlDefer**) and the publish/subscribe
event bus (**AxlPubsub**), both integrated with the loop.

Headers:

- `<axl/axl-loop.h>` — Event loop core
- `<axl/axl-defer.h>` — Deferred work queue (ring buffer)
- `<axl/axl-pubsub.h>` — Publish/subscribe event bus

## When to Use What

| I need to... | Use |
|---|---|
| Run code every N milliseconds | `axl_loop_add_timer` |
| Run code once after a delay | `axl_loop_add_timeout` |
| React to keyboard input | `axl_loop_add_key_press` |
| Do background work between events | `axl_loop_add_idle` |
| Know when a UEFI protocol appears | `axl_loop_add_protocol_notify` |
| Integrate a TCP/custom EFI_EVENT | `axl_loop_add_event` |
| Schedule work from a constrained context | `axl_defer` |
| Decouple modules with events | `axl_pubsub_publish` / `axl_pubsub_subscribe` |
| Run a simple event-driven app | `axl_loop_run` |
| Build a FUSE-style driver loop | `axl_loop_next_event` / `axl_loop_dispatch_event` |
| Share a loop across modules | `axl_loop_default` (runtime-owned singleton) |
| Wait in a callback without freezing outer sources | `axl_loop_iterate_until` |

## Overview

UEFI applications are single-threaded and event-driven. The event loop
is the central dispatcher: it waits for events (timers, keyboard input,
network I/O, custom events) and calls registered callbacks.

## Basic Pattern

```c
#include <axl.h>

static bool on_timer(void *data) {
    axl_printf("tick\n");
    return AXL_SOURCE_CONTINUE;  // keep firing
}

static bool on_timeout(void *data) {
    axl_loop_quit(data);
    return AXL_SOURCE_REMOVE;    // one-shot, auto-removed
}

int main(int argc, char **argv) {
    AXL_AUTOPTR(AxlLoop) loop = axl_loop_new();

    axl_loop_add_timer(loop, 1000, on_timer, NULL);    // every 1s
    axl_loop_add_timeout(loop, 5000, on_timeout, loop); // quit after 5s

    axl_loop_run(loop);  // blocks until axl_loop_quit
    return 0;
}
```

## Callback Signatures

All loop callbacks return `bool`:
- `AXL_SOURCE_CONTINUE` (`true`) — keep the source active
- `AXL_SOURCE_REMOVE` (`false`) — remove it from the loop

```c
// Generic callback (timers, timeouts, idle, protocol, raw events)
typedef bool (*AxlLoopCallback)(void *data);

// Key press callback (receives the key)
typedef bool (*AxlKeyCallback)(AxlInputKey key, void *data);
```

Every `axl_loop_add_*` function returns a `uint32_t` source ID. Use it
with `axl_loop_remove_source(loop, id)` to remove a source early:

```c
uint32_t timer_id = axl_loop_add_timer(loop, 1000, on_tick, NULL);
// ...later...
axl_loop_remove_source(loop, timer_id);  // stop the timer
```

## Source Types

### Timer (repeating)

Fires every N milliseconds. Returns CONTINUE to keep firing.

```c
static bool heartbeat(void *data) {
    send_keepalive(data);
    return AXL_SOURCE_CONTINUE;
}
axl_loop_add_timer(loop, 30000, heartbeat, conn);  // every 30s
```

### Timeout (one-shot)

Fires once after a delay, then auto-removes. Useful for deadlines.

```c
static bool connection_timeout(void *data) {
    axl_warning("connection timed out");
    axl_loop_quit(data);
    return AXL_SOURCE_REMOVE;
}
axl_loop_add_timeout(loop, 10000, connection_timeout, loop);
```

### Idle

Runs on every loop iteration before the blocking wait. Use for
background work (progress updates, polling, animations).

```c
static bool update_progress(void *data) {
    int *pct = data;
    axl_printf("\rprogress: %d%%", *pct);
    return (*pct < 100) ? AXL_SOURCE_CONTINUE : AXL_SOURCE_REMOVE;
}
axl_loop_add_idle(loop, update_progress, &percent);
```

### Key Press

Fires on console keyboard input with the key data.

```c
static bool on_key(AxlInputKey key, void *data) {
    if (key.unicode_char == 'q') {
        axl_loop_quit(data);
        return AXL_SOURCE_REMOVE;
    }
    axl_printf("key: %c\n", (char)key.unicode_char);
    return AXL_SOURCE_CONTINUE;
}
axl_loop_add_key_press(loop, on_key, loop);
```

### Protocol Notify

Fires when a UEFI protocol is installed on any handle. Use this to
react to hot-plug events (NIC driver loaded, new filesystem mounted).

```c
static bool on_nic_ready(void *data) {
    axl_info("network interface appeared");
    start_network(data);
    return AXL_SOURCE_REMOVE;  // only need the first one
}

// Watch for the SNP (Simple Network Protocol) GUID
axl_loop_add_protocol_notify(loop, &gEfiSimpleNetworkProtocolGuid,
                             on_nic_ready, app_ctx);
```

### Raw Event

Integrates a UEFI event into the loop. The entry point takes an
`AxlEventHandle` (raw `EFI_EVENT`) so the same API works for both
AXL-managed events (`AxlEvent *`, via `axl_event_handle(e)`) and
firmware-owned handles (TCP completion tokens, protocol-notify
events). The caller owns the event.

```c
// AXL-managed event (new/free/signal/reset state machine in AXL):
AxlEvent *my_event = axl_event_new();

axl_loop_add_event(loop, axl_event_handle(my_event),
                   on_custom_event, ctx);

// From another context (e.g., a protocol callback):
axl_event_signal(my_event);  // triggers on_custom_event on next tick

// Cleanup (after removing from loop):
axl_event_free(my_event);
```

See [../event/README.md](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) for AxlEvent semantics
(signal / reset / is_set / wait_timeout) and its typed stop-token
cousin, `AxlCancellable`.

## Lifecycle & Cleanup

Tear down caller-owned resources — sockets, async ops, custom
`AxlEvent` sources — before the loop they were registered against.
If `axl_loop_free` finds a raw `AxlEvent` source still active it logs
an error naming the source id, which usually points at a resource
freed in the wrong order (e.g. the loop outlived by a lingering async
op's completion event). Source types owned by the loop (timers,
idle, key-press, protocol-notify, defer) are cleaned up automatically.

## Run vs. Next+Dispatch

`axl_loop_run` blocks until `axl_loop_quit` is called. For manual
control (e.g., FUSE-style drivers), use the step API:

```c
while (running) {
    int rc = axl_loop_next_event(loop, true);  // block until event
    if (rc == -1) break;                        // Ctrl-C
    axl_loop_dispatch_event(loop);              // fire callbacks
    // ... do other work between iterations ...
}
```

Use `axl_loop_dispatch(loop, false)` for a non-blocking single step
(check + dispatch if ready, return immediately if not).

## Nested Waits (`axl_loop_iterate_until`)

The standard ephemeral-loop approach for waiting (`axl_event_wait_timeout`,
`axl_wait_*`) creates a throwaway loop for the duration of the wait
-- the caller's outer loop is paused, and its sources (timers, idle,
etc.) don't fire until the wait returns. That's usually what you
want; it's also clean because the inner loop's sources can't leak
into the outer.

But sometimes a source callback needs to wait on an async producer
*and* keep the outer loop's own sources alive. For that, use
`axl_loop_iterate_until` on the outer loop directly:

```c
int rc = axl_loop_iterate_until(
    outer,             /* the caller's own loop */
    done_event,        /* NULL OK -- only timeout wakes */
    timeout_us);       /* 0 = wait forever */
```

Drives `outer` until `done` is signalled, the timeout elapses, or
Ctrl-C. Does NOT set `outer->quit_requested`, so the enclosing
`axl_loop_run` resumes normally afterwards. Returns 0 on done,
-1 on timeout, `AXL_CANCELLED` on interrupt. See
[`docs/AXL-Runtime.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md) §5.6.

## Default Loop (`axl_loop_default`)

The runtime (see [`src/runtime/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/runtime/README.md))
exposes a shared singleton loop, **created lazily on the first
`axl_loop_default()` call** (CRT0 does not pre-create it) and
freed during `_axl_cleanup` if it was ever materialized. Apps
can:

1. Ignore it entirely — `axl_yield()` still observes Ctrl-C by
   polling the break flag directly when `mDefaultLoop == NULL`.
2. Register sources on it and call `axl_yield()` in a tight CPU
   loop — yields dispatch the loop non-blocking, so timers,
   timeouts, defers, and raw events fire in line. **Idle sources
   are a footgun in this mode**: they run on every yield, not
   just when the loop is genuinely idle. See
   [`docs/AXL-Runtime.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Runtime.md) §2.6.
3. Call `axl_loop_run(axl_loop_default())` to hand control to the
   loop — appropriate for event-driven servers.

Private loops via `axl_loop_new()` remain first-class and are
often the right choice for scoped work.

## AxlDefer

Deferred work queue — schedules a function to run on the next loop
iteration. Use in constrained contexts where complex work isn't safe:

- Protocol notification callbacks (UEFI restricts what you can call)
- Nested callbacks (avoid re-entrancy)
- Interrupt-like handlers (need to return quickly)

### Callback Signature

```c
typedef void (*AxlDeferCallback)(void *data);
```

### Usage

```c
// Called from a protocol notification (constrained — can't do Boot Services)
void on_protocol_installed(void *ctx) {
    axl_defer(loop, initialize_new_protocol, ctx);
}

// Runs safely on the next main loop tick (full Boot Services available)
void initialize_new_protocol(void *ctx) {
    locate_and_configure(ctx);
}
```

### Cancellation

```c
uint32_t handle = axl_defer(loop, some_work, ctx);
// ... changed my mind ...
axl_defer_cancel(loop, handle);  // no-op if already fired
```

The queue is a fixed-capacity ring buffer with no dynamic allocation
in the hot path. Deferred work is drained automatically at the start
of each loop iteration.

## AxlPubsub

Publish/subscribe event bus for decoupling modules. Modules publish on
named topics; other modules subscribe with callbacks. Delivery is deferred
(via **AxlDefer**) so handlers always run in a safe main-loop context.

### When to Use Pub/sub

- **Decoupling** — a producer doesn't know (or care) who its consumers are
- **Multiple consumers** — adding a new subscriber requires zero changes to the producer
- **Cross-module events** — "network is ready", "config changed", "shutdown requested"

For point-to-point communication (one caller, one callee), use a
direct function call or a callback pointer instead.

### Callback Signature

```c
typedef void (*AxlPubsubCallback)(
    void *event_data,  // from axl_pubsub_publish (may be NULL)
    void *user_data    // from axl_pubsub_subscribe
);
```

### Producer / Consumer Example

```c
// --- Producer (network module) ---

typedef struct {
    char ip[16];
    char gateway[16];
} NetConfig;

void on_dhcp_complete(AxlLoop *loop, NetConfig *cfg) {
    // Publish to all subscribers — producer doesn't know who listens
    axl_pubsub_publish(loop, "ip-changed", cfg);
}

// --- Consumer 1 (splash screen) ---

void on_ip_changed(void *event_data, void *user_data) {
    NetConfig *cfg = event_data;
    update_splash_ip(cfg->ip);
}

uint32_t handle = axl_pubsub_subscribe(loop, "ip-changed", on_ip_changed, NULL);

// --- Consumer 2 (REST API) --- completely independent

void on_ip_changed_api(void *event_data, void *user_data) {
    NetConfig *cfg = event_data;
    restart_http_server(cfg->ip);
}

axl_pubsub_subscribe(loop, "ip-changed", on_ip_changed_api, NULL);
```

### Data Lifetime

**Important:** `event_data` passed to `axl_pubsub_publish` must remain valid
until the next loop tick, because delivery is deferred. Stack variables
are fine if `publish` and the next `loop_dispatch` happen in the same
function scope. For longer lifetimes, heap-allocate or use a static.

### Unsubscribe

```c
uint32_t handle = axl_pubsub_subscribe(loop, "ip-changed", on_ip_changed, NULL);
// ...later (e.g., on module shutdown)...
axl_pubsub_unsubscribe(loop, handle);
```

Always unsubscribe before freeing the `user_data` pointer, or the
callback will fire with a dangling pointer.

Topics are auto-created on first `subscribe` or `publish`.
`axl_pubsub_reset(loop)` clears all topics and subscribers (for shutdown
or between test runs).

## See also

- [`docs/AXL-Concurrency.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Concurrency.md) — the
  full primitive-selection taxonomy across dispatch / coordination /
  notification / offload, including where `AxlLoop`, `AxlDefer`, and
  `AxlPubsub` fit alongside `AxlEvent`, `AxlCancellable`, and the
  `AxlTask` pool.
- [`src/event/README.md`](https://github.com/aximcode/axl-sdk-releases/blob/main/src/event/README.md) — `AxlEvent`,
  `AxlCancellable`, and the `axl_wait_*` helpers.
