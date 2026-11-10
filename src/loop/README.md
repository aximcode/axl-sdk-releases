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

## Dispatch Order

Each iteration dispatches **one** signalled source. Selection is
round-robin: every wait starts just past the source picked last, so a
source that is ready on every iteration — a listener under a
connection flood, or a callback that re-signals its own event — takes
its turn and then waits behind every other signalled source, instead
of winning each time. The loop's own events (the poll timer that
drains keypress sources, the serial Ctrl-C check) take part in the
same rotation. Without it both `gBS->WaitForEvent` and the
non-blocking check favour the lowest-numbered source, and an
always-ready source starves everything registered after it. The
shell's Ctrl-C is checked before every wait regardless of order. No
other ordering is promised.

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

Every `axl_loop_add_*` function returns an `AxlSourceId` (a 64-bit handle;
0 means failure). Ids come from a single process-global counter, so a stale
id never collides with a source on another loop. Use it with
`axl_loop_remove_source(loop, id)` to remove a source early:

```c
AxlSourceId timer_id = axl_loop_add_timer(loop, 1000, on_tick, NULL);
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

## Driver Mode (`axl_loop_attach_driver`)

`axl_loop_run` is the foreground driver — it owns `TPL_APPLICATION`
and blocks in `gBS->WaitForEvent`. UEFI driver entry points have no
foreground caller: `DriverEntry` returns to the firmware after
publishing protocols. Without a foreground caller, sources never
dispatch and timers never fire — anything async in the loop is
dead. `axl_loop_attach_driver` is the bridge for the DXE-driver use
case (HTTP server inside a driver image, async pubsub-driven
worker, etc.).

```c
EFI_STATUS EFIAPI DriverEntry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    axl_driver_init(image, st);
    axl_driver_set_unload(MyUnload);

    AxlLoop *loop = axl_loop_new();
    AxlHttpServer *server = axl_http_server_new(...);
    axl_http_server_start(server, loop);
    axl_http_server_listen(server, 80);

    /* Hand the loop to firmware-managed dispatch. 50 ms is the
       typical period — frequent enough for a responsive HTTP
       server, sparse enough to leave headroom. */
    if (axl_loop_attach_driver(loop, 50) != AXL_OK) {
        axl_printf("FAIL: loop attach\n");
        return EFI_ABORTED;
    }
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI MyUnload(EFI_HANDLE image) {
    /* Detach BEFORE freeing the loop so no notify is in flight
       when consumer state goes away. */
    axl_loop_detach_driver(loop);
    axl_http_server_free(server);
    axl_loop_free(loop);
    return EFI_SUCCESS;
}
```

### The TPL Contract — and Why You Don't Roll Your Own

UEFI 2.11 §7.1 allows only `TPL_CALLBACK` or `TPL_NOTIFY` for
`EVT_NOTIFY_SIGNAL` events — there is no signal queue at
`TPL_APPLICATION`. `axl_loop_attach_driver` uses `TPL_CALLBACK`,
the same TPL that co-located firmware drivers (TCP4, MNP, SNP)
use for their own state-machine notifies. Because they share the
TPL, the firmware's FIFO notify queue alternates between them and
us — as long as no one holds `TPL_CALLBACK` for too long, everyone
makes progress.

**The notify-budget rule.** The consumer's loop source callbacks
run inside `axl_loop_dispatch` at `TPL_CALLBACK`. If a callback
does heavy work — large allocation, synchronous I/O, a
multi-millisecond loop, a blocking protocol call — it holds
`TPL_CALLBACK` for that whole duration. While we're holding
`TPL_CALLBACK`, **TCP4 / MNP / SNP cannot advance their own
notifies** (same level, no preemption). At best you see latency
spikes; at worst, a co-located TCP4 listener can't progress its
accept-rearm state machine and connections start failing.

The pre-built helper drains every signaled event per tick (capped
at `2 × AXL_MAX_SOURCES` as a runaway guard; hitting the cap is
logged), in the round-robin order described under
[Dispatch Order](#dispatch-order). Per-tick drain is what matches the consumer's expected
contract — under HTTP load a recv-data callback synchronously
submits `axl_tcp_send_async`, TCP4 typically completes the
Transmit inline, and the tx-event needs to be drained the same
tick or the on_response_sent callback queues behind whatever
else fires next. A naive one-`axl_loop_dispatch`-per-tick loop
quietly starves completion handlers under sequential request
load — accept (slot 0) keeps preempting, conn-pool slots fill
with `active=true` connections whose response-completion never
fires, and the listener appears wedged after exactly
`HTTP_DEFAULT_MAX_CONNS` requests. Rolling your own with
`gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
...)` calling `axl_loop_dispatch` directly *can* work — but you
need the same drain pattern AND the same notify-budget
discipline.

**Boot-Services TPL ceiling.** `gBS->WaitForEvent` returns
`EFI_UNSUPPORTED` above `TPL_APPLICATION`, so the dispatch is
non-blocking-only. Don't call `axl_loop_iterate_until` with a
non-zero timeout from inside a source callback — it would try to
WaitForEvent at the wrong TPL.

**What to do with slow work.** Break it up, and yield between the
pieces with a one-shot timer (`axl_loop_add_timeout`): it fires no
earlier than the firmware's next timer interrupt, so in most ticks
`TPL_CALLBACK` drops back to the firmware between pieces and co-located
drivers (MNP, TCP4) progress. Not in a tick that is idle-extended past
that interrupt: there the timer is signalled mid-tick and the same tick
dispatches it, while the firmware's own timers still wait. A driver
budget of 0 (`axl_loop_set_driver_budget_ms`) is the strict yield.
`axl_defer` (`<axl/axl-defer.h>`) is NOT a yield here: deferred work
keeps a tick going like a busy idle source, so a
chain of deferred pieces runs back to back for up to the tick's
budget -- 5 ms of a busy foreground's tick by default, up to nine
tenths of the period while the foreground waits. Use it to run work
outside the callback that queued it, in the same tick.

**Busy idle sources and the tick budget.** A tick normally ends at
the first pass that finds nothing signalled. Deferred work queued in a
pass (work a deferred callback defers) and a busy idle source
(`axl_loop_add_idle_busy` -- AxlTcp's transmit pacer is the one in
the library) keep it iterating for the loop's driver budget
(`axl_loop_set_driver_budget_ms`, 5 ms by default), clamped to half
the tick period so a busy foreground keeps the other half.

When the tick finds the foreground *waiting* instead -- a Shell at its
prompt, anything halted in `gBS->WaitForEvent` -- that half would only
idle the CPU, so the tick uses the idle budget
(`axl_loop_set_driver_idle_budget_ms`): by default nine tenths of the
period, never past `AXL_LOOP_DRIVER_BUDGET_MAX_MS`. "Waiting" means
waiting through both periods before the tick without running once, so
a Shell that goes idle at its prompt extends from the third tick on.
EDK2 signals its idle-loop event group from every idle pass of
`WaitForEvent` and then halts the CPU. The loop notes each pass, at
TPL_NOTIFY ahead of the halt, and each halt's end. A period is idle
when the foreground was waiting at the tick that began it, every pass
in it came at once after that tick or a halt ended, and the last halt
ended in the interrupt that queued this tick. A tick queued behind the
loop's own previous one, which ran into its interrupt, is no evidence
either way -- the foreground never had the CPU -- so it does not extend
and changes nothing, and an extended tick stops starting passes in time
for the next one (the last pass's length predicts the next).
Sampling only at tick instants is not enough: EDK2 fires every timer on
its 10 ms system tick, so a renderer's wakes land on driver ticks, and
an extension there runs before the renderer resumes. Measured with
that rule, a renderer doing 4 ms of work per frame beside a 10 ms tick
ran at half its frame rate; with this one it keeps all of it. On
firmware that does not signal the group, or orders its members
otherwise, every tick keeps the clamp.

The two settings are separate on purpose: the driver budget bounds
what a busy foreground loses, the idle budget bounds how late a
waiting one answers (a GUI host promising keypress latency sets it
low). A driver budget of 0 turns off all extension; an idle budget of
0 only the idle one. Measured with sshd's 10 ms tick on x64 QEMU
(RELEASE): a 16 MiB SFTP download went from 7.7 to 11.5 MB/s on
e1000 and from 9.2 to 13.5 MB/s on virtio-net.

### Ctrl-C belongs to the Shell

A driver-attached loop does not intercept Ctrl-C unless you ask it
to. A resident driver is not the foreground: the Shell's break is
meant for whatever the Shell is running, and the Shell's script
runner polls the same `ExecutionBreak` event between lines. Reading
that event consumes it, so a tick that checked it every 10 ms would
swallow every Ctrl-C sent to the Shell, from the keyboard, the
serial line or a remote console. A quit would also stop the
driver's loop for good, because the tick re-arms `running` but
never clears the quit request.

So with no explicit choice, a loop intercepts only in the
foreground: an application image's loop that is not
driver-attached. It leaves the break alone while driver-attached,
and a loop created anywhere in a driver image leaves it alone too,
attached or not. That covers the short-lived loop a synchronous
call builds (`axl_wait_ms`, the sync TCP/UDP/DNS wrappers): the
Shell's break would otherwise end it, and reading it would take the
break from the Shell. The library's own break poll under long
operations (file copies, sorts, digests, the sync HTTP client) is
skipped in a driver image as well; it would otherwise also try to
exit an image that cannot exit. Detaching an app's loop restores
the foreground default. An explicit
`axl_loop_set_intercept_break()` wins either way. Opting a driver
loop in means the Shell's next Ctrl-C stops it from dispatching for
the rest of its residency.

The cost is deliberate: a driver whose entry point blocks in a
synchronous wait cannot be aborted with Ctrl-C. Bound such waits
with a timeout or an `AxlCancellable`.

### Cleanup

`axl_loop_detach_driver` cancels the timer, drains any in-flight
notify, and frees the bridging context. If `DriverUnload` forgets
to call it, `axl_loop_free` will detach as a safety net (with a
warning) — but the right place is `DriverUnload`, BEFORE freeing
the loop and BEFORE unregistering protocols, so no notify is mid-
dispatch when consumer state goes away.

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
AxlStatus rc = axl_loop_iterate_until(
    outer,             /* the caller's own loop */
    done_event,        /* NULL OK -- only timeout wakes */
    timeout_us);       /* 0 = wait forever */
```

Drives `outer` until `done` is signalled, the timeout elapses, or
Ctrl-C. Does NOT set `outer->quit_requested`, so the enclosing
`axl_loop_run` resumes normally afterwards. Returns `AXL_OK` on done,
`AXL_TIMEOUT` on timeout, `AXL_CANCELLED` on interrupt. See
[`docs/AXL-Lifecycle.md` §5.6](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#56-nested-wait-primitive-axl_loop_iterate_until).

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
   [`docs/AXL-Lifecycle.md` §2.6](https://github.com/aximcode/axl-sdk-releases/blob/main/docs/AXL-Lifecycle.md#26-idle-callbacks-and-yield-driven-loops).
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
of each loop iteration. Work queued after that drain -- by a deferred
callback itself -- runs on the very next iteration: while any is
queued, the loop only checks its events instead of waiting on them, so
it is never held until the 10 ms poll tick. Under
`axl_loop_attach_driver` the next iteration is the next driver tick.

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
