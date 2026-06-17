# AXL Loop Re-entrancy / Nesting Remediation Plan

**Status:** In progress. **Created:** 2026-06-16. **Updated:** 2026-06-16.
Items **4** (raised-TPL safety net, shipped), **5** (`axl_yield` split), and
**1** (re-entrancy warn-guard) are **done**. The handler-I/O audit (the plan's
own gate) found **no current consumer** for Items **2** (deferred responses)
and **3** (async HTTP client), so both are **design-ready but deferred** rather
than built speculatively — see their sections.
**Companion to:** [AXL-Concurrency.md](AXL-Concurrency.md) (the model this
refines), [AXL-Lifecycle.md](AXL-Lifecycle.md), [AXL-SoftBMC-Port-Design.md](AXL-SoftBMC-Port-Design.md).

**Regression detection** for this whole bug family — the consumer-emulator
harness (the resident-driver / shared-loop / raised-TPL topology that surfaced
every wedge below), the `AXL_DEBUG_ASSERT` invariant guards, and the liveness
watchdog — lives in [AXL-Concurrency.md § Testing the model](AXL-Concurrency.md#testing-the-model).
This plan owns the *structural fix*; that section owns *catching the next
instance in-repo* before a consumer does.

**Items 2–3 are now being built** (un-deferred by SoftBMC's alert webhook, the
first concrete in-callback-I/O consumer). Live implementation status —
the async HTTP client + async DNS resolve + the sync-wraps-async refactors —
is tracked in [AXL-Async-HTTP-Plan.md](AXL-Async-HTTP-Plan.md).

This plan addresses the single recurring bug family surfaced by the SoftBMC
EDK2 → axl-sdk port: **blocking on an event loop that is already running.**
It supersedes the near-term stance in `AXL-Concurrency.md` § "Where this breaks
down" ("the sync wrappers are the answer") with concrete data — the sync
wrappers *are* the pain point for a long-running reactive service.

---

## 1. Problem

axl-sdk's goal was "implement SoftBMC as a single main loop with multiple
sources." In practice the port hit a string of wedges, all the same shape: a
**synchronous call made from inside a callback already running on a loop.**

Confirmed instances (all fixed reactively, one patch at a time):

| Symptom | Trigger | Fix |
|---|---|---|
| WS-teardown wedges driver tick | sync send in a WS frame handler | `e90b87e4` |
| syslog wedges server under console-mirror | sync `axl_udp_send` from the driver pump (raised TPL) | `d249a9b6` |
| TLS resident-loop handshake hang | sync handshake on a running loop | (storage/tls session) |
| server2 never dispatches | fresh nested `axl_loop_new()` source-id collides with the outer loop | `adbf5461` |

The whack-a-mole cadence is the real signal: each is a fresh manifestation of
the same structural fault, found only at runtime.

## 2. Diagnosis

The fault is **not "nested loops are bad" in the abstract.** It is **blocking
on a loop that is already running**, which splits into two sub-families:

- **(a) New-loop artifacts.** The sync wait does `axl_loop_new()` +
  `axl_loop_run()` ([src/event/axl-wait.c:148](../src/event/axl-wait.c#L148)),
  creating a *second* loop. Its per-loop source-id space used to overlap the
  outer loop's, so an id outliving the ephemeral loop could be removed from the
  outer one and kill an unrelated source (`adbf5461`). **Fixed at the root:**
  source ids now come from a single process-global counter
  ([src/loop/axl-loop.c](../src/loop/axl-loop.c)), so a stale id never matches a
  source on another loop and the cross-loop removal is a no-op — the collision
  class is unrepresentable, and the sync wrappers' collision-guard id-clearing
  (and its asserts) were removed. (The poll-timer is still per-loop but is
  removed on ephemeral-loop teardown.)
- **(b) Fundamental re-entrancy.** You are dispatching a loop from inside its
  own callback; another callback runs while the first is suspended on the
  stack. Unavoidable for *any* blocking-on-reactive call, however clean the
  implementation. This is the one that bites.

Industry precedent: GLib/GTK's `gtk_dialog_run` ran a nested `GMainLoop` and
was a re-entrancy footgun — **GTK4 deleted it.** Our instinct is aligned.

**The key contextual distinction.** Blocking is safe in exactly one situation:
**no outer loop is running** (the CLI tools — `fetch`, `netinfo`, ping — have
no main loop, so the nested run *is* the only loop). It is poison the moment a
loop is running (a server handler, a driver pump). The rule is therefore
*contextual*, not "never nest."

**The structural cause for services.** The HTTP handler contract is
synchronous —
[`AxlHttpHandler`](../include/axl/axl-http-server.h) must fill `resp` and
return. So any handler that does I/O (a downstream HTTP call, a DNS lookup, an
async-over-UEFI BMC query) is *forced* to block → which nests. **The
architecture funnels I/O-bound handlers into nested loops.** You cannot have
"one loop, no nesting" *and* "handlers that block for I/O" — they are
contradictory. The fix is to make blocking *unnecessary* on the reactive path,
not to make nested loops safer.

## 3. Target model

1. **Services are async-first.** A long-running reactive app (SoftBMC, the
   dashboard server) uses async APIs + callbacks exclusively. No blocking
   networking from a loop callback.
2. **Library code never implicitly re-enters the consumer's dispatch.** A
   helper called from a consumer callback must not silently pump or nest the
   consumer's loop behind their back.
3. **Sync/blocking APIs remain first-class for the no-loop case** (CLI tools).
   They become *self-protecting*: loud failure if misused on a running loop,
   rather than a silent wedge.
4. **Defense in depth.** Where a blocking call slips through anyway, it
   degrades to a bounded latency spike, never a hard wedge.

---

## 4. Work items

The four items from the architecture discussion, plus Item 5 (the
`axl_yield` re-dispatch seam, analyzed 2026-06-16 — same family).

### Item 1 — Re-entrancy guard on the sync wait primitive  ·  *highest leverage, cheapest*

**What.** Make `_axl_event_wait_timeout_with_tick`
([src/event/axl-wait.c](../src/event/axl-wait.c)) detect "a loop is already
running/dispatching in this (single-threaded) call stack" and return a loud
`AXL_BUSY` / `AXL_WOULD_BLOCK` instead of silently spinning up a nested loop.

**Why.** Converts every latent wedge into an immediate, debuggable error *at
the offending call site*. Would have turned all four §1 wedges into instant,
obvious failures during the port. It is also a no-op for the CLI case (no loop
running → sync still works), so it costs the safe consumers nothing.

**Design sketch.** Maintain a process-global "active dispatch" marker
(current-loop pointer or depth counter) set by `axl_loop_run`,
`axl_loop_dispatch`, and the driver pump (`driver_dispatch_notify`), restored
on exit. The wait primitive checks it before `axl_loop_new()`. Raised-TPL is a
subset of this condition, so the marker subsumes the TPL check too.

**Tension with Item 4 (resolve explicitly).** The raised-TPL fix makes
sync-from-pump *work* (spin); this guard makes it *fail loudly*. They are
complementary, not contradictory: the guard is the **policy** ("don't block on
a running loop — fix your code"); the raised-TPL fix is the **safety net**
beneath it for anything that slips through. Roll the guard out **warn-first**
(log + proceed via the raised-TPL fallback) so existing consumers don't break
on contact, then flip to hard `AXL_BUSY` once call sites are converted.

**Scope / blast radius.** Anything that today relies on sync-from-callback
nesting now warns (then errors). Requires an audit of in-repo call sites first.
Possible escape hatch for a deliberate nested-loop case (an explicit
`axl_loop_run_nested()` that opts in) — only if a legitimate one is found.

**Test.** From inside a loop callback, `axl_udp_send` / `axl_wait_ms` →
`AXL_BUSY` (or warn in warn-mode). From a no-loop context → still works.

**Status:** ✅ Done (warn-first). A process-global callback-depth
(`_axl_loop_in_callback()`, src/loop/axl-loop.c) brackets every dispatched
loop callback — source, timer, idle, keypress, and drained deferred work. The
sync wait primitive `_axl_event_wait_timeout_with_tick` (src/event/axl-wait.c)
checks it and logs a warning when a blocking wait is nested inside a callback;
it still PROCEEDS (the raised-TPL fallback keeps it from wedging). Catches the
foreground-callback AND driver-pump (raised-TPL) cases with one check.
Verified: 0 spurious warns across the full HTTP suite (189 tests) and the
HTTPS driver-pump test — only deliberate sync-from-callback warns. Marker
unit-tested (`test_loop_in_callback_marker`, RED-confirmed).

**The hard-error flip was evaluated (2026-06-16, after Item 3 shipped + SoftBMC
migrated) and REJECTED — warn-only is the permanent state.** The in-repo
call-site audit found:
1. **The flip's precondition was never built.** It was sequenced after Item 2
   (deferred responses), which was itself deferred because *zero* handlers do
   in-handler I/O (all CPU-only). A hard `AXL_BUSY` with no deferred-response
   escape would strand any future in-handler-I/O handler.
2. **The library itself reaches the wait primitive from inside callbacks** —
   `axl_tcp_close`'s sync-fallback `_axl_tcp_wait` runs from
   `on_connect_complete`/`on_connect_cancel` teardown (axl-tcp-async.c, which
   NULL `async_loop` to force an inline close). A hard error there is the library
   tripping its own guard.
3. **No async peer exists for `axl_net_resolve_ptr` (reverse DNS) or DHCP**, both
   of which route through the primitive — flipping them would break a
   sync-from-callback consumer with no migration path (the Ask-1 concern).
4. **The flip would not catch the target pattern anyway:** phases 3–4 moved the
   main sync HTTP/UDP/TCP ops onto direct ephemeral `axl_loop_run`, a different
   path than the warned `_axl_event_wait_timeout_with_tick`.

The strategic goal — services are async-first, no sync-net-from-a-callback — is
already achieved via the async client (Item 3) + the SoftBMC migration. The warn
(loud, debuggable, proceeds via the raised-TPL net) is the correct permanent
behavior. **Item 1 is now fully complete (no pending flip).**

### Item 2 — Deferred HTTP responses  ·  *the linchpin*

**What.** Let a handler defer its response: return an `AXL_HTTP_PENDING`
sentinel, kick off async work, and complete later via
`axl_http_response_finish(token)` (or `axl_http_request_respond(req, resp)`)
from a callback.

**Why.** Without it, an I/O-bound handler has *no choice* but to block (§2). It
is the precondition for "single loop, no nesting" on the server path: once
handlers can defer, the sync net APIs become unnecessary inside handlers and
Item 1 can forbid them there.

**Design sketch.** The server connection lifecycle
([src/net/axl-http-server.c](../src/net/axl-http-server.c) + conn/dispatch
units) grows a *pending* state: on `AXL_HTTP_PENDING`, hold the connection open
(don't send, don't recycle the slot), keep `req`/`resp` alive, and hand the
handler a completion token. Must handle: a pending response that never
completes (connection timeout), client disconnect while pending, and the
interaction with keep-alive and the WebSocket upgrade path (which already does
its own thing).

**Scope / blast radius.** Largest item; touches connection lifecycle. Gate the
design on the Item 3 handler-I/O audit (how many handlers actually need it).

**Test.** A handler that defers, schedules a timer, and completes on the timer
→ client receives the response; a deferred handler whose client disconnects →
clean teardown, no leak.

**Handler-I/O audit result (2026-06-16) — gates this item.** SoftBMC's ~16 HTTP
route handlers (module-manager, files, dashboard-core, softbmc) are **all
CPU-only** — read SMBIOS/inventory, serve files, format JSON. The **only**
downstream I/O in SoftBMC is in `alert.c` (`axl_http_post` webhook,
`axl_udp_send` syslog), and that is **timer-driven outbound transport, NOT an
HTTP request handler** — it already works (sync UDP/TCP are now pump-safe via
the raised-TPL fixes; Item 1 warns if nested). **So zero HTTP handlers need
deferred responses today.** Building this 5-hazard public API for no consumer
contradicts the plan's own gate and `AXL-Concurrency.md`'s "don't build it
speculatively." → **Deferred until a concrete in-handler-I/O handler exists**
(e.g. a reverse-proxy or auth-against-a-remote-IdP route on the dashboard
server).

**Design map (captured for when it IS built — server-lifecycle survey).**
- Handler invoked at `axl-http-dispatch.c:354`; `send_response` runs
  synchronously right after, and the **stack** `AxlHttpResponse` is freed at
  `dispatch.c:414-420`. A `PENDING` return must skip the send + free and move
  the response to **conn-owned** storage.
- The closest precedent is **`conn->upload_resp`** (a conn-embedded
  `AxlHttpResponse` that already survives many async chunk callbacks and is
  freed in `on_response_sent`/`reset_connection`); the **WS-upgrade hold-open**
  + the `ws_out_*` outbound FIFO are the second precedent for "hold a conn open
  past the handler and send from later code." Reuse both.
- `on_response_sent` is the "on the wire" completion + keep-alive re-arm hook;
  `axl_http_response_finish(token)` would fill the conn-owned response and call
  the existing `send_response`.
- **Hazards (ranked):** (1) token validity across slot recycling — a raw
  `HttpConn *` token can dangle onto a memset+reused slot (`conn.c:492`); needs
  a generation/handle, OR keep a recv armed during deferral; (2) client
  disconnect while pending is **invisible** (no recv armed) until `finish`'s
  send fails — WS avoids this by keeping recv armed; (3) **no request/idle
  timeout exists** (`keep_alive_sec` is boolean-only) so a never-finished
  deferral pins a pool slot forever — a new timer is required; (4) pool
  starvation (`max_conns` default 8); (5) conditionalizing the dispatch-tail
  send+free on `PENDING`.

**Status:** Design-ready, **deferred** — no current consumer (audit above).
Build when a concrete in-handler-I/O route appears; contract-first design review
at that point (it is public surface).

### Item 3 — Async-API coverage + convert call sites

**What.** Enumerate the sync networking APIs services call from handlers;
provide async equivalents where missing; convert the call sites.

**Why.** Async-first is only viable if the async primitives exist for every
in-handler I/O need.

**Already async:** `axl_udp_send_async`, `axl_tcp_send_async`,
`axl_udp_recv_async`, `axl_tls_handshake_async`. **Likely gaps to confirm:**
async DNS resolve, an async HTTP *client* (for proxy/auth-against-remote
handlers).

**Scope.** Discovery first (audit SoftBMC handlers — see Open Questions). The
conversion of SoftBMC's own handlers happens consumer-side, in a SoftBMC
session.

**Audit result (2026-06-16).** Existing async ops + the now-pump-safe sync ops
cover every current consumer. SoftBMC's only outbound I/O is the alert
transport: syslog (`axl_udp_send` — `axl_udp_send_async` exists; sync is now
pump-safe) and webhook (`axl_http_post` — sync, now pump-safe). The one true
gap is an **async HTTP client** (no `axl_http_get_async`/`_post_async` exists),
but its only would-be consumer (the webhook) works synchronously today, so
there is no forcing need. Async DNS: not currently called from any handler.

**Status:** **Deferred** — no gap with a current consumer. Build the async HTTP
client when a handler must proxy/fetch without blocking (pairs with Item 2).

### Item 4 — Keep the raised-TPL fix as a safety net  ·  *done; reframe*

**What.** The backend raised-TPL fallback (`d249a9b6`,
[src/backend/native/axl-backend-native-event.c](../src/backend/native/axl-backend-native-event.c))
stays — but as **defense in depth, not the strategy.**

**Why.** A stray sync call from a pump degrades to a bounded latency spike
instead of a hard wedge. It is the net beneath Item 1's policy, not a license
to block on a running loop.

**Status:** ✅ Shipped (`d249a9b6`). No further work beyond keeping the docs
honest that it is a fallback.

### Item 5 — Split `axl_yield()`: library code must not re-dispatch the consumer's loop

**What.** `axl_yield()`
([src/runtime/axl-runtime.c:55](../src/runtime/axl-runtime.c#L55)) does two
things: poll the Ctrl-C break flag, *and* — if the default loop exists —
non-blocking-dispatch it (firing real consumer defer/idle/timer/event
callbacks). It is called from **8 in-library sites** (digest, sort, fs ×3,
http-client ×2, ipmi-kcs). All 8 want *only* the break check. Factor the
break-flag + auto-exit logic into an internal `_axl_poll_break()` and point the
8 sites at it; leave **public `axl_yield()` unchanged** (it keeps pumping the
default loop for app code that opts into that idiom).

**Why.** Today, if a consumer runs on the default loop
(`axl_loop_run(axl_loop_default())` — the *documented* server idiom — or
`AxlService`), an in-library `axl_yield()` re-dispatches the consumer's sources
re-entrantly from deep inside an unrelated op (worst case: an HTTP-client call
from a handler re-enters the server's accept/recv sources mid-request). Same
disease as §1: library code re-entering consumer dispatch unannounced.

**Why option (b), not strip-it-from-`axl_yield`.** The loop-dispatch is a
deliberate, **documented, test-pinned** public contract for *app* code
(`AXL-Lifecycle.md §2.4` "yield services the timer";
`test_yield_dispatches_ready_work`). Stripping it would break that contract.
The 8 library callers, by contrast, provably need only the break check
(http-client already runs its own ephemeral loop for the actual I/O).

**Scope / blast radius.** Zero consumer-visible change, no doc rewrite, no test
change. One uncertainty: an *external* consumer relying on an in-library
yield to also pump *their* default loop (fragile, undocumented — nothing
in-repo does it).

**Test.** A library call (e.g. `axl_digest` over a big buffer) does **not**
fire a user source registered on the default loop, while a direct
`axl_yield()` still does.

**Status:** ✅ Done. `_axl_poll_break()` (src/runtime/axl-runtime.c, declared
in axl-signal-internal.h) is the break-only poll; the 8 in-library `axl_yield()`
call sites (digest, sort, fs ×3, http-client ×2, ipmi-kcs) now call it instead,
so library code never re-dispatches the consumer's default loop. Public
`axl_yield()` is unchanged (still pumps the default loop — the documented
yield-as-scheduler contract). Test `test_library_yield_does_not_dispatch`
(axl-test-runtime.c): `axl_qsort` over a large array does NOT fire a registered
default-loop idle, while a direct `axl_yield()` still does. RED-confirmed.
SoftBMC was **not** exposed (it runs on its own explicit loop,
[softbmc.c:242](../../softbmc/src/softbmc.c#L242), never the default loop), so
this hardens the blessed path rather than fixing an active fire.

---

## 5. Sequencing

1. **Item 5** (yield split) — smallest, isolated, no consumer impact. Lands
   the "library never implicitly re-dispatches" principle.
2. **Item 1** (re-entrancy guard, warn-first) — start catching every
   sync-on-running-loop regression loudly. Pairs with an in-repo call-site
   audit.
3. **Handler-I/O audit** (Item 3 discovery) — measure how much in-handler I/O
   SoftBMC actually does; this scopes Item 2.
4. **Item 2** (deferred responses) — design-reviewed, then built. The
   precondition for flipping Item 1 to hard-error on the server path.
5. **Item 3** (fill async gaps) + SoftBMC handler conversion (consumer-side).
6. ~~Flip Item 1 to **hard `AXL_BUSY`** for services once handlers are
   converted.~~ **Evaluated 2026-06-16 and rejected** — warn-only is the
   permanent state (see Item 1 status for the audit rationale). The async-first
   goal was met via Item 3 + the SoftBMC migration; a hard error would harm the
   library's own teardown and break sync DNS/DHCP-from-callback consumers for no
   benefit.

Item 4 is already shipped and underpins all of the above. **All items are now
complete or intentionally closed; this plan is DONE.**

## 6. Open questions

- **Item 1:** warn-first vs hard-error timeline; is there *any* legitimate
  deliberate-nesting case that needs an explicit opt-in escape hatch?
- **Item 2:** the deferred-response API shape (token vs `req`-keyed); timeout
  and client-disconnect-while-pending semantics; interaction with keep-alive
  and the WS upgrade path.
- **Scoping:** how many SoftBMC handlers actually need in-handler async I/O? If
  most are CPU-only (read SMBIOS, format JSON) and only a few proxy/auth
  remotely, Item 2 is small and targeted. **This audit gates the size of the
  whole effort.**

## 7. Non-goals

- A macro `async`/`await` layer — rejected in `AXL-Concurrency.md` and still
  rejected (legibility cost, surprises UEFI devs).
- Multithreading — UEFI is single-threaded; not on the table.
- Removing the sync APIs for CLI tools — they are correct and ergonomic there;
  the guard just makes them self-protecting.
- A speculative `AxlFuture`/promise layer — only if a concrete pain point
  survives Items 1–3.
