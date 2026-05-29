# axl-display — Network Display Protocol Design

**Status:** design — not started. Target: server in axl-sdk
(`src/display/`); client library hosted-build target, location TBD
when implementation starts (likely a sibling repo since axl-sdk is
freestanding UEFI, but no commitment yet).
This doc establishes the design before implementation; supersedes
the Phase R stub in [ROADMAP.md](ROADMAP.md).

## Goal

A network protocol that lets a remote process drive a UEFI-side
display server. The remote client says "fill rectangle, draw text,
present buffer"; the UEFI side renders against its local GOP
framebuffer. Mouse / keyboard / touch events flow the other way.

X-shaped (the client/server boundary is the network boundary by
design), not Wayland-shaped (compositor-and-clients-share-memory is
the wrong model when the parties are on different machines).

The protocol is extension-first: a small core covers basic remote
rendering and input; everything else — including the screen-mirror
("VNC-shape") capability — slots in as a named extension. Designed-in
extensibility is the X11 lesson worth keeping; the 1980s opcode-
allocation mechanics are not.

## Non-goals

- **Not Wayland-shape.** Buffer-passing over a network is the wrong
  bandwidth/latency profile.
- **Not OpenGL / GLX.** UEFI has no GPU driver stack; remote-3D is
  not a use case worth designing for.
- **Not an OS-level windowing system.** No multi-app coexistence on
  the same display; pre-boot UEFI typically has one app foregrounded.
- **Not a font server.** Fonts ship with the server (axl-gfx's built-
  in `AxlFont` table) or are uploaded by the client; no separate
  font-service protocol.
- **Not transport-agnostic.** TCP only for v0.1 — that's what UEFI
  has. Unix sockets / shared memory are not in the picture.

## Layering

```
       Remote process (Linux / macOS / Windows / another UEFI host)
                              │
                              ▼
                       libaxl-display
                              │  protocol over TCP
                              ▼
                       axl-display (axl-sdk)
                       │       │       │
                       ▼       ▼       ▼
                  axl-gfx  axl-input  axl-loop
                              │
                              ▼
                  UEFI GOP + Simple Pointer + Simple Text Input
```

**Server side** lives in `src/display/` as a peer to `src/gfx/` and
`src/input/`. Runs on `axl-loop`, draws via `axl-gfx`, reads input
via `axl-input`. Adds **nothing** to those substrates — same
discipline rule as AGT
([AGT-Design.md §"Substrate discipline rules"](https://github.com/aximcode/agt/blob/main/docs/AGT-Design.md#substrate-discipline-rules)).

**Client side** is a normal Linux / macOS / Windows shared library
— hosted code, can't share axl-sdk's freestanding-UEFI build. When
implementation starts, the client probably lives outside axl-sdk
proper (sibling repo, or its own subtree with a host toolchain); the
decision is deferred until R1a actually needs it. Reference language
is C; idiomatic bindings (Python, Rust, C++) sit on top of the C ABI
in the same location.

Names follow X11's role split — server / client library / reference
tool — without inventing a protocol acronym:

- **`axl-display`** — the UEFI server (binary: `axl-display.efi`)
- **`libaxl-display`** — the host-side client library
- **`axl-display-viewer`** — reference CLI tool that connects + opens
  a host window mirroring the remote framebuffer (Phase R6)
- The wire format itself is "the axl-display protocol" — no
  acronym, mirroring how "the Redis protocol" or "the HTTP/2 protocol"
  is referenced without a TLA.

Toolkits that target axl-display (e.g. AGT's hypothetical remote-
backend) sit on top of `libaxl-display`, not on top of the raw wire
format.

## Wire protocol

### Framing

Length-prefixed TLV records on a single bidirectional TCP stream.
Each record:

```
+---------+---------+---------+---------+
| length (u32)      | type-tag (u16)    |
+---------+---------+---------+---------+
| flags (u16)       | payload …         |
+---------+---------+---------+---------+
```

- `length` covers the whole record including header.
- `type-tag` identifies the request / event / reply kind.
- `flags` reserved for per-record metadata (compression hint,
  fragment indicator, reply-expected bit).

Records are sent without explicit per-record acknowledgement; the
transport is request-stream + reply-stream + event-stream
multiplexed onto one TCP connection, with reply records correlated
to requests by a client-supplied sequence number in the payload (X11
shape).

### Type-tag namespace

Type-tags are interned strings. At connection setup the client and
server exchange a list of `(namespace, name)` pairs they intend to
use; each side returns a `u16` tag the other should use thereafter.
Tag 0 is reserved for the framework itself
(QueryExtension, ListExtensions, RegisterTag, error reply).

```
core/poly_fill_rect          → tag 17  (assigned by server at handshake)
core/draw_text               → tag 18
mirror/get_region            → tag 64  (after QueryExtension("mirror"))
mirror/damage_event          → tag 65
```

Why string-namespaced, not opcode-allocated: X11's 1-byte opcode
range (128–255 reserved for extensions) is a 1980s memory
constraint. With ~16 bytes of one-time setup cost, extensions get a
flat 65 535-tag space and no central allocation authority. Cost-
recovery on the wire is identical to a fixed-opcode design after
handshake.

### Request / event / reply correlation

Each request carries a `u32` sequence number. Replies and errors
echo it; events carry sequence 0. This is the X11 model; clients
chain requests asynchronously and match replies as they arrive.

## Resource model

Drawables, graphics contexts, and uploaded resources (fonts,
glyphs, pixmaps) are referenced by client-allocated 32-bit IDs.
Each session gets an ID range (`(client_id << 24) | resource_id`)
to prevent collisions across multiple clients. X11's resource-ID
discipline carries over verbatim — it's a good answer.

| Resource | Maps to | Notes |
|---|---|---|
| Drawable | `AxlGfxBuffer` or screen | The screen is drawable-id 0 |
| GC | server-side state | Color, font, clip stack |
| Font | `const AxlFont *` | Built-in name OR uploaded glyphs |
| Pixmap | `AxlGfxBuffer` | Same as drawable; offscreen targets |

A graphics context (GC) is server-side mutable state: color, font,
clip, draw target. Requests don't repeat these on every call —
clients update the GC, then send draws that reference it. Saves
bytes on the wire and mirrors `axl-gfx`'s native call shape.

## Core drawing requests

Mechanical serialization of `<axl/axl-gfx.h>`, split across two
phases by complexity:

**Phase R1a — minimal interactive subset:**

| Request | axl-gfx call | Notes |
|---|---|---|
| `create_gc(gc_id)` | server-side | initial defaults |
| `set_gc_color(gc_id, color)` | server-side GC update | |
| `set_gc_font(gc_id, font_ref)` | server-side GC update | built-in fonts only in R1a |
| `poly_fill_rect(gc_id, rects[])` | `axl_gfx_fill_rect` × N | batched |
| `draw_text(gc_id, x, y, utf8)` | `axl_gfx_draw_text` | |
| `present_screen()` | `axl_gfx_buffer_present` (implicit) | full-screen flush |

R1a targets the screen drawable only; no off-screen buffers, no
clipping, no full GC. Just enough to land an end-to-end "Hello world
from Linux to QEMU UEFI" client demo.

**Phase R1b — full drawing surface:**

| Request | axl-gfx call | Notes |
|---|---|---|
| `create_drawable(id, w, h)` | `axl_gfx_buffer_new` | off-screen buffer |
| `free_drawable(id)` | `axl_gfx_buffer_free` | |
| `target(drawable_id)` | `axl_gfx_target_buffer` | `id == 0` → screen |
| `set_gc_clip(gc_id, rects[])` | `axl_gfx_push_clip` × N | flat in protocol, stack server-side |
| `poly_line(gc_id, points[])` | `axl_gfx_draw_polyline` | |
| `poly_draw_rect(gc_id, rects[])` | `axl_gfx_draw_rect` × N | outlines |
| `put_image(drawable, x, y, pixels)` | `axl_gfx_blit` | |
| `get_image(drawable, x, y, w, h) → reply` | `axl_gfx_capture` | reply-bearing |
| `present(drawable, dst_x, dst_y)` | `axl_gfx_buffer_present` | |

Each request is two-way only if it returns data (`get_image`); fire-
and-forget otherwise. Errors are asynchronous reply records that
reference the offending request's sequence number.

## Input events (Phase R2)

Server→client event records carrying serialized `AxlInputEvent`.
Subscription is per-session: client sends `select_input(mask)` once
with the event-type bitmask it wants, and the server pushes matching
events as `axl-input` produces them.

Subscription bits map 1:1 to `AxlInputType` (mouse_move, key_down,
touch_*, etc.). No coordinate translation server-side — the client
sees device-native coordinates and rescales itself, matching axl-
input's existing contract.

## Extension framework

A single core mechanism, used the same way by every extension:

```
1. Client: query_extension("mirror")
2. Server: extension_info{ name="mirror", version=1, base_tag=64, ok=true }
3. Client: register_tag("mirror/get_region")   → server returns tag 64
4. Client: register_tag("mirror/damage_event") → server returns tag 65
5. Client uses tags 64/65 in subsequent records
```

Extensions are compiled into the server build at link time; runtime
extension loading is out of scope for v0.1 (UEFI has no dlopen).
`query_extension` returns `ok=false` when an extension isn't
present, and clients are responsible for graceful degradation.

### Worked example: `mirror` extension (Phase R4)

The "VNC-shape" / screen-capture use case modeled as an extension:

| Request / event | Maps to | Notes |
|---|---|---|
| `mirror/get_region(x, y, w, h) → reply` | `axl_gfx_capture` | snapshot |
| `mirror/subscribe_damage(rects[])` | server-side dirty tracker | |
| `mirror/damage_event(rects[])` | server → client | dirty notification |
| `mirror/inject_input(event)` | `axl-input` synthesizer (TBD) | mouse/key replay |

A client that only wants RFB-style behavior registers `mirror`,
never touches `core` drawing, and gets the screen-mirror experience.
A client that only wants remote drawing registers `core`, never
touches `mirror`, and gets the X-shape experience. Both run against
the same server simultaneously.

This is why the extension framework matters from day 1 — without it,
the two use cases would be two different protocols.

### Future extensions (sketches)

Listed for design pressure on the core, not committed to:

- `font/upload(glyphs[])` — client-side fonts uploaded as `AxlGlyph`
  arrays; server constructs an `AxlFont` and assigns a font-ref.
- `cursor/set(image, hotspot)` — software cursor for displays
  without hardware cursor support.
- `shm/import_buffer(...)` — local-only (Unix socket transport)
  zero-copy pixel transfer. Not applicable to UEFI but reserved
  in case the client lib someday targets a different server.

## Authentication & security

UEFI is pre-boot. The threat model is **not** desktop X11's; it's
closer to BMC virtual-console. Anyone who can speak to the server
can inject keystrokes into the firmware console — that's the
high-value capability the protocol exposes. Network-attached input
injection is the whole point of the protocol AND its primary risk.

### Layered options, simplest first

**Layer 0 — bind discipline (v0.1 default).** Server binds to
`127.0.0.1` by default. Network exposure requires explicit
`--bind <iface>` (e.g. `--bind 0.0.0.0` or `--bind eth0`). When
bound to a non-loopback interface, an optional `--allowlist <ip,…>`
restricts accepted source IPs. No in-protocol auth at this layer —
intended for management-network / BMC-sideband deployments where the
carrier is already restricted. Defaults shape: same as PostgreSQL or
Redis (localhost-only out of the box). Operators who expose to the
network are making an explicit choice.

**Layer 1 — MIT-MAGIC-COOKIE-shape (Phase R5).** Server generates a
128-bit random cookie at startup, stores it in NVRAM, **prints it
prominently on the console boot log** for out-of-band retrieval, and
exposes a runtime rotation primitive (admin regenerates; old cookie
invalidated). Client must present a matching cookie in the
connection-setup record. Defeats casual scanning; trusts the channel
against active MITM. Same security profile as X11 over an unencrypted
socket. Cost: trivial server-side code. Recommended for network-bound
deployments once R5 ships.

**Layer 2 — PSK challenge-response (Phase R5+ when consumer asks).**
Server holds a long-lived secret (provisioned via firmware setup or
NVRAM). Client proves knowledge via HMAC challenge-response without
sending the secret on the wire. SSH-host-key shape. Defeats passive
sniffing of the cookie. Cost: HMAC-SHA-256 (axl-digest already has
this) + a few hundred lines for the handshake.

**Layer 3 — mTLS (deferred indefinitely).** `axl-tls` exists (opt-
in), so this is *buildable*, but UEFI cert provisioning, expiration
handling, and revocation are operational nightmares. Don't promise
this until a consumer specifically asks AND brings their own cert
management story.

### Per-session resource limits

Independent of authentication: a connected client can exhaust UEFI
memory by allocating drawables. v0.1 caps:

- Drawables per session: 16
- Total pixel memory per session: 64 MiB
- GC count per session: 64
- Request rate cap: 1000 records/sec (anti-busy-loop)

Limits are server-side configurable and refusal is signalled via the
existing error-reply mechanism.

### Input injection auditing

Every key/pointer event the server synthesizes via the protocol is
emittable to `axl-log` (config-gated, off by default since pre-boot
log persistence is unreliable). When on, downstream tooling can
distinguish "user pressed key" from "remote client injected key" in
post-mortem analysis.

### Threats considered but not mitigated in v0.1

- **Eavesdropping** — Layer 0/1 don't encrypt. Move to Layer 3
  when there's a consumer who needs it.
- **Active MITM** — Layer 0/1/2 don't authenticate the server.
  TLS solves it; pre-shared server fingerprint is a poorer
  alternative.
- **Compromised UEFI firmware speaking the protocol** — out of scope;
  pre-boot firmware integrity is Secure Boot's job, not ours.

## Phased plan

Phases are sized to land independently. Each one results in something
testable on its own. End-to-end interactivity (drawing + input)
lands by R2 so feedback-rich iteration starts as early as possible.

### Phase R0 — wire framing + handshake + framework

`<axl/axl-display.h>` declarations, `src/display/` skeleton. All the
machinery that R1+ shouldn't have to invent.

- TLV framing (encode / decode round-trip tests)
- Connection handshake — exchanges byte order, protocol-version
  range, and a **server capability record**:
  - max record size accepted
  - framebuffer dimensions + supported pixel formats
  - server software version
  - extension list (built-in)
- Tag-interning mechanism (`register_tag`, lookup tables both ways)
- Async sequence-number-correlated reply machinery
- **Error-reply framing** — common record type, numeric code +
  symbolic name + sequence-number reference + extension-specific
  payload area. Code space partitioned by namespace (`core/*`,
  `<extension>/*`) so extensions don't collide.
- **Graceful disconnect** — explicit `bye` record vs. raw socket
  close; server distinguishes "client done" from "client crashed"
  for logging and per-session cleanup.
- Layer-0 auth: default bind to `127.0.0.1`; `--bind <iface>` opens
  to the network; optional `--allowlist <ip,…>` for network binds.
- Server binds to an axl-loop source; one session = one TCP
  connection; per-session state owned by the loop source.
- Test harness: tiny in-tree client that does handshake +
  list-extensions + clean disconnect.

No drawing yet. The win is "you can connect to a UEFI box, exchange
capabilities, and disconnect cleanly — and the wire format pins all
the framework concerns the later phases would otherwise re-invent."

### Phase R1a — minimal drawing (end-to-end demo)

Smallest viable serialization of `axl-gfx`: `fill_rect`, `draw_text`,
`present_screen`, plus the GC color + font setters. Screen drawable
only, no clip, no buffers.

- GC server-side state (color + font, R1a subset)
- Resource ID allocation + collision detection (for GCs and the
  font-ref namespace, even though R1a only uses built-in fonts)
- Per-session resource limits (GCs, request rate)
- Client-library prototype (C, hosted build): connect + sample app
  draws "Hello from axl-display" to a remote UEFI box

Visual proof: client running on Linux draws to a QEMU UEFI display
via the protocol.

### Phase R2 — input event channel

Pull input forward of full drawing — the next interactive piece is
more valuable than the rest of the drawing surface.

- Server-side `axl-input` integration; subscription via
  `select_input(mask)` bitmask
- Event-record framing on the server-event side
- Per-session input subscription state (multi-client safe)
- Client-library callback API for event delivery
- Roundtrip test: client clicks mouse, server reports the event;
  client presses key, server reports the keycode

### Phase R1b — full drawing surface

The rest of `axl-gfx` not in R1a: off-screen drawables / target
switching, full clip stack, line / polyline / rect-outline,
put_image / get_image, multi-drawable present.

- `create_drawable` / `free_drawable` / `target`
- `set_gc_clip` flat-in-protocol → push/pop stack server-side
- Remaining draw primitives + the reply-bearing `get_image`
- Per-session resource limits extended (drawable count, pixel
  memory cap)
- Client-library API extended; sample app demonstrates
  back-buffer compositing over the wire

### Phase R3 — first non-trivial extension

Proves the extension framework works end-to-end with something
useful AND not as fragile as `mirror`. Candidate: **`font`
extension** — client uploads `AxlGlyph[]` + font metadata; server
constructs an `AxlFont` and returns a font-ref usable in
`set_gc_font`. Forces the framework to handle extension-allocated
resources and extension-specific replies.

### Phase R4 — `mirror` extension (VNC-shape capability)

The future-option-1 use case as a proper extension.

- `mirror/get_region` — wraps `axl_gfx_capture`
- Server-side dirty-rect tracker (instrumented inside drawing
  primitives — `fill_rect`/`draw_text` etc. accumulate dirty
  regions when a mirror subscription is active; zero overhead
  when no subscriber). Implementation strategy (always-on cost vs.
  hook-pointer installed on first subscribe) is an
  [open question](#open-questions).
- `mirror/damage_event` — pushed to subscribers on present /
  timeout
- `mirror/inject_input` — synthesizes `AxlInputEvent` via
  axl-input's yet-to-be-designed injection API (Phase R4 includes
  adding that to axl-input; substrate-discipline-compatible because
  it's a pure C primitive).
- Reference client: a minimal RFB-shape viewer that connects, gets
  a mirror, and reports input back. Not a full RFB bridge in v0.1 —
  that's downstream work.

### Phase R5 — authentication hardening

Two layers shipped, plus operational hygiene.

- **Layer 1 — MIT-MAGIC-COOKIE-shape**: server generates a 128-bit
  cookie at startup; client presents matching cookie in connection
  setup. Cookie stored in NVRAM and **printed prominently on
  console boot** so admins can retrieve it out-of-band.
- **Layer 2 — PSK challenge-response (HMAC-SHA-256)**: designed +
  stubbed. Full implementation when a consumer asks.
- **Session idle timeout**: configurable; disconnect after N
  minutes of no traffic. Cookie remains valid; client reconnects.
- **Cookie rotation primitive**: admin can regenerate at runtime;
  old cookie invalidated immediately.
- **Input-injection audit log** (axl-log, opt-in): every
  protocol-injected `AxlInputEvent` is logged with session id so
  post-mortem can distinguish remote injection from local input.

### Phase R6 — language bindings + tooling

- `libaxl-display` 1.0 (C library polish)
- Python bindings (ctypes or cffi)
- Rust bindings (manual; bindgen if it cooperates)
- `axl-display-viewer` reference CLI tool — connects + opens a
  window on the host display (GTK or SDL2) mirroring the remote
  UEFI framebuffer via the `mirror` extension

### Scheduling vs. AGT

Phase R is **independent of and parallel to** AGT phases.

- R does not block AGT: AGT v0.1 is local-only and doesn't care
  whether anyone is also remote-driving the same display.
- AGT does not block R: the protocol serializes `axl-gfx`, which
  is already shipped.
- Practical sequencing: R is lower priority than Phase 1 (axlmm
  CPP1 validation) and Phase 2 (AGT bootstrap) because both of
  those have closer consumers. R picks up opportunistically or
  when a real consumer of remote display emerges.

## Open questions

- **Backpressure.** Client floods server with draws faster than
  GOP can keep up — TCP backpressure handles the network, but the
  server's `axl-loop` dispatch can still build up. Need a server-
  side "high-water mark" that stops reading from the socket until
  the dispatch queue drains. Cleaner than per-request flow control.

- **Damage tracking overhead.** Instrumenting every `axl-gfx`
  primitive with dirty-rect updates costs cycles even when no
  mirror subscriber exists. Two paths: (a) accept the cost
  always, (b) install a "damage hook" pointer that the mirror
  extension sets when first subscribed and clears on last
  unsubscribe. (b) is cleaner; (a) is simpler. Decide at R4 with
  benchmarks.

- **GC stacking vs. flat.** X11 has flat GCs (every change is a
  set, no push/pop). axl-gfx has push/pop for clip. The protocol
  could expose either; flat is simpler but loses the clip-stack
  efficiency that AGT will want. Probably: flat GC for color/font,
  push/pop wrapper for clip via dedicated requests.

- **Compression.** Extension hook reserved (`flags` field in TLV
  header) for per-record compression. Not in any planned phase;
  picked up if real-world bandwidth measurements demand it.

- **Multi-display.** UEFI systems with multiple GOP handles
  (multi-output cards, multi-headed servers). v0.1 binds to the
  first GOP handle. Extension for multi-display selection is
  drafted only when a consumer needs it.

- **Authentication bootstrap.** How does the cookie get from the
  UEFI server to a remote client the first time? Options: print
  on serial console, store in NVRAM readable by a UEFI shell
  helper that prints it on demand, push out-of-band via BMC.
  Probably need all three documented as deployment recipes.
