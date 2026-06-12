# AXL Dashboard-Server Design / Roadmap

**Status:** DRAFT — living doc, for iteration. Opened 2026-06-11.
**Scope:** the HTTP-server enhancements axl-sdk would want to be a
first-class base for a "native-SPA" eHTML5 dashboard (the BMC web-UI
route: server-rendered shell → single-page app, live telemetry, secure
config/firmware upload). This is a candidate **sub-project** of axl-sdk.

This doc starts from an external enhancement proposal (a heavy consumer's
wishlist, §1 below) and layers an axl-sdk-side assessment on top (§2+):
re-baselined against what already ships, with the substrate-vs-application
line drawn explicitly. Nothing here is committed — it's the thing we
iterate on before writing code.

---

## 0. Guiding principle — where the line is

axl-sdk is "GLib-for-UEFI": a general-purpose library, not a web
framework. The single most important call for this sub-project is
**which pieces are substrate (general HTTP/transport primitives that
belong in axl-sdk) vs. application/SoftBMC policy (Redfish semantics,
role models, the asset bundler, cert-storage policy) that belong in the
consumer.** Several wishlist items straddle that line; the default answer
when unsure is "ship the *mechanism* in axl-sdk, leave the *policy* to the
consumer."

A web framework with opinions baked into a firmware library is a
liability: binary footprint and attack surface both matter for a BMC
component, and opinions age badly. Provide hooks and primitives; let the
consumer own semantics.

---

## 1. The original proposal (condensed, attributed to the consumer)

> **Tier 1 — high value, modest effort**
> 1. **SSE + pubsub→push bridge** — `text/event-stream` helper fanning an
>    `axl-pubsub` topic to all subscribed connections. (SSE beats WS for
>    one-way telemetry: simpler, proxy-friendly.)
> 2. **Real SPA static-asset pipeline** — build-time bundler (embed `dist/`
>    via `axl-embed`), content-type by extension, gzip/brotli precompression
>    + `Accept-Encoding` negotiation (`axl-compress` exists),
>    ETag/Cache-Control/304, SPA fallback (serve `index.html` for unknown
>    non-API paths).
> 3. **Multipart/form-data + streaming upload** — firmware/ISO/config upload
>    streamed to RAM-disk/ESP without buffering the whole body.
>
> **Tier 2 — security, do-once-in-substrate**
> 4. **Auth/session/RBAC middleware + token store** — login → session token
>    (cookie/bearer) → middleware validate → role check, matching Redfish's
>    SessionService shape.
> 5. **TLS self-signed cert lifecycle** — generate + persist a cert
>    (CN/SAN = host/IP) on first boot, replace/CSR later.
>
> **Tier 3 — standards alignment (optional)**
> 6. **Redfish/REST helpers on `axl-json`** — typed resource handlers, OData
>    `$select`/`$expand`/`$filter`, a standard `@odata` error envelope,
>    per-resource ETags, a Redfish EventService over the SSE primitive.
>
> **Tier 4 — perf, later**
> 7. **HTTP/2** (multiplexing many small asset GETs) and **range requests**.

The direction is sound and domain-aware. The assessment below re-baselines
it.

---

## 2. Current baseline — what already ships (don't rebuild)

Re-inventoried against `include/axl/axl-http-server.h`, `axl-tls.h`,
`axl-pubsub.h`, and `src/net/` as of 2026-06-11. The net-new surface is
**smaller and differently shaped** than the 7 items imply.

| Proposal item | Already in tree | Real remaining gap |
|---|---|---|
| #7 range requests | **Done** — `axl_http_response_set_range` / `set_content_range` | HTTP/2 only |
| #5 self-signed cert | **Done** — `axl_tls_generate_self_signed` (ECDSA P-256) | persist/rotate **lifecycle** (mostly policy) |
| (push) | **Done** — WebSocket: `add_websocket`, `ws_broadcast`, `axl-http-ws.c` + `axl-websocket.c` | SSE is a *different* (simpler, one-way) primitive — still wanted |
| #4 auth | **Hook present** — `use_auth`, `add_route_auth`, middleware pipeline (`use`) | session token **store** + RBAC **policy layer** |
| #2 static serving | **Present** — `add_static(prefix, fs_path)` serves a directory; `set_static` = single embedded blob; `set_file` | encoding negotiation, ETag/304, SPA fallback (the *polish*) |
| #2 caching | **Server-side response cache** — `use_cache(max_entries)` + `set_route_ttl` (memoization, TTL) | this is NOT HTTP conditional caching; client-side ETag/`If-None-Match`/304/`Cache-Control` is the gap |
| #1 SSE | **Substrate present** — `set_streamer` (pull-based chunk source) + `axl-pubsub` (topic register/subscribe/publish) | the bridge + **backpressure policy** (the hard part) |
| #3 multipart | **Absent** — `add_upload_route` streams the *raw* body | `multipart/form-data` parser |
| #2 content-encoding | **Absent** in the response/dispatch path (`axl-compress` exists but isn't wired to responses) | `Accept-Encoding` negotiation + gzip response encoding |

Genuinely-missing surface, distilled: **SSE bridge, response
content-encoding negotiation, ETag/304 conditional requests, multipart
parser, a session/RBAC *mechanism*, cert *lifecycle*.** Tighter than 7.

---

## 3. Re-prioritized gap list (substrate first)

Each item tagged **[substrate]** (belongs in axl-sdk) or **[app]**
(consumer/SoftBMC), with effort/risk and the actual design problem.

### 3.1 SSE pubsub→push bridge — **[substrate]**, *optional* (was Tier 1)
- **Priority note (per the SoftBMC inventory):** the first consumer,
  SoftBMC, already pushes live telemetry over **WebSocket** (which axl-sdk
  ships), so SSE is **adopt-or-skip, not a port blocker** — build it when a
  one-way stream genuinely wants the simpler/proxy-friendlier transport (or
  for a Redfish EventService), not on the critical path. It remains a clean,
  generally-useful primitive; it's just no longer the must-do-first item it
  looked like before we walked the consumer.
- **What:** a `text/event-stream` response helper that subscribes a
  connection to an `axl-pubsub` topic and emits each message as an SSE
  `data:` frame, built over the existing `set_streamer`.
- **The real work is NOT the wiring — it's backpressure.** The streamer
  runs on the connection's task and the header already warns "a slow
  streamer stalls other connections." A dead/slow browser tab must not
  stall the pubsub publisher or the event loop. **Design question #1:**
  per-connection bounded queue with an explicit **drop-oldest** or
  **disconnect-slow-client** policy, non-blocking writes, and a publisher
  that never blocks on a slow subscriber. Get this wrong and one laptop
  that closed its lid wedges the dashboard for everyone.
- Secondary: SSE `id:`/`Last-Event-ID` replay (optional), heartbeat
  comments to keep proxies open, `retry:` hint.
- **Effort:** modest once the queue policy is decided. **Risk:** medium
  (the backpressure model is load-bearing).

### 3.2 Static-asset *polish* — **[substrate]**, Tier 1
Build on the existing `add_static`/`set_static`; this kills the most
boilerplate and **makes HTTP/2 moot** (see §4).
- **Content-encoding negotiation:** wire `axl-compress` into the response
  path — `Accept-Encoding: gzip` → serve a precompressed variant (prefer
  build-time `.gz` siblings to avoid per-request CPU on a BMC), set
  `Content-Encoding` + `Vary`.
- **ETag + conditional requests:** emit a strong/weak ETag, honor
  `If-None-Match` → `304 Not Modified` (the status string already exists;
  the handling does not). Same for `Last-Modified`/`If-Modified-Since`.
  Distinct from the existing *server-side* response cache.
- **`Cache-Control`** per asset class (immutable hashed bundles vs
  `index.html` no-cache).
- **SPA fallback:** serve `index.html` for unknown non-API GETs so
  client-side routing works. Needs a clean "is this an API route or an
  asset route?" predicate so the fallback never shadows a 404 that should
  be a 404.
- **NOT in scope:** the build-time **bundler** is glue (a Makefile/script
  + the existing `axl-embed`), not a C API — see §4.
- **Effort:** moderate, mostly independent pieces. **Risk:** low.

### 3.3 Multipart/form-data parser — **[substrate]**, Tier 1
- **What:** a streaming `multipart/form-data` parser feeding the existing
  upload path, so firmware/ISO/config uploads stream to RAM-disk/ESP
  without buffering the whole body. Today `add_upload_route` streams the
  *raw* body only.
- **Design:** incremental boundary scanner over the chunked body; per-part
  `Content-Disposition`/filename/`Content-Type`; bounded part headers;
  hand each part's payload to a streaming sink. Must respect
  `set_body_limit` and never buffer a whole part.
- **Effort:** moderate (a careful incremental parser). **Risk:** medium
  (parsers are where bugs and DoS live — fuzz it).

### 3.4 Session / RBAC **mechanism** — **[substrate mechanism + app policy]**, Tier 2
- **Substrate:** a generic session-token store (create/lookup/expire,
  cookie *and* bearer), and an RBAC-*enforcement* middleware that checks a
  caller-supplied predicate. The hook (`use_auth`/`add_route_auth`) already
  exists; this is the token store + the validate/role-check plumbing.
- **App (NOT baked in):** the **role model**. Don't hard-code Redfish's
  `Administrator/Operator/ReadOnly` — the substrate gets "this request has
  identity X with claims C; route requires predicate P," the consumer
  defines the roles. Match the *session shape* to Redfish SessionService so
  the consumer can implement it without fighting the substrate.
- **Security-critical → build once, scrutinize hard.** This is the highest-
  risk item; smallest possible mechanism, no clever extras.
- **Effort:** moderate. **Risk:** high (security surface).

### 3.5 TLS cert **lifecycle** — **[mostly app]**, Tier 2
- Generation is **done** (`axl_tls_generate_self_signed`). The gap is
  *persistence/rotation*: store the cert+key on first boot (where? NVRAM,
  ESP, RAM-disk — that's policy), regenerate on host/IP change, support
  replace/CSR. Most of this is "where do I keep it and when do I rotate,"
  which is consumer policy.
- **Substrate add (small):** a convenience that wires CN/SAN from the live
  host/IP and a "load-or-generate-and-persist" helper taking a storage
  callback. Don't pick the storage location for them.
- **Effort:** small. **Risk:** medium (key material handling).

### 3.6 OData / `@odata` helpers on `axl-json` — **[substrate for the cheap bits]**, Tier 3
- **In scope (cheap, general):** `$select`/`$expand` projection helpers
  and the `@odata` error/response envelope shape — these are JSON
  serialization concerns that fit `axl-json`. Per-resource ETag ties into
  §3.2.
- **Defer / out:** **`$filter` is a rabbit hole** — a mini query-language
  that over-fits to Redfish. Ship `$select`/`$expand` + envelope; defer
  `$filter` until a concrete consumer needs it (and then question whether
  it's substrate at all). The Redfish EventService is the consumer's layer
  over the §3.1 SSE primitive, not an axl-sdk feature.
- **Effort:** small for the in-scope bits. **Risk:** low.

### 3.7 HTTP/2 — **[defer, possibly never]**, Tier 4
See §4 — explicit non-goal for now.

---

## 4. Explicit non-goals / pushback

1. **HTTP/2: probably never, not just "later."** In a freestanding UEFI C
   library that's HPACK + framing + flow-control + a stream state machine —
   large and security-sensitive — for a dashboard that fits in HTTP/1.1
   keep-alive. The stated motivation (many small asset GETs) **evaporates
   if the SPA is bundled** (few large assets). Don't let "we'll want
   HTTP/2" justify itself; make §3.2 obviate it. Revisit only on a
   *measured* need.
2. **The asset bundler does not belong in the library.** It's a build step
   (Makefile/script + the existing `axl-embed`), not a runtime C API. Keep
   it in the consumer.
3. **`$filter` is deferred** (§3.6) — mini-query-language, over-fits
   Redfish.
4. **No baked-in role model** (§3.4) — mechanism in axl-sdk, roles in the
   consumer.

---

## 5. Cross-cutting concerns

- **SSE backpressure** (§3.1) — *if/when SSE is built*, this is its design
  problem, not a wiring task: decide the per-connection queue + slow-client
  policy first. (SSE itself is now optional — see §3.1.) The same hazard
  already lives in the shipped WebSocket path, so it's worth getting the
  policy right wherever a slow client can stall the loop.
- **Security surface / footprint.** Every item adds binary size and attack
  surface to a firmware component. Auth (§3.4) and TLS (§3.5) are the
  security-critical ones; parsers (§3.3) are the DoS-prone ones — fuzz
  them. Keep each mechanism minimal.
- **CORS** — irrelevant same-origin (the shipping dashboard), but matters
  if the dev workflow serves the SPA from a separate dev server against the
  live BMC API. A tiny opt-in CORS middleware may be worth it for DX.
- **Testing.** Each gets QEMU integration coverage (the project already has
  HTTP + Redfish integration suites to extend). Parsers get fuzz coverage.

---

## 6. Recommended sequence

Shallow dependency graph → order by leverage and risk. Re-ordered after the
SoftBMC inventory (SSE is no longer first — the consumer pushes over WebSocket
today):

1. **Static polish** (§3.2) — encoding negotiation + ETag/304 + SPA
   fallback; independent; kills the most boilerplate; needed by *any* SPA;
   makes HTTP/2 moot.
2. **Session/RBAC mechanism + cert lifecycle** (§3.4, §3.5) — directly
   replaces SoftBMC's hand-rolled `Auth.c`/`Session.c`; security-critical,
   "build once"; session shape ≈ Redfish SessionService.
3. **Multipart parser** (§3.3) — independent; unblocks firmware / virtual-media
   upload; fuzz it.
4. **OData `$select`/`$expand` + `@odata` envelope** (§3.6) in `axl-json`;
   defer `$filter`.
5. **SSE pubsub→push bridge** (§3.1) — *optional / when wanted* (a one-way
   stream that prefers SSE over WS, or a Redfish EventService); *design
   backpressure first* if/when built.
6. **HTTP/2** (§3.7) — only if a measured need survives bundling +
   keep-alive.

---

## 7. Open questions / to iterate

- **Precompressed assets** (§3.2, first up): build-time `.gz`/`.br` siblings
  (no per-request CPU) vs on-the-fly `axl-compress` (simpler, costs CPU per
  request on a BMC)? Leaning build-time.
- **SPA fallback predicate** (§3.2): how does the server know "API route →
  real 404" vs "asset route → index.html"? Explicit API-prefix registration,
  or derive from registered routes?
- **Session storage** (§3.4): in-memory only (lost on reset, fine for a BMC
  session) vs persisted? Token format (opaque random vs signed)? Match the
  semantics of SoftBMC's existing `Auth.c`/`Session.c` (the parity reference).
- **SSE backpressure policy** (§3.1, *only if/when SSE is built*):
  drop-oldest vs disconnect-slow-client vs bounded-block-with-timeout? Per-
  connection queue depth? Decides the §3.1 API shape.
- **Scope check per item:** for each §3 entry, re-confirm substrate-vs-app
  before coding — the §0 line is the whole game.

---

## Appendix — relevant existing surface

- HTTP server: `include/axl/axl-http-server.h` — `add_route[_auth]`,
  `add_static`, `add_upload_route`, `add_websocket`, `ws_broadcast`,
  `use`/`use_auth`/`use_cache`, `set_streamer`, `set_range`/
  `set_content_range`, `set_static`/`set_file`/`set_bytes`/`set_json`,
  `set_keep_alive`/`set_body_limit`/`set_max_connections`.
- Pubsub: `include/axl/axl-pubsub.h` — `register`/`subscribe`/
  `unsubscribe`/`publish`/`reset` (topic-based).
- Compress: `include/axl/axl-compress.h` — gzip/zlib/DEFLATE codec +
  stream filters (not yet wired to HTTP responses).
- TLS: `include/axl/axl-tls.h` — transport + `axl_tls_generate_self_signed`.
- Embed: `axl-embed` — build-time blob embedding (the bundler's substrate).
- Source: `src/net/axl-http-*.c` (core/conn/dispatch/request/response/
  route/server/upload/webdav/ws/client), `axl-websocket.c`, `axl-tls.c`.
